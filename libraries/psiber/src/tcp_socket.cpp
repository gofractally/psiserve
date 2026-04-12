#include <psiber/tcp_socket.hpp>
#include <psiber/scheduler.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/types.h>
#endif

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <system_error>

namespace psiber
{
   // ── Helpers ───────────────────────────────────────────────────────────────

   namespace
   {
      void set_nonblocking(int fd)
      {
         int flags = ::fcntl(fd, F_GETFL, 0);
         if (flags < 0)
            throw std::system_error(errno, std::system_category(), "fcntl F_GETFL");
         if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
            throw std::system_error(errno, std::system_category(), "fcntl O_NONBLOCK");
      }

      void set_reuseaddr(int fd)
      {
         int val = 1;
         ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
      }

      /// Try a POSIX I/O call. On EAGAIN, yield the fiber and retry.
      /// Returns io_result with bytes transferred, 0 for EOF, -1 for error.
      template <typename Fn>
      io_result try_io(Scheduler& sched, RealFd fd, EventKind wait_for, Fn&& fn)
      {
         while (true)
         {
            ssize_t n = fn();
            if (n > 0)
               return {n, 0};
            if (n == 0)
               return {0, 0};  // EOF
            // n < 0
            int err = errno;
            if (err == EAGAIN || err == EWOULDBLOCK)
            {
               sched.yield(fd, wait_for);
               continue;
            }
            if (err == EINTR)
               continue;
            return {-1, err};
         }
      }
   }  // namespace

   // ── tcp_socket ────────────────────────────────────────────────────────────

   tcp_socket::~tcp_socket()
   {
      if (_fd != invalid_real_fd)
         ::close(*_fd);
   }

   tcp_socket::tcp_socket(tcp_socket&& o) noexcept : _fd(o._fd)
   {
      o._fd = invalid_real_fd;
   }

   tcp_socket& tcp_socket::operator=(tcp_socket&& o) noexcept
   {
      if (this != &o)
      {
         if (_fd != invalid_real_fd)
            ::close(*_fd);
         _fd   = o._fd;
         o._fd = invalid_real_fd;
      }
      return *this;
   }

   tcp_socket tcp_socket::connect(Scheduler&             sched,
                                  const struct sockaddr* addr,
                                  socklen_t              addrlen)
   {
      int fd = ::socket(addr->sa_family, SOCK_STREAM, 0);
      if (fd < 0)
         throw std::system_error(errno, std::system_category(), "socket");

      set_nonblocking(fd);

      RealFd rfd{fd};
      int    rc = ::connect(fd, addr, addrlen);
      if (rc == 0)
         return tcp_socket(rfd);

      if (errno != EINPROGRESS)
      {
         ::close(fd);
         throw std::system_error(errno, std::system_category(), "connect");
      }

      // Wait for connect to complete (writable = connected or error)
      sched.yield(rfd, Writable);

      // Check if connect succeeded
      int       err    = 0;
      socklen_t errlen = sizeof(err);
      ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen);
      if (err != 0)
      {
         ::close(fd);
         throw std::system_error(err, std::system_category(), "connect");
      }

      return tcp_socket(rfd);
   }

   tcp_socket tcp_socket::connect(Scheduler&       sched,
                                  std::string_view host,
                                  uint16_t         port)
   {
      struct addrinfo hints{};
      hints.ai_family   = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;

      char port_str[8];
      std::snprintf(port_str, sizeof(port_str), "%u", port);

      // DNS resolution (blocking — acceptable for now, async DNS is a future enhancement)
      std::string host_str(host);
      struct addrinfo* result = nullptr;
      int              rc     = ::getaddrinfo(host_str.c_str(), port_str, &hints, &result);
      if (rc != 0)
         throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(rc));

      std::system_error last_error(0, std::system_category());
      for (struct addrinfo* rp = result; rp; rp = rp->ai_next)
      {
         try
         {
            auto sock = connect(sched, rp->ai_addr, rp->ai_addrlen);
            ::freeaddrinfo(result);
            return sock;
         }
         catch (const std::system_error& e)
         {
            last_error = e;
         }
      }

      ::freeaddrinfo(result);
      throw last_error;
   }

   tcp_socket tcp_socket::from_fd(RealFd fd)
   {
      return tcp_socket(fd);
   }

   // ── I/O ───────────────────────────────────────────────────────────────────

   io_result tcp_socket::read(Scheduler& sched, void* buf, size_t len)
   {
      return try_io(sched, _fd, Readable, [&]() { return ::read(*_fd, buf, len); });
   }

   io_result tcp_socket::write(Scheduler& sched, const void* buf, size_t len)
   {
      return try_io(sched, _fd, Writable, [&]() { return ::write(*_fd, buf, len); });
   }

   io_result tcp_socket::write_all(Scheduler& sched, const void* buf, size_t total)
   {
      const char* p         = static_cast<const char*>(buf);
      size_t      remaining = total;

      while (remaining > 0)
      {
         io_result r = write(sched, p, remaining);
         if (!r)
            return {static_cast<ssize_t>(total - remaining), r.is_eof() ? 0 : r.error};
         p += r.bytes;
         remaining -= static_cast<size_t>(r.bytes);
      }
      return {static_cast<ssize_t>(total), 0};
   }

   io_result tcp_socket::read_all(Scheduler& sched, void* buf, size_t total)
   {
      char*  p         = static_cast<char*>(buf);
      size_t remaining = total;

      while (remaining > 0)
      {
         io_result r = read(sched, p, remaining);
         if (!r)
            return {static_cast<ssize_t>(total - remaining), r.is_eof() ? 0 : r.error};
         p += r.bytes;
         remaining -= static_cast<size_t>(r.bytes);
      }
      return {static_cast<ssize_t>(total), 0};
   }

   // ── Scatter/gather ────────────────────────────────────────────────────────

   io_result tcp_socket::readv(Scheduler& sched, struct iovec* iov, int iovcnt)
   {
      return try_io(sched, _fd, Readable, [&]() { return ::readv(*_fd, iov, iovcnt); });
   }

   io_result tcp_socket::writev(Scheduler& sched, struct iovec* iov, int iovcnt)
   {
      return try_io(sched, _fd, Writable, [&]() { return ::writev(*_fd, iov, iovcnt); });
   }

   // ── Zero-copy ─────────────────────────────────────────────────────────────

   io_result tcp_socket::sendfile(Scheduler& sched, RealFd file_fd, off_t offset, size_t count)
   {
      size_t total_sent = 0;

      while (total_sent < count)
      {
#ifdef __APPLE__
         // macOS: sendfile(fd, s, offset, len, hdtr, flags)
         // len is in/out — on return, *len = bytes actually sent
         off_t len = static_cast<off_t>(count - total_sent);
         int   rc  = ::sendfile(*file_fd, *_fd, offset + static_cast<off_t>(total_sent),
                                &len, nullptr, 0);
         if (rc == 0 || (rc < 0 && len > 0))
         {
            // Partial or full send
            total_sent += static_cast<size_t>(len);
            if (rc == 0 && total_sent == count)
               break;  // Done
            // Partial send — yield and retry
            sched.yield(_fd, Writable);
            continue;
         }
         if (rc < 0)
         {
            int err = errno;
            if (err == EAGAIN)
            {
               sched.yield(_fd, Writable);
               continue;
            }
            if (err == EINTR)
               continue;
            return {static_cast<ssize_t>(total_sent > 0 ? total_sent : -1),
                    total_sent > 0 ? 0 : err};
         }
#else
         // Linux: sendfile(out_fd, in_fd, offset, count)
         off_t   off = offset + static_cast<off_t>(total_sent);
         ssize_t n   = ::sendfile(*_fd, *file_fd, &off, count - total_sent);
         if (n > 0)
         {
            total_sent += static_cast<size_t>(n);
            continue;
         }
         if (n == 0)
            break;  // EOF on input file
         int err = errno;
         if (err == EAGAIN)
         {
            sched.yield(_fd, Writable);
            continue;
         }
         if (err == EINTR)
            continue;
         return {static_cast<ssize_t>(total_sent > 0 ? total_sent : -1),
                 total_sent > 0 ? 0 : err};
#endif
      }

      return {static_cast<ssize_t>(total_sent), 0};
   }

   // ── Socket options ────────────────────────────────────────────────────────

   void tcp_socket::set_nodelay(bool enable)
   {
      int val = enable ? 1 : 0;
      ::setsockopt(*_fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val));
   }

   void tcp_socket::set_cork(bool enable)
   {
#ifdef __APPLE__
      int val = enable ? 1 : 0;
      ::setsockopt(*_fd, IPPROTO_TCP, TCP_NOPUSH, &val, sizeof(val));
#else
      int val = enable ? 1 : 0;
      ::setsockopt(*_fd, IPPROTO_TCP, TCP_CORK, &val, sizeof(val));
#endif
   }

   void tcp_socket::set_keepalive(bool enable, int idle_secs)
   {
      int val = enable ? 1 : 0;
      ::setsockopt(*_fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val));

#ifdef __APPLE__
      if (enable)
         ::setsockopt(*_fd, IPPROTO_TCP, TCP_KEEPALIVE, &idle_secs, sizeof(idle_secs));
#else
      if (enable)
         ::setsockopt(*_fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle_secs, sizeof(idle_secs));
#endif
   }

   void tcp_socket::set_send_buffer(int size)
   {
      ::setsockopt(*_fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
   }

   void tcp_socket::set_recv_buffer(int size)
   {
      ::setsockopt(*_fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
   }

   // ── Lifecycle ─────────────────────────────────────────────────────────────

   void tcp_socket::shutdown_read()
   {
      if (_fd != invalid_real_fd)
         ::shutdown(*_fd, SHUT_RD);
   }

   void tcp_socket::shutdown_write()
   {
      if (_fd != invalid_real_fd)
         ::shutdown(*_fd, SHUT_WR);
   }

   void tcp_socket::close()
   {
      if (_fd != invalid_real_fd)
      {
         ::close(*_fd);
         _fd = invalid_real_fd;
      }
   }

   // ── tcp_listener ──────────────────────────────────────────────────────────

   tcp_listener::~tcp_listener()
   {
      if (_fd != invalid_real_fd)
         ::close(*_fd);
   }

   tcp_listener::tcp_listener(tcp_listener&& o) noexcept : _fd(o._fd)
   {
      o._fd = invalid_real_fd;
   }

   tcp_listener& tcp_listener::operator=(tcp_listener&& o) noexcept
   {
      if (this != &o)
      {
         if (_fd != invalid_real_fd)
            ::close(*_fd);
         _fd   = o._fd;
         o._fd = invalid_real_fd;
      }
      return *this;
   }

   tcp_listener tcp_listener::bind(uint16_t port, int backlog)
   {
      return bind("0.0.0.0", port, backlog);
   }

   tcp_listener tcp_listener::bind(const char* addr, uint16_t port, int backlog)
   {
      struct addrinfo hints{};
      hints.ai_family   = AF_UNSPEC;
      hints.ai_socktype = SOCK_STREAM;
      hints.ai_flags    = AI_PASSIVE;

      char port_str[8];
      std::snprintf(port_str, sizeof(port_str), "%u", port);

      struct addrinfo* result = nullptr;
      int              rc     = ::getaddrinfo(addr, port_str, &hints, &result);
      if (rc != 0)
         throw std::runtime_error(std::string("getaddrinfo: ") + gai_strerror(rc));

      int fd = ::socket(result->ai_family, SOCK_STREAM, 0);
      if (fd < 0)
      {
         ::freeaddrinfo(result);
         throw std::system_error(errno, std::system_category(), "socket");
      }

      set_reuseaddr(fd);
      set_nonblocking(fd);

      if (::bind(fd, result->ai_addr, result->ai_addrlen) < 0)
      {
         int err = errno;
         ::close(fd);
         ::freeaddrinfo(result);
         throw std::system_error(err, std::system_category(), "bind");
      }

      ::freeaddrinfo(result);

      if (::listen(fd, backlog) < 0)
      {
         int err = errno;
         ::close(fd);
         throw std::system_error(err, std::system_category(), "listen");
      }

      return tcp_listener(RealFd{fd});
   }

   tcp_socket tcp_listener::accept(Scheduler& sched)
   {
      struct sockaddr_storage peer;
      socklen_t               peerlen = sizeof(peer);
      return accept(sched, peer, peerlen);
   }

   tcp_socket tcp_listener::accept(Scheduler&              sched,
                                   struct sockaddr_storage& peer_addr,
                                   socklen_t&               peer_len)
   {
      while (true)
      {
         peer_len = sizeof(peer_addr);
         int fd   = ::accept(*_fd, reinterpret_cast<struct sockaddr*>(&peer_addr), &peer_len);
         if (fd >= 0)
         {
            set_nonblocking(fd);
            return tcp_socket::from_fd(RealFd{fd});
         }
         int err = errno;
         if (err == EAGAIN || err == EWOULDBLOCK)
         {
            sched.yield(_fd, Readable);
            continue;
         }
         if (err == EINTR)
            continue;
         throw std::system_error(err, std::system_category(), "accept");
      }
   }

   void tcp_listener::close()
   {
      if (_fd != invalid_real_fd)
      {
         ::close(*_fd);
         _fd = invalid_real_fd;
      }
   }

   uint16_t tcp_listener::port() const
   {
      struct sockaddr_storage addr{};
      socklen_t               len = sizeof(addr);
      if (::getsockname(*_fd, reinterpret_cast<struct sockaddr*>(&addr), &len) < 0)
         return 0;
      if (addr.ss_family == AF_INET)
         return ntohs(reinterpret_cast<struct sockaddr_in*>(&addr)->sin_port);
      if (addr.ss_family == AF_INET6)
         return ntohs(reinterpret_cast<struct sockaddr_in6*>(&addr)->sin6_port);
      return 0;
   }

}  // namespace psiber
