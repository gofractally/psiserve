// bc_connection.cpp — HTTP front end for the blockchain PoC.
//
// Phase 1 entry point. Owns the listen socket, the accept loop, and
// all HTTP parsing / response writing. Knows nothing about the chain
// schema or UI content — for every request it calls into
// <blockchain-example/blockchain.hpp> or
// <blockchain-example/token_ui.hpp> and ships the bytes back on the
// wire.
//
// Phase 2: this file runs in its own per-connection wasm instance
// spawned on thread::fresh and dispatches actions to blockchain.wasm
// via the runtime. The HTTP code below is unchanged at that point;
// only the direct function calls become runtime::call invocations.

#include <blockchain-example/blockchain.hpp>
#include <blockchain-example/token_ui.hpp>

#include <psi/host.h>
#include <psi/http.h>
#include <psi/log.hpp>
#include <psi/tcp.h>

#include <psio/name.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace chain  = blockchain_example::chain;
namespace tui    = blockchain_example::token_ui;

// ─── Small parsing helpers ───────────────────────────────────────────
//
// ISSUE fracpack-action-dispatch: Phase 2 replaces this JSON body
// parsing with fracpack'd actions built by the client. The typed
// chain::* facades will become push_action(...) taking opaque bytes,
// and the mapping from HTTP → action will move into a thin rule
// table here.

static bool url_starts_with(const char* url, int url_len, const char* pfx)
{
   int plen = 0; while (pfx[plen]) ++plen;
   if (url_len < plen) return false;
   for (int i = 0; i < plen; ++i)
      if (url[i] != pfx[i]) return false;
   return true;
}

static std::string_view query_param(const char* path, int path_len,
                                    const char* key, int key_len)
{
   for (int i = 0; i < path_len; ++i)
   {
      if (path[i] == '?')
      {
         int qlen = path_len - (i + 1);
         int vlen = 0;
         const char* v = http_query_param(path + i + 1, qlen,
                                          key, key_len, &vlen);
         return v ? std::string_view(v, vlen) : std::string_view{};
      }
   }
   return {};
}

static void extract_string_field(std::string_view body, const char* key,
                                  std::string&     out)
{
   int klen = 0; while (key[klen]) ++klen;
   auto p = body.find(key);
   if (p == std::string_view::npos) return;
   p += klen;
   while (p < body.size() &&
          (body[p] == '"' || body[p] == ':' || body[p] == ' '))
      ++p;
   auto end = body.find('"', p);
   if (end != std::string_view::npos)
      out.assign(body.substr(p, end - p));
}

static uint64_t extract_uint_field(std::string_view body, const char* key)
{
   int klen = 0; while (key[klen]) ++klen;
   auto p = body.find(key);
   if (p == std::string_view::npos) return 0;
   p += klen;
   while (p < body.size() && (body[p] < '0' || body[p] > '9'))
      ++p;
   uint64_t v = 0;
   while (p < body.size() && body[p] >= '0' && body[p] <= '9')
      v = v * 10 + (body[p++] - '0');
   return v;
}

// ─── Route handlers ──────────────────────────────────────────────────

static void handle_balance(tcp_conn* conn, const char* path, int path_len)
{
   auto account_str = query_param(path, path_len, "account", 7);
   if (account_str.empty()) account_str = "bank";

   psio::name_id account{account_str};
   uint64_t      bal = chain::balance(account);
   // In this PoC, "found" just means non-zero. A real build would
   // surface an explicit bit from chain::balance (or from a
   // balance_of that returns optional).
   bool found = bal > 0;

   auto json = tui::render_balance(account_str, bal, found);
   http_respond(conn, 200, "application/json",
                json.data(), static_cast<int>(json.size()));
}

static void handle_transfer(tcp_conn* conn, const http_request* req)
{
   char bodybuf[2048];
   int  body_len = http_read_body(conn, req, bodybuf, sizeof(bodybuf));
   std::string_view body(bodybuf, body_len > 0 ? body_len : 0);

   std::string from_str, to_str;
   extract_string_field(body, "from", from_str);
   extract_string_field(body, "to",   to_str);
   uint64_t amount = extract_uint_field(body, "amount");

   psio::name_id from{from_str};
   psio::name_id to  {to_str};

   psi::log::info("transfer {} -> {} amt={}", from_str, to_str, amount);

   auto result = chain::transfer(from, to, amount, std::string_view{});
   auto json   = tui::render_transfer_result(
      result == chain::ActionResult::Ok,
      result == chain::ActionResult::Ok ? "transfer complete"
                                        : "transfer aborted");

   http_respond(conn, 200, "application/json",
                json.data(), static_cast<int>(json.size()));
}

static void handle_blocks(tcp_conn* conn, const char* path, int path_len)
{
   int limit = 10;
   auto count_str = query_param(path, path_len, "count", 5);
   if (!count_str.empty())
   {
      int n = 0;
      for (char c : count_str)
         if (c >= '0' && c <= '9') n = n * 10 + (c - '0');
      if (n > 0 && n <= 200) limit = n;
   }

   auto blocks = chain::recent_blocks(limit);
   auto json   = tui::render_blocks(blocks);

   http_respond(conn, 200, "application/json",
                json.data(), static_cast<int>(json.size()));
}

static void handle_request(tcp_conn* conn, http_request* req)
{
   int         path_len;
   const char* path = http_path(req, &path_len);

   if (http_method_is(req, "GET") &&
       url_starts_with(path, path_len, "/api/balance"))
   {
      handle_balance(conn, path, path_len);
      return;
   }

   if (http_method_is(req, "POST") &&
       url_starts_with(path, path_len, "/api/transfer"))
   {
      handle_transfer(conn, req);
      return;
   }

   if (http_method_is(req, "GET") &&
       url_starts_with(path, path_len, "/api/blocks"))
   {
      handle_blocks(conn, path, path_len);
      return;
   }

   if (http_method_is(req, "GET") && path_len == 1 && path[0] == '/')
   {
      auto html = tui::index_html();
      http_respond(conn, 200, "text/html; charset=utf-8",
                   html.data(), static_cast<int>(html.size()));
      return;
   }

   http_respond_str(conn, 404, "text/plain", "Not Found");
}

// ─── Entry point ─────────────────────────────────────────────────────
//
// Phase 1: one fiber, sequential accept. Between accepts, we give the
// chain a chance to produce a block on schedule.
//
// ISSUE phase-2-listener-split: Phase 2 moves the accept loop into
// blockchain.wasm's listener fiber and instantiates bc_connection per
// connection on thread::fresh. The handler body below is unchanged at
// that point; it's literally called with `sock_fd` as its only input.

extern "C" [[clang::export_name("_start")]]
void _start(void)
{
   if (!chain::init())
   {
      psi::log::error("bc_connection: chain::init failed — exiting");
      return;
   }

   psi::log::info("bc_connection: entering accept loop on fd 0");

   for (;;)
   {
      chain::maybe_produce_block();

      tcp_conn conn = tcp_accept(0);
      if (!tcp_is_open(&conn))
      {
         psi::log::warn("bc_connection: tcp_accept(0) closed — exiting");
         break;
      }

      char         buf[8192];
      http_request req;
      int          r = http_read_request(&conn, &req, buf, sizeof(buf));
      if (r > 0)
         handle_request(&conn, &req);

      tcp_close(&conn);
   }

   chain::shutdown();
}
