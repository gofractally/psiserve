// blockchain.cpp — Chain state, transaction lifecycle, block producer.
//
// Implements the API declared in <blockchain-example/blockchain.hpp>.
// Owns the on-disk store, the write-tx helper, and the `blocks` table.
// Delegates all contract-level logic to the relevant service (today
// only TokenService) running in-process in Phase 1.
//
// This file does not know about HTTP, TCP, HTML, JSON, or the UI.
// Those concerns live in bc_connection.cpp and token_ui.cpp.

#include <blockchain-example/blockchain.hpp>
#include <blockchain-example/token.hpp>

#include <psi/blockchain_guest.hpp>
#include <psi/db_imports.hpp>
#include <psi/host.h>
#include <psi/log.hpp>

#include <cstdint>
#include <cstring>
#include <string_view>

namespace blockchain_example::chain
{

   // ─── Process-wide state ────────────────────────────────────────────

   namespace
   {
      uint32_t g_db_handle     = 0;
      uint64_t g_block_num     = 0;
      uint64_t g_last_block_ns = 0;

      // ── Binary helpers ──────────────────────────────────────────────

      psi::db::bytes u64_be(uint64_t v)
      {
         // Big-endian so cursors scan blocks in numeric order.
         psi::db::bytes out(8);
         for (int i = 0; i < 8; ++i)
            out[i] = static_cast<uint8_t>(v >> (56 - 8 * i));
         return out;
      }

      // ── Transaction lifecycle helper ────────────────────────────────
      //
      // Runs `action` inside a fresh write transaction, publishing the
      // transaction handle via psi::blockchain::current_tx() for the
      // duration of the call. Commits if `action` returns true,
      // aborts otherwise. This is the only place in Phase 1 that opens
      // or ends a transaction — services never touch the lifecycle
      // themselves.

      template <typename Action>
      bool with_write_tx(Action&& action)
      {
         auto tx   = db::db_start_write(
            psio::borrow<psi::db::database>{g_db_handle});
         auto tx_h = tx.handle;

         bool ok;
         {
            psi::blockchain::detail::active_tx_scope guard(tx_h);
            ok = action();
         }

         if (ok)
            db::tx_commit(psio::borrow<psi::db::transaction>{tx_h});
         else
            db::tx_abort(psio::borrow<psi::db::transaction>{tx_h});
         return ok;
      }

      // ── Genesis ─────────────────────────────────────────────────────

      void seed_database()
      {
         with_write_tx([]() {
            // ISSUE psitri-subtree-persist: psitri skips write-back on
            // subtrees that never received a row, so a freshly created
            // empty table is gone at commit. We drop a sentinel row at
            // key=0 into `blocks` so subsequent produce_block calls
            // take the fast tx_open_table path instead of having to
            // tx_create_table every time.
            auto blocks = psi::blockchain::current_tx().get_table("blocks");
            if (blocks && !blocks.get(u64_be(0)))
               blocks.upsert(u64_be(0), psi::db::bytes(12, 0));

            // Hand genesis off to the token service — the contract
            // owns the balances schema, not the chain.
            TokenService{}.init();
            return true;
         });
      }
   }  // namespace

   // ─── Lifecycle ─────────────────────────────────────────────────────

   bool init()
   {
      auto db_res = db::store_open("blockchain");
      if (!db_res)
      {
         psi::log::error("chain::init: store_open failed");
         return false;
      }
      g_db_handle     = db_res->handle;
      g_last_block_ns = psi_clock(PSI_CLOCK_MONOTONIC);

      seed_database();
      psi::log::info("chain::init complete; store handle={}", g_db_handle);
      return true;
   }

   void shutdown()
   {
      if (g_db_handle != 0)
      {
         db::db_drop(psio::own<psi::db::database>{g_db_handle});
         g_db_handle = 0;
      }
   }

   // ─── Block production ──────────────────────────────────────────────

   void maybe_produce_block()
   {
      int64_t now = psi_clock(PSI_CLOCK_MONOTONIC);
      if (now - static_cast<int64_t>(g_last_block_ns) < BLOCK_INTERVAL_NS)
         return;

      g_block_num    += 1;
      g_last_block_ns = static_cast<uint64_t>(now);

      bool committed = with_write_tx([]() {
         auto tbl = psi::blockchain::current_tx().get_table("blocks");
         if (!tbl)
            return false;

         // value layout: timestamp_ns (u64 LE) | tx_count (u32 LE)
         psi::db::bytes value(12);
         std::memcpy(value.data(),     &g_last_block_ns, 8);
         uint32_t txc = 0;
         std::memcpy(value.data() + 8, &txc,             4);

         tbl.upsert(u64_be(g_block_num), std::move(value));
         return true;
      });

      // ISSUE psitri-write-cursor-budget: psitri currently drops new
      // block commits after roughly three successful produce_block
      // cycles on a single write-session (tracked upstream). We log
      // the abort and keep running — /api/blocks will reflect only
      // the persisted subset until psitri is fixed.
      if (!committed)
         psi::log::warn("produce_block: commit aborted for #{}",
                        g_block_num);
   }

   uint64_t head_block_num() { return g_block_num; }
   uint64_t head_block_ns()  { return g_last_block_ns; }

   std::vector<BlockHeader> recent_blocks(int limit)
   {
      if (limit <= 0) limit = 10;

      std::vector<BlockHeader> out;
      out.reserve(limit);

      with_write_tx([&]() {
         auto tbl = psi::blockchain::current_tx().get_table("blocks");
         if (!tbl)
            return false;

         uint64_t start = g_block_num;
         if (start == 0) return false;
         uint64_t end = start > uint64_t(limit) - 1
                           ? start - uint64_t(limit) + 1
                           : 1;

         for (uint64_t bn = start;
              bn >= end && out.size() < static_cast<size_t>(limit);
              --bn)
         {
            auto v = tbl.get(u64_be(bn));
            if (v && v->size() >= 12)
            {
               BlockHeader h;
               h.number = bn;
               std::memcpy(&h.timestamp_ns, v->data(),     8);
               std::memcpy(&h.tx_count,     v->data() + 8, 4);
               out.push_back(h);
            }
            if (bn == 1) break;
         }
         return false;  // read-only — roll back to release the lock.
      });

      return out;
   }

   // ─── Action dispatch ───────────────────────────────────────────────
   //
   // These are the Phase-1 typed facades. Each wraps the call in a
   // write transaction and invokes TokenService in-process. Phase 2
   // replaces the body of each with a cross-instance dispatch; the
   // signature, and everything that calls it, stays put.

   ActionResult transfer(psio::name_id    from,
                         psio::name_id    to,
                         uint64_t         amount,
                         std::string_view memo)
   {
      // ISSUE psibase-check-surface-errors: TokenService::transfer
      // calls psibase::check which traps on missing sender /
      // insufficient funds. Until we wrap the service invocation with
      // a trap catch, any logical failure unwinds the whole wasm —
      // this function only distinguishes "all good" from "tx rolled
      // back by the host" today.
      bool committed = with_write_tx([&]() {
         TokenService{}.transfer(from, to, amount, memo);
         return true;
      });
      return committed ? ActionResult::Ok : ActionResult::Aborted;
   }

   uint64_t balance(psio::name_id account)
   {
      uint64_t bal = 0;
      with_write_tx([&]() {
         bal = TokenService{}.balance(account);
         return false;  // read-only: roll back.
      });
      return bal;
   }

}  // namespace blockchain_example::chain
