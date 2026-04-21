// bc_connection.cpp — Per-connection handler for the blockchain PoC.
//
// Instantiated per TCP connection on thread::fresh by blockchain.wasm.
// Parses HTTP requests and routes them back to blockchain.wasm via
// synchronous cross-instance calls.
//
// Phase 2 module — requires async_call, thread::fresh, and socket
// ownership transfer to be wired in the host infrastructure.

#include "shared_interfaces.hpp"

#include <psi/host.h>
#include <psi/tcp.h>
#include <psi/http.h>

#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

PSIO_WIT_SECTION(bc_connection_iface)

struct bc_connection_impl
{
   void handle_connection(uint32_t blockchain_handle, uint32_t sock_fd)
   {
      tcp_conn conn;
      conn.fd = static_cast<int>(sock_fd);

      char buf[8192];
      http_request req;

      while (tcp_is_open(&conn))
      {
         int r = http_read_request(&conn, &req, buf, sizeof(buf));
         if (r <= 0)
            break;

         int path_len;
         const char* path = http_path(&req, &path_len);

         // TODO: Route requests to blockchain.wasm via cross-instance call:
         //   call(blockchain_handle, "push_transaction"_n, tx_bytes)
         //   call(blockchain_handle, "query_ui"_n, {service, path, query})
         //
         // For now, this module serves as a compilation test to verify
         // the guest build pipeline works for multi-module projects.

         http_respond_str(&conn, 200, "text/plain",
                          "bc_connection: Phase 2 stub");
      }

      tcp_close(&conn);
   }
};

PSIO_MODULE(bc_connection_impl, handle_connection)
