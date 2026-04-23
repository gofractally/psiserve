#pragma once

#include <psiber/detail/bitset_pool.hpp>
#include <psiber/detail/bounded_counter.hpp>
#include <psiber/detail/fiber.hpp>
#include <psiber/detail/mpsc_stack.hpp>
#include <psiber/detail/send_queue.hpp>
#include <psiber/io_engine.hpp>
#include <psiber/spin_lock.hpp>

// Full engine definition required — basic_scheduler owns Engine by value.
#if defined(__APPLE__) || defined(__FreeBSD__)
#include <psiber/detail/io_engine_kqueue.hpp>
#elif defined(__linux__)
#include <psiber/detail/io_engine_epoll.hpp>
#endif

#include <boost/context/continuation.hpp>
#include <boost/context/detail/exception.hpp>

#include <sys/mman.h>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

namespace psiber
{
   /// mmap-based stack allocator for Boost.Context fibers.
   ///
   /// Uses anonymous mmap/munmap instead of malloc/free.  This isolates
   /// fiber stacks from the heap entirely.  Boost.Context's fixedsize_stack
   /// (which uses malloc) causes sporadic heap metadata corruption when
   /// stacks are freed — confirmed on macOS ARM64, other platforms not
   /// yet tested.  The Boost.Context assembly and C++ code are correct by
   /// inspection (no out-of-bounds access), so the root cause appears to
   /// be in the interaction between the system malloc and having sp point
   /// into a malloc'd region during context switches.  The mmap approach
   /// sidesteps the issue because mmap regions are separate VM mappings
   /// with no adjacent heap metadata.
   ///
   /// Stack sizes are rounded up to the system page size.
   struct mmap_stack
   {
      std::size_t size_;

      explicit mmap_stack(std::size_t size = 65536) noexcept
         : size_((size + 4095) & ~std::size_t(4095))
      {
      }

      boost::context::stack_context allocate()
      {
         void* vp = ::mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
         if (vp == MAP_FAILED)
            throw std::bad_alloc();
         boost::context::stack_context sctx;
         sctx.size = size_;
         sctx.sp   = static_cast<char*>(vp) + size_;
         return sctx;
      }

      void deallocate(boost::context::stack_context& sctx) noexcept
      {
         void* vp = static_cast<char*>(sctx.sp) - sctx.size;
         ::munmap(vp, sctx.size);
      }
   };

   /// Policy for handling WorkItem pool exhaustion in post().
   ///
   /// The scheduler maintains a fixed pool of 256 WorkItem slots plus
   /// a configurable heap overflow limit (default 256, adjustable via
   /// `setWorkHeapLimit()`).
   ///
   /// **When to use each policy:**
   ///
   ///   - `fail` — Default.  Throws immediately when the pool is full.
   ///     Use when the caller has a natural way to push back (reject
   ///     the request, close the connection, return an error upstream).
   ///
   ///   - `heap` — Overflow to the heap up to the heap limit.  Use
   ///     for bursty workloads where transient spikes are expected and
   ///     the burst will drain quickly.  Throws `pool_exhausted` when
   ///     the heap limit is reached.
   ///
   ///   - `block` — Preferred for backpressure.  Tries pool, then heap
   ///     overflow, then parks the fiber with a timeout until a slot
   ///     frees.  The caller naturally slows down when capacity is
   ///     saturated.  The timeout (default 1 s) acts as deadlock
   ///     detection — if no slot frees within the deadline, a
   ///     `pool_exhausted` exception is thrown.
   enum class post_overflow
   {
      fail,   ///< Throw pool_exhausted immediately
      heap,   ///< Heap overflow up to limit; throw when exhausted
      block   ///< Pool → heap → park with timeout; throw on likely deadlock
   };

   /// Controls whether try_post() may heap-allocate on pool exhaustion.
   enum class try_post_overflow
   {
      pool_only,   ///< Only use the fixed pool; return false if full
      allow_heap   ///< Overflow to heap (up to limit) before returning false
   };

   /// Thrown when post() pool is exhausted and policy is `fail`, or
   /// when `block` policy times out (likely deadlock).
   struct pool_exhausted : std::runtime_error
   {
      pool_exhausted()
         : std::runtime_error("post() pool exhausted — all 256 WorkItem slots in use") {}
   };
   /// Cooperative fiber scheduler (single OS thread).
   ///
   /// Manages a set of fibers on one thread.  Fibers yield on I/O
   /// (kqueue/epoll), lock contention, or explicit parking.  The scheduler
   /// polls the I/O engine, processes cross-thread wakes and tasks,
   /// and resumes the next ready fiber.
   ///
   /// The Engine template parameter selects the platform I/O backend.
   /// It must satisfy the io_engine concept.  The engine is owned by
   /// value — all calls are direct, no virtual dispatch.
   ///
   /// Cross-thread communication is lock-free:
   /// - wake(): CAS-push fiber onto atomic wake list
   /// - post():  CAS-push callable onto atomic work list
   /// - postTask(): CAS-push task slot onto atomic task list
   ///
   /// kevent trigger is only sent when the receiver is blocked in poll,
   /// not when it's spinning or running fibers.
   ///
   /// **Work item execution:** post() callables are each assigned to
   /// their own fiber, giving them full fiber context (can call post,
   /// sleep, yield, spawn, block on I/O).  A pre-allocated pool of
   /// 256 WorkItems (128-byte inline storage each) provides
   /// zero-allocation post() on the hot path.  Concurrency self-sizes:
   /// when a work item blocks, the scheduler picks up the next ready
   /// item on another fiber.  Fiber pool reuse keeps the common case
   /// (short non-blocking callables) cheap.
   // Forward declarations for friend access
   class thread;
   class reactor;

   template <typename Engine>
   class basic_scheduler
   {
      using Fiber          = detail::Fiber;
      using FiberState     = detail::FiberState;
      using TaskSlotHeader = detail::TaskSlotHeader;

      friend class thread;
      friend class reactor;

     public:
      ~basic_scheduler();

      /// Thread-local access.  Returns the Scheduler for this OS thread.
      /// Creates one on first access if none exists (lazy init).
      static basic_scheduler& current();

      // ── Fiber lifecycle ─────────────────────────────────────────────

      /// Create a new fiber that will execute `entry` on its own native stack.
      /// Templated to avoid std::function heap allocation — the callable is
      /// moved directly into the Boost.Context closure on the fiber's stack.
      /// Optional name for debugging (must point to stable storage).
      /// Optional stack_size in bytes (0 = use default_fiber_stack_size).
      template <typename F>
      void spawnFiber(F&& entry, const char* name = nullptr, std::size_t stack_size = 0);

      /// Main scheduler loop.  Runs until all fibers complete or interrupt().
      void run();

      /// Signal the scheduler to stop after current fibers drain.
      void interrupt() { _interrupted = true; }

      /// Set a global shutdown check function.
      void setShutdownCheck(std::function<bool()> check) { _shutdownCheck = std::move(check); }

      // ── Fiber suspension ────────────────────────────────────────────

      /// Suspend current fiber until fd is ready for the given events.
      void yield(RealFd fd, EventKind events);

      /// Suspend current fiber for a duration.
      void sleep(std::chrono::milliseconds duration);

      /// Park the current fiber (for mutexes, promises, arbitrary waits).
      /// The fiber stays parked until explicitly woken via wake().
      void parkCurrentFiber();

      /// Yield the current fiber back to the ready queue.
      /// The fiber is re-enqueued and will be resumed later.
      void yieldCurrentFiber();

      /// Place a fiber just promoted to strand::_active into this
      /// scheduler's local ready queue.  Used by callers that obtained
      /// a fiber from strand::release() (e.g. strand::sync migration)
      /// and need to schedule it locally — matches the run loop's
      /// post-release handoff.
      void promoteStrandWaiter(Fiber* fiber);

      /// Register a POSIX signal for fiber-aware delivery.
      /// Must be called before run(). Blocks the signal from default handling.
      void registerSignal(int signo);

      /// Suspend the current fiber until the given signal is delivered.
      /// The signal must have been registered via registerSignal().
      void waitForSignal(int signo);

      // ── Cross-thread operations (thread-safe) ───────────────────────

      /// Wake a parked fiber.  Thread-safe — any thread can call this.
      static void wake(Fiber* fiber);

      /// Post a fire-and-forget callable to run on this scheduler's thread.
      ///
      /// Thread-safe — any thread (fiber or non-fiber) can call this.
      /// Zero heap allocation — the callable is placement-new'd into a
      /// pre-allocated pool of WorkItems (256 slots, 128 bytes each).
      ///
      /// **Execution model:** Each callable gets its own fiber with
      /// full fiber context — callables can call post(), sleep(),
      /// yield(), spawnFiber(), do I/O, and use fiber-aware locks.
      /// If a callable blocks, the scheduler picks up the next ready
      /// work item on another fiber.  Concurrency self-sizes to the
      /// blocking depth.
      ///
      /// **Requirements:**
      ///   - `alignof(F) <= 16`
      ///
      /// **Size tiers (compile-time dispatch):**
      ///   - `sizeof(F) <= 48` — single slot, zero allocation
      ///   - `sizeof(F) > 48`  — single slot + heap-allocated callable
      ///
      /// **Pool exhaustion (controlled by `policy`):**
      ///   - `fail` — throw `pool_exhausted`
      ///   - `heap` — heap overflow up to limit, then throw
      ///   - `block` — pool → heap → park with timeout, then throw
      template <typename F>
      void post(F&& fn, post_overflow policy = post_overflow::block,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{1000});

      /// Non-throwing post.  Returns true if the callable was enqueued,
      /// false if capacity is exhausted.  Never blocks, never throws.
      /// Default `allow_heap` overflows to the heap (up to limit)
      /// before returning false.  `pool_only` skips heap allocation.
      template <typename F>
      bool try_post(F&& fn,
                    try_post_overflow overflow = try_post_overflow::allow_heap) noexcept;

      /// Post a task slot to this scheduler's intake queue.
      /// Thread-safe.  The slot must have been allocated from a SendQueue.
      void postTask(TaskSlotHeader* slot);

      // ── Accessors ───────────────────────────────────────────────────

      Engine&     io() { return _io; }
      Fiber*      currentFiber() { return _current; }
      uint32_t    index() const { return _index; }

      /// Current fiber name for debug output. Returns "?" if unnamed.
      const char* currentFiberName() const
      {
         return (_current && _current->name) ? _current->name : "?";
      }

      /// Set the maximum number of heap-overflow work items allowed.
      /// When the 256-slot pool is full, `heap` and `block` policies
      /// heap-allocate overflow items up to this limit.  Default: 256.
      void setWorkHeapLimit(uint32_t max_items) { _work_heap_overflow.set_max(max_items); }

      /// Current number of heap-allocated overflow work items.
      uint32_t workHeapCount() const { return _work_heap_overflow.count(); }

     private:
      explicit basic_scheduler(uint32_t index = 0);

      void notifyIfPolling();
      void drainWakeList();
      void drainTaskList();
      void drainWorkList();
      void pollAndUnblock(bool blocking);
      Fiber* popFromReadyQueues();
      void addToReadyQueue(Fiber* fiber);

      /// Route a fiber waking from Blocked/Sleeping into the ready queue,
      /// re-entering its home strand if it has one.  If the strand is free,
      /// the fiber becomes active and is placed in the local ready queue.
      /// If the strand is busy, the fiber is queued in the strand's wait
      /// list (state set to Parked) and will be promoted on release().
      void resumeBlockedFiber(Fiber* fiber);

      // ── LIFO slot (Tokio pattern) ───────────────────────────────────
      Fiber*   _lifo_slot        = nullptr;
      uint32_t _lifo_consecutive = 0;
      static constexpr uint32_t lifo_cap          = 3;
      static constexpr int      max_spin          = 4096;  // upper bound on spin iterations

      // ── Adaptive spin budget ───────────────────────────────────────
      // Grows on successful spin (work arrived during spin window),
      // shrinks on timeout (spin expired with no work).  Idle threads
      // converge to 0 (straight to kevent), busy threads stay at max_spin.
      int _spin_budget = 0;

      // (padding kept for cache-line alignment of wake_head below)

      // ── Cross-thread MPSC queues (cache-line padded) ────────────────
      detail::mpsc_stack<Fiber, &Fiber::next_wake>              _wake_list;
      detail::mpsc_stack<TaskSlotHeader, &TaskSlotHeader::next> _task_list;

      // ── Cross-thread callable work list (MPSC, inline storage) ──────
      //
      // WorkItems use type-erased inline storage (no std::function heap).
      // Zero heap allocation on the hot path.  Pool exhaustion falls back
      // to heap allocation (configurable).
      static constexpr size_t   work_payload_size = 48;
      static constexpr uint32_t work_pool_size    = 256;

      struct WorkItem
      {
         WorkItem*  next = nullptr;           // intrusive list pointer (work queue)
         void (*run)(void*)  = nullptr;       // type-erased invoke
         void (*dtor)(void*) = nullptr;       // type-erased destructor
         alignas(16) char payload[work_payload_size];
      };

      /// Return a work item to the pool (or free if heap-allocated).
      void return_work_slots(WorkItem* item);

      /// Place a type-erased callable into a WorkItem's payload.
      /// Inline if sizeof(Decay) <= 48, else heap-allocated with pointer indirection.
      template <typename Decay>
      void place_callable_in_item(WorkItem* item, Decay&& fn, bool pool_slot);

      detail::mpsc_stack<WorkItem, &WorkItem::next> _work_list;
      detail::bitset_pool<work_pool_size>           _work_slots;
      std::unique_ptr<WorkItem[]>                   _work_pool;
      detail::bounded_counter                       _work_heap_overflow;

      // ── Pool wait list (fibers waiting for a free slot) ──────────────
      // FIFO queue of fibers parked on post(block).  Scheduler-local —
      // only accessed from the scheduler thread.  Uses next_blocked for
      // linkage (a fiber can only be in one wait state at a time).
      Fiber*   _pool_wait_head = nullptr;
      Fiber*   _pool_wait_tail = nullptr;

      /// Wake one waiter (if any) after a pool slot is freed.
      /// Skips fibers whose timer already fired (state != Sleeping).
      void wakePoolWaiter();

      /// Remove a fiber from the pool wait list (O(n) scan).
      /// Called on resume from the block path — no-op if already removed.
      void removeFromPoolWait(Fiber* fiber);

      // ── Priority ready queues (3 levels, FIFO within each) ──────────
      std::deque<Fiber*> _ready_queues[3];
      uint64_t           _posted_counter = 0;

      uint32_t _index = 0;

      Engine                               _io;
      reactor*                             _reactor     = nullptr;
      Fiber*                               _current     = nullptr;
      bool                                 _interrupted = false;
      std::function<bool()>                _shutdownCheck;
      std::vector<std::unique_ptr<Fiber>>  _fibers;
      std::vector<Fiber*>                  _free_fibers;  // pooled recyclable fibers
      uint32_t                             _next_id = 0;

      /// Default native stack size for fibers (64 KB).
      /// WASM handlers should compute their required stack from the
      /// module's call graph and pass it to spawnFiber explicitly.
      /// Internal coordination fibers (I/O, timers) rarely need
      /// more than a few KB.
      static constexpr std::size_t default_fiber_stack_size = 64 * 1024;

      /// Thrown inside a fiber when the scheduler is shutting down.
      struct shutdown_exception {};

      /// Post-creation bookkeeping (non-template, lives in .cpp).
      void registerFiber(std::unique_ptr<Fiber> fiber);
   };

   /// Default scheduler type — uses the platform I/O engine.
   using Scheduler = basic_scheduler<>;

   // Re-export detail types used in the public API
   using detail::Fiber;
   using detail::FiberState;
   using detail::TaskSlotHeader;

   // ── spawnFiber template implementation ────────────────────────────────

   template <typename Engine>
   template <typename F>
   void basic_scheduler<Engine>::spawnFiber(F&& entry, const char* name, std::size_t stack_size)
   {
      if (stack_size == 0)
         stack_size = default_fiber_stack_size;

      // Check freelist — reuse a pooled fiber whose stack is large enough
      for (auto it = _free_fibers.begin(); it != _free_fibers.end(); ++it)
      {
         if ((*it)->native_stack_size >= stack_size)
         {
            Fiber* fp = *it;
            _free_fibers.erase(it);

            fp->setEntry(std::forward<F>(entry));
            fp->name        = name;
            fp->state       = FiberState::Ready;
            fp->priority    = 1;
            fp->posted_num  = _posted_counter++;
            fp->home_sched  = this;
            fp->home_strand = nullptr;  // clear stale strand affinity
            addToReadyQueue(fp);
            return;
         }
      }

      // Cold path: allocate new fiber + native stack
      auto   fiber = std::make_unique<Fiber>();
      Fiber* fp    = fiber.get();
      fp->id               = _next_id++;
      fp->name             = name;
      fp->home_sched       = this;
      fp->native_stack_size = stack_size;
      fp->setEntry(std::forward<F>(entry));

      fp->cont = ctx::callcc(
         std::allocator_arg,
         mmap_stack(stack_size),
         [this, fp](ctx::continuation&& sched) mutable
         {
            fp->sched_cont = &sched;
            sched = sched.resume();  // initial yield back to spawnFiber

            // Entry loop: run callable, recycle, wait for next entry
            while (true)
            {
               try
               {
                  fp->runEntry();
               }
               catch (const shutdown_exception&)
               {
               }
               catch (const boost::context::detail::forced_unwind&)
               {
                  throw;  // must propagate for proper stack cleanup
               }
               catch (...)
               {
               }

               // Yield as Recyclable — scheduler will move us to freelist
               fp->state = FiberState::Recyclable;
               sched = sched.resume();

               // Exit cleanly if the scheduler has marked us Done.
               // This avoids relying on forced_unwind during ~Fiber,
               // which is safe with fixedsize_stack but adds overhead.
               if (fp->state == FiberState::Done)
                  return std::move(sched);

               // Woken with a new entry — loop back
            }

            return std::move(sched);  // unreachable
         });

      registerFiber(std::move(fiber));
   }

   // ── return_work_slots ──────────────────────────────────────────────

   template <typename Engine>
   void basic_scheduler<Engine>::return_work_slots(WorkItem* item)
   {
      // Range check: pool item or heap overflow?
      if (item < &_work_pool[0] || item >= &_work_pool[work_pool_size])
      {
         delete[] reinterpret_cast<char*>(item);
         _work_heap_overflow.decrement();
         return;
      }

      uint32_t idx = static_cast<uint32_t>(item - &_work_pool[0]);
      _work_slots.push(idx);
   }

   // ── Callable placement helper (shared by post / try_post) ──────────

   template <typename Engine>
   template <typename Decay>
   void basic_scheduler<Engine>::place_callable_in_item(
      WorkItem* item, Decay&& fn, bool pool_slot)
   {
      if constexpr (sizeof(Decay) <= work_payload_size)
      {
         // Inline — callable fits in the 48-byte payload
         new (item->payload) Decay(std::forward<Decay>(fn));
         item->run  = [](void* p) { (*static_cast<Decay*>(p))(); };
         item->dtor = [](void* p) { static_cast<Decay*>(p)->~Decay(); };
      }
      else
      {
         // Heap — callable too large for inline storage.
         // Allocate on the heap, store a pointer in the payload.
         auto* heap = static_cast<Decay*>(::operator new(sizeof(Decay)));
         new (heap) Decay(std::forward<Decay>(fn));
         *reinterpret_cast<Decay**>(item->payload) = heap;
         item->run  = [](void* p) { (**static_cast<Decay**>(p))(); };
         item->dtor = [](void* p) {
            auto* obj = *static_cast<Decay**>(p);
            obj->~Decay();
            ::operator delete(obj);
         };
      }
   }

   // ── post() template implementation ─────────────────────────────────

   template <typename Engine>
   template <typename F>
   void basic_scheduler<Engine>::post(F&& fn, post_overflow policy,
                                      std::chrono::milliseconds timeout)
   {
      using Decay = std::decay_t<F>;
      static_assert(alignof(Decay) <= 16,
                    "Callable alignment too large for WorkItem");

      WorkItem* item     = nullptr;
      bool      pool_slot = false;

      // Try to claim a pool slot
      auto try_pool = [&]() -> bool {
         auto idx = _work_slots.try_pop();
         if (idx)
         {
            item      = &_work_pool[*idx];
            pool_slot = true;
            return true;
         }
         return false;
      };

      if (!try_pool())
      {
         // Pool exhausted — apply caller's overflow policy
         switch (policy)
         {
            case post_overflow::fail:
               throw pool_exhausted{};

            case post_overflow::heap:
            {
               if (!_work_heap_overflow.try_increment())
                  throw pool_exhausted{};
               auto* raw = new char[sizeof(WorkItem)];
               item = new (raw) WorkItem{};
               break;
            }

            case post_overflow::block:
            {
               // Try heap overflow before parking
               if (_work_heap_overflow.try_increment())
               {
                  auto* raw = new char[sizeof(WorkItem)];
                  item = new (raw) WorkItem{};
                  break;
               }

               // Both pool and heap exhausted — park with timeout.
               // Fiber joins the pool wait list and sets a wake_time.
               // Either a slot frees (wakePoolWaiter sets state to
               // Ready, "cancelling" the timer) or the timer fires
               // (pollAndUnblock sets state to Ready).
               auto& sched = basic_scheduler::current();
               if (&sched == this && sched.currentFiber())
               {
                  Fiber* fiber = sched.currentFiber();

                  // Add to pool wait list (FIFO)
                  fiber->next_blocked = nullptr;
                  if (_pool_wait_tail)
                     _pool_wait_tail->next_blocked = fiber;
                  else
                     _pool_wait_head = fiber;
                  _pool_wait_tail = fiber;

                  // Park with timeout — pollAndUnblock will wake us
                  // if the timer expires before a slot frees
                  fiber->state     = FiberState::Sleeping;
                  fiber->wake_time = std::chrono::steady_clock::now() + timeout;

                  auto& sc = *fiber->sched_cont;
                  sc = sc.resume();

                  if (_interrupted)
                     throw shutdown_exception{};

                  // Remove from wait list if still there (timer path)
                  removeFromPoolWait(fiber);

                  // Slot-free wake: try_pool succeeds.
                  // Timer wake: try_pool fails → throw.
                  if (!try_pool())
                     throw pool_exhausted{};
               }
               else
               {
                  // Cross-scheduler or non-fiber: spin with timeout
                  auto deadline = std::chrono::steady_clock::now() + timeout;
                  while (!try_pool())
                  {
                     if (std::chrono::steady_clock::now() >= deadline)
                        throw pool_exhausted{};
                     std::this_thread::yield();
                  }
               }
               break;
            }
         }
      }

      place_callable_in_item<Decay>(item, std::forward<F>(fn), pool_slot);

      _work_list.push(item);
      notifyIfPolling();
   }

   // ── try_post() template implementation ────────────────────────────────

   template <typename Engine>
   template <typename F>
   bool basic_scheduler<Engine>::try_post(F&& fn, try_post_overflow overflow) noexcept
   {
      using Decay = std::decay_t<F>;
      static_assert(alignof(Decay) <= 16,
                    "Callable alignment too large for WorkItem");

      WorkItem* item     = nullptr;
      bool      pool_slot = false;

      // Try pool first
      auto idx = _work_slots.try_pop();
      if (idx)
      {
         item      = &_work_pool[*idx];
         pool_slot = true;
      }
      else
      {
         if (overflow == try_post_overflow::pool_only)
            return false;

         if (!_work_heap_overflow.try_increment())
            return false;

         auto* raw = new (std::nothrow) char[sizeof(WorkItem)];
         if (!raw)
         {
            _work_heap_overflow.decrement();
            return false;
         }
         item = new (raw) WorkItem{};
      }

      place_callable_in_item<Decay>(item, std::forward<F>(fn), pool_slot);

      _work_list.push(item);
      notifyIfPolling();
      return true;
   }

}  // namespace psiber
