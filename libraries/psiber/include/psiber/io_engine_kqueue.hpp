#pragma once

#include <psiber/io_engine.hpp>

namespace psiber
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
      void registerUserEvent(uintptr_t ident) override;
      void triggerUserEvent(uintptr_t ident) override;
      void registerSignal(int signo) override;

     private:
      RealFd _kq = invalid_real_fd;
   };

}  // namespace psiber
