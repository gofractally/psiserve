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

#include <signal.h>

#include <atomic>
#include <barrier>
#include <stdexcept>
#include <thread>
#include <typeinfo>
#include <vector>

namespace psiserve
{
   using namespace psizam;

   // ── Shutdown signaling ─────────────────────────────────────────────

   // Global listen fd for the signal handler to shut down.
   static std::atomic<int>  g_listen_fd{-1};
   static std::atomic<bool> g_shutdown{false};

   static void shutdownHandler(int /*sig*/)
   {
      g_shutdown.store(true, std::memory_order_relaxed);

      // close() invalidates the fd for all threads.  Every worker's kqueue
      // will deliver an error event on the closed fd, breaking the accept loop.
      int fd = g_listen_fd.exchange(-1, std::memory_order_relaxed);
      if (fd >= 0)
         ::close(fd);
   }

   // ── Runtime ────────────────────────────────────────────────────────

   Runtime::Runtime(Config cfg)
      : _cfg(std::move(cfg))
   {
   }

   static RealFd createListenSocket(Port port, bool reuse_port = false)
   {
      int fd = ::socket(AF_INET, SOCK_STREAM, 0);
      if (fd < 0)
         throw std::runtime_error("socket() failed");

      int opt = 1;
      ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

      if (reuse_port)
         ::setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt));

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

      // Derive process name from module filename
      _process_name = _cfg.wasm_path.stem().string();

      // TLS setup (optional — SSL_CTX is thread-safe, shared across workers)
      std::unique_ptr<TlsListenContext> tls;
      SSL_CTX*                          ssl_ctx = nullptr;
      if (!_cfg.tls_cert.empty())
      {
         tls     = std::make_unique<TlsListenContext>(_cfg.tls_cert, _cfg.tls_key);
         ssl_ctx = tls->get();
         PSI_INFO("TLS enabled: cert={} key={}", _cfg.tls_cert.string(), _cfg.tls_key.string());
      }

      // Clamp thread count
      uint32_t num_threads = _cfg.threads;
      if (num_threads == 0)
         num_threads = std::max(1u, std::thread::hardware_concurrency());

      // Single shared listen socket — all workers accept from it.
      // On macOS, SO_REUSEPORT doesn't distribute connections (unlike Linux),
      // so we share one socket and let the kernel wake one blocked acceptor.
      RealFd listen_fd = createListenSocket(_cfg.port);

      // Install signal handlers for graceful shutdown.
      // shutdown() on the listen socket breaks all workers' accept() calls.
      g_listen_fd.store(*listen_fd, std::memory_order_relaxed);

      struct sigaction sa = {};
      sa.sa_handler       = shutdownHandler;
      sigemptyset(&sa.sa_mask);
      sa.sa_flags = 0;
      ::sigaction(SIGINT, &sa, nullptr);
      ::sigaction(SIGTERM, &sa, nullptr);

      PSI_INFO("Listening on port {} ({}) with {} worker{}",
               *_cfg.port, tls ? "https" : "http",
               num_threads, num_threads > 1 ? "s" : "");

      if (!_cfg.webroot.empty())
         PSI_INFO("Webroot: {}", _cfg.webroot.string());

      // Barrier: all workers must finish WASM compilation before any
      // start accepting, so no single worker monopolizes connections.
      std::barrier ready_barrier(num_threads);

      // Spawn N-1 worker threads; main thread becomes worker 0.
      std::vector<std::thread> workers;
      workers.reserve(num_threads - 1);
      for (uint32_t i = 1; i < num_threads; ++i)
      {
         workers.emplace_back([this, i, listen_fd, ssl_ctx, &ready_barrier]()
         {
            std::string name = "worker-" + std::to_string(i);
            log::set_thread_name(name.c_str());
            try
            {
               runWorker(static_cast<int>(i), listen_fd, ssl_ctx, ready_barrier);
            }
            catch (const std::exception& e)
            {
               PSI_ERROR("Worker {} fatal: {}", i, e.what());
            }
         });
      }

      // Main thread is worker 0
      try
      {
         runWorker(0, listen_fd, ssl_ctx, ready_barrier);
      }
      catch (const std::exception& e)
      {
         PSI_ERROR("Worker 0 fatal: {}", e.what());
      }

      // Wait for all worker threads
      for (auto& t : workers)
         t.join();

      // Clean up — listen fd may already be closed by signal handler
      g_shutdown.store(false, std::memory_order_relaxed);
      int fd = g_listen_fd.exchange(-1, std::memory_order_relaxed);
      if (fd >= 0)
         ::close(fd);
      PSI_INFO("Server stopped");
   }

   void Runtime::runWorker(int worker_id, RealFd listen_fd, SSL_CTX* ssl_ctx,
                           std::barrier<>& ready_barrier)
   {
      // Each worker gets its own host function table
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
      table.add<&HostApi::psiConnect>("psi", "connect");
      table.add<&HostApi::psiUdpBind>("psi", "udp_bind");
      table.add<&HostApi::psiRecvFrom>("psi", "recvfrom");
      table.add<&HostApi::psiSendTo>("psi", "sendto");

      // Each worker gets its own scheduler (engine created internally)
      auto sched = psiber::scheduler_access::make(static_cast<uint32_t>(worker_id));

      // Each worker gets its own process (own fd table, shared listen socket)
      Process proc;
      proc.name = _process_name.c_str();
      proc.fds.alloc(SocketFd{listen_fd, ssl_ctx, nullptr});  // fd 0 = listen socket

      // Open webroot directory independently per worker (each needs its own dir fd)
      if (!_cfg.webroot.empty())
      {
         int dir_fd = ::open(_cfg.webroot.c_str(), O_RDONLY | O_DIRECTORY);
         if (dir_fd < 0)
            throw std::runtime_error("Failed to open webroot: " + _cfg.webroot.string());
         proc.fds.alloc(DirFd{RealFd{dir_fd}});  // fd 1 = webroot
      }

      log::set_active_logger(log::create_logger(proc.name));
      sched.setShutdownCheck([]() { return g_shutdown.load(std::memory_order_relaxed); });

      // Each worker compiles its own WASM module (own wasm_allocator = own linear memory)
      wasm_allocator wa;
      auto           code = read_wasm(_cfg.wasm_path.string());
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
      compiled_module mod(std::move(code), std::move(table), &wa, {.eng = engine::jit_llvm});
#else
      compiled_module mod(std::move(code), std::move(table), &wa, {.eng = engine::jit});
#endif

      // Create the main execution instance for this worker
      auto main_inst = mod.create_instance();

      HostApi host(proc, sched, main_inst.linear_memory());
      main_inst.set_host(&host);

      auto start = main_inst.get_function<void()>("_start");

      // Fiber runner for this worker's scheduler.
      // Lives on the worker thread stack; all fibers hold a pointer to it.
      FiberRunner fiber_runner =
         [&mod, &proc, &sched, &fiber_runner](uint32_t func_idx, int32_t arg)
         {
            auto inst = std::make_shared<instance>(mod.create_fiber_instance());

            sched.spawnFiber(
               [inst, &proc, &sched, &fiber_runner, func_idx, arg]()
               {
                  HostApi fiber_host(proc, sched, inst->linear_memory());
                  fiber_host.setFiberRunner(&fiber_runner);
                  inst->set_host(&fiber_host);
                  try
                  {
                     auto handler = inst->get_table_function<void(int32_t)>(func_idx);
                     handler(arg);
                  }
                  catch (const wasm_exit_exception&)
                  {
                     // Normal exit
                  }
                  catch (const std::exception& e)
                  {
                     PSI_ERROR("fiber exception: func_idx={} arg={}: type={} what={}",
                               func_idx, arg, typeid(e).name(), e.what());
                  }
               },
               "wasm");
         };
      host.setFiberRunner(&fiber_runner);

      // Wait for all workers to finish compilation before accepting
      PSI_INFO("Worker {} ready", worker_id);
      ready_barrier.arrive_and_wait();

      // Run _start in a fiber.  When _start returns (e.g., due to listen socket
      // shutdown), interrupt the scheduler so blocked connection fibers don't
      // keep the worker alive indefinitely.
      proc.markRunning();
      sched.spawnFiber(
         [&start, &sched]()
         {
            try
            {
               start();
            }
            catch (const wasm_exit_exception&)
            {
               // Normal exit
            }
            // Accept loop exited — tell scheduler to stop waiting for
            // blocked fibers (idle keep-alive connections).
            sched.interrupt();
         },
         "_start");

      // Run scheduler — blocks until all fibers complete
      try
      {
         sched.run();
      }
      catch (const std::exception& e)
      {
         PSI_ERROR("Worker {} error: {}", worker_id, e.what());
      }

      proc.markDone();
      PSI_INFO("Worker {} stopping (accepted {} connections)", worker_id, host.acceptCount());
   }

}  // namespace psiserve
