#pragma once

// psi/blockchain.hpp — contract-facing view of the blockchain DB.
//
// WIT identity: psi:blockchain/api@0.1.0
//
// The orchestrator (blockchain.wasm) opens a write transaction for
// each action it dispatches and publishes that transaction as the
// "current" one. Services then obtain a `tx_view` via `current_tx()`
// and open tables within it:
//
//     auto tbl = psi::blockchain::current_tx().get_table("balances");
//     if (!tbl)                    // table ensure failed; bail out
//         return;
//     if (auto v = tbl.get(key))
//         ...
//     tbl.upsert(key, value);
//     // tbl destructor runs table_drop → write_back_table.
//
// Everything here is intentionally a view / RAII wrapper so handle
// ownership is obvious at the call site. The host still owns the
// compiled WIT resources; we just make sure the guest never leaks one
// back through a return value.

#include <psi/db.hpp>
#include <psio/reflect.hpp>
#include <psio/structural.hpp>
#include <psio/wit_resource.hpp>

#include <cstdint>
#include <optional>
#include <string_view>

PSIO_PACKAGE(psi, blockchain, "0.1.0");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(psi_blockchain)

namespace psi::blockchain
{

   // ── table_view ─────────────────────────────────────────────────────
   //
   // RAII wrapper around an open table handle belonging to the current
   // write transaction. Destruction runs `table_drop` on the host; for
   // a table we wrote to, the host's write_back_table pushes the new
   // subtree root into the parent transaction before commit sees it.
   //
   // Movable, not copyable. Callers keep a `table_view` for the local
   // scope of one service operation and let the destructor clean up.

   class table_view
   {
      psio::own<psi::db::table> _tbl;

     public:
      table_view() noexcept
         : _tbl{psio::own<psi::db::table>::null_handle}
      {
      }

      explicit table_view(psio::own<psi::db::table>&& t) noexcept
         : _tbl(std::move(t))
      {
      }

      table_view(table_view&&) noexcept            = default;
      table_view& operator=(table_view&&) noexcept = default;

      table_view(const table_view&)            = delete;
      table_view& operator=(const table_view&) = delete;

      explicit operator bool() const noexcept
      {
         return _tbl.handle != psio::own<psi::db::table>::null_handle;
      }

      uint32_t handle() const noexcept { return _tbl.handle; }

      /// Read `key`. Returns nullopt if the key is absent, the table
      /// is invalid, or the host reported an error.
      std::optional<psi::db::bytes> get(psi::db::bytes key) const;

      /// Insert or replace `key` → `value`. Silent no-op on an invalid
      /// table (the caller already knows from `operator bool`).
      void upsert(psi::db::bytes key, psi::db::bytes value);

      /// Remove `key`. Returns true if the key was present and removed.
      bool remove(psi::db::bytes key);
   };

   // ── tx_view ────────────────────────────────────────────────────────
   //
   // Non-owning reference to the currently-active transaction. Services
   // never construct one directly — they call `current_tx()` below.

   class tx_view
   {
      uint32_t _tx_h;

     public:
      explicit tx_view(uint32_t tx_h) noexcept : _tx_h(tx_h) {}

      /// True if an orchestrator has actually installed a transaction.
      explicit operator bool() const noexcept { return _tx_h != 0; }

      uint32_t handle() const noexcept { return _tx_h; }

      /// Open `name`, creating the table if it does not yet exist.
      /// Returns an invalid `table_view` if neither operation succeeds.
      table_view get_table(std::string_view name) const;
   };

   // ── Active-transaction accessor ────────────────────────────────────
   //
   // Defined inline in psi/blockchain_guest.hpp. The declaration lives
   // in the interface header so services that depend only on the
   // interface shape can still use `current_tx()` in signatures etc.
   namespace detail
   {
      uint32_t active_tx_handle() noexcept;
   }

   /// Obtain a view onto the orchestrator-installed write transaction.
   /// Calling this outside of an action dispatch returns an invalid
   /// `tx_view` (operator bool == false).
   inline tx_view current_tx() noexcept
   {
      return tx_view{detail::active_tx_handle()};
   }

}  // namespace psi::blockchain

// ── interface_info specialization ─────────────────────────────────────
namespace psio::detail
{
   // Placeholder — the blockchain "interface" for cross-instance calls
   // lives as a set of raw imports at `psi:db/*` for now. When we wire
   // the orchestrator as a separately hashed service, this name will be
   // used by psizam::runtime::bind.
   struct psi_blockchain_api_tag
   {
   };
   template <>
   struct interface_info<psi_blockchain_api_tag>
   {
      static constexpr ::psio::FixedString name = "psi:blockchain/api";
   };
}  // namespace psio::detail
