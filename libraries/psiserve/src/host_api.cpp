#include <psiserve/host_api.hpp>
#include <psiserve/io_engine.hpp>
#include <psiserve/log.hpp>

#include <pfs/store.hpp>

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
         case PsiError::not_found:    return "not found";
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

   HostApi::HostApi(Process& proc, Scheduler& sched, char* wasm_memory,
                    pfs::store* ipfs)
      : _proc(&proc), _sched(&sched), _wasm_memory(wasm_memory), _ipfs(ipfs)
   {
   }

   void HostApi::setFiberRunner(const FiberRunner* runner)
   {
      _fiberRunner = runner;
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

      // TCP_NODELAY — send small writes immediately (prevents Nagle deadlocks
      // in proxy scenarios where both sides wait for each other's data)
      int nodelay = 1;
      ::setsockopt(raw_conn, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

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
      if (++_accept_count % 1000 == 1)
         PSI_INFO("accepted {} connection(s)", _accept_count);
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
      (*_fiberRunner)(*func_table_idx, static_cast<int32_t>(*arg));
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
      // macOS: TCP_NOPUSH has broken uncork semantics (doesn't flush).
      // Use TCP_NODELAY=0 to re-enable Nagle batching during cork.
      int off = 0;
      ::setsockopt(*sock->real_fd, IPPROTO_TCP, TCP_NODELAY, &off, sizeof(off));
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
      // Re-enable TCP_NODELAY to flush any Nagle-buffered data and
      // ensure subsequent writes go out immediately.
      int on = 1;
      ::setsockopt(*sock->real_fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
#else
      int off = 0;
      ::setsockopt(*sock->real_fd, IPPROTO_TCP, TCP_CORK, &off, sizeof(off));
#endif
   }

   // ── Async DNS resolver ─────────────────────────────────────────────────
   //
   // Resolves hostnames via raw UDP DNS queries, fully integrated with the
   // fiber scheduler (no threads).  Sends an A record query to the system
   // nameserver, yields the fiber until the response arrives, parses the
   // answer section for the first A record.

   namespace
   {
      // Cached system nameserver (parsed once from /etc/resolv.conf)
      struct NameserverCache
      {
         struct sockaddr_in addr;
         bool               valid = false;

         static NameserverCache& instance()
         {
            static NameserverCache cache;
            if (!cache.valid)
               cache.load();
            return cache;
         }

        private:
         void load()
         {
            addr           = {};
            addr.sin_family = AF_INET;
            addr.sin_port   = htons(53);

            // Default: 127.0.0.1 (systemd-resolved, dnsmasq, etc.)
            inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

            FILE* f = fopen("/etc/resolv.conf", "r");
            if (!f) { valid = true; return; }

            char line[256];
            while (fgets(line, sizeof(line), f))
            {
               // Skip comments and empty lines
               if (line[0] == '#' || line[0] == ';' || line[0] == '\n')
                  continue;

               // Match "nameserver <ip>"
               if (std::strncmp(line, "nameserver", 10) == 0 && line[10] == ' ')
               {
                  char* ip = line + 11;
                  // Trim trailing whitespace
                  char* end = ip;
                  while (*end && *end != '\n' && *end != ' ' && *end != '\t') ++end;
                  *end = '\0';

                  if (inet_pton(AF_INET, ip, &addr.sin_addr) == 1)
                     break;
               }
            }
            fclose(f);
            valid = true;
         }
      };

      // Encode a hostname as DNS wire-format labels.
      // "www.example.com" → "\x03www\x07example\x03com\x00"
      // Returns length written, or -1 on error.
      int dnsEncodeName(const char* hostname, char* out, int max_len)
      {
         int         pos = 0;
         const char* p   = hostname;

         while (*p)
         {
            const char* dot = p;
            while (*dot && *dot != '.') ++dot;
            int label_len = static_cast<int>(dot - p);

            if (label_len == 0 || label_len > 63)
               return -1;
            if (pos + 1 + label_len >= max_len)
               return -1;

            out[pos++] = static_cast<char>(label_len);
            for (int i = 0; i < label_len; ++i)
               out[pos++] = p[i];

            p = dot;
            if (*p == '.') ++p;
         }

         if (pos >= max_len)
            return -1;
         out[pos++] = 0;  // root label terminator
         return pos;
      }

      // Build a DNS A record query packet.
      // Returns total packet length, or -1 on error.
      int dnsBuildQuery(const char* hostname, uint16_t query_id, char* buf, int buf_size)
      {
         if (buf_size < 16)
            return -1;

         // Header (12 bytes)
         // ID
         buf[0] = static_cast<char>(query_id >> 8);
         buf[1] = static_cast<char>(query_id & 0xFF);
         // Flags: standard query, recursion desired (0x0100)
         buf[2] = 0x01;
         buf[3] = 0x00;
         // QDCOUNT = 1
         buf[4] = 0x00;
         buf[5] = 0x01;
         // ANCOUNT, NSCOUNT, ARCOUNT = 0
         buf[6] = buf[7] = buf[8] = buf[9] = buf[10] = buf[11] = 0;

         int pos = 12;

         // Question section: encoded name
         int name_len = dnsEncodeName(hostname, buf + pos, buf_size - pos - 4);
         if (name_len < 0)
            return -1;
         pos += name_len;

         // QTYPE = A (1)
         buf[pos++] = 0x00;
         buf[pos++] = 0x01;
         // QCLASS = IN (1)
         buf[pos++] = 0x00;
         buf[pos++] = 0x01;

         return pos;
      }

      // Skip a DNS wire-format name (handles label compression pointers).
      // Returns new position, or -1 on error.
      int dnsSkipName(const char* buf, int len, int pos)
      {
         while (pos < len)
         {
            uint8_t label = static_cast<uint8_t>(buf[pos]);
            if (label == 0)
               return pos + 1;
            if ((label & 0xC0) == 0xC0)
               return pos + 2;  // compression pointer (2 bytes)
            pos += 1 + label;
         }
         return -1;
      }

      // Parse a DNS response and extract the first A record's IPv4 address.
      // Returns 0 on success, -1 on failure.
      int dnsParseResponse(const char* buf, int len, uint16_t expected_id,
                           struct in_addr* addr_out)
      {
         if (len < 12)
            return -1;

         // Verify ID matches
         uint16_t id = (static_cast<uint8_t>(buf[0]) << 8) | static_cast<uint8_t>(buf[1]);
         if (id != expected_id)
            return -1;

         // Check flags: response bit set (0x8000), no error (rcode = 0)
         uint16_t flags = (static_cast<uint8_t>(buf[2]) << 8) | static_cast<uint8_t>(buf[3]);
         if (!(flags & 0x8000))
            return -1;
         if (flags & 0x000F)
            return -1;

         uint16_t qdcount = (static_cast<uint8_t>(buf[4]) << 8) | static_cast<uint8_t>(buf[5]);
         uint16_t ancount = (static_cast<uint8_t>(buf[6]) << 8) | static_cast<uint8_t>(buf[7]);

         if (ancount == 0)
            return -1;

         int pos = 12;

         // Skip question section
         for (uint16_t q = 0; q < qdcount; ++q)
         {
            pos = dnsSkipName(buf, len, pos);
            if (pos < 0 || pos + 4 > len)
               return -1;
            pos += 4;  // QTYPE + QCLASS
         }

         // Parse answer section, looking for first A record
         for (uint16_t a = 0; a < ancount && pos < len; ++a)
         {
            pos = dnsSkipName(buf, len, pos);
            if (pos < 0 || pos + 10 > len)
               return -1;

            uint16_t rtype = (static_cast<uint8_t>(buf[pos]) << 8) |
                              static_cast<uint8_t>(buf[pos + 1]);
            pos += 2;
            pos += 2;  // RCLASS
            pos += 4;  // TTL
            uint16_t rdlength = (static_cast<uint8_t>(buf[pos]) << 8) |
                                 static_cast<uint8_t>(buf[pos + 1]);
            pos += 2;

            if (rtype == 1 && rdlength == 4)
            {
               // A record — 4-byte IPv4 address
               if (pos + 4 > len)
                  return -1;
               std::memcpy(addr_out, buf + pos, 4);
               return 0;
            }

            pos += rdlength;
         }

         return -1;  // no A record found
      }

      // Async DNS resolution: sends UDP query, yields fiber, parses response.
      // Returns 0 on success and fills addr, or -1 on failure.
      int dnsResolveAsync(Scheduler& sched, const char* hostname,
                          struct in_addr* addr_out)
      {
         // Try parsing as an IP literal first — skip DNS entirely
         if (inet_pton(AF_INET, hostname, addr_out) == 1)
            return 0;

         auto& ns = NameserverCache::instance();

         // Create non-blocking UDP socket
         int udp_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
         if (udp_fd < 0)
            return -1;

         int flags = ::fcntl(udp_fd, F_GETFL, 0);
         ::fcntl(udp_fd, F_SETFL, flags | O_NONBLOCK);

         // Build DNS query
         static thread_local uint16_t next_query_id = 1;
         uint16_t                     query_id      = next_query_id++;

         char query_buf[512];
         int  query_len = dnsBuildQuery(hostname, query_id, query_buf, sizeof(query_buf));
         if (query_len < 0)
         {
            ::close(udp_fd);
            return -1;
         }

         // Send query to nameserver
         ssize_t sent = ::sendto(udp_fd, query_buf, query_len, 0,
                                 (struct sockaddr*)&ns.addr, sizeof(ns.addr));
         if (sent < 0)
         {
            ::close(udp_fd);
            return -1;
         }

         // Yield fiber until response arrives
         RealFd rfd{udp_fd};
         sched.yield(rfd, Readable);

         // Read response
         char    resp_buf[512];
         ssize_t resp_len = ::recv(udp_fd, resp_buf, sizeof(resp_buf), 0);
         ::close(udp_fd);

         if (resp_len <= 0)
            return -1;

         return dnsParseResponse(resp_buf, static_cast<int>(resp_len),
                                 query_id, addr_out);
      }
   }  // namespace

   PsiResult HostApi::psiConnect(WasmPtr host_ptr, WasmSize host_len, int32_t port)
   {
      uint32_t len = *host_len;
      if (len == 0 || len > 253)
         return PsiResult::err(PsiError::bad_fd);

      // Copy hostname from WASM memory and null-terminate
      char hostname[256];
      std::memcpy(hostname, _wasm_memory + *host_ptr, len);
      hostname[len] = '\0';

      // Async DNS resolution — sends UDP query, yields fiber, no threads
      struct in_addr resolved_ip;
      if (dnsResolveAsync(*_sched, hostname, &resolved_ip) != 0)
         return PsiResult::err(PsiError::conn_refused);

      struct sockaddr_in dest = {};
      dest.sin_family         = AF_INET;
      dest.sin_addr           = resolved_ip;
      dest.sin_port           = htons(static_cast<uint16_t>(port));

      // Create non-blocking TCP socket with TCP_NODELAY
      int raw_fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (raw_fd < 0)
         return PsiResult::fromErrno();

      int flags = ::fcntl(raw_fd, F_GETFL, 0);
      ::fcntl(raw_fd, F_SETFL, flags | O_NONBLOCK);

      int nodelay = 1;
      ::setsockopt(raw_fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));

      // Non-blocking connect — yields fiber until connected
      int rc = ::connect(raw_fd, (struct sockaddr*)&dest, sizeof(dest));

      if (rc < 0 && errno != EINPROGRESS)
      {
         ::close(raw_fd);
         return PsiResult::fromErrno();
      }

      if (rc < 0)
      {
         // EINPROGRESS — wait for socket to become writable
         RealFd real_fd{raw_fd};
         if (auto r = waitReady(*_sched, real_fd, Writable); r.isErr())
         {
            ::close(raw_fd);
            return r;
         }

         // Check if connect succeeded
         int       so_err = 0;
         socklen_t so_len = sizeof(so_err);
         ::getsockopt(raw_fd, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
         if (so_err != 0)
         {
            ::close(raw_fd);
            errno = so_err;
            return PsiResult::fromErrno();
         }
      }

      VirtualFd vfd = _proc->fds.alloc(SocketFd{RealFd{raw_fd}, nullptr, nullptr});
      if (vfd == invalid_virtual_fd)
      {
         ::close(raw_fd);
         return PsiResult::err(PsiError::too_many_fds);
      }
      return PsiResult::ok(*vfd);
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

   // ── IPFS / content-addressed storage ────────────────────────────

   PsiResult HostApi::psiIpfsPut(WasmPtr data, WasmSize data_len,
                                 WasmPtr cid_out, WasmSize cid_cap)
   {
      if (!_ipfs)
         return PsiResult::err(PsiError::io_failure);

      try
      {
         auto span = psio1::bytes_view(
             reinterpret_cast<const uint8_t*>(_wasm_memory + *data), *data_len);

         auto content_cid = _ipfs->put(span);
         auto cid_str     = content_cid.to_string();

         if (*cid_cap > 0)
         {
            uint32_t copy_len = std::min(*cid_cap, static_cast<uint32_t>(cid_str.size()));
            std::memcpy(_wasm_memory + *cid_out, cid_str.data(), copy_len);
         }
         return PsiResult::ok(static_cast<int32_t>(cid_str.size()));
      }
      catch (...)
      {
         return PsiResult::err(PsiError::io_failure);
      }
   }

   PsiResult HostApi::psiIpfsGet(WasmPtr cid_ptr, WasmSize cid_len,
                                 int64_t offset,
                                 WasmPtr buf, WasmSize buf_len)
   {
      if (!_ipfs)
         return PsiResult::err(PsiError::io_failure);

      try
      {
         std::string_view cid_str(_wasm_memory + *cid_ptr, *cid_len);
         auto c = pfs::cid::from_string(cid_str);
         auto fh = _ipfs->open(c);

         uint64_t remaining = (offset < static_cast<int64_t>(fh.size()))
                                  ? fh.size() - offset
                                  : 0;
         uint64_t to_read = std::min(remaining, static_cast<uint64_t>(*buf_len));
         if (to_read == 0)
            return PsiResult::ok(0);

         char*    dst      = _wasm_memory + *buf;
         uint64_t written  = 0;
         fh.read(offset, to_read, [&](psio1::bytes_view chunk)
         {
            std::memcpy(dst + written, chunk.data(), chunk.size());
            written += chunk.size();
         });

         return PsiResult::ok(static_cast<int32_t>(written));
      }
      catch (...)
      {
         return PsiResult::err(PsiError::not_found);
      }
   }

   PsiResult HostApi::psiIpfsStat(WasmPtr cid_ptr, WasmSize cid_len, WasmPtr size_out)
   {
      if (!_ipfs)
         return PsiResult::err(PsiError::io_failure);

      try
      {
         std::string_view cid_str(_wasm_memory + *cid_ptr, *cid_len);
         auto c  = pfs::cid::from_string(cid_str);
         auto fh = _ipfs->open(c);

         uint64_t size = fh.size();
         std::memcpy(_wasm_memory + *size_out, &size, sizeof(size));
         return PsiResult::ok(0);
      }
      catch (...)
      {
         return PsiResult::err(PsiError::not_found);
      }
   }

}  // namespace psiserve
