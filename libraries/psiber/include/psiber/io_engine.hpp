#pragma once

#include <psiber/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace psiber
{
   /// Abstract I/O readiness notification engine (kqueue, epoll, io_uring).
   ///
   /// Concrete implementations translate the batch-oriented `updateFds`/`poll`
   /// interface into platform-specific syscalls.  The batch API is the
   /// primitive -- single-fd convenience wrappers (`addFd`, `removeFd`) are
   /// provided as inline non-virtual methods that delegate to `updateFds`.
   class IoEngine
   {
     public:
      virtual ~IoEngine() = default;

      /// Apply a batch of fd changes in a single syscall where possible.
      virtual void updateFds(std::span<const FdChange> changes) = 0;

      /// Poll for ready events into caller-provided buffer.
      /// nullopt means block indefinitely.
      /// 0ms means non-blocking poll.
      /// Returns the number of events written to `out`.
      virtual int poll(std::span<IoEvent>                        out,
                       std::optional<std::chrono::milliseconds> timeout) = 0;

      /// Remove specific event filters for a single fd.
      virtual void removeFdEvents(RealFd real_fd, EventKind events) = 0;

      /// Register a user-defined event for cross-thread waking.
      /// On kqueue: EVFILT_USER with EV_ADD | EV_CLEAR.
      /// On epoll: eventfd.
      virtual void registerUserEvent(uintptr_t ident) = 0;

      /// Trigger a user-defined event (thread-safe, idempotent).
      /// Multiple triggers between polls collapse into one.
      virtual void triggerUserEvent(uintptr_t ident) = 0;

      /// Register a POSIX signal for event-driven delivery.
      /// On kqueue: EVFILT_SIGNAL.  On Linux: signalfd.
      /// The signal is blocked from default handling (SIG_IGN).
      /// Poll returns it as IoEvent{RealFd{signo}, Signal}.
      virtual void registerSignal(int signo) = 0;

      /// Convenience: add or replace interest for a single fd.
      void addFd(RealFd real_fd, EventKind events)
      {
         FdChange c{real_fd, events};
         updateFds({&c, 1});
      }

      /// Convenience: remove a single fd from the interest set (all filters).
      void removeFd(RealFd real_fd)
      {
         FdChange c{real_fd, {}};
         updateFds({&c, 1});
      }
   };

}  // namespace psiber
