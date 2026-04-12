#include <psiber/detail/io_engine_kqueue.hpp>

#include <sys/event.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>

namespace psiber::detail
{
   KqueueEngine::KqueueEngine()
   {
      int fd = ::kqueue();
      if (fd < 0)
         throw std::runtime_error("kqueue() failed");
      _kq = RealFd{fd};
   }

   KqueueEngine::~KqueueEngine()
   {
      if (_kq != invalid_real_fd)
         ::close(*_kq);
   }

   void KqueueEngine::updateFds(std::span<const FdChange> changes)
   {
      constexpr int max_batch = 128;
      struct kevent adds[max_batch];
      struct kevent dels[max_batch];
      int           nadd = 0;
      int           ndel = 0;

      auto flushAdds = [&]() {
         if (nadd > 0)
         {
            if (::kevent(*_kq, adds, nadd, nullptr, 0, nullptr) < 0)
               throw std::runtime_error("kevent updateFds (add) failed");
            nadd = 0;
         }
      };

      auto flushDels = [&]() {
         if (ndel > 0)
         {
            // Best-effort: fd may have been closed or filter never registered
            ::kevent(*_kq, dels, ndel, nullptr, 0, nullptr);
            ndel = 0;
         }
      };

      for (auto& c : changes)
      {
         if (c.events == EventKind{})
         {
            EV_SET(&dels[ndel], *c.real_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
            ++ndel;
            EV_SET(&dels[ndel], *c.real_fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
            ++ndel;
            if (ndel >= max_batch - 2)
               flushDels();
         }
         else
         {
            if (c.events & Readable)
            {
               EV_SET(&adds[nadd], *c.real_fd, EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, nullptr);
               ++nadd;
            }
            if (c.events & Writable)
            {
               EV_SET(&adds[nadd], *c.real_fd, EVFILT_WRITE, EV_ADD | EV_ENABLE, 0, 0, nullptr);
               ++nadd;
            }
            if (nadd >= max_batch - 2)
               flushAdds();
         }
      }

      flushDels();
      flushAdds();
   }

   int KqueueEngine::poll(std::span<IoEvent>                        out,
                          std::optional<std::chrono::milliseconds> timeout)
   {
      constexpr int   max_kevents = 64;
      int             capacity = static_cast<int>(std::min<std::size_t>(out.size(), max_kevents));
      struct kevent   kevents[max_kevents];
      struct timespec ts;
      struct timespec* tsp = nullptr;

      if (timeout)
      {
         auto ms    = timeout->count();
         ts.tv_sec  = ms / 1000;
         ts.tv_nsec = (ms % 1000) * 1000000L;
         tsp        = &ts;
      }

      int n = ::kevent(*_kq, nullptr, 0, kevents, capacity, tsp);
      if (n < 0)
      {
         if (errno == EINTR)
            return 0;
         throw std::runtime_error("kevent poll failed");
      }

      for (int i = 0; i < n; ++i)
      {
         EventKind ev{};
         if (kevents[i].filter == EVFILT_READ)
            ev = ev | Readable;
         if (kevents[i].filter == EVFILT_WRITE)
            ev = ev | Writable;
         if (kevents[i].filter == EVFILT_USER)
            ev = ev | UserWake;
         if (kevents[i].filter == EVFILT_SIGNAL)
            ev = ev | Signal;
         out[i] = {RealFd{static_cast<int>(kevents[i].ident)}, ev};
      }

      return n;
   }

   void KqueueEngine::removeFdEvents(RealFd real_fd, EventKind events)
   {
      struct kevent dels[2];
      int           ndel = 0;

      if (events & Readable)
      {
         EV_SET(&dels[ndel], *real_fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
         ++ndel;
      }
      if (events & Writable)
      {
         EV_SET(&dels[ndel], *real_fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
         ++ndel;
      }

      if (ndel > 0)
         ::kevent(*_kq, dels, ndel, nullptr, 0, nullptr);  // best-effort
   }

   void KqueueEngine::registerUserEvent(uintptr_t ident)
   {
      struct kevent ev;
      EV_SET(&ev, ident, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
      if (::kevent(*_kq, &ev, 1, nullptr, 0, nullptr) < 0)
         throw std::runtime_error("kevent registerUserEvent failed");
   }

   void KqueueEngine::triggerUserEvent(uintptr_t ident)
   {
      struct kevent ev;
      EV_SET(&ev, ident, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
      // Thread-safe: kevent() for EVFILT_USER trigger is safe from any thread.
      // Idempotent: multiple triggers between polls collapse into one.
      ::kevent(*_kq, &ev, 1, nullptr, 0, nullptr);
   }

   void KqueueEngine::registerSignal(int signo)
   {
      // Block the signal from default delivery so kqueue gets it
      ::signal(signo, SIG_IGN);

      struct kevent ev;
      EV_SET(&ev, signo, EVFILT_SIGNAL, EV_ADD, 0, 0, nullptr);
      if (::kevent(*_kq, &ev, 1, nullptr, 0, nullptr) < 0)
         throw std::runtime_error("kevent registerSignal failed");
   }

}  // namespace psiber::detail
