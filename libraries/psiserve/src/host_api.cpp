#include <psiserve/host_api.hpp>
#include <psiserve/log.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/uio.h>
#endif

namespace psiserve
{
   // ── PsiResult::fromErrno ──────────────────────────────────────────────────
   //
   // Maps POSIX errno (set by the most recent syscall) to a PsiError.
   // Called immediately after a failed syscall, before anything else can
   // clobber errno.

   const char* PsiResult::errorString() const
   {
      switch (error())
      {
         case PsiError::none:         return "ok";
         case PsiError::bad_fd:       return "bad file descriptor";
         case PsiError::not_socket:   return "not a socket";
         case PsiError::too_many_fds: return "too many open file descriptors";
         case PsiError::io_failure:   return "I/O engine failure";
         case PsiError::conn_refused: return "connection refused";
         case PsiError::conn_reset:   return "connection reset by peer";
         case PsiError::broken_pipe:  return "broken pipe";
         case PsiError::timed_out:    return "operation timed out";
         case PsiError::would_block:  return "operation would block";
         case PsiError::unknown:      return "unknown error";
         case PsiError::count_:       break;
      }
      return "undefined error";
   }

   PsiResult PsiResult::fromErrno()
   {
      switch (errno)
      {
         case EBADF:        return err(PsiError::bad_fd);
         case ENOTSOCK:     return err(PsiError::not_socket);
         case EMFILE:
         case ENFILE:       return err(PsiError::too_many_fds);
         case ECONNREFUSED: return err(PsiError::conn_refused);
         case ECONNRESET:   return err(PsiError::conn_reset);
         case EPIPE:        return err(PsiError::broken_pipe);
         case ETIMEDOUT:    return err(PsiError::timed_out);
         case EAGAIN:
#if EAGAIN != EWOULDBLOCK
         case EWOULDBLOCK:
#endif
                            return err(PsiError::would_block);
         default:           return err(PsiError::unknown);
      }
   }

   // ── helpers ───────────────────────────────────────────────────────────────

   /// Block until the fd is ready.  Suspends the current fiber and returns
   /// to the scheduler.  When this returns, the fd is ready.
   static PsiResult waitReady(Scheduler& sched, RealFd fd, EventKind events)
   {
      try
      {
         sched.yield(fd, events);
         return PsiResult::ok(0);
      }
      catch (...)
      {
         return PsiResult::err(PsiError::io_failure);
      }
   }

   // ── host function implementations ─────────────────────────────────────────

   HostApi::HostApi(Process& proc, Scheduler& sched, char* wasm_memory)
      : _proc(&proc), _sched(&sched), _wasm_memory(wasm_memory)
   {
   }

   void HostApi::setFiberRunner(FiberRunner runner)
   {
      _fiberRunner = std::move(runner);
   }

   PsiResult HostApi::psiAccept(VirtualFd listen_fd)
   {
      auto* entry = _proc->fds.get(listen_fd);
      if (!entry)
         return PsiResult::err(PsiError::bad_fd);

      auto* sock = std::get_if<SocketFd>(entry);
      if (!sock)
         return PsiResult::err(PsiError::not_socket);

      RealFd   real_listen = sock->real_fd;
      SSL_CTX* ssl_ctx     = sock->ssl_ctx;  // non-null if TLS listener

      if (auto r = waitReady(*_sched, real_listen, Readable); r.isErr())
         return r;

      struct sockaddr_storage addr;
      socklen_t               addrlen = sizeof(addr);
      int raw_conn = ::accept(*real_listen, (struct sockaddr*)&addr, &addrlen);
      if (raw_conn < 0)
         return PsiResult::fromErrno();

      RealFd real_conn{raw_conn};

      // Set non-blocking — required for try-before-yield I/O pattern
      int conn_flags = ::fcntl(raw_conn, F_GETFL, 0);
      ::fcntl(raw_conn, F_SETFL, conn_flags | O_NONBLOCK);

      // TLS handshake if listener has an SSL_CTX
      SSL* ssl = nullptr;
      if (ssl_ctx)
      {
         ssl = SSL_new(ssl_ctx);
         if (!ssl)
         {
            ::close(*real_conn);
            return PsiResult::err(PsiError::io_failure);
         }
         SSL_set_fd(ssl, *real_conn);

         for (;;)
         {
            int ret = SSL_accept(ssl);
            if (ret == 1)
               break;  // handshake complete

            int err = SSL_get_error(ssl, ret);
            if (err == SSL_ERROR_WANT_READ)
            {
               if (auto r = waitReady(*_sched, real_conn, Readable); r.isErr())
               {
                  SSL_free(ssl);
                  ::close(*real_conn);
                  return r;
               }
            }
            else if (err == SSL_ERROR_WANT_WRITE)
            {
               if (auto r = waitReady(*_sched, real_conn, Writable); r.isErr())
               {
                  SSL_free(ssl);
                  ::close(*real_conn);
                  return r;
               }
            }
            else
            {
               PSI_DEBUG("TLS handshake failed: {}", ERR_reason_error_string(ERR_get_error()));
               SSL_free(ssl);
               ::close(*real_conn);
               return PsiResult::err(PsiError::io_failure);
            }
         }
      }

      VirtualFd vfd = _proc->fds.alloc(SocketFd{real_conn, nullptr, ssl});
      if (vfd == invalid_virtual_fd)
      {
         if (ssl)
            SSL_free(ssl);
         ::close(*real_conn);
         return PsiResult::err(PsiError::too_many_fds);
      }
      return PsiResult::ok(*vfd);
   }

   PsiResult HostApi::psiRead(VirtualFd fd, WasmPtr buf, WasmSize len)
   {
      if (*len == 0)
         return PsiResult::ok(0);

      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return PsiResult::err(PsiError::bad_fd);

      char* dst = _wasm_memory + *buf;

      if (auto* sock = std::get_if<SocketFd>(entry))
      {
         if (sock->ssl)
         {
            // TLS read — retry on WANT_READ/WANT_WRITE
            for (;;)
            {
               int n = SSL_read(sock->ssl, dst, *len);
               if (n > 0)
                  return PsiResult::ok(n);
               if (n == 0)
                  return PsiResult::ok(0);  // EOF / clean shutdown

               int err = SSL_get_error(sock->ssl, n);
               if (err == SSL_ERROR_WANT_READ)
                  _sched->yield(sock->real_fd, Readable);
               else if (err == SSL_ERROR_WANT_WRITE)
                  _sched->yield(sock->real_fd, Writable);
               else
                  return PsiResult::err(PsiError::io_failure);
            }
         }
         else
         {
            // Plain TCP — try first, yield only on EAGAIN
            for (;;)
            {
               ssize_t n = ::read(*sock->real_fd, dst, *len);
               if (n >= 0)
                  return PsiResult::ok(static_cast<int32_t>(n));
               if (errno != EAGAIN && errno != EWOULDBLOCK)
                  return PsiResult::fromErrno();
               if (auto r = waitReady(*_sched, sock->real_fd, Readable); r.isErr())
                  return r;
            }
         }
      }
      else if (auto* file = std::get_if<FileFd>(entry))
      {
         // Regular files are always ready — no poll needed
         ssize_t n = ::read(*file->real_fd, dst, *len);
         if (n < 0)
            return PsiResult::fromErrno();
         return PsiResult::ok(static_cast<int32_t>(n));
      }

      return PsiResult::err(PsiError::bad_fd);
   }

   PsiResult HostApi::psiWrite(VirtualFd fd, WasmPtr buf, WasmSize len)
   {
      if (*len == 0)
         return PsiResult::ok(0);

      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return PsiResult::err(PsiError::bad_fd);

      auto* sock = std::get_if<SocketFd>(entry);
      if (!sock)
         return PsiResult::err(PsiError::not_socket);

      const char* src = _wasm_memory + *buf;

      if (sock->ssl)
      {
         // TLS write — retry on WANT_READ/WANT_WRITE
         for (;;)
         {
            int n = SSL_write(sock->ssl, src, *len);
            if (n > 0)
               return PsiResult::ok(n);

            int err = SSL_get_error(sock->ssl, n);
            if (err == SSL_ERROR_WANT_READ)
               _sched->yield(sock->real_fd, Readable);
            else if (err == SSL_ERROR_WANT_WRITE)
               _sched->yield(sock->real_fd, Writable);
            else
               return PsiResult::err(PsiError::io_failure);
         }
      }
      else
      {
         // Plain TCP — try first, yield only on EAGAIN
         for (;;)
         {
            ssize_t n = ::write(*sock->real_fd, src, *len);
            if (n >= 0)
               return PsiResult::ok(static_cast<int32_t>(n));
            if (errno != EAGAIN && errno != EWOULDBLOCK)
               return PsiResult::fromErrno();
            if (auto r = waitReady(*_sched, sock->real_fd, Writable); r.isErr())
               return r;
         }
      }
   }

   PsiResult HostApi::psiOpen(VirtualFd dir_fd, WasmPtr path_ptr, WasmSize path_len)
   {
      auto* entry = _proc->fds.get(dir_fd);
      if (!entry)
         return PsiResult::err(PsiError::bad_fd);

      auto* dir = std::get_if<DirFd>(entry);
      if (!dir)
         return PsiResult::err(PsiError::bad_fd);

      // Extract path string from WASM memory
      const char* path_start = _wasm_memory + *path_ptr;
      uint32_t    len        = *path_len;

      // Reject path traversal: no ".." components
      for (uint32_t i = 0; i + 1 < len; ++i)
      {
         if (path_start[i] == '.' && path_start[i + 1] == '.')
         {
            if ((i == 0 || path_start[i - 1] == '/') &&
                (i + 2 >= len || path_start[i + 2] == '/'))
               return PsiResult::err(PsiError::bad_fd);
         }
      }

      // Strip leading slash — openat paths are relative
      if (len > 0 && path_start[0] == '/')
      {
         path_start++;
         len--;
      }

      // Null-terminate on the stack for openat
      char path_buf[512];
      if (len >= sizeof(path_buf))
         return PsiResult::err(PsiError::bad_fd);
      std::memcpy(path_buf, path_start, len);
      path_buf[len] = '\0';

#ifdef __linux__
      // Linux: kernel-enforced sandbox via openat2 + RESOLVE_BENEATH.
      // Rejects any path that escapes dir_fd's subtree, including via
      // symlinks, ".." after symlink resolution, or mount-point traversal.
      struct open_how how = {};
      how.flags   = O_RDONLY;
      how.resolve = RESOLVE_BENEATH | RESOLVE_NO_MAGICLINKS;
      int raw_fd  = ::syscall(SYS_openat2, *dir->real_fd, path_buf, &how, sizeof(how));
#else
      // macOS: openat + post-open path verification.
      int raw_fd = ::openat(*dir->real_fd, path_buf, O_RDONLY);
#endif
      if (raw_fd < 0)
         return PsiResult::fromErrno();

#ifndef __linux__
      // Verify the opened file's real path is under the sandbox directory.
      // This catches symlink escapes that the string-level ".." check misses.
      char real_path[PATH_MAX];
      char dir_path[PATH_MAX];
      if (::fcntl(raw_fd, F_GETPATH, real_path) < 0 ||
          ::fcntl(*dir->real_fd, F_GETPATH, dir_path) < 0)
      {
         ::close(raw_fd);
         return PsiResult::err(PsiError::bad_fd);
      }

      size_t dir_len = std::strlen(dir_path);
      if (std::strncmp(real_path, dir_path, dir_len) != 0 ||
          (real_path[dir_len] != '/' && real_path[dir_len] != '\0'))
      {
         ::close(raw_fd);
         return PsiResult::err(PsiError::bad_fd);
      }
#endif

      VirtualFd vfd = _proc->fds.alloc(FileFd{RealFd{raw_fd}});
      if (vfd == invalid_virtual_fd)
      {
         ::close(raw_fd);
         return PsiResult::err(PsiError::too_many_fds);
      }
      return PsiResult::ok(*vfd);
   }

   PsiResult HostApi::psiFstat(VirtualFd fd, WasmPtr out_ptr)
   {
      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return PsiResult::err(PsiError::bad_fd);

      RealFd real_fd = invalid_real_fd;

      if (auto* file = std::get_if<FileFd>(entry))
         real_fd = file->real_fd;
      else if (auto* sock = std::get_if<SocketFd>(entry))
         real_fd = sock->real_fd;
      else
         return PsiResult::err(PsiError::bad_fd);

      struct stat st;
      if (::fstat(*real_fd, &st) < 0)
         return PsiResult::fromErrno();

      // Write file size as uint64_t to WASM memory
      uint64_t size = static_cast<uint64_t>(st.st_size);
      std::memcpy(_wasm_memory + *out_ptr, &size, sizeof(size));

      return PsiResult::ok(0);
   }

   void HostApi::psiClose(VirtualFd fd)
   {
      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return;

      if (auto* sock = std::get_if<SocketFd>(entry))
      {
         if (sock->ssl)
         {
            SSL_shutdown(sock->ssl);
            SSL_free(sock->ssl);
         }
         ::close(*sock->real_fd);
      }
      else if (auto* udp = std::get_if<UdpFd>(entry))
         ::close(*udp->real_fd);
      else if (auto* file = std::get_if<FileFd>(entry))
         ::close(*file->real_fd);
      else if (auto* dir = std::get_if<DirFd>(entry))
         ::close(*dir->real_fd);

      _proc->fds.close(fd);
   }

   void HostApi::psiSpawn(WasmPtr func_table_idx, WasmPtr arg)
   {
      _fiberRunner(*func_table_idx, static_cast<int32_t>(*arg));
   }

   int64_t HostApi::psiClock(int32_t clock_id)
   {
      if (clock_id == 0)
      {
         // REALTIME — wall clock, ns since Unix epoch
         auto now = std::chrono::system_clock::now();
         return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now.time_since_epoch())
            .count();
      }
      else
      {
         // MONOTONIC — steady clock, ns since arbitrary origin
         auto now = std::chrono::steady_clock::now();
         return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   now.time_since_epoch())
            .count();
      }
   }

   void HostApi::psiSleepUntil(int64_t deadline_ns)
   {
      auto deadline = std::chrono::steady_clock::time_point(
         std::chrono::nanoseconds{deadline_ns});

      auto now = std::chrono::steady_clock::now();
      if (deadline <= now)
         return;

      _sched->sleep(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now));
   }

   PsiResult HostApi::psiSendFile(VirtualFd sock_fd, VirtualFd file_fd, int64_t len)
   {
      auto* sock_entry = _proc->fds.get(sock_fd);
      if (!sock_entry)
         return PsiResult::err(PsiError::bad_fd);
      auto* sock = std::get_if<SocketFd>(sock_entry);
      if (!sock)
         return PsiResult::err(PsiError::not_socket);

      auto* file_entry = _proc->fds.get(file_fd);
      if (!file_entry)
         return PsiResult::err(PsiError::bad_fd);
      auto* file = std::get_if<FileFd>(file_entry);
      if (!file)
         return PsiResult::err(PsiError::bad_fd);

      int64_t total_sent = 0;

      if (sock->ssl)
      {
         // TLS: can't use OS sendfile — must copy through user-space buffer
         // for OpenSSL to encrypt.
         char buf[32768];
         while (total_sent < len)
         {
            size_t  chunk = std::min(static_cast<int64_t>(sizeof(buf)), len - total_sent);
            ssize_t nr    = ::read(*file->real_fd, buf, chunk);
            if (nr <= 0)
               break;

            int offset = 0;
            while (offset < nr)
            {
               int nw = SSL_write(sock->ssl, buf + offset, static_cast<int>(nr - offset));
               if (nw > 0)
               {
                  offset += nw;
                  continue;
               }
               int err = SSL_get_error(sock->ssl, nw);
               if (err == SSL_ERROR_WANT_WRITE)
                  _sched->yield(sock->real_fd, Writable);
               else if (err == SSL_ERROR_WANT_READ)
                  _sched->yield(sock->real_fd, Readable);
               else
                  return PsiResult::ok(static_cast<int32_t>(total_sent));
            }
            total_sent += nr;
         }
      }
      else
      {
         // Plain TCP: use OS sendfile for zero-copy transfer
         while (total_sent < len)
         {
#ifdef __APPLE__
            off_t sbytes = len - total_sent;
            int   rc     = ::sendfile(*file->real_fd, *sock->real_fd,
                                      total_sent, &sbytes, nullptr, 0);
            if (sbytes > 0)
               total_sent += sbytes;
            if (rc < 0)
            {
               if (errno == EAGAIN || errno == EWOULDBLOCK)
               {
                  _sched->yield(sock->real_fd, Writable);
                  continue;
               }
               break;
            }
#else
            ssize_t n = ::sendfile(*sock->real_fd, *file->real_fd, nullptr,
                                   static_cast<size_t>(len - total_sent));
            if (n > 0)
            {
               total_sent += n;
               continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            {
               _sched->yield(sock->real_fd, Writable);
               continue;
            }
            break;
#endif
         }
      }

      return PsiResult::ok(static_cast<int32_t>(total_sent));
   }

   void HostApi::psiCork(VirtualFd fd)
   {
      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return;
      auto* sock = std::get_if<SocketFd>(entry);
      if (!sock)
         return;

#ifdef __APPLE__
      // macOS: TCP_NOPUSH is the equivalent of TCP_CORK
      int on = 1;
      ::setsockopt(*sock->real_fd, IPPROTO_TCP, TCP_NOPUSH, &on, sizeof(on));
#else
      int on = 1;
      ::setsockopt(*sock->real_fd, IPPROTO_TCP, TCP_CORK, &on, sizeof(on));
#endif
   }

   void HostApi::psiUncork(VirtualFd fd)
   {
      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return;
      auto* sock = std::get_if<SocketFd>(entry);
      if (!sock)
         return;

#ifdef __APPLE__
      int off = 0;
      ::setsockopt(*sock->real_fd, IPPROTO_TCP, TCP_NOPUSH, &off, sizeof(off));
#else
      int off = 0;
      ::setsockopt(*sock->real_fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
#endif
   }

   PsiResult HostApi::psiUdpBind(int32_t port)
   {
      int fd = ::socket(AF_INET, SOCK_DGRAM, 0);
      if (fd < 0)
         return PsiResult::fromErrno();

      struct sockaddr_in addr = {};
      addr.sin_family      = AF_INET;
      addr.sin_addr.s_addr = INADDR_ANY;
      addr.sin_port        = htons(static_cast<uint16_t>(port));

      if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
      {
         ::close(fd);
         return PsiResult::fromErrno();
      }

      // Set non-blocking for kqueue/epoll integration
      int flags = ::fcntl(fd, F_GETFL, 0);
      ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

      VirtualFd vfd = _proc->fds.alloc(UdpFd{RealFd{fd}});
      if (vfd == invalid_virtual_fd)
      {
         ::close(fd);
         return PsiResult::err(PsiError::too_many_fds);
      }
      return PsiResult::ok(*vfd);
   }

   PsiResult HostApi::psiRecvFrom(VirtualFd fd, WasmPtr buf, WasmSize len, WasmPtr addr_ptr)
   {
      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return PsiResult::err(PsiError::bad_fd);

      auto* udp = std::get_if<UdpFd>(entry);
      if (!udp)
         return PsiResult::err(PsiError::not_socket);

      // Wait for readability
      if (auto r = waitReady(*_sched, udp->real_fd, Readable); r.isErr())
         return r;

      struct sockaddr_in sender = {};
      socklen_t          slen   = sizeof(sender);
      char*              dst    = _wasm_memory + *buf;

      ssize_t n = ::recvfrom(*udp->real_fd, dst, *len, 0,
                             (struct sockaddr*)&sender, &slen);
      if (n < 0)
         return PsiResult::fromErrno();

      // Write sender address to WASM memory: { uint32_t ip4; uint16_t port; uint16_t pad; }
      char* addr_out = _wasm_memory + *addr_ptr;
      std::memcpy(addr_out, &sender.sin_addr.s_addr, 4);
      std::memcpy(addr_out + 4, &sender.sin_port, 2);
      std::memset(addr_out + 6, 0, 2);

      return PsiResult::ok(static_cast<int32_t>(n));
   }

   PsiResult HostApi::psiSendTo(VirtualFd fd, WasmPtr buf, WasmSize len, WasmPtr addr_ptr)
   {
      auto* entry = _proc->fds.get(fd);
      if (!entry)
         return PsiResult::err(PsiError::bad_fd);

      auto* udp = std::get_if<UdpFd>(entry);
      if (!udp)
         return PsiResult::err(PsiError::not_socket);

      // Read destination address from WASM memory
      const char* addr_in = _wasm_memory + *addr_ptr;

      struct sockaddr_in dest = {};
      dest.sin_family = AF_INET;
      std::memcpy(&dest.sin_addr.s_addr, addr_in, 4);
      std::memcpy(&dest.sin_port, addr_in + 4, 2);

      const char* src = _wasm_memory + *buf;
      ssize_t     n   = ::sendto(*udp->real_fd, src, *len, 0,
                                 (struct sockaddr*)&dest, sizeof(dest));
      if (n < 0)
         return PsiResult::fromErrno();

      return PsiResult::ok(static_cast<int32_t>(n));
   }

}  // namespace psiserve
