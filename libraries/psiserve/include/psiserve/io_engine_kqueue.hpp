#pragma once

#include <psiserve/io_engine.hpp>

namespace psiserve
{
   class KqueueEngine : public IoEngine
   {
     public:
      KqueueEngine();
      ~KqueueEngine() override;

      KqueueEngine(const KqueueEngine&)            = delete;
      KqueueEngine& operator=(const KqueueEngine&) = delete;

      void updateFds(std::span<const FdChange> changes) override;
      int  poll(std::span<IoEvent> out, std::optional<std::chrono::milliseconds> timeout) override;
      void removeFdEvents(RealFd real_fd, EventKind events) override;

     private:
      RealFd _kq = invalid_real_fd;
   };

}  // namespace psiserve
