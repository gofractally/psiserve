#include <catch2/catch.hpp>

#include <psiber/tcp_socket.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/scheduler.hpp>

#include <cstring>
#include <thread>
#include <vector>

using namespace psiber;

// ── Basic loopback: listener + connector in same scheduler ──────────────────

TEST_CASE("tcp_socket: loopback echo on same scheduler", "[tcp][socket]")
{
   auto sched = scheduler_access::make(600);

   fiber_promise<uint16_t> port_promise;
   bool                    echo_ok = false;

   // Server fiber
   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);  // OS picks port
      port_promise.set_value(listener.port());

      auto conn = listener.accept(sched);
      conn.set_nodelay(true);

      char buf[64];
      auto r = conn.read(sched, buf, sizeof(buf));
      REQUIRE(r);
      conn.write_all(sched, buf, static_cast<size_t>(r.bytes));
      conn.close();
      listener.close();
   });

   // Client fiber
   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();  // let server start
      uint16_t port = port_promise.get();

      auto sock = tcp_socket::connect(sched, "127.0.0.1", port);
      sock.set_nodelay(true);

      const char* msg = "hello psiber";
      sock.write_all(sched, msg, std::strlen(msg));

      char buf[64];
      auto r = sock.read_all(sched, buf, std::strlen(msg));
      REQUIRE(r);
      REQUIRE(std::string_view(buf, static_cast<size_t>(r.bytes)) == "hello psiber");
      echo_ok = true;
      sock.close();
   });

   sched.run();
   REQUIRE(echo_ok);
}

// ── write_all ensures complete delivery ─────────────────────────────────────

TEST_CASE("tcp_socket: write_all delivers all bytes", "[tcp][socket]")
{
   auto sched = scheduler_access::make(601);

   fiber_promise<uint16_t> port_promise;

   constexpr int   payload_size = 64 * 1024;  // 64KB — larger than typical socket buffer
   std::vector<char> send_buf(payload_size, 'A');
   std::vector<char> recv_buf(payload_size, 0);
   bool              match = false;

   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      auto conn = listener.accept(sched);

      // Read all bytes
      auto r = conn.read_all(sched, recv_buf.data(), payload_size);
      REQUIRE(r.bytes == payload_size);
      match = (send_buf == recv_buf);
      conn.close();
      listener.close();
   });

   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      uint16_t port = port_promise.get();

      auto sock = tcp_socket::connect(sched, "127.0.0.1", port);
      auto r    = sock.write_all(sched, send_buf.data(), payload_size);
      REQUIRE(r.bytes == payload_size);
      sock.close();
   });

   sched.run();
   REQUIRE(match);
}

// ── Scatter/gather I/O ──────────────────────────────────────────────────────

TEST_CASE("tcp_socket: writev sends multiple buffers atomically", "[tcp][socket]")
{
   auto sched = scheduler_access::make(602);

   fiber_promise<uint16_t> port_promise;
   bool                    ok = false;

   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      auto conn = listener.accept(sched);
      char buf[32];
      auto r = conn.read_all(sched, buf, 11);
      REQUIRE(r.bytes == 11);
      REQUIRE(std::string_view(buf, 11) == "hello world");
      ok = true;
      conn.close();
      listener.close();
   });

   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      auto sock = tcp_socket::connect(sched, "127.0.0.1", port_promise.get());

      char         buf1[] = "hello";
      char         buf2[] = " world";
      struct iovec iov[2] = {{buf1, 5}, {buf2, 6}};
      auto         r      = sock.writev(sched, iov, 2);
      REQUIRE(r.bytes == 11);
      sock.close();
   });

   sched.run();
   REQUIRE(ok);
}

// ── Multiple concurrent connections ─────────────────────────────────────────

TEST_CASE("tcp_socket: multiple concurrent connections", "[tcp][socket]")
{
   auto sched = scheduler_access::make(603);

   fiber_promise<uint16_t> port_promise;
   constexpr int           num_clients = 4;
   std::atomic<int>        completed{0};

   // Server: accept and echo num_clients connections sequentially
   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      for (int i = 0; i < num_clients; ++i)
      {
         auto conn = listener.accept(sched);
         char buf[64];
         auto r = conn.read(sched, buf, sizeof(buf));
         if (r)
            conn.write_all(sched, buf, static_cast<size_t>(r.bytes));
         conn.close();
      }
      listener.close();
   });

   // Clients
   for (int c = 0; c < num_clients; ++c)
   {
      sched.spawnFiber([&, c]() {
         sched.yieldCurrentFiber();
         auto sock = tcp_socket::connect(sched, "127.0.0.1", port_promise.get());

         char msg[16];
         std::snprintf(msg, sizeof(msg), "client%d", c);
         size_t len = std::strlen(msg);

         sock.write_all(sched, msg, len);

         char buf[64];
         auto r = sock.read_all(sched, buf, len);
         REQUIRE(r.bytes == static_cast<ssize_t>(len));
         REQUIRE(std::string_view(buf, len) == msg);
         completed.fetch_add(1, std::memory_order_relaxed);
         sock.close();
      });
   }

   sched.run();
   REQUIRE(completed.load() == num_clients);
}

// ── Cross-thread: server and client on different schedulers ─────────────────

TEST_CASE("tcp_socket: cross-thread client and server", "[tcp][socket]")
{
   fiber_promise<uint16_t> port_promise;
   std::atomic<bool>       client_done{false};

   // Server thread
   std::thread server_thread([&]() {
   auto sched = scheduler_access::make(610);

      sched.spawnFiber([&]() {
         auto listener = tcp_listener::bind(0);
         uint16_t port = listener.port();
         port_promise.set_value(port);

         auto conn = listener.accept(sched);
         char buf[64];
         auto r = conn.read(sched, buf, sizeof(buf));
         if (r)
            conn.write_all(sched, buf, static_cast<size_t>(r.bytes));
         conn.close();
         listener.close();
      });

      sched.run();
   });

   // Client thread
   std::thread client_thread([&]() {
   auto sched = scheduler_access::make(611);

      sched.spawnFiber([&]() {
         if (port_promise.try_register_waiter(sched.currentFiber()))
            sched.parkCurrentFiber();

         uint16_t port = port_promise.get();
         auto     sock = tcp_socket::connect(sched, "127.0.0.1", port);

         sock.write_all(sched, "cross-thread", 12);
         char buf[64];
         auto r = sock.read_all(sched, buf, 12);
         REQUIRE(r.bytes == 12);
         REQUIRE(std::string_view(buf, 12) == "cross-thread");
         client_done.store(true, std::memory_order_relaxed);
         sock.close();
      });

      sched.run();
   });

   server_thread.join();
   client_thread.join();
   REQUIRE(client_done.load());
}

// ── EOF detection ───────────────────────────────────────────────────────────

TEST_CASE("tcp_socket: detects EOF when peer closes", "[tcp][socket]")
{
   auto sched = scheduler_access::make(604);

   fiber_promise<uint16_t> port_promise;
   bool                    eof_detected = false;

   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      auto conn = listener.accept(sched);
      // Read until EOF
      char buf[64];
      while (true)
      {
         auto r = conn.read(sched, buf, sizeof(buf));
         if (r.is_eof())
         {
            eof_detected = true;
            break;
         }
      }
      conn.close();
      listener.close();
   });

   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      auto sock = tcp_socket::connect(sched, "127.0.0.1", port_promise.get());
      sock.write_all(sched, "data", 4);
      sock.shutdown_write();
      // Give server time to detect EOF
      sched.sleep(std::chrono::milliseconds{10});
      sock.close();
   });

   sched.run();
   REQUIRE(eof_detected);
}

// ── Socket options ──────────────────────────────────────────────────────────

TEST_CASE("tcp_socket: socket options don't crash", "[tcp][socket]")
{
   auto sched = scheduler_access::make(605);

   fiber_promise<uint16_t> port_promise;

   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());
      auto conn = listener.accept(sched);
      conn.set_nodelay(true);
      conn.set_cork(true);
      conn.set_cork(false);
      conn.set_keepalive(true, 30);
      conn.set_send_buffer(32768);
      conn.set_recv_buffer(32768);
      conn.close();
      listener.close();
   });

   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      auto sock = tcp_socket::connect(sched, "127.0.0.1", port_promise.get());
      sock.close();
   });

   sched.run();
}

// ── tcp_listener: port() returns actual bound port ──────────────────────────

TEST_CASE("tcp_listener: port() returns bound port", "[tcp][socket]")
{
   auto listener = tcp_listener::bind(0);
   REQUIRE(listener.port() > 0);
   listener.close();
}
