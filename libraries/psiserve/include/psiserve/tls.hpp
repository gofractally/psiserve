#pragma once

// RAII wrappers for OpenSSL types and TLS listener context.
//
// Uses zero-size functor deleters with std::unique_ptr — same footprint
// as a raw pointer, correct by construction.
//
// Usage:
//   TlsListenContext ctx("cert.pem", "key.pem");
//   SSL* ssl = ctx.newConnection(raw_fd);
//   // ... SSL_accept / SSL_read / SSL_write ...
//   // SSL_free handled by SocketFd cleanup

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace psiserve
{
   // ── RAII pointer aliases ──────────────────────────────────────────────────

   struct SslCtxDelete
   {
      void operator()(SSL_CTX* p) const { SSL_CTX_free(p); }
   };
   struct SslDelete
   {
      void operator()(SSL* p) const { SSL_free(p); }
   };

   using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDelete>;
   using SslPtr    = std::unique_ptr<SSL, SslDelete>;

   // ── TLS error formatting ─────────────────────────────────────────────────

   /// Pull the most recent OpenSSL error string from the thread-local queue.
   inline std::string sslErrorString()
   {
      unsigned long err = ERR_get_error();
      if (err == 0)
         return "unknown TLS error";
      char buf[256];
      ERR_error_string_n(err, buf, sizeof(buf));
      return buf;
   }

   // ── TlsListenContext ─────────────────────────────────────────────────────
   //
   // Owns the SSL_CTX for one TLS listener.  Created once at startup from
   // cert + key paths, lives for the duration of the process.  Each accepted
   // connection gets an SSL* via newConnection().

   class TlsListenContext
   {
     public:
      TlsListenContext(const std::filesystem::path& cert_path,
                       const std::filesystem::path& key_path)
      {
         _ctx.reset(SSL_CTX_new(TLS_server_method()));
         if (!_ctx)
            throw std::runtime_error("SSL_CTX_new failed: " + sslErrorString());

         SSL_CTX_set_min_proto_version(_ctx.get(), TLS1_2_VERSION);

         if (SSL_CTX_use_certificate_chain_file(_ctx.get(), cert_path.c_str()) != 1)
            throw std::runtime_error("Failed to load TLS certificate: " +
                                     cert_path.string() + ": " + sslErrorString());

         if (SSL_CTX_use_PrivateKey_file(_ctx.get(), key_path.c_str(), SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("Failed to load TLS private key: " + key_path.string() +
                                     ": " + sslErrorString());

         if (SSL_CTX_check_private_key(_ctx.get()) != 1)
            throw std::runtime_error("TLS certificate and private key do not match");
      }

      /// Create a new SSL connection bound to a raw socket fd.
      /// Caller takes ownership of the returned SSL* (wrap in SslPtr).
      SSL* newConnection(int raw_fd) const
      {
         SSL* ssl = SSL_new(_ctx.get());
         if (!ssl)
            throw std::runtime_error("SSL_new failed: " + sslErrorString());
         SSL_set_fd(ssl, raw_fd);
         return ssl;
      }

      SSL_CTX* get() const { return _ctx.get(); }

     private:
      SslCtxPtr _ctx;
   };

}  // namespace psiserve
