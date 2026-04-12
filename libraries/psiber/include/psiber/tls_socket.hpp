#pragma once

#include <psiber/tcp_socket.hpp>

#include <memory>
#include <string_view>

// Forward-declare OpenSSL types
typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st     SSL;

namespace psiber
{
   class Scheduler;

   /// TLS context — wraps SSL_CTX.
   ///
   /// Create once, reuse for many connections.  Thread-safe for
   /// concurrent use from multiple schedulers.
   class tls_context
   {
     public:
      /// Create a server context from PEM certificate and key files.
      static tls_context server(const char* cert_file, const char* key_file);

      /// Create a client context.
      /// If ca_file is provided, load it as trusted CA; otherwise use system trust store.
      static tls_context client(const char* ca_file = nullptr);

      tls_context(tls_context&& o) noexcept;
      tls_context& operator=(tls_context&& o) noexcept;
      ~tls_context();

      tls_context(const tls_context&)            = delete;
      tls_context& operator=(const tls_context&) = delete;

      SSL_CTX* get() const { return _ctx; }

     private:
      explicit tls_context(SSL_CTX* ctx) : _ctx(ctx) {}
      SSL_CTX* _ctx = nullptr;
   };

   /// Fiber-aware TLS socket — wraps tcp_socket + OpenSSL SSL.
   ///
   /// Upgrades an existing TCP connection to TLS, or connects directly.
   /// All I/O yields the fiber on SSL_ERROR_WANT_READ/WANT_WRITE,
   /// making TLS transparent to the caller.
   ///
   /// Move-only RAII — destructor shuts down TLS and closes the socket.
   class tls_socket
   {
     public:
      tls_socket() = default;
      ~tls_socket();

      tls_socket(tls_socket&& o) noexcept;
      tls_socket& operator=(tls_socket&& o) noexcept;

      tls_socket(const tls_socket&)            = delete;
      tls_socket& operator=(const tls_socket&) = delete;

      // ── Construction ────────────────────────────────────────────────

      /// Upgrade an existing TCP socket to TLS (client side).
      /// Performs the TLS handshake, yielding the fiber as needed.
      /// The hostname is used for SNI and certificate verification.
      static tls_socket upgrade_client(Scheduler&   sched,
                                       tcp_socket&& sock,
                                       tls_context& ctx,
                                       std::string_view hostname);

      /// Upgrade an existing TCP socket to TLS (server side).
      /// Performs the TLS handshake, yielding the fiber as needed.
      static tls_socket upgrade_server(Scheduler&   sched,
                                       tcp_socket&& sock,
                                       tls_context& ctx);

      /// Connect via TCP then upgrade to TLS (client).
      /// DNS resolution is currently blocking.
      static tls_socket connect(Scheduler&       sched,
                                std::string_view host,
                                uint16_t         port,
                                tls_context&     ctx);

      // ── I/O (fiber-aware) ───────────────────────────────────────────

      io_result read(Scheduler& sched, void* buf, size_t len);
      io_result write(Scheduler& sched, const void* buf, size_t len);
      io_result write_all(Scheduler& sched, const void* buf, size_t len);
      io_result read_all(Scheduler& sched, void* buf, size_t len);

      // ── Lifecycle ───────────────────────────────────────────────────

      /// Send TLS close_notify. Yields during the handshake.
      void shutdown(Scheduler& sched);

      /// Close without clean TLS shutdown (emergency/timeout).
      void close();

      RealFd fd() const { return _sock.fd(); }
      bool   is_open() const { return _ssl != nullptr; }

     private:
      tls_socket(tcp_socket&& sock, SSL* ssl) : _sock(std::move(sock)), _ssl(ssl) {}
      tcp_socket _sock;
      SSL*       _ssl = nullptr;
   };

}  // namespace psiber
