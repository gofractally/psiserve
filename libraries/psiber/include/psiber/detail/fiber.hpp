#pragma once

#include <psiber/detail/platform_engine.hpp>
#include <psiber/types.hpp>

#include <boost/context/continuation.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace psiber
{
   namespace ctx = boost::context;

   template <typename Engine = detail::PlatformEngine>
   class basic_scheduler;

   class strand;

   namespace detail
   {
      /// Lifecycle state of a fiber.
      enum class FiberState
      {
         Ready,
         Running,
         Blocked,     // waiting on I/O (fd readiness)
         Sleeping,    // waiting on a timer (wake_time)
         Parked,      // waiting on a mutex, promise, or explicit park
         Recyclable,  // entry finished, fiber available for reuse
         Done
      };

      /// A cooperative execution context.
      ///
      /// Each fiber has its own native stack (managed by Boost.Context).
      /// All fiber metadata lives on the host side.
      ///
      /// The Fiber struct doubles as an intrusive MPSC linked list node
      /// for zero-allocation cross-thread wake notifications.
      ///
      /// Fibers are pooled: after their entry callable completes, they
      /// yield as Recyclable.  The scheduler reuses them for the next
      /// spawnFiber, avoiding mmap/munmap of native stacks.
      struct Fiber
      {
         uint32_t    id    = 0;
         FiberState  state = FiberState::Ready;
         const char* name  = nullptr;  // debug name (non-owning, must outlive fiber)

         // ── Type-erased entry callable (inline storage) ────────────────
         //
         // The callable is placement-new'd here by spawnFiber (via setEntry).
         // The fiber's callcc loop invokes it, destroys it, then yields.
         // 80 bytes handles shared_ptr + 4 references + 2 ints with room.
         static constexpr std::size_t entry_buf_size = 80;
         alignas(16) char entry_buf[entry_buf_size];
         void (*entry_run)(void*)  = nullptr;  // type-erased operator()
         void (*entry_dtor)(void*) = nullptr;  // type-erased destructor

         template <typename F>
         void setEntry(F&& f)
         {
            using Decay = std::decay_t<F>;
            static_assert(sizeof(Decay) <= entry_buf_size,
                          "Fiber entry callable too large for inline storage");
            static_assert(alignof(Decay) <= 16,
                          "Fiber entry callable alignment too large");
            entry_run = [](void* p) { (*static_cast<Decay*>(p))(); };
            entry_dtor = [](void* p) { static_cast<Decay*>(p)->~Decay(); };
            new (entry_buf) Decay(std::forward<F>(f));
         }

         void runEntry()
         {
            if (!entry_run)
               return;  // no entry (already cleaned up after exception)

            // Clear pointers before invoking — prevents double-execution
            // if the fiber is erroneously re-activated after an exception.
            auto run  = entry_run;
            auto dtor = entry_dtor;
            entry_run  = nullptr;
            entry_dtor = nullptr;

            try        { run(entry_buf); }
            catch (...) { dtor(entry_buf); throw; }
            dtor(entry_buf);
         }

         // ── Native stack ────────────────────────────────────────────────

         /// Size of this fiber's native stack (bytes).
         /// Stored for pool reuse — a pooled fiber can only be reused
         /// for work that fits within its allocated stack.
         std::size_t native_stack_size = 0;

         // ── Boost.Context ──────────────────────────────────────────────

         /// The fiber's suspended continuation (held by the scheduler).
         ctx::continuation cont;

         /// Pointer to the scheduler's continuation, which lives as a local
         /// variable on this fiber's native stack (inside the callcc lambda).
         ctx::continuation* sched_cont = nullptr;

         // ── I/O blocking ───────────────────────────────────────────────

         /// What this fiber is blocked on (valid when state == Blocked).
         RealFd    blocked_fd     = invalid_real_fd;
         EventKind blocked_events = {};

         // ── Timer ──────────────────────────────────────────────────────

         /// When this fiber should wake up (valid when state == Sleeping).
         std::chrono::steady_clock::time_point wake_time;

         // ── Cross-thread wake (intrusive MPSC node) ────────────────────

         /// Next pointer for the atomic wake list. The fiber IS the node --
         /// zero allocation, naturally bounded by total fiber count.
         Fiber* next_wake = nullptr;

         /// Next pointer for mutex wait queues (FIFO linked list).
         Fiber* next_blocked = nullptr;

         // ── Priority ───────────────────────────────────────────────────

         /// Priority level: 0 = high, 1 = normal (default), 2 = low.
         /// FIFO ordering within the same priority level.
         uint8_t  priority   = 1;

         /// Daemon fibers don't prevent scheduler shutdown.
         /// When all non-daemon fibers finish, run() exits.
         bool     daemon     = false;

         /// Monotonically increasing counter for FIFO within priority.
         uint64_t posted_num = 0;

         // ── Owning scheduler ──────────────────────────────────────────

         /// The scheduler this fiber belongs to, for cross-thread wake routing.
         /// Updated when the fiber migrates to a new scheduler (reactor pool).
         basic_scheduler<PlatformEngine>* home_sched = nullptr;

         // ── Strand affinity ─────────────────────────────────────────────

         /// The strand this fiber belongs to (nullptr if standalone).
         /// Strand fibers are serialized: at most one active per strand.
         psiber::strand* home_strand = nullptr;

         // ── Transaction state (wound-wait) ─────────────────────────────

         /// Timestamp for wound-wait deadlock prevention.
         uint64_t          tx_timestamp = 0;

         /// Set by an older transaction to wound (abort) this fiber's transaction.
         std::atomic<bool> wounded{false};
      };

   }  // namespace detail
}  // namespace psiber
