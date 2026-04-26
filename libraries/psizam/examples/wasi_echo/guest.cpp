// guest.cpp — WASI TCP echo service as a guest WASM module.
//
// Uses WASI 0.2.3 sockets and IO stream imports to:
//   1. Create a TCP socket
//   2. Bind to 0.0.0.0:PORT
//   3. Listen for connections
//   4. Accept connections and echo received data back
//
// All blocking is handled by the host (psiber scheduler fibers).
// The guest sees simple synchronous function calls.

#include "shared.hpp"

#include <wasi/0.2.3/sockets.hpp>
#include <wasi/0.2.3/io.hpp>

#include <psio1/guest_alloc.hpp>
#include <psizam/module.hpp>

// ── WIT custom sections ────────────────────────────────────────────
PSIO1_WIT_SECTION(wasi_echo_service)
PSIO1_WIT_SECTION(wasi_io_streams)
PSIO1_WIT_SECTION(wasi_io_poll)
PSIO1_WIT_SECTION(wasi_sockets_tcp)
PSIO1_WIT_SECTION(wasi_sockets_tcp_create_socket)
PSIO1_WIT_SECTION(wasi_sockets_instance_network)

// ── Raw WASM import declarations ───────────────────────────────────
// PSIO1_IMPORT_IMPL generates extern "C" [[clang::import_module(...)]]
// thunks for each method. We call through ImportProxy, not the struct
// methods, so the inline stubs in the WASI headers don't conflict.

PSIO1_IMPORT_IMPL(wasi_sockets_instance_network, instance_network)
PSIO1_IMPORT_IMPL(wasi_sockets_tcp_create_socket, create_tcp_socket)
PSIO1_IMPORT_IMPL(wasi_sockets_tcp,
   tcp_socket_start_bind,
   tcp_socket_finish_bind,
   tcp_socket_start_listen,
   tcp_socket_finish_listen,
   tcp_socket_accept,
   tcp_socket_local_address,
   tcp_socket_shutdown)
PSIO1_IMPORT_IMPL(wasi_io_streams,
   input_stream_blocking_read,
   output_stream_blocking_write_and_flush)

// ── Import call helper ─────────────────────────────────────────────
// WASI_CALL(interface, method, args...) lowers args, calls the raw
// WASM import, and lifts the return (including return-area protocol
// for complex types like result<tuple<...>, error-code>).

#define WASI_CALL(IFACE, METHOD, ...) \
   ::psizam::ImportProxy::call_impl<decltype(&IFACE::METHOD)>( \
      reinterpret_cast<::psizam::raw_import_fn>( \
         &PSIO1_IMPORT_RAW_NAME(IFACE, METHOD)) \
      __VA_OPT__(,) __VA_ARGS__)

// ── Error checking helpers ─────────────────────────────────────────

template <typename T>
static T unwrap_sock(socket_result<T> r)
{
   if (r.index() == 1)
      __builtin_trap();
   return static_cast<T&&>(std::get<0>(r));
}

static void check_sock(socket_result_void r)
{
   if (r.index() == 1)
      __builtin_trap();
}

template <typename T>
static T unwrap_io(wasi_result<T> r)
{
   if (r.index() == 1)
      __builtin_trap();
   return static_cast<T&&>(std::get<0>(r));
}

static void check_io(wasi_result_void r)
{
   if (r.index() == 1)
      __builtin_trap();
}

// ── Guest implementation ───────────────────────────────────────────

struct wasi_echo_impl
{
   void run(uint32_t port)
   {
      // Get the default network
      auto net = WASI_CALL(wasi_sockets_instance_network, instance_network);

      // Create a TCP socket
      auto sock = unwrap_sock(
         WASI_CALL(wasi_sockets_tcp_create_socket, create_tcp_socket,
                   ip_address_family::ipv4));

      // Bind to 0.0.0.0:port
      ip_socket_address bind_addr = ipv4_socket_address{
         static_cast<uint16_t>(port), ipv4_address{{0, 0, 0, 0}}};

      check_sock(
         WASI_CALL(wasi_sockets_tcp, tcp_socket_start_bind,
                   psio1::borrow<tcp_socket>{sock.handle},
                   psio1::borrow<network>{net.handle},
                   bind_addr));

      check_sock(
         WASI_CALL(wasi_sockets_tcp, tcp_socket_finish_bind,
                   psio1::borrow<tcp_socket>{sock.handle}));

      // Read back the assigned address (port may differ if 0 was passed)
      auto local = unwrap_sock(
         WASI_CALL(wasi_sockets_tcp, tcp_socket_local_address,
                   psio1::borrow<tcp_socket>{sock.handle}));

      // Listen
      check_sock(
         WASI_CALL(wasi_sockets_tcp, tcp_socket_start_listen,
                   psio1::borrow<tcp_socket>{sock.handle}));

      check_sock(
         WASI_CALL(wasi_sockets_tcp, tcp_socket_finish_listen,
                   psio1::borrow<tcp_socket>{sock.handle}));

      // Accept loop
      for (;;)
      {
         auto accept_r = WASI_CALL(wasi_sockets_tcp, tcp_socket_accept,
                                   psio1::borrow<tcp_socket>{sock.handle});
         if (accept_r.index() == 1)
            break;

         auto [client_sock, is, os] = std::get<0>(std::move(accept_r));

         // Echo loop for this connection
         for (;;)
         {
            auto read_r = WASI_CALL(wasi_io_streams, input_stream_blocking_read,
                                    psio1::borrow<input_stream>{is.handle},
                                    uint64_t{4096});
            if (read_r.index() == 1)
               break;

            auto& data = std::get<0>(read_r);
            if (data.empty())
               break;

            auto write_r = WASI_CALL(wasi_io_streams,
                                     output_stream_blocking_write_and_flush,
                                     psio1::borrow<output_stream>{os.handle},
                                     std::move(data));
            if (write_r.index() == 1)
               break;
         }

         WASI_CALL(wasi_sockets_tcp, tcp_socket_shutdown,
                   psio1::borrow<tcp_socket>{client_sock.handle},
                   shutdown_type::both);
      }
   }
};

PSIO1_MODULE(wasi_echo_impl, run)
