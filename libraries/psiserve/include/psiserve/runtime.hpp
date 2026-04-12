#pragma once

#include <psiserve/host_api.hpp>
#include <psiserve/io_engine_kqueue.hpp>
#include <psiserve/process.hpp>
#include <psiserve/scheduler.hpp>

#include <psizam/backend.hpp>

#include <barrier>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace psiserve
{
   /// Top-level runtime: loads WASM, creates listener, runs scheduler.
   class Runtime
   {
     public:
      /// Parameters for constructing a Runtime instance.
      struct Config
      {
         std::filesystem::path wasm_path;  ///< Path to the .wasm module to load.
         Port                  port{8080}; ///< TCP port to listen on.
         std::filesystem::path webroot;    ///< Directory to serve files from (fd 1).
         std::filesystem::path tls_cert;   ///< PEM certificate file (empty = no TLS).
         std::filesystem::path tls_key;    ///< PEM private key file.
         uint32_t threads = std::thread::hardware_concurrency();  ///< Worker threads (0 = auto).
      };

      explicit Runtime(Config cfg);

      /// Create the listen socket, load WASM, and run.
      void run();

     private:
      void runWorker(int worker_id, RealFd listen_fd, SSL_CTX* ssl_ctx,
                     std::barrier<>& ready_barrier);

      Config      _cfg;
      std::string _process_name;  ///< Derived from wasm filename, owns the storage.
   };

}  // namespace psiserve
