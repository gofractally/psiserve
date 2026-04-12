#include <psiber/tls_socket.hpp>
#include <psiber/scheduler.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <sys/socket.h>

#include <stdexcept>
#include <string>

namespace psiber
{
   // ── Helpers ───────────────────────────────────────────────────────────────

   namespace
   {
      std::string ssl_error_string()
      {
         unsigned long err = ERR_get_error();
         if (err == 0)
            return "unknown TLS error";
         char buf[256];
         ERR_error_string_n(err, buf, sizeof(buf));
         return buf;
      }

      /// Perform an SSL handshake operation (connect or accept), yielding
      /// the fiber on WANT_READ / WANT_WRITE.  Returns the SSL* on
      /// success, throws on failure (frees the SSL* before throwing).
      void ssl_handshake(Scheduler& sched, RealFd fd, SSL* ssl,
                         int (*handshake_fn)(SSL*), const char* op_name)
      {
         while (true)
         {
            int ret = handshake_fn(ssl);
            if (ret == 1)
               return;  // handshake complete

            int err = SSL_get_error(ssl, ret);
            if (err == SSL_ERROR_WANT_READ)
               sched.yield(fd, Readable);
            else if (err == SSL_ERROR_WANT_WRITE)
               sched.yield(fd, Writable);
            else
            {
               std::string msg = std::string(op_name) + " failed: " + ssl_error_string();
               SSL_free(ssl);
               throw std::runtime_error(msg);
            }
         }
      }
   }  // namespace

   // ── tls_context ───────────────────────────────────────────────────────────

   tls_context tls_context::server(const char* cert_file, const char* key_file)
   {
      SSL_CTX* ctx = SSL_CTX_new(TLS_server_method());
      if (!ctx)
         throw std::runtime_error("SSL_CTX_new(server) failed: " + ssl_error_string());

      SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

      if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) != 1)
      {
         SSL_CTX_free(ctx);
         throw std::runtime_error(std::string("Failed to load TLS cert: ") +
                                  cert_file + ": " + ssl_error_string());
      }

      if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) != 1)
      {
         SSL_CTX_free(ctx);
         throw std::runtime_error(std::string("Failed to load TLS key: ") +
                                  key_file + ": " + ssl_error_string());
      }

      if (SSL_CTX_check_private_key(ctx) != 1)
      {
         SSL_CTX_free(ctx);
         throw std::runtime_error("TLS certificate and private key do not match");
      }

      return tls_context(ctx);
   }

   tls_context tls_context::client(const char* ca_file)
   {
      SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
      if (!ctx)
         throw std::runtime_error("SSL_CTX_new(client) failed: " + ssl_error_string());

      SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);

      if (ca_file)
      {
         if (SSL_CTX_load_verify_locations(ctx, ca_file, nullptr) != 1)
         {
            SSL_CTX_free(ctx);
            throw std::runtime_error(std::string("Failed to load CA file: ") +
                                     ca_file + ": " + ssl_error_string());
         }
      }
      else
      {
         SSL_CTX_set_default_verify_paths(ctx);
      }

      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);

      return tls_context(ctx);
   }

   tls_context::tls_context(tls_context&& o) noexcept : _ctx(o._ctx)
   {
      o._ctx = nullptr;
   }

   tls_context& tls_context::operator=(tls_context&& o) noexcept
   {
      if (this != &o)
      {
         if (_ctx)
            SSL_CTX_free(_ctx);
         _ctx   = o._ctx;
         o._ctx = nullptr;
      }
      return *this;
   }

   tls_context::~tls_context()
   {
      if (_ctx)
         SSL_CTX_free(_ctx);
   }

   // ── tls_socket ────────────────────────────────────────────────────────────

   tls_socket::~tls_socket()
   {
      if (_ssl)
      {
         SSL_free(_ssl);
         _ssl = nullptr;
      }
      // _sock destructor handles closing the fd
   }

   tls_socket::tls_socket(tls_socket&& o) noexcept
       : _sock(std::move(o._sock)), _ssl(o._ssl)
   {
      o._ssl = nullptr;
   }

   tls_socket& tls_socket::operator=(tls_socket&& o) noexcept
   {
      if (this != &o)
      {
         if (_ssl)
            SSL_free(_ssl);
         _sock  = std::move(o._sock);
         _ssl   = o._ssl;
         o._ssl = nullptr;
      }
      return *this;
   }

   // ── Construction ──────────────────────────────────────────────────────────

   tls_socket tls_socket::upgrade_client(Scheduler&       sched,
                                         tcp_socket&&     sock,
                                         tls_context&     ctx,
                                         std::string_view hostname)
   {
      SSL* ssl = SSL_new(ctx.get());
      if (!ssl)
         throw std::runtime_error("SSL_new failed: " + ssl_error_string());

      // Prevent SIGPIPE on macOS when peer closes during TLS I/O
#ifdef SO_NOSIGPIPE
      int one = 1;
      ::setsockopt(*sock.fd(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

      SSL_set_fd(ssl, *sock.fd());

      // SNI
      std::string host_str(hostname);
      SSL_set_tlsext_host_name(ssl, host_str.c_str());

      // Hostname verification
      SSL_set1_host(ssl, host_str.c_str());

      ssl_handshake(sched, sock.fd(), ssl, SSL_connect, "SSL_connect");

      return tls_socket(std::move(sock), ssl);
   }

   tls_socket tls_socket::upgrade_server(Scheduler&   sched,
                                         tcp_socket&& sock,
                                         tls_context& ctx)
   {
      SSL* ssl = SSL_new(ctx.get());
      if (!ssl)
         throw std::runtime_error("SSL_new failed: " + ssl_error_string());

#ifdef SO_NOSIGPIPE
      int one = 1;
      ::setsockopt(*sock.fd(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif

      SSL_set_fd(ssl, *sock.fd());

      ssl_handshake(sched, sock.fd(), ssl, SSL_accept, "SSL_accept");

      return tls_socket(std::move(sock), ssl);
   }

   tls_socket tls_socket::connect(Scheduler&       sched,
                                  std::string_view host,
                                  uint16_t         port,
                                  tls_context&     ctx)
   {
      auto sock = tcp_socket::connect(sched, host, port);
      return upgrade_client(sched, std::move(sock), ctx, host);
   }

   // ── I/O ───────────────────────────────────────────────────────────────────

   io_result tls_socket::read(Scheduler& sched, void* buf, size_t len)
   {
      while (true)
      {
         int n = SSL_read(_ssl, buf, static_cast<int>(len));
         if (n > 0)
            return {n, 0};
         if (n == 0)
            return {0, 0};  // clean shutdown / EOF

         int err = SSL_get_error(_ssl, n);
         if (err == SSL_ERROR_WANT_READ)
            sched.yield(_sock.fd(), Readable);
         else if (err == SSL_ERROR_WANT_WRITE)
            sched.yield(_sock.fd(), Writable);
         else
            return {-1, err};
      }
   }

   io_result tls_socket::write(Scheduler& sched, const void* buf, size_t len)
   {
      while (true)
      {
         int n = SSL_write(_ssl, buf, static_cast<int>(len));
         if (n > 0)
            return {n, 0};

         int err = SSL_get_error(_ssl, n);
         if (err == SSL_ERROR_WANT_READ)
            sched.yield(_sock.fd(), Readable);
         else if (err == SSL_ERROR_WANT_WRITE)
            sched.yield(_sock.fd(), Writable);
         else
            return {-1, err};
      }
   }

   io_result tls_socket::write_all(Scheduler& sched, const void* buf, size_t total)
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

   io_result tls_socket::read_all(Scheduler& sched, void* buf, size_t total)
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

   // ── Lifecycle ─────────────────────────────────────────────────────────────

   void tls_socket::shutdown(Scheduler& sched)
   {
      if (!_ssl)
         return;

      while (true)
      {
         int ret = SSL_shutdown(_ssl);
         if (ret >= 0)
            break;  // 0 = sent close_notify, 1 = both sides done

         int err = SSL_get_error(_ssl, ret);
         if (err == SSL_ERROR_WANT_READ)
            sched.yield(_sock.fd(), Readable);
         else if (err == SSL_ERROR_WANT_WRITE)
            sched.yield(_sock.fd(), Writable);
         else
            break;  // error — just bail
      }
   }

   void tls_socket::close()
   {
      if (_ssl)
      {
         SSL_free(_ssl);
         _ssl = nullptr;
      }
      _sock.close();
   }

}  // namespace psiber
