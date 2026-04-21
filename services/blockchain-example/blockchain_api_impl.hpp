#pragma once

// blockchain_api_impl.hpp — Guest-side implementation of psi::blockchain::api.
//
// Wires the contract-level API (get_database, get_table, get, upsert)
// to the raw db_imports.hpp stubs. This is the glue layer that will
// be replaced by auto-generated proxy imports when PSIO_IMPORT_IMPL
// supports namespaced resource types.

#include <psi/blockchain.hpp>
#include "db_imports.hpp"

namespace psi::blockchain
{
   inline db_handle api::get_database()
   {
      auto db_res = ::db::store_open("blockchain");
      uint32_t db_h = db_res ? db_res->handle : 0;

      auto tx = ::db::db_start_write(psio::borrow<psi::db::database>{db_h});
      return db_handle{db_h, tx.handle};
   }

   inline std::uint64_t api::get_head_block_num()
   {
      return 0;
   }

   inline table_handle db_handle::get_table(std::string name)
   {
      auto tbl = ::db::tx_open_table(
         psio::borrow<psi::db::transaction>{_tx_h}, name);
      if (!tbl)
         tbl = ::db::tx_create_table(
            psio::borrow<psi::db::transaction>{_tx_h}, name);
      return table_handle{tbl ? tbl->handle : 0};
   }

   inline std::expected<psi::db::bytes, psi::db::error>
   table_handle::get(psi::db::bytes key)
   {
      return ::db::tbl_get(psio::borrow<psi::db::table>{_h}, key);
   }

   inline std::expected<void, psi::db::error>
   table_handle::upsert(psi::db::bytes key, psi::db::bytes value)
   {
      return ::db::tbl_upsert(psio::borrow<psi::db::table>{_h}, key, value);
   }

   inline std::expected<bool, psi::db::error>
   table_handle::remove(psi::db::bytes key)
   {
      return ::db::tbl_remove(psio::borrow<psi::db::table>{_h}, key);
   }

}  // namespace psi::blockchain
