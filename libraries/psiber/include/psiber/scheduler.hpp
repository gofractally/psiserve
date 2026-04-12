#pragma once

#include <psiber/detail/fiber.hpp>
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
#include <boost/context/protected_fixedsize_stack.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace psiber
{
   /// Thrown when a post()/invoke() callable attempts to yield.
   /// The callable should use spawn()/async() instead.
   struct drain_yield_error : std::runtime_error
   {
      drain_yield_error()
         : std::runtime_error("post()/invoke() callable attempted to yield — "
                              "use spawn()/async() for work that may suspend") {}
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
   /// **Work item execution:** post() callables are executed by a
   /// dedicated daemon drain fiber, giving them full fiber context
   /// (can call post, sleep, yield, spawn).  The drain fiber runs at
   /// priority 0 (high) and is poked awake when items arrive.  A
   /// pre-allocated pool of 256 WorkItems (128-byte inline storage
   /// each) provides zero-allocation post() on the hot path.  Pool
   /// exhaustion triggers fiber-aware back-pressure (park + wake)
   /// rather than unbounded spin.
   // Forward declarations for friend access
   class thread;
   class reactor;
   struct scheduler_access;  // test/benchmark helper

   template <typename Engine>
   class basic_scheduler
   {
      using Fiber          = detail::Fiber;
      using FiberState     = detail::FiberState;
      using TaskSlotHeader = detail::TaskSlotHeader;

      friend class thread;
      friend class reactor;
      friend struct scheduler_access;

     public:
      ~basic_scheduler() = default;

      /// Thread-local access.  Returns the Scheduler running on this
      /// OS thread, or nullptr if none.
      static basic_scheduler* current();

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
      /// **Execution model:** Callables run **serially** on a single
      /// dedicated drain fiber (high priority, FIFO order).  The drain
      /// fiber has full fiber context — callables can call post(),
      /// sleep(), yield(), spawnFiber(), and use fiber-aware locks.
      /// However, each callable blocks the drain pipeline while it
      /// runs, so keep callables short.
      ///
      /// **post() vs spawnFiber():**
      ///   - `post()` — short coordination: counters, state updates,
      ///     spawning fibers, posting follow-ups.  Runs on the shared
      ///     drain fiber.  Blocks the drain while running.
      ///   - `spawnFiber()` — independent work that may block on I/O,
      ///     locks, or anything long-running.  Gets its own fiber and
      ///     stack, can suspend without affecting other work.
      ///
      /// **Requirements:**
      ///   - `sizeof(F) <= 128` — must fit in WorkItem inline storage
      ///   - `alignof(F) <= 16`
      ///   - `F` must be `noexcept`-invocable — fire-and-forget has no
      ///     error channel; mark your callable `noexcept` or ensure the
      ///     lambda's body is noexcept
      ///
      /// **Back-pressure (pool exhaustion):**
      ///   - Fiber caller: parks until the drain fiber returns items to
      ///     the freelist, then retries.  No spin, no busy-wait.
      ///   - Non-fiber caller (plain OS thread): spin-pauses until space
      ///     is available (no fiber to park).
      ///
      /// **Common patterns:**
      /// ```
      ///   // Fire-and-forget coordination
      ///   sched.post([&]() noexcept { counter++; });
      ///
      ///   // Chaining: post → work → post follow-up
      ///   sched.post([&]() noexcept {
      ///      do_setup();
      ///      sched.post([&]() noexcept { do_followup(); });
      ///   });
      ///
      ///   // Spawn a fiber for long-running work
      ///   sched.post([&]() noexcept {
      ///      sched.spawnFiber([&]() { handle_request(); });
      ///   });
      /// ```
      template <typename F>
      void post(F&& fn);

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

     private:
      explicit basic_scheduler(uint32_t index = 0);

      void notifyIfPolling();
      void drainWakeList();
      void drainTaskList();
      void drainWorkList();
      void pollAndUnblock(bool blocking);
      Fiber* popFromReadyQueues();
      void addToReadyQueue(Fiber* fiber);
      void checkNotDrainFiber() const;

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

      // ── Cross-thread wake list (MPSC, cache-line padded) ────────────
      alignas(cache_line_size) std::atomic<Fiber*> _wake_head{nullptr};

      // ── Cross-thread task intake (MPSC, cache-line padded) ──────────
      alignas(cache_line_size) std::atomic<TaskSlotHeader*> _task_head{nullptr};

      // ── Cross-thread callable work list (MPSC, inline storage) ──────
      //
      // Same CAS-push / exchange-drain topology as wake list.
      // WorkItems use type-erased inline storage (no std::function heap).
      // Items come from a pre-allocated pool with a lock-free freelist
      // (CAS-pop to acquire, CAS-push to release).  Zero heap allocation
      // after warmup.  Back-pressure: pool exhaustion → fiber-aware park
      // until drain returns items to freelist.
      //
      // Work items are executed by a dedicated daemon drain fiber, giving
      // posted callables full fiber context (can call post, sleep, yield).
      static constexpr size_t work_payload_size = 128;
      static constexpr uint32_t work_pool_size  = 256;

      struct WorkItem
      {
         WorkItem*  next = nullptr;           // intrusive list pointer
         void (*run)(void*)  = nullptr;       // type-erased invoke
         void (*dtor)(void*) = nullptr;       // type-erased destructor
         alignas(16) char payload[work_payload_size];
      };

      alignas(cache_line_size) std::atomic<WorkItem*> _work_head{nullptr};
      alignas(cache_line_size) std::atomic<WorkItem*> _work_free{nullptr};

      // Pool storage (constructed in constructor)
      std::unique_ptr<WorkItem[]> _work_pool;

      // ── Drain fiber ────────────────────────────────────────────────
      // Dedicated daemon fiber that executes work items in fiber context.
      // drainWorkList() pokes it when items arrive on _work_head.
      Fiber* _drain_fiber      = nullptr;
      bool   _drain_executing  = false;  // true while a work item callable is running

      // ── Pool-exhaustion wait list ──────────────────────────────────
      // Fibers waiting for WorkItem pool space.  Protected by _work_space_lock.
      // Drain fiber wakes waiters after returning items to freelist.
      spin_lock _work_space_lock;
      Fiber*    _work_space_wait_head = nullptr;

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

   /// Test/benchmark helper — grants access to the private constructor.
   /// Not part of the public API.  Library consumers should use
   /// psiber::thread, which owns a Scheduler internally.
   struct scheduler_access
   {
      static Scheduler make(uint32_t index = 0) { return Scheduler(index); }
   };

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
            fp->name       = name;
            fp->state      = FiberState::Ready;
            fp->priority   = 1;
            fp->posted_num = _posted_counter++;
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
         ctx::protected_fixedsize_stack(stack_size),
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

               // Woken with a new entry — loop back
            }

            return std::move(sched);  // unreachable
         });

      registerFiber(std::move(fiber));
   }

   // ── post() template implementation ─────────────────────────────────

   template <typename Engine>
   template <typename F>
   void basic_scheduler<Engine>::post(F&& fn)
   {
      using Decay = std::decay_t<F>;
      static_assert(sizeof(Decay) <= work_payload_size,
                    "Callable too large for WorkItem inline storage");
      static_assert(alignof(Decay) <= 16,
                    "Callable alignment too large for WorkItem");
      static_assert(std::is_nothrow_invocable_v<Decay>,
                    "post() callables must be noexcept — fire-and-forget has no error channel");

      // CAS-pop a WorkItem from the freelist (lock-free, zero alloc)
      WorkItem* item;
      while (true)
      {
         item = _work_free.load(std::memory_order_acquire);
         if (item)
         {
            if (_work_free.compare_exchange_weak(
                   item, item->next, std::memory_order_acquire, std::memory_order_relaxed))
               break;
            continue;
         }

         // Pool exhausted — fiber-aware wait if possible
         basic_scheduler* caller = basic_scheduler::current();
         if (caller && caller->currentFiber())
         {
            Fiber* me = caller->currentFiber();
            _work_space_lock.lock();
            // Double-check under lock (items may have been returned)
            item = _work_free.load(std::memory_order_acquire);
            if (item && _work_free.compare_exchange_strong(
                   item, item->next, std::memory_order_acquire, std::memory_order_relaxed))
            {
               _work_space_lock.unlock();
               break;
            }
            // Enqueue and park
            me->next_wake = _work_space_wait_head;
            _work_space_wait_head = me;
            _work_space_lock.unlock();
            caller->parkCurrentFiber();
            continue;  // retry after wake
         }

         // Non-fiber caller — spin-pause
#if defined(__x86_64__)
         __builtin_ia32_pause();
#elif defined(__aarch64__)
         asm volatile("yield" ::: "memory");
#endif
      }

      // Placement-new callable into inline storage
      new (item->payload) Decay(std::forward<F>(fn));
      item->run  = [](void* p) noexcept { (*static_cast<Decay*>(p))(); };
      item->dtor = [](void* p) noexcept { static_cast<Decay*>(p)->~Decay(); };

      // CAS-push onto the work list
      WorkItem* old_head = _work_head.load(std::memory_order_relaxed);
      do
      {
         item->next = old_head;
      } while (!_work_head.compare_exchange_weak(
         old_head, item, std::memory_order_release, std::memory_order_relaxed));

      notifyIfPolling();
   }

}  // namespace psiber
