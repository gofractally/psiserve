#include <psiber/detail/io_engine_epoll.hpp>

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <signal.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace psiber::detail
{
   EpollEngine::EpollEngine()
   {
      int fd = ::epoll_create1(EPOLL_CLOEXEC);
      if (fd < 0)
         throw std::runtime_error("epoll_create1() failed");
      _epfd = RealFd{fd};
   }

   EpollEngine::~EpollEngine()
   {
      if (_signalfd != invalid_real_fd)
         ::close(*_signalfd);
      if (_eventfd != invalid_real_fd)
         ::close(*_eventfd);
      if (_epfd != invalid_real_fd)
         ::close(*_epfd);
   }

   void EpollEngine::updateFds(std::span<const FdChange> changes)
   {
      for (auto& c : changes)
      {
         if (c.events == EventKind{})
         {
            // Remove — best-effort (fd may have been closed)
            ::epoll_ctl(*_epfd, EPOLL_CTL_DEL, *c.real_fd, nullptr);
         }
         else
         {
            struct epoll_event ev{};
            ev.data.fd = *c.real_fd;
            if (c.events & Readable)
               ev.events |= EPOLLIN;
            if (c.events & Writable)
               ev.events |= EPOLLOUT;

            if (::epoll_ctl(*_epfd, EPOLL_CTL_MOD, *c.real_fd, &ev) < 0)
            {
               if (errno == ENOENT)
               {
                  if (::epoll_ctl(*_epfd, EPOLL_CTL_ADD, *c.real_fd, &ev) < 0)
                     throw std::runtime_error("epoll_ctl ADD failed");
               }
               else
               {
                  throw std::runtime_error("epoll_ctl MOD failed");
               }
            }
         }
      }
   }

   int EpollEngine::poll(std::span<IoEvent>                        out,
                         std::optional<std::chrono::milliseconds> timeout)
   {
      constexpr int max_events = 64;
      int           capacity = static_cast<int>(std::min<std::size_t>(out.size(), max_events));
      struct epoll_event events[max_events];

      int timeout_ms = -1;  // block indefinitely
      if (timeout)
         timeout_ms = static_cast<int>(timeout->count());

      int n = ::epoll_wait(*_epfd, events, capacity, timeout_ms);
      if (n < 0)
      {
         if (errno == EINTR)
            return 0;
         throw std::runtime_error("epoll_wait failed");
      }

      for (int i = 0; i < n; ++i)
      {
         EventKind ev{};
         int fd = events[i].data.fd;

         if (_eventfd != invalid_real_fd && fd == *_eventfd)
         {
            // Drain the eventfd counter
            uint64_t val;
            ::read(*_eventfd, &val, sizeof(val));
            ev = ev | UserWake;
            out[i] = {_eventfd, ev};
            continue;
         }

         if (_signalfd != invalid_real_fd && fd == *_signalfd)
         {
            // Read the signalfd_siginfo to get the signal number
            struct signalfd_siginfo ssi;
            ::read(*_signalfd, &ssi, sizeof(ssi));
            ev = ev | Signal;
            out[i] = {RealFd{static_cast<int>(ssi.ssi_signo)}, ev};
            continue;
         }

         if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR))
            ev = ev | Readable;
         if (events[i].events & EPOLLOUT)
            ev = ev | Writable;
         out[i] = {RealFd{fd}, ev};
      }

      return n;
   }

   void EpollEngine::removeFdEvents(RealFd real_fd, EventKind events)
   {
      // epoll doesn't support removing individual event filters —
      // we must DEL and re-ADD with the remaining interest.
      // For simplicity (matching kqueue best-effort semantics),
      // just remove the fd entirely.  The caller (yield) re-adds
      // on the next wait.
      ::epoll_ctl(*_epfd, EPOLL_CTL_DEL, *real_fd, nullptr);
   }

   void EpollEngine::registerUserEvent(uintptr_t /*ident*/)
   {
      if (_eventfd != invalid_real_fd)
         return;  // already registered

      int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
      if (fd < 0)
         throw std::runtime_error("eventfd() failed");
      _eventfd = RealFd{fd};

      struct epoll_event ev{};
      ev.events  = EPOLLIN;
      ev.data.fd = fd;
      if (::epoll_ctl(*_epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
         throw std::runtime_error("epoll_ctl ADD eventfd failed");
   }

   void EpollEngine::triggerUserEvent(uintptr_t /*ident*/)
   {
      if (_eventfd == invalid_real_fd)
         return;
      uint64_t val = 1;
      ::write(*_eventfd, &val, sizeof(val));
   }

   void EpollEngine::registerSignal(int signo)
   {
      // Block the signal from default delivery
      sigset_t mask;
      sigemptyset(&mask);
      sigaddset(&mask, signo);
      ::sigprocmask(SIG_BLOCK, &mask, nullptr);

      if (_signalfd == invalid_real_fd)
      {
         int fd = ::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
         if (fd < 0)
            throw std::runtime_error("signalfd() failed");
         _signalfd = RealFd{fd};

         struct epoll_event ev{};
         ev.events  = EPOLLIN;
         ev.data.fd = fd;
         if (::epoll_ctl(*_epfd, EPOLL_CTL_ADD, fd, &ev) < 0)
            throw std::runtime_error("epoll_ctl ADD signalfd failed");
      }
      else
      {
         // Update the existing signalfd with the new signal set
         sigset_t all_mask;
         sigemptyset(&all_mask);
         sigaddset(&all_mask, signo);
         ::signalfd(*_signalfd, &all_mask, SFD_NONBLOCK | SFD_CLOEXEC);
      }
   }

}  // namespace psiber::detail
