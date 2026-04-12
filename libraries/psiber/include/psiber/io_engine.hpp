#pragma once

#include <psiber/types.hpp>

#include <chrono>
#include <concepts>
#include <cstdint>
#include <optional>
#include <span>

namespace psiber
{
   /// Concept for an I/O readiness notification engine (kqueue, epoll, io_uring).
   ///
   /// Concrete engines translate the batch-oriented updateFds/poll interface
   /// into platform-specific syscalls.  The Scheduler owns the engine by value
   /// and calls its methods directly — no virtual dispatch.
   template <typename E>
   concept io_engine = requires(E                                          e,
                                std::span<const FdChange>                  changes,
                                std::span<IoEvent>                         out,
                                std::optional<std::chrono::milliseconds> timeout,
                                RealFd                                     fd,
                                EventKind                                  events,
                                uintptr_t                                  ident,
                                int                                        signo)
   {
      /// Apply a batch of fd changes in a single syscall where possible.
      e.updateFds(changes);

      /// Poll for ready events into caller-provided buffer.
      /// nullopt means block indefinitely.  0ms means non-blocking.
      /// Returns the number of events written to `out`.
      { e.poll(out, timeout) } -> std::same_as<int>;

      /// Remove specific event filters for a single fd.
      e.removeFdEvents(fd, events);

      /// Register a user-defined event for cross-thread waking.
      e.registerUserEvent(ident);

      /// Trigger a user-defined event (thread-safe, idempotent).
      e.triggerUserEvent(ident);

      /// Register a POSIX signal for event-driven delivery.
      e.registerSignal(signo);
   };

   /// Convenience: add or replace interest for a single fd.
   template <io_engine Engine>
   inline void addFd(Engine& e, RealFd fd, EventKind events)
   {
      FdChange c{fd, events};
      e.updateFds({&c, 1});
   }

   /// Convenience: remove a single fd from the interest set (all filters).
   template <io_engine Engine>
   inline void removeFd(Engine& e, RealFd fd)
   {
      FdChange c{fd, {}};
      e.updateFds({&c, 1});
   }

}  // namespace psiber
