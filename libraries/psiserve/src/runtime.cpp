#include <psiserve/app_server.hpp>
#include <psiserve/db_host.hpp>
#include <psiserve/log.hpp>
#include <psiserve/tls.hpp>

#include <pfs/store.hpp>
#include <psitri/database.hpp>
#include <psitri/database_impl.hpp>

#include <psizam/error_codes.hpp>
#include <psizam/runtime.hpp>
#include <psizam/utils.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <signal.h>

#include <boost/thread/thread.hpp>

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

   AppServer::AppServer(Config cfg)
      : _cfg(std::move(cfg))
   {
   }

   AppServer::~AppServer() = default;

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

   void AppServer::run()
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

      // IPFS content store (optional)
      if (!_cfg.datadir.empty())
      {
         std::filesystem::create_directories(_cfg.datadir);
         auto db_path = (_cfg.datadir / "pfs.db").string();
         auto db = psitri::database::open(db_path);
         _ipfs = std::make_unique<pfs::store>(std::move(db));
         PSI_INFO("IPFS store: {}", db_path);
      }

      // Barrier: all workers must finish WASM compilation before any
      // start accepting, so no single worker monopolizes connections.
      std::barrier ready_barrier(num_threads);

      // Spawn N-1 worker threads; main thread becomes worker 0.
      // LLVM JIT needs deep stacks — 8MB matches the main thread default.
      boost::thread::attributes thread_attrs;
      thread_attrs.set_stack_size(8 * 1024 * 1024);

      std::vector<boost::thread> workers;
      workers.reserve(num_threads - 1);
      for (uint32_t i = 1; i < num_threads; ++i)
      {
         workers.emplace_back(thread_attrs,
            [this, i, listen_fd, ssl_ctx, &ready_barrier]()
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

   void AppServer::runWorker(int worker_id, RealFd listen_fd, SSL_CTX* ssl_ctx,
                           std::barrier<>& ready_barrier)
   {
      auto& sched = psiber::Scheduler::current();

      Process proc;
      proc.name = _process_name.c_str();
      proc.fds.alloc(SocketFd{listen_fd, ssl_ctx, nullptr});  // fd 0 = listen

      if (!_cfg.webroot.empty())
      {
         int dir_fd = ::open(_cfg.webroot.c_str(), O_RDONLY | O_DIRECTORY);
         if (dir_fd < 0)
            throw std::runtime_error("Failed to open webroot: " + _cfg.webroot.string());
         proc.fds.alloc(DirFd{RealFd{dir_fd}});  // fd 1 = webroot
      }

      log::set_active_logger(log::create_logger(proc.name));
      sched.setShutdownCheck([]() { return g_shutdown.load(std::memory_order_relaxed); });

      // Each worker prepares its own module handle (own host function
      // table, own host pointer). The psizam::runtime caches compiled
      // code internally, but each worker gets a separate handle so
      // there's no concurrent mutation of the table.
      auto code = read_wasm(_cfg.wasm_path.string());
      instance_policy policy;
      // jit1 for now — jit_llvm requires psizam-exec to link
      // psizam-llvm (tracked separately in psizam CMake).
      policy.initial = instance_policy::compile_tier::jit1;
      policy.metering = instance_policy::meter_mode::none;

      // Clear the cache so each worker gets a fresh module_handle_impl
      // with its own table. TODO: runtime should support cloning a
      // module_handle for per-worker host binding without re-parsing.
      _rt.evict(wasm_bytes{code});
      auto mod = _rt.prepare(wasm_bytes{code}, policy);

      // Register host functions on the module's table (transitional —
      // will move to PSIO_HOST_MODULE interfaces incrementally)
      auto& table = mod.table();
      table.add<&HostApi::psiAccept>("psi", "accept");
      table.add<&HostApi::psiRead>("psi", "read");
      table.add<&HostApi::psiWrite>("psi", "write");
      table.add<&HostApi::psiOpen>("psi", "open");
      table.add<&HostApi::psiFstat>("psi", "fstat");
      table.add<&HostApi::psiClose>("psi", "close");
      table.add<&HostApi::psiClock>("psi", "clock");
      table.add<&HostApi::psiSleepUntil>("psi", "sleep_until");
      table.add<&HostApi::psiSendFile>("psi", "sendfile");
      table.add<&HostApi::psiCork>("psi", "cork");
      table.add<&HostApi::psiUncork>("psi", "uncork");
      table.add<&HostApi::psiConnect>("psi", "connect");
      table.add<&HostApi::psiUdpBind>("psi", "udp_bind");
      table.add<&HostApi::psiRecvFrom>("psi", "recvfrom");
      table.add<&HostApi::psiSendTo>("psi", "sendto");
      table.add<&HostApi::psiIpfsPut>("psi", "ipfs_put");
      table.add<&HostApi::psiIpfsGet>("psi", "ipfs_get");
      table.add<&HostApi::psiIpfsStat>("psi", "ipfs_stat");

      // HostApi is per-worker — each worker has its own process/scheduler.
      // We set it as the host pointer so trampolines receive it.
      HostApi host(proc, sched, nullptr, _ipfs.get());
      mod.set_host_ptr(&host);

      // Database host — provides psi:db/* imports. Opened per-worker
      // so each worker gets its own write session (psitri write sessions
      // are not thread-safe). The database itself is thread-safe.
      //
      // Uses PSIO_HOST_MODULE registration (db_host.hpp defines it) via
      // the canonical-ABI 16-slot slow_dispatch path. Each entry gets a
      // host_override pointing at the db_host instance so it doesn't
      // conflict with the HostApi module-level host pointer.
      std::unique_ptr<db_host> db;
      std::filesystem::path db_dir = _cfg.datadir.empty()
         ? std::filesystem::temp_directory_path() / "psiserve_db"
         : _cfg.datadir;
      std::filesystem::create_directories(db_dir);
      {
         auto db_path = (db_dir / "blockchain.db").string();
         db = std::make_unique<db_host>();
         db->db = psitri::database::open(db_path);
         db->ws = db->db->start_write_session();
         db->name_to_root["blockchain"] = 0;

         _rt.provide<db_host>(mod, *db);

         // wasi-libc links fd_write for abort/panic output.
         // Stub it so the import resolves.
         table.add<&db_host::fd_write>("wasi_snapshot_preview1", "fd_write");
         table.entries_mutable().back().host_override = db.get();
      }

      auto inst = _rt.instantiate(mod, policy);

      // Now that the instance exists, update HostApi with the linear
      // memory pointer (not known until after instantiation).
      host.updateMemory(inst.linear_memory());

      PSI_INFO("Worker {} ready", worker_id);
      ready_barrier.arrive_and_wait();

      proc.markRunning();
      sched.spawnFiber(
         [&inst, &sched]()
         {
            try
            {
               inst.run_start();
            }
            catch (const psizam::wasm_exit_exception&)
            {
            }
            sched.interrupt();
         },
         "_start");

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
