#include <catch2/catch.hpp>

#include <psiber/tls_socket.hpp>
#include <psiber/tcp_socket.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/io_engine_kqueue.hpp>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace psiber;

// ── Test certificate generation ────────────────────────────────────────────
//
// Generates a self-signed cert + key in a temp directory for tests.
// Cleaned up by the destructor.

struct test_certs
{
   std::filesystem::path dir;
   std::filesystem::path cert;
   std::filesystem::path key;

   test_certs()
   {
      dir  = std::filesystem::temp_directory_path() / "psiber_tls_test";
      cert = dir / "cert.pem";
      key  = dir / "key.pem";

      std::filesystem::create_directories(dir);

      std::string cmd = "openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 "
                        "-keyout " +
                        key.string() + " -out " + cert.string() +
                        " -days 1 -nodes -subj '/CN=localhost' "
                        "-addext 'subjectAltName=DNS:localhost,IP:127.0.0.1' "
                        "2>/dev/null";
      int rc = std::system(cmd.c_str());
      if (rc != 0)
         throw std::runtime_error("Failed to generate test certificates");
   }

   ~test_certs() { std::filesystem::remove_all(dir); }
};

// ── TLS loopback echo ──────────────────────────────────────────────────────

TEST_CASE("tls_socket: loopback echo", "[tls][socket]")
{
   test_certs certs;

   auto      server_ctx = tls_context::server(certs.cert.c_str(), certs.key.c_str());
   auto      client_ctx = tls_context::client(certs.cert.c_str());  // trust our self-signed cert

   auto      io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 900);

   fiber_promise<uint16_t> port_promise;
   bool                    echo_ok = false;

   // Server fiber
   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      auto conn     = listener.accept(sched);
      auto tls_conn = tls_socket::upgrade_server(sched, std::move(conn), server_ctx);

      char buf[64];
      auto r = tls_conn.read(sched, buf, sizeof(buf));
      REQUIRE(r);
      tls_conn.write_all(sched, buf, static_cast<size_t>(r.bytes));
      tls_conn.shutdown(sched);
      tls_conn.close();
      listener.close();
   });

   // Client fiber
   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      uint16_t port = port_promise.get();

      auto sock     = tcp_socket::connect(sched, "127.0.0.1", port);
      auto tls_sock = tls_socket::upgrade_client(sched, std::move(sock), client_ctx, "localhost");

      const char* msg = "hello tls";
      tls_sock.write_all(sched, msg, std::strlen(msg));

      char buf[64];
      auto r = tls_sock.read_all(sched, buf, std::strlen(msg));
      REQUIRE(r);
      REQUIRE(std::string_view(buf, static_cast<size_t>(r.bytes)) == "hello tls");
      echo_ok = true;
      tls_sock.shutdown(sched);
      tls_sock.close();
   });

   sched.run();
   REQUIRE(echo_ok);
}

// ── TLS large transfer ─────────────────────────────────────────────────────

TEST_CASE("tls_socket: large transfer", "[tls][socket]")
{
   test_certs certs;

   auto server_ctx = tls_context::server(certs.cert.c_str(), certs.key.c_str());
   auto client_ctx = tls_context::client(certs.cert.c_str());

   auto      io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 901);

   fiber_promise<uint16_t> port_promise;

   constexpr int         payload_size = 64 * 1024;
   std::vector<char>     send_buf(payload_size, 'X');
   std::vector<char>     recv_buf(payload_size, 0);
   bool                  match = false;

   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      auto conn     = listener.accept(sched);
      auto tls_conn = tls_socket::upgrade_server(sched, std::move(conn), server_ctx);

      auto r = tls_conn.read_all(sched, recv_buf.data(), payload_size);
      REQUIRE(r.bytes == payload_size);
      match = (send_buf == recv_buf);
      tls_conn.shutdown(sched);
      tls_conn.close();
      listener.close();
   });

   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      auto sock     = tcp_socket::connect(sched, "127.0.0.1", port_promise.get());
      auto tls_sock = tls_socket::upgrade_client(sched, std::move(sock), client_ctx, "localhost");

      auto r = tls_sock.write_all(sched, send_buf.data(), payload_size);
      REQUIRE(r.bytes == payload_size);
      tls_sock.shutdown(sched);
      tls_sock.close();
   });

   sched.run();
   REQUIRE(match);
}

// ── tls_context factory methods ─────────────────────────────────────────────

TEST_CASE("tls_context: server and client create valid contexts", "[tls]")
{
   test_certs certs;

   auto server_ctx = tls_context::server(certs.cert.c_str(), certs.key.c_str());
   REQUIRE(server_ctx.get() != nullptr);

   auto client_ctx = tls_context::client(certs.cert.c_str());
   REQUIRE(client_ctx.get() != nullptr);

   // System trust store client
   auto sys_ctx = tls_context::client();
   REQUIRE(sys_ctx.get() != nullptr);
}

// ── tls_context move semantics ──────────────────────────────────────────────

TEST_CASE("tls_context: move semantics", "[tls]")
{
   test_certs certs;

   auto ctx1 = tls_context::client(certs.cert.c_str());
   REQUIRE(ctx1.get() != nullptr);

   auto ctx2 = std::move(ctx1);
   REQUIRE(ctx2.get() != nullptr);
   REQUIRE(ctx1.get() == nullptr);
}
