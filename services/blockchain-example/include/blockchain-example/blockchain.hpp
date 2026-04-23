#pragma once

// blockchain-example/blockchain.hpp — the chain's public API.
//
// This is the seam between "the chain" (block producer, transaction
// lifecycle, smart-contract dispatch) and everything else. In Phase 1
// all four services are linked into a single wasm and these functions
// are direct C++ calls. In Phase 2 the same signatures are implemented
// by a cross-instance dispatch into blockchain.wasm; nothing outside
// blockchain.cpp changes.
//
// What belongs here:
//   * init_storage / maybe_produce_block / head_block_num
//   * strongly-typed facades for the single service we ship today
//     (token — transfer, balance)
//   * read helpers for chain metadata (recent_blocks)
//
// What does NOT belong here (intentionally):
//   * anything HTTP, TCP, or HTML
//   * the token schema or token encodings (see
//     <blockchain-example/token.hpp>)

#include <psio/name.hpp>

#include <cstdint>
#include <string_view>
#include <vector>

namespace blockchain_example::chain
{

   // ── Lifecycle ──────────────────────────────────────────────────────

   /// Open the on-disk store, run genesis if the chain is new, and
   /// leave the state ready for block production and action dispatch.
   /// Returns false if the backing store cannot be opened.
   bool init();

   /// Tear down the on-disk store handle. Idempotent.
   void shutdown();

   // ── Block production ───────────────────────────────────────────────
   //
   // The chain produces one block every BLOCK_INTERVAL_NS nanoseconds.
   // The orchestrator (bc_connection.cpp in Phase 1, blockchain.wasm's
   // own producer fiber in Phase 2) calls maybe_produce_block() on a
   // schedule and the chain decides whether a new block is due.

   constexpr int64_t BLOCK_INTERVAL_NS = 3'000'000'000LL;  // 3s

   /// If enough time has elapsed since the last block, produce a new
   /// one. No-op otherwise.
   void maybe_produce_block();

   uint64_t head_block_num();
   uint64_t head_block_ns();

   struct BlockHeader
   {
      uint64_t number       = 0;
      uint64_t timestamp_ns = 0;
      uint32_t tx_count     = 0;
   };

   /// Return up to `limit` most-recent block headers, newest first.
   /// Reads are snapshot-consistent against a single transaction.
   std::vector<BlockHeader> recent_blocks(int limit);

   // ── Action dispatch ────────────────────────────────────────────────
   //
   // Phase 1 exposes one typed entry per contract method we support.
   // The implementation wraps start_write → service method → commit
   // internally so callers never see a transaction handle.
   //
   // ISSUE phase-2-generic-dispatch: these typed facades will be
   // replaced by `push_action(service, method, fracpack_args)` once
   // bc_connection fracpacks its HTTP inputs directly. The typed
   // helpers keep existing until we've validated the generic path.

   enum class ActionResult
   {
      Ok,
      Aborted,  // transaction rolled back (e.g. trap in the service)
   };

   ActionResult transfer(psio::name_id    from,
                         psio::name_id    to,
                         uint64_t         amount,
                         std::string_view memo);

   // ── Read-only queries ──────────────────────────────────────────────

   uint64_t balance(psio::name_id account);

}  // namespace blockchain_example::chain
