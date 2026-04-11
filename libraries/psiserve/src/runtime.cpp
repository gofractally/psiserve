#include <psiserve/log.hpp>
#include <psiserve/runtime.hpp>
#include <psiserve/tls.hpp>

#include <psizam/error_codes.hpp>
#include <psizam/host_function_table.hpp>
#include <psizam/psizam.hpp>
#include <psizam/utils.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <stdexcept>

namespace psiserve
{
   using namespace psizam;

   Runtime::Runtime(Config cfg)
      : _cfg(std::move(cfg))
   {
   }

   static RealFd createListenSocket(Port port)
   {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
         throw std::runtime_error("socket() failed");

      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      struct sockaddr_in addr = {};
      addr.sin_family         = AF_INET;
      addr.sin_addr.s_addr    = INADDR_ANY;
      addr.sin_port           = htons(*port);

      if (::bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0)
      {
         ::close(fd);
         throw std::runtime_error("bind() failed on port " + std::to_string(*port));
      }

      if (::listen(fd, 128) < 0)
      {
         ::close(fd);
         throw std::runtime_error("listen() failed");
      }

      return RealFd{fd};
   }

   void Runtime::run()
   {
      log::init();
      log::set_thread_name("main");

      // Register host functions into a table
      host_function_table table;
      table.add<&HostApi::psiAccept>("psi", "accept");
      table.add<&HostApi::psiRead>("psi", "read");
      table.add<&HostApi::psiWrite>("psi", "write");
      table.add<&HostApi::psiOpen>("psi", "open");
      table.add<&HostApi::psiFstat>("psi", "fstat");
      table.add<&HostApi::psiClose>("psi", "close");
      table.add<&HostApi::psiSpawn>("psi", "spawn");
      table.add<&HostApi::psiClock>("psi", "clock");
      table.add<&HostApi::psiSleepUntil>("psi", "sleep_until");
      table.add<&HostApi::psiSendFile>("psi", "sendfile");
      table.add<&HostApi::psiCork>("psi", "cork");
      table.add<&HostApi::psiUncork>("psi", "uncork");
      table.add<&HostApi::psiUdpBind>("psi", "udp_bind");
      table.add<&HostApi::psiRecvFrom>("psi", "recvfrom");
      table.add<&HostApi::psiSendTo>("psi", "sendto");

      // Create listen socket
      RealFd listen_fd = createListenSocket(_cfg.port);

      // TLS setup (optional)
      std::unique_ptr<TlsListenContext> tls;
      SSL_CTX*                          ssl_ctx = nullptr;
      if (!_cfg.tls_cert.empty())
      {
         tls     = std::make_unique<TlsListenContext>(_cfg.tls_cert, _cfg.tls_key);
         ssl_ctx = tls->get();
         PSI_INFO("TLS enabled: cert={} key={}", _cfg.tls_cert.string(), _cfg.tls_key.string());
      }

      PSI_INFO("Listening on port {} ({})", *_cfg.port, tls ? "https" : "http");

      // Set up I/O engine and scheduler
      auto io = std::make_unique<KqueueEngine>();
      Scheduler sched(std::move(io));

      // Derive process name from module filename (persists in _cfg)
      _process_name = _cfg.wasm_path.stem().string();

      // Set up process with listen socket as fd 0, webroot as fd 1
      Process proc;
      proc.name = _process_name.c_str();
      proc.fds.alloc(SocketFd{listen_fd, ssl_ctx, nullptr});  // fd 0 = listen socket

      if (!_cfg.webroot.empty())
      {
         int dir_fd = ::open(_cfg.webroot.c_str(), O_RDONLY | O_DIRECTORY);
         if (dir_fd < 0)
            throw std::runtime_error("Failed to open webroot: " + _cfg.webroot.string());
         proc.fds.alloc(DirFd{RealFd{dir_fd}});  // fd 1 = webroot
         PSI_INFO("Webroot: {}", _cfg.webroot.string());
      }

      sched.setProcess(&proc);

      // Compile WASM module — engine selected at runtime, no ifdefs
      wasm_allocator wa;
      auto           code = read_wasm(_cfg.wasm_path.string());
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
      compiled_module mod(std::move(code), std::move(table), &wa, {.eng = engine::jit_llvm});
#else
      compiled_module mod(std::move(code), std::move(table), &wa, {.eng = engine::jit});
#endif

      // Create the main execution instance
      auto main_inst = mod.create_instance();

      // Now that linear memory is allocated, construct the host API
      HostApi host(proc, sched, main_inst.linear_memory());
      main_inst.set_host(&host);

      // Resolve _start once
      auto start = main_inst.get_function<void()>("_start");

      // Set up the fiber runner
      host.setFiberRunner(
         [&mod, &proc, &sched](uint32_t func_idx, int32_t arg)
         {
            auto inst = std::make_shared<instance>(mod.create_fiber_instance());

            sched.spawnFiber(
               [inst, &proc, &sched, func_idx, arg]()
               {
                  HostApi fiber_host(proc, sched, inst->linear_memory());
                  inst->set_host(&fiber_host);
                  auto handler = inst->get_table_function<void(int32_t)>(func_idx);
                  handler(arg);
               });
         });

      // Spawn the main fiber (runs _start)
      proc.markRunning();
      sched.spawnFiber(
         [&start]()
         {
            try
            {
               start();
            }
            catch (const wasm_exit_exception&)
            {
               // Normal exit
            }
         });

      // Run the scheduler — blocks until all fibers complete
      try
      {
         sched.run();
      }
      catch (const std::exception& e)
      {
         PSI_ERROR("WASM error: {}", e.what());
      }

      proc.markDone();

      // Clean up listen socket
      ::close(*listen_fd);
      PSI_INFO("Server stopped");
   }

}  // namespace psiserve
