#include <psiserve/host_api.hpp>
#include <psiserve/log.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

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
            // Plain TCP
            if (auto r = waitReady(*_sched, sock->real_fd, Readable); r.isErr())
               return r;
            ssize_t n = ::read(*sock->real_fd, dst, *len);
            if (n < 0)
               return PsiResult::fromErrno();
            return PsiResult::ok(static_cast<int32_t>(n));
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
         // Plain TCP
         if (auto r = waitReady(*_sched, sock->real_fd, Writable); r.isErr())
            return r;
         ssize_t n = ::write(*sock->real_fd, src, *len);
         if (n < 0)
            return PsiResult::fromErrno();
         return PsiResult::ok(static_cast<int32_t>(n));
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
      // Check for ".." at start, "/.." anywhere, or lone ".."
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

      int raw_fd = ::openat(*dir->real_fd, path_buf, O_RDONLY);
      if (raw_fd < 0)
         return PsiResult::fromErrno();

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

}  // namespace psiserve
