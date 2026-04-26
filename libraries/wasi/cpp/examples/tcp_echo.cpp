// tcp_echo.cpp — WASI 0.2.3 sockets host API demonstration.
//
// Creates a TCP echo server and a client on the same scheduler.
// The server accepts one connection, reads data, and echoes it back.
// The client connects, sends a message, reads the echo, and verifies.
// All I/O is fiber-based and non-blocking via the psiber scheduler.
//
// Build: cmake --build build/Debug --target wasi_tcp_echo
// Run:   ./build/Debug/bin/wasi_tcp_echo

#include <wasi/0.2.3/io_host.hpp>
#include <wasi/0.2.3/sockets_host.hpp>

#include <psiber/scheduler.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

using namespace wasi_host;
using psiber::Scheduler;

template <typename T>
static T unwrap(socket_result<T>& r, const char* ctx)
{
   if (r.index() == 1)
   {
      std::cerr << ctx << ": socket error " << static_cast<int>(std::get<1>(r)) << "\n";
      std::abort();
   }
   return std::move(std::get<0>(r));
}

static void check(socket_result_void& r, const char* ctx)
{
   if (r.index() == 1)
   {
      std::cerr << ctx << ": socket error " << static_cast<int>(std::get<1>(r)) << "\n";
      std::abort();
   }
}

template <typename T>
static T unwrap_io(wasi_result<T>& r, const char* ctx)
{
   if (r.index() == 1)
   {
      std::cerr << ctx << ": stream error\n";
      std::abort();
   }
   return std::move(std::get<0>(r));
}

static void check_io(wasi_result_void& r, const char* ctx)
{
   if (r.index() == 1)
   {
      std::cerr << ctx << ": stream error\n";
      std::abort();
   }
}

int main()
{
   auto& sched = Scheduler::current();

   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   uint16_t port = 0;
   bool     server_ready = false;

   // ── Server fiber ──────────────────────────────────────────────────
   sched.spawnFiber(
       [&]
       {
          std::cout << "[server] Creating TCP socket...\n";
          auto sock_r = sockets.create_tcp_socket(ip_address_family::ipv4);
          auto sock   = unwrap(sock_r, "create_tcp_socket");

          auto net = sockets.instance_network();

          // Bind to localhost:0 (OS picks a free port)
          ip_socket_address bind_addr = ipv4_socket_address{0, ipv4_address{{127, 0, 0, 1}}};

          auto bind_r = sockets.tcp_socket_start_bind(
              psio1::borrow<tcp_socket>{sock.handle},
              psio1::borrow<network>{net.handle}, bind_addr);
          check(bind_r, "start_bind");

          auto finish_bind_r = sockets.tcp_socket_finish_bind(
              psio1::borrow<tcp_socket>{sock.handle});
          check(finish_bind_r, "finish_bind");

          // Read back the assigned port
          auto local_r = sockets.tcp_socket_local_address(
              psio1::borrow<tcp_socket>{sock.handle});
          auto local_addr = unwrap(local_r, "local_address");
          port = std::get<ipv4_socket_address>(local_addr).port;
          std::cout << "[server] Bound to 127.0.0.1:" << port << "\n";

          // Listen
          auto listen_r = sockets.tcp_socket_start_listen(
              psio1::borrow<tcp_socket>{sock.handle});
          check(listen_r, "start_listen");

          auto finish_listen_r = sockets.tcp_socket_finish_listen(
              psio1::borrow<tcp_socket>{sock.handle});
          check(finish_listen_r, "finish_listen");

          server_ready = true;
          std::cout << "[server] Listening...\n";

          // Accept one connection (yields until a client connects)
          auto accept_r = sockets.tcp_socket_accept(
              psio1::borrow<tcp_socket>{sock.handle});
          auto [client_sock, is, os] = unwrap(accept_r, "accept");
          std::cout << "[server] Accepted connection\n";

          // Read data from client
          auto read_r = io.input_stream_blocking_read(
              psio1::borrow<input_stream>{is.handle}, 4096);
          auto data = unwrap_io(read_r, "server read");
          std::string msg(data.begin(), data.end());
          std::cout << "[server] Received: \"" << msg << "\"\n";

          // Echo it back
          auto write_r = io.output_stream_write(
              psio1::borrow<output_stream>{os.handle}, std::move(data));
          check_io(write_r, "server write");
          std::cout << "[server] Echoed back\n";

          // Shutdown
          sockets.tcp_socket_shutdown(
              psio1::borrow<tcp_socket>{client_sock.handle},
              shutdown_type::both);
       },
       "server");

   // ── Client fiber ──────────────────────────────────────────────────
   sched.spawnFiber(
       [&]
       {
          // Wait for server to be ready
          while (!server_ready)
             sched.yieldCurrentFiber();

          std::cout << "[client] Connecting to 127.0.0.1:" << port << "...\n";

          auto sock_r = sockets.create_tcp_socket(ip_address_family::ipv4);
          auto sock   = unwrap(sock_r, "client create_tcp_socket");

          auto net = sockets.instance_network();

          ip_socket_address remote = ipv4_socket_address{port, ipv4_address{{127, 0, 0, 1}}};

          auto conn_r = sockets.tcp_socket_start_connect(
              psio1::borrow<tcp_socket>{sock.handle},
              psio1::borrow<network>{net.handle}, remote);
          check(conn_r, "start_connect");

          // Wait for connection to complete
          Scheduler::current().yield(
              sockets.tcp_sockets.get(sock.handle)->fd, psiber::Writable);

          auto finish_r = sockets.tcp_socket_finish_connect(
              psio1::borrow<tcp_socket>{sock.handle});
          auto [is, os] = unwrap(finish_r, "finish_connect");
          std::cout << "[client] Connected\n";

          // Send a message
          std::string message = "Hello from WASI sockets!";
          std::vector<uint8_t> payload(message.begin(), message.end());
          auto write_r = io.output_stream_write(
              psio1::borrow<output_stream>{os.handle}, std::move(payload));
          check_io(write_r, "client write");
          std::cout << "[client] Sent: \"" << message << "\"\n";

          // Read the echo
          auto read_r = io.input_stream_blocking_read(
              psio1::borrow<input_stream>{is.handle}, 4096);
          auto data = unwrap_io(read_r, "client read");
          std::string echo(data.begin(), data.end());
          std::cout << "[client] Received echo: \"" << echo << "\"\n";

          if (echo == message)
             std::cout << "\n=== SUCCESS: TCP echo round-trip verified ===\n";
          else
             std::cout << "\n=== FAILURE: echo mismatch ===\n";

          sched.interrupt();
       },
       "client");

   sched.run();
   return 0;
}
