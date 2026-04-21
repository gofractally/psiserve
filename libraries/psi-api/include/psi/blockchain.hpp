#pragma once

// psi/blockchain.hpp — blockchain_api interface declaration.
//
// The interface that blockchain.wasm exports to smart contracts.
// Contracts import it; blockchain.wasm implements it.
// runtime::bind wires the connection at instantiation time.
//
// Contract-level view: get_database() returns a scoped subtree,
// get_table() opens a table within it. No explicit transactions —
// blockchain.wasm manages the tx lifecycle (commit on success,
// abort on trap).

#include <psi/db.hpp>
#include <psio/name.hpp>
#include <psio/reflect.hpp>
#include <psio/wit_resource.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <vector>

namespace psi::blockchain
{
   // ── Contract-level table handle ──────────────────────────────────
   //
   // Wraps a psi::db::table handle with the simplified contract view:
   // get/upsert/remove only, no transaction management.

   class table_handle
   {
      uint32_t _h;
   public:
      explicit table_handle(uint32_t h) : _h(h) {}

      std::expected<psi::db::bytes, psi::db::error>
      get(psi::db::bytes key);

      std::expected<void, psi::db::error>
      upsert(psi::db::bytes key, psi::db::bytes value);

      std::expected<bool, psi::db::error>
      remove(psi::db::bytes key);

      uint32_t handle() const { return _h; }
   };

   // ── Contract-level database handle ───────────────────────────────
   //
   // Wraps the scoped subtree root. get_table() opens or creates
   // a table within the implicit transaction.

   class db_handle
   {
      uint32_t _h;
      uint32_t _tx_h;
   public:
      explicit db_handle(uint32_t db, uint32_t tx) : _h(db), _tx_h(tx) {}

      table_handle get_table(std::string name);

      uint32_t handle() const { return _h; }
   };

   // ── blockchain API ───────────────────────────────────────────────

   struct api
   {
      static db_handle get_database();

      static std::uint64_t get_head_block_num();
   };

}  // namespace psi::blockchain
