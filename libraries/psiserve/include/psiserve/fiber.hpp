#pragma once

#include <psiserve/io_engine.hpp>

#include <boost/context/continuation.hpp>

#include <chrono>
#include <cstdint>

namespace psiserve
{
   namespace ctx = boost::context;

   /// Lifecycle state of a fiber within a process.
   enum class FiberState
   {
      Ready,
      Running,
      Blocked,   // waiting on I/O (fd readiness)
      Sleeping,  // waiting on a timer (wake_time)
      Done
   };

   /// A cooperative execution context within a WASM process.
   ///
   /// Each fiber has its own native stack (managed by Boost.Context) and
   /// its own psizam execution_context (operand stack, call stack, globals).
   /// All fibers in a process share the same WASM linear memory and fd table.
   ///
   /// All fiber metadata lives on the host side — nothing in WASM linear memory.
   struct Fiber
   {
      uint32_t          id    = 0;
      FiberState        state = FiberState::Ready;

      /// The fiber's suspended continuation (held by the scheduler).
      /// When the scheduler calls cont.resume(), the fiber resumes.
      ctx::continuation cont;

      /// Pointer to the scheduler's continuation, which lives as a local
      /// variable on this fiber's native stack (inside the callcc lambda).
      /// Used by yield() to switch back to the scheduler.
      ctx::continuation* sched_cont = nullptr;

      /// What this fiber is blocked on (valid when state == Blocked).
      RealFd    blocked_fd     = invalid_real_fd;
      EventKind blocked_events = {};

      /// When this fiber should wake up (valid when state == Sleeping).
      std::chrono::steady_clock::time_point wake_time;
   };

}  // namespace psiserve
