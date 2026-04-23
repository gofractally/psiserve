#pragma once

#include <psiber/detail/fiber.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/spin_lock.hpp>
#include <psiber/types.hpp>

#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <type_traits>
#include <variant>

namespace psiber
{
   class reactor;

   /// Serialization barrier with per-strand arena allocator.
   ///
   /// A strand guarantees at most one fiber in Ready/Running state at a
   /// time across all threads in a reactor pool.  Fibers are FIFO within
   /// a strand.  When the active fiber blocks or finishes, the next
   /// waiting fiber becomes active and the strand re-posts to the
   /// reactor's ready queue.
   ///
   /// Each strand owns a private arena for zero-allocation task storage.
   /// Free is lock-free (CAS push onto return stack).  Alloc is
   /// single-caller (strand serialization guarantees at most one fiber
   /// runs at a time), so it needs no lock — it drains the atomic
   /// return stack, coalesces, and bumps without contention.
   class strand
   {
      friend class reactor;

      using Fiber = detail::Fiber;

     public:
      explicit strand(uint32_t arena_bytes = 4096);
      strand(reactor& pool, uint32_t arena_bytes = 4096);
      ~strand();

      strand(const strand&)            = delete;
      strand& operator=(const strand&) = delete;
      strand(strand&&)                 = delete;
      strand& operator=(strand&&)      = delete;

      // ── Arena allocator ─────────────────────────────────────────

      /// Allocate `bytes` from the per-strand arena.
      /// Returns nullptr if the arena is exhausted.
      /// Single-caller only (strand serialization guarantees this).
      void* alloc(uint32_t bytes);

      /// Free a pointer previously returned by alloc().
      /// Lock-free — any thread can call this (CAS push).
      void free(void* ptr);

      // ── Posting work ─────────────────────────────────────────

      /// Post a callable to run as a fiber on this strand.
      /// The callable is allocated from the strand's arena (zero heap
      /// allocation) and wrapped in a Fiber that enters the strand's
      /// scheduling queue.  Requires a Scheduler context to spawn the fiber.
      ///
      /// Returns true on success, false if the arena is exhausted.
      template <typename F>
      bool post(F&& fn);

      /// Synchronously run `fn` on this strand, temporarily migrating the
      /// calling fiber from its current strand (if any).  The fiber releases
      /// its home strand, enters this strand, runs `fn`, then releases this
      /// strand and re-enters its original home.  Returns fn's return value
      /// (or void).  Exceptions from fn propagate after cleanup.
      ///
      /// Must be called from inside a fiber, and `this` must differ from
      /// the caller's current home_strand.  Callers should not migrate
      /// recursively (A→B→A on the same fiber) — the order of re-entry
      /// is not enforced against deadlock.
      template <typename F>
      auto sync(F&& fn) -> std::invoke_result_t<F>;

      // ── Strand scheduling ──────────────────────────────────────

      /// Enqueue a fiber into this strand.
      /// If no fiber is currently active, the fiber becomes active.
      /// - If bound to a reactor: posts to the reactor's ready queue,
      ///   returns nullptr (reactor will schedule it).
      /// - If standalone: returns the fiber for the caller to enqueue
      ///   directly into its local ready queue.
      /// If another fiber is active, the fiber waits in the strand's
      /// queue and nullptr is returned.
      Fiber* enqueue(Fiber* fiber);

      /// Release the current active fiber (it blocked or finished).
      /// Returns the next waiting fiber, or nullptr if the strand is empty.
      /// If a fiber is returned, the strand re-posts to the ready queue.
      Fiber* release();

      /// The currently active fiber (or nullptr).
      Fiber* active() const { return _active.load(std::memory_order_acquire); }

      /// Enter the strand from a running fiber (used by strand::post).
      /// Returns true if this fiber became active (keep running).
      /// Returns false if queued behind another fiber (should park).
      /// Unlike enqueue(), does NOT call maybe_post() — the fiber is
      /// already running on a scheduler.
      bool enter(Fiber* fiber);

     private:
      void maybe_post();

      // ── Arena internals ─────────────────────────────────────────

      struct Block
      {
         uint32_t size;  // total size: sizeof(Block) + payload, 16-aligned
         Block*   next;  // free-list linkage
      };

      char*    _region;
      uint32_t _capacity;
      uint32_t _bump = 0;

      // Lock-free return stack: any thread can free() via CAS push
      alignas(cache_line_size) std::atomic<Block*> _returned{nullptr};

      // Alloc-side state (single-caller under strand serialization)
      Block* _free_list = nullptr;

      // ── Scheduling internals ────────────────────────────────────

      // Currently active fiber (at most one across all pool threads)
      std::atomic<Fiber*> _active{nullptr};

      // Wait queue: fibers ready to run but blocked on _active
      Fiber*    _wait_head = nullptr;
      Fiber*    _wait_tail = nullptr;
      spin_lock _queue_lock;

      // Intrusive node for reactor's ready-strand queue
      strand* next_ready = nullptr;

      // Prevents double-posting to the ready queue
      std::atomic<bool> _queued{false};

      // Owning reactor (nullptr until bound)
      reactor* _pool = nullptr;
   };

   // ── strand::post<F>() template implementation ────────────────────────
   //
   // The fiber enters the strand at the start of its execution, not at
   // spawn time.  This avoids accessing scheduler internals from strand.
   //
   // Flow:
   // 1. spawnFiber creates the fiber (no home_strand yet)
   // 2. Fiber runs: calls strand::enqueue(self)
   //    - If it becomes active: set home_strand, run callable
   //    - If queued behind another: park (home_strand unset, so
   //      the run loop won't call strand::release on park)
   // 3. When the preceding fiber finishes, strand::release() returns
   //    our fiber.  The run loop puts it back in the ready queue.
   //    We resume from park, set home_strand, run callable.
   // 4. When our callable finishes (Recyclable), the run loop calls
   //    strand::release() (home_strand is set), activating the next.

   template <typename F>
   bool strand::post(F&& fn)
   {
      using Decay = std::decay_t<F>;

      // Allocate the callable from the strand arena
      void* mem = alloc(sizeof(Decay));
      if (!mem)
         return false;

      auto* obj = new (mem) Decay(std::forward<F>(fn));

      auto& sched = Scheduler::current();

      sched.spawnFiber([this, obj]() {
         auto& s  = Scheduler::current();
         auto* me = s.currentFiber();

         // Enter strand serialization.  enter() sets us as active
         // or queues us behind the current active fiber.  Unlike
         // enqueue(), it does NOT call maybe_post() — we're already
         // running on a scheduler, not arriving via cross-thread wake.
         if (!enter(me))
         {
            // Queued behind another fiber — park until our turn.
            // home_strand is still nullptr so the run loop won't
            // call strand::release() when we park.
            s.parkCurrentFiber();
         }

         // Now we're the active fiber in this strand
         me->home_strand = this;

         try
         {
            (*obj)();
         }
         catch (...)
         {
            obj->~Decay();
            this->free(obj);
            throw;
         }
         obj->~Decay();
         this->free(obj);

         // Fiber finishes → Recyclable → run loop calls strand::release()
      });

      return true;
   }

   // ── strand::sync<F>() template implementation ───────────────────────
   //
   // Migration: the caller's fiber leaves its home strand, enters `this`
   // strand, runs fn(), then leaves this strand and re-enters home.
   //
   // State of home_strand during the transition:
   //   me->home_strand = src   initially
   //   me->home_strand = nullptr after releasing src (in transit)
   //   me->home_strand = this  while running fn()
   //   me->home_strand = nullptr after releasing this (in transit)
   //   me->home_strand = src   after re-entering src
   //
   // The run loop's release-on-park logic uses home_strand to decide
   // which strand to release; setting it to nullptr during transit
   // prevents misfires when we park waiting to enter.  Matches the
   // same discipline used by strand::post (strand.hpp line 180-183).

   template <typename F>
   auto strand::sync(F&& fn) -> std::invoke_result_t<F>
   {
      using R = std::invoke_result_t<F>;

      auto&  sched = Scheduler::current();
      Fiber* me    = sched.currentFiber();
      assert(me && "strand::sync must be called from within a fiber");
      assert(_active.load(std::memory_order_relaxed) != me &&
             "strand::sync called on the caller's own strand");

      strand* src = me->home_strand;

      // 1. Release source strand (hand off next waiter locally).
      me->home_strand = nullptr;
      if (src)
      {
         Fiber* next = src->release();
         if (next)
            sched.promoteStrandWaiter(next);
      }

      // 2. Enter target strand; park if queued behind another fiber.
      if (!this->enter(me))
         sched.parkCurrentFiber();
      me->home_strand = this;

      // 3. Run fn(), capturing its result or exception.
      std::exception_ptr err;
      std::optional<std::conditional_t<std::is_void_v<R>, std::monostate, R>> result;
      try
      {
         if constexpr (std::is_void_v<R>)
            fn();
         else
            result.emplace(fn());
      }
      catch (...)
      {
         err = std::current_exception();
      }

      // 4. Release target strand; hand off its next waiter.  The fiber
      //    may have moved schedulers during fn() (cross-thread wakes
      //    through home_strand re-entry).  Use current scheduler.
      auto&  sched2 = Scheduler::current();
      me->home_strand = nullptr;
      {
         Fiber* next = this->release();
         if (next)
            sched2.promoteStrandWaiter(next);
      }

      // 5. Re-enter source strand.  Park if someone else holds it.
      //    parkCurrentFiber may throw shutdown_exception — preserve
      //    any earlier err from fn() ahead of a late shutdown.
      if (src)
      {
         try
         {
            if (!src->enter(me))
               sched2.parkCurrentFiber();
            me->home_strand = src;
         }
         catch (...)
         {
            if (!err)
               err = std::current_exception();
         }
      }

      if (err)
         std::rethrow_exception(err);
      if constexpr (!std::is_void_v<R>)
         return std::move(*result);
   }

}  // namespace psiber
