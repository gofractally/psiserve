#pragma once

#include <psiserve/host_api.hpp>
#include <psiserve/io_engine_kqueue.hpp>
#include <psiserve/process.hpp>
#include <psiserve/scheduler.hpp>

#include <psizam/runtime.hpp>

#include <barrier>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

namespace pfs { class store; }

namespace psiserve
{
   /// Top-level runtime: loads WASM, creates listener, runs scheduler.
   class Runtime
   {
     public:
      struct Config
      {
         std::filesystem::path wasm_path;
         Port                  port{8080};
         std::filesystem::path webroot;
         std::filesystem::path datadir;
         std::filesystem::path tls_cert;
         std::filesystem::path tls_key;
         uint32_t threads = std::thread::hardware_concurrency();
      };

      explicit Runtime(Config cfg);
      ~Runtime();

      void run();

     private:
      void runWorker(int worker_id, RealFd listen_fd, SSL_CTX* ssl_ctx,
                     std::barrier<>& ready_barrier);

      Config                      _cfg;
      std::string                 _process_name;
      std::unique_ptr<pfs::store> _ipfs;
      psizam::runtime             _rt;
   };

}  // namespace psiserve
