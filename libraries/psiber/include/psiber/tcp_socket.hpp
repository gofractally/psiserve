#pragma once

#include <psiber/detail/platform_engine.hpp>
#include <psiber/types.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <sys/socket.h>
#include <sys/uio.h>

namespace psiber
{
   template <typename Engine>
   class basic_scheduler;
   using Scheduler = basic_scheduler<detail::PlatformEngine>;

   /// Result of a socket I/O operation.
   ///
   /// - bytes > 0: success, bytes transferred
   /// - bytes == 0: EOF (peer closed connection)
   /// - bytes == -1: error, `error` holds errno
   struct io_result
   {
      ssize_t bytes = 0;
      int     error = 0;

      explicit operator bool() const { return bytes > 0; }
      bool     is_eof() const { return bytes == 0 && error == 0; }
   };

   /// Fiber-aware non-blocking TCP socket.
   ///
   /// All I/O methods yield the calling fiber on EAGAIN instead of
   /// blocking the OS thread.  The fiber resumes when the socket
   /// becomes ready, transparent to the caller.
   ///
   /// Move-only RAII — destructor closes the fd.
   class tcp_socket
   {
     public:
      tcp_socket() = default;
      ~tcp_socket();

      tcp_socket(tcp_socket&& o) noexcept;
      tcp_socket& operator=(tcp_socket&& o) noexcept;

      tcp_socket(const tcp_socket&)            = delete;
      tcp_socket& operator=(const tcp_socket&) = delete;

      // ── Construction ────────────────────────────────────────────────

      /// Connect to a remote address. Yields until connected.
      /// Throws std::system_error on failure.
      static tcp_socket connect(Scheduler& sched,
                                const struct sockaddr* addr,
                                socklen_t              addrlen);

      /// Connect by hostname and port (resolves DNS, then connects).
      /// DNS resolution is currently blocking; the connect itself yields.
      /// Throws std::system_error on failure.
      static tcp_socket connect(Scheduler& sched,
                                std::string_view host,
                                uint16_t         port);

      /// Wrap an existing non-blocking fd.
      static tcp_socket from_fd(RealFd fd);

      // ── I/O (fiber-aware) ───────────────────────────────────────────

      /// Read up to `len` bytes. Yields on EAGAIN. Returns partial results.
      io_result read(Scheduler& sched, void* buf, size_t len);

      /// Write up to `len` bytes. Yields on EAGAIN. Returns partial results.
      io_result write(Scheduler& sched, const void* buf, size_t len);

      /// Write all `len` bytes (loops until complete or error/EOF).
      io_result write_all(Scheduler& sched, const void* buf, size_t len);

      /// Read all `len` bytes (loops until complete or error/EOF).
      io_result read_all(Scheduler& sched, void* buf, size_t len);

      // ── Scatter/gather I/O ──────────────────────────────────────────

      /// Vectored read (readv). Yields on EAGAIN.
      io_result readv(Scheduler& sched, struct iovec* iov, int iovcnt);

      /// Vectored write (writev). Yields on EAGAIN.
      io_result writev(Scheduler& sched, struct iovec* iov, int iovcnt);

      // ── Zero-copy ──────────────────────────────────────────────────

      /// Send a file via the OS sendfile syscall (zero-copy).
      /// On macOS uses sendfile(2), on Linux uses sendfile(2).
      /// Yields the fiber during partial sends.
      io_result sendfile(Scheduler& sched, RealFd file_fd, off_t offset, size_t count);

      // ── Socket options ──────────────────────────────────────────────

      void set_nodelay(bool enable);
      void set_cork(bool enable);
      void set_keepalive(bool enable, int idle_secs = 60);
      void set_send_buffer(int size);
      void set_recv_buffer(int size);

      // ── Lifecycle ───────────────────────────────────────────────────

      void shutdown_read();
      void shutdown_write();
      void close();

      RealFd fd() const { return _fd; }
      bool   is_open() const { return _fd != invalid_real_fd; }

     private:
      explicit tcp_socket(RealFd fd) : _fd(fd) {}
      RealFd _fd = invalid_real_fd;
   };

   /// Fiber-aware TCP listener.
   ///
   /// Accepts incoming connections by yielding the fiber until a
   /// connection is ready, then returns a tcp_socket for the new
   /// connection.
   class tcp_listener
   {
     public:
      tcp_listener() = default;
      ~tcp_listener();

      tcp_listener(tcp_listener&& o) noexcept;
      tcp_listener& operator=(tcp_listener&& o) noexcept;

      tcp_listener(const tcp_listener&)            = delete;
      tcp_listener& operator=(const tcp_listener&) = delete;

      // ── Construction ────────────────────────────────────────────────

      /// Bind to all interfaces on the given port.
      /// port=0 lets the OS assign a free port.
      static tcp_listener bind(uint16_t port, int backlog = 128);

      /// Bind to a specific address and port.
      static tcp_listener bind(const char* addr, uint16_t port, int backlog = 128);

      // ── Accepting ───────────────────────────────────────────────────

      /// Accept a connection. Yields until one arrives.
      tcp_socket accept(Scheduler& sched);

      /// Accept with peer address information.
      tcp_socket accept(Scheduler&              sched,
                        struct sockaddr_storage& peer_addr,
                        socklen_t&               peer_len);

      // ── Accessors ───────────────────────────────────────────────────

      void     close();
      RealFd   fd() const { return _fd; }
      uint16_t port() const;  ///< Actual bound port (useful when bind(0))

     private:
      explicit tcp_listener(RealFd fd) : _fd(fd) {}
      RealFd _fd = invalid_real_fd;
   };

}  // namespace psiber
