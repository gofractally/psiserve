#pragma once

#include <psiber/types.hpp>

#include <boost/context/continuation.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>

namespace psiber
{
   namespace ctx = boost::context;

   class Scheduler;

   /// Lifecycle state of a fiber.
   enum class FiberState
   {
      Ready,
      Running,
      Blocked,   // waiting on I/O (fd readiness)
      Sleeping,  // waiting on a timer (wake_time)
      Parked,    // waiting on a mutex, promise, or explicit park
      Done
   };

   /// A cooperative execution context.
   ///
   /// Each fiber has its own native stack (managed by Boost.Context).
   /// All fiber metadata lives on the host side.
   ///
   /// The Fiber struct doubles as an intrusive MPSC linked list node
   /// for zero-allocation cross-thread wake notifications.
   struct Fiber
   {
      uint32_t   id    = 0;
      FiberState state = FiberState::Ready;

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

      /// Monotonically increasing counter for FIFO within priority.
      uint64_t posted_num = 0;

      // ── Owning scheduler ──────────────────────────────────────────

      /// The scheduler this fiber belongs to, for cross-thread wake routing.
      Scheduler* home_sched = nullptr;

      // ── Transaction state (wound-wait) ─────────────────────────────

      /// Timestamp for wound-wait deadlock prevention.
      uint64_t          tx_timestamp = 0;

      /// Set by an older transaction to wound (abort) this fiber's transaction.
      std::atomic<bool> wounded{false};
   };

}  // namespace psiber
