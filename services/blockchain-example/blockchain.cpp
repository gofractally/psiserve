// blockchain.cpp — Blockchain orchestrator for the PoC.
//
// Phase 1 (sequential): handles HTTP directly in the accept loop,
// dispatches token actions inline, produces blocks on a timer.
// Phase 2: uses async_call for listener fiber + bc_connection.
//
// This is the module loaded by psiserve-cli. fd 0 = listen socket.

#include "types.hpp"
#include "db_imports.hpp"

#include <psi/host.h>
#include <psi/tcp.h>
#include <psi/http.h>

#include <psio/fracpack.hpp>
#include <psio/to_json.hpp>
#include <psio/from_json.hpp>

#include "token_ui_html.h"

static uint64_t    g_block_num    = 0;
static uint64_t    g_last_block_ns = 0;
static const int64_t BLOCK_INTERVAL_NS = 3000000000LL;  // 3 seconds

static uint32_t g_db_handle = 0;

static void seed_database()
{
   using namespace psi::db;

   auto tx = db::db_start_write(psio::borrow<database>{g_db_handle});
   auto tx_h = tx.handle;

   // Check if balances table already has the bank account
   auto tbl_res = db::tx_open_table(psio::borrow<transaction>{tx_h}, "balances");
   if (tbl_res)
   {
      auto bank_check = db::tbl_get(
         psio::borrow<table>{tbl_res->handle},
         db::make_bytes("bank"));
      if (bank_check)
      {
         db::tbl_drop(psio::own<table>{tbl_res->handle});
         db::tx_abort(psio::borrow<transaction>{tx_h});
         db::tx_drop(psio::own<transaction>{tx_h});
         return;
      }
      db::tbl_drop(psio::own<table>{tbl_res->handle});
      db::tx_abort(psio::borrow<transaction>{tx_h});
      db::tx_drop(psio::own<transaction>{tx_h});
      tx = db::db_start_write(psio::borrow<database>{g_db_handle});
      tx_h = tx.handle;
   }

   // Create balances table with bank account
   auto bal_tbl = db::tx_create_table(psio::borrow<transaction>{tx_h}, "balances");
   if (!bal_tbl)
   {
      db::tx_drop(psio::own<transaction>{tx_h});
      return;
   }

   uint64_t initial_balance = 1000000;
   psi::db::bytes bal_bytes(sizeof(initial_balance));
   __builtin_memcpy(bal_bytes.data(), &initial_balance, sizeof(initial_balance));

   db::tbl_upsert(psio::borrow<table>{bal_tbl->handle},
                   db::make_bytes("bank"), bal_bytes);
   db::tbl_drop(psio::own<table>{bal_tbl->handle});

   // Create blocks table
   db::tx_create_table(psio::borrow<transaction>{tx_h}, "blocks");

   db::tx_commit(psio::borrow<transaction>{tx_h});
   db::tx_drop(psio::own<transaction>{tx_h});
}

static void produce_block()
{
   g_block_num++;
   g_last_block_ns = psi_clock(PSI_CLOCK_MONOTONIC);
}

static bool url_starts_with(const char* url, int url_len, const char* prefix)
{
   int plen = 0;
   while (prefix[plen]) plen++;
   if (url_len < plen) return false;
   for (int i = 0; i < plen; i++)
      if (url[i] != prefix[i]) return false;
   return true;
}

static void handle_request(tcp_conn* conn, http_request* req)
{
   int path_len;
   const char* path = http_path(req, &path_len);

   if (http_method_is(req, "GET") &&
       url_starts_with(path, path_len, "/api/balance"))
   {
      // Parse ?account=NAME from URL
      const char* qmark = nullptr;
      for (int i = 0; i < path_len; i++)
      {
         if (path[i] == '?') { qmark = path + i + 1; break; }
      }

      std::string_view account = "bank";
      if (qmark)
      {
         int qlen = path_len - (int)(qmark - path);
         int vlen;
         const char* val = http_query_param(qmark, qlen, "account", 7, &vlen);
         if (val) account = std::string_view(val, vlen);
      }

      // Read balance from DB
      using namespace psi::db;
      auto tx = db::db_start_read(psio::borrow<database>{g_db_handle}, 0);
      auto tbl = db::tx_open_table(psio::borrow<transaction>{tx.handle}, "balances");

      uint64_t balance = 0;
      bool found = false;
      if (tbl)
      {
         auto val = db::tbl_get(psio::borrow<table>{tbl->handle},
                                db::make_bytes(account));
         if (val && val->size() >= sizeof(uint64_t))
         {
            __builtin_memcpy(&balance, val->data(), sizeof(uint64_t));
            found = true;
         }
         db::tbl_drop(psio::own<table>{tbl->handle});
      }
      db::tx_drop(psio::own<transaction>{tx.handle});

      char json[256];
      int jlen;
      if (found)
      {
         jlen = 0;
         const char* pre = "{\"account\":\"";
         for (int i = 0; pre[i]; i++) json[jlen++] = pre[i];
         for (int i = 0; i < (int)account.size() && jlen < 200; i++)
            json[jlen++] = account[i];
         const char* mid = "\",\"balance\":";
         for (int i = 0; mid[i]; i++) json[jlen++] = mid[i];

         char digits[20];
         int dlen = 0;
         uint64_t tmp = balance;
         if (tmp == 0) digits[dlen++] = '0';
         else while (tmp > 0) { digits[dlen++] = '0' + (tmp % 10); tmp /= 10; }
         for (int i = dlen - 1; i >= 0; i--) json[jlen++] = digits[i];
         json[jlen++] = '}';
      }
      else
      {
         const char* nf = "{\"account\":\"not_found\",\"balance\":0}";
         jlen = 0;
         for (int i = 0; nf[i]; i++) json[jlen++] = nf[i];
      }

      http_respond(conn, 200, "application/json", json, jlen);
      return;
   }

   if (http_method_is(req, "POST") &&
       url_starts_with(path, path_len, "/api/transfer"))
   {
      // Read POST body (after headers)
      int body_offset = req->raw_len;
      int cl_len;
      const char* cl_val = http_find_header(req, "Content-Length", 14, &cl_len);
      int content_length = 0;
      if (cl_val)
         for (int i = 0; i < cl_len; i++)
            content_length = content_length * 10 + (cl_val[i] - '0');

      // The body may already be in the header buffer past raw_len
      // For simplicity, read the body separately
      char body[1024] = {};
      int body_len = 0;
      if (content_length > 0 && content_length < (int)sizeof(body))
      {
         // Check if body is already in the header buffer
         // (http_read_request may have read past the headers)
         body_len = tcp_read_all(conn, body, content_length);
      }

      // Parse JSON body: {"from":"X","to":"Y","amount":N}
      // Minimal manual parser for PoC
      std::string_view bv(body, body_len > 0 ? body_len : 0);
      std::string from_str, to_str;
      uint64_t amount = 0;

      auto extract = [&](const char* key, std::string& out) {
         auto klen = __builtin_strlen(key);
         auto pos = bv.find(key);
         if (pos == std::string_view::npos) return;
         pos += klen;
         while (pos < bv.size() && (bv[pos] == '"' || bv[pos] == ':' || bv[pos] == ' '))
            pos++;
         auto end = bv.find('"', pos);
         if (end != std::string_view::npos)
            out = std::string(bv.substr(pos, end - pos));
      };

      extract("from", from_str);
      extract("to", to_str);

      auto amt_pos = bv.find("amount");
      if (amt_pos != std::string_view::npos)
      {
         amt_pos += 6;
         while (amt_pos < bv.size() && (bv[amt_pos] < '0' || bv[amt_pos] > '9'))
            amt_pos++;
         while (amt_pos < bv.size() && bv[amt_pos] >= '0' && bv[amt_pos] <= '9')
         {
            amount = amount * 10 + (bv[amt_pos] - '0');
            amt_pos++;
         }
      }

      // Execute transfer directly (inline token logic for Phase 1)
      using namespace psi::db;
      auto tx = db::db_start_write(psio::borrow<database>{g_db_handle});
      auto tx_h = tx.handle;
      auto tbl_res = db::tx_open_table(psio::borrow<transaction>{tx_h}, "balances");

      const char* result_json;
      if (!tbl_res)
      {
         db::tx_abort(psio::borrow<transaction>{tx_h});
         db::tx_drop(psio::own<transaction>{tx_h});
         result_json = "{\"success\":false,\"message\":\"balances table not found\"}";
      }
      else
      {
         auto tbl_h = tbl_res->handle;

         auto from_bal_r = db::tbl_get(psio::borrow<table>{tbl_h},
                                       db::make_bytes(from_str));
         if (!from_bal_r)
         {
            db::tbl_drop(psio::own<table>{tbl_h});
            db::tx_abort(psio::borrow<transaction>{tx_h});
            db::tx_drop(psio::own<transaction>{tx_h});
            result_json = "{\"success\":false,\"message\":\"sender account not found\"}";
         }
         else
         {
            uint64_t from_bal = 0;
            __builtin_memcpy(&from_bal, from_bal_r->data(),
                             from_bal_r->size() < 8 ? from_bal_r->size() : 8);

            if (from_bal < amount)
            {
               db::tbl_drop(psio::own<table>{tbl_h});
               db::tx_abort(psio::borrow<transaction>{tx_h});
               db::tx_drop(psio::own<transaction>{tx_h});
               result_json = "{\"success\":false,\"message\":\"insufficient funds\"}";
            }
            else
            {
               from_bal -= amount;
               db::tbl_upsert(psio::borrow<table>{tbl_h},
                              db::make_bytes(from_str),
                              db::make_bytes(std::string_view(
                                 reinterpret_cast<const char*>(&from_bal), 8)));

               uint64_t to_bal = 0;
               auto to_bal_r = db::tbl_get(psio::borrow<table>{tbl_h},
                                           db::make_bytes(to_str));
               if (to_bal_r && to_bal_r->size() >= 8)
                  __builtin_memcpy(&to_bal, to_bal_r->data(), 8);

               to_bal += amount;
               db::tbl_upsert(psio::borrow<table>{tbl_h},
                              db::make_bytes(to_str),
                              db::make_bytes(std::string_view(
                                 reinterpret_cast<const char*>(&to_bal), 8)));

               db::tbl_drop(psio::own<table>{tbl_h});
               db::tx_commit(psio::borrow<transaction>{tx_h});
               db::tx_drop(psio::own<transaction>{tx_h});
               result_json = "{\"success\":true,\"message\":\"transfer complete\"}";
            }
         }
      }

      int rlen = 0;
      while (result_json[rlen]) rlen++;
      http_respond(conn, 200, "application/json", result_json, rlen);
      return;
   }

   if (http_method_is(req, "GET") &&
       url_starts_with(path, path_len, "/api/blocks"))
   {
      // Return current block info as JSON array
      char json[256];
      int jlen = 0;
      json[jlen++] = '[';
      if (g_block_num > 0)
      {
         const char* pre = "{\"number\":";
         for (int i = 0; pre[i]; i++) json[jlen++] = pre[i];

         char digits[20];
         int dlen = 0;
         uint64_t tmp = g_block_num;
         if (tmp == 0) digits[dlen++] = '0';
         else while (tmp > 0) { digits[dlen++] = '0' + (tmp % 10); tmp /= 10; }
         for (int i = dlen - 1; i >= 0; i--) json[jlen++] = digits[i];

         const char* mid = ",\"timestamp_ns\":";
         for (int i = 0; mid[i]; i++) json[jlen++] = mid[i];

         dlen = 0;
         tmp = g_last_block_ns;
         if (tmp == 0) digits[dlen++] = '0';
         else while (tmp > 0) { digits[dlen++] = '0' + (tmp % 10); tmp /= 10; }
         for (int i = dlen - 1; i >= 0; i--) json[jlen++] = digits[i];

         const char* suf = ",\"tx_count\":0}";
         for (int i = 0; suf[i]; i++) json[jlen++] = suf[i];
      }
      json[jlen++] = ']';

      http_respond(conn, 200, "application/json", json, jlen);
      return;
   }

   if (http_method_is(req, "GET") &&
       (path_len == 1 && path[0] == '/'))
   {
      http_respond(conn, 200, "text/html; charset=utf-8",
                   TOKEN_UI_HTML, TOKEN_UI_HTML_LEN);
      return;
   }

   http_respond_str(conn, 404, "text/plain", "Not Found");
}

extern "C" [[clang::export_name("_start")]]
void _start(void)
{
   // Open database
   auto db_res = db::store_open("blockchain");
   if (!db_res) return;
   g_db_handle = db_res->handle;

   seed_database();
   g_last_block_ns = psi_clock(PSI_CLOCK_MONOTONIC);

   for (;;)
   {
      // Check if it's time to produce a block
      int64_t now = psi_clock(PSI_CLOCK_MONOTONIC);
      if (now - g_last_block_ns >= BLOCK_INTERVAL_NS)
         produce_block();

      // Accept a connection (blocks until one arrives or shutdown)
      tcp_conn conn = tcp_accept(0);
      if (!tcp_is_open(&conn))
         break;

      char buf[8192];
      http_request req;
      int r = http_read_request(&conn, &req, buf, sizeof(buf));
      if (r > 0)
         handle_request(&conn, &req);

      tcp_close(&conn);
   }

   db::db_drop(psio::own<psi::db::database>{g_db_handle});
}
