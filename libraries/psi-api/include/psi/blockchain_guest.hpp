#pragma once

// psi/blockchain_guest.hpp — inline guest-side implementation of
// <psi/blockchain.hpp>.
//
// Split from the interface header so that:
//   * host code that only needs the type shape can include the
//     interface without pulling in the raw import stubs, and
//   * the inline definitions here are emitted in exactly the
//     translation units that wire against the raw imports (the
//     guest wasm modules).
//
// The orchestrator (blockchain.wasm) is responsible for installing
// the active transaction before dispatching an action:
//
//     auto tx = db::db_start_write(db_handle);
//     {
//        psi::blockchain::detail::active_tx_scope guard(tx.handle);
//        TokenService{}.transfer(...);
//     }
//     db::tx_commit(tx);
//
// `active_tx_scope` is a minimal nestable RAII helper so handlers
// that call into sub-services (not yet wired) or that need to
// temporarily switch transactions can do so without threading a
// handle through every call.

#include <psi/blockchain.hpp>
#include <psi/db_imports.hpp>

namespace psi::blockchain
{

   namespace detail
   {
      // ISSUE phase-2-per-instance-tx: once the runtime dispatches
      // actions across instance boundaries, every sub-instance will
      // need its own active_tx_handle. A thread-local works for
      // Phase 1 (single-worker, single-wasm) but becomes wrong the
      // moment token.wasm runs in its own instance. The fix is to
      // plumb the tx handle through the dispatch ABI (or a bound
      // import) instead of storing it on the guest side.
      inline thread_local uint32_t tl_active_tx_handle = 0;

      inline uint32_t active_tx_handle() noexcept
      {
         return tl_active_tx_handle;
      }

      /// RAII guard: publishes `tx_h` as the active transaction for
      /// the duration of its scope and restores the previous value on
      /// exit. Safe to nest.
      class active_tx_scope
      {
         uint32_t _prev;

        public:
         explicit active_tx_scope(uint32_t tx_h) noexcept
            : _prev(tl_active_tx_handle)
         {
            tl_active_tx_handle = tx_h;
         }

         ~active_tx_scope() noexcept { tl_active_tx_handle = _prev; }

         active_tx_scope(const active_tx_scope&)            = delete;
         active_tx_scope& operator=(const active_tx_scope&) = delete;
      };
   }  // namespace detail

   // ── tx_view ────────────────────────────────────────────────────────

   inline table_view tx_view::get_table(std::string_view name) const
   {
      if (_tx_h == 0)
         return table_view{};

      // tx_open_table / tx_create_table return
      // std::expected<psio::own<table>, error>. We move the own<>
      // straight into the view so no table_drop fires between the
      // host call and the caller seeing the handle.
      std::string n(name);
      auto opened = ::db::tx_open_table(
         psio::borrow<psi::db::transaction>{_tx_h}, n);
      if (opened)
         return table_view{std::move(*opened)};

      auto created = ::db::tx_create_table(
         psio::borrow<psi::db::transaction>{_tx_h}, n);
      if (created)
         return table_view{std::move(*created)};

      return table_view{};
   }

   // ── table_view ─────────────────────────────────────────────────────

   inline std::optional<psi::db::bytes>
   table_view::get(psi::db::bytes key) const
   {
      if (_tbl.handle == psio::own<psi::db::table>::null_handle)
         return std::nullopt;
      auto r = ::db::tbl_get(
         psio::borrow<psi::db::table>{_tbl.handle}, std::move(key));
      if (!r)
         return std::nullopt;
      return std::move(*r);
   }

   inline void table_view::upsert(psi::db::bytes key,
                                   psi::db::bytes value)
   {
      if (_tbl.handle == psio::own<psi::db::table>::null_handle)
         return;
      ::db::tbl_upsert(psio::borrow<psi::db::table>{_tbl.handle},
                       std::move(key), std::move(value));
   }

   inline bool table_view::remove(psi::db::bytes key)
   {
      if (_tbl.handle == psio::own<psi::db::table>::null_handle)
         return false;
      auto r = ::db::tbl_remove(
         psio::borrow<psi::db::table>{_tbl.handle}, std::move(key));
      return r && *r;
   }

}  // namespace psi::blockchain
