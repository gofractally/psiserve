#pragma once

#include <psiber/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace psiber::detail
{
   /// macOS/BSD kqueue I/O engine.
   ///
   /// Concrete class — no virtual dispatch.  Satisfies the io_engine concept.
   /// Owned by value inside basic_scheduler<KqueueEngine>.
   class KqueueEngine
   {
     public:
      KqueueEngine();
      ~KqueueEngine();

      KqueueEngine(const KqueueEngine&)            = delete;
      KqueueEngine& operator=(const KqueueEngine&) = delete;

      void updateFds(std::span<const FdChange> changes);
      int  poll(std::span<IoEvent> out, std::optional<std::chrono::milliseconds> timeout);
      void removeFdEvents(RealFd real_fd, EventKind events);
      void registerUserEvent(uintptr_t ident);
      void triggerUserEvent(uintptr_t ident);
      void registerSignal(int signo);

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

     private:
      RealFd _kq = invalid_real_fd;
   };

}  // namespace psiber::detail
