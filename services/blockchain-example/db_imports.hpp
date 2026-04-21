#pragma once

// db_imports.hpp — Guest-side import stubs for psi:db/* interfaces.
//
// Reusable header for any WASM guest that needs database access.
// Provides typed wrappers around the 16-slot canonical-ABI raw imports
// that match what PSIO_HOST_MODULE registers in db_host.hpp.

#include <psi/db.hpp>
#include <psio/guest_alloc.hpp>
#include <psizam/component_proxy.hpp>

using psizam::flat_val;
using psizam::raw_import_fn;
using psizam::ImportProxy;

#define DB_RAW_IMPORT(MODULE, NAME)                                           \
   extern "C" [[clang::import_module(MODULE), clang::import_name(#NAME)]]     \
   flat_val _raw_##NAME(                                                       \
      flat_val, flat_val, flat_val, flat_val,                                   \
      flat_val, flat_val, flat_val, flat_val,                                   \
      flat_val, flat_val, flat_val, flat_val,                                   \
      flat_val, flat_val, flat_val, flat_val)

DB_RAW_IMPORT("psi:db/store",       open);
DB_RAW_IMPORT("psi:db/database",    start_write);
DB_RAW_IMPORT("psi:db/database",    start_read);
DB_RAW_IMPORT("psi:db/database",    database_drop);
DB_RAW_IMPORT("psi:db/transaction", create_table);
DB_RAW_IMPORT("psi:db/transaction", open_table);
DB_RAW_IMPORT("psi:db/transaction", commit);
DB_RAW_IMPORT("psi:db/transaction", abort);
DB_RAW_IMPORT("psi:db/transaction", list_tables);
DB_RAW_IMPORT("psi:db/transaction", transaction_drop);
DB_RAW_IMPORT("psi:db/table",       upsert);
DB_RAW_IMPORT("psi:db/table",       get);
DB_RAW_IMPORT("psi:db/table",       remove);
DB_RAW_IMPORT("psi:db/table",       open_cursor);
DB_RAW_IMPORT("psi:db/table",       get_stats);
DB_RAW_IMPORT("psi:db/table",       table_drop);
DB_RAW_IMPORT("psi:db/cursor",      seek);
DB_RAW_IMPORT("psi:db/cursor",      seek_first);
DB_RAW_IMPORT("psi:db/cursor",      next);
DB_RAW_IMPORT("psi:db/cursor",      on_row);
DB_RAW_IMPORT("psi:db/cursor",      key);
DB_RAW_IMPORT("psi:db/cursor",      value);
DB_RAW_IMPORT("psi:db/cursor",      cursor_drop);

#undef DB_RAW_IMPORT

namespace db
{
   using namespace psi::db;

   inline auto store_open(std::string_view name)
   {
      using fn = std::expected<psio::own<database>, error> (*)(std::string_view);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_open), name);
   }

   inline auto db_start_write(psio::borrow<database> self)
   {
      using fn = psio::own<transaction> (*)(psio::borrow<database>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_start_write), self);
   }

   inline auto db_start_read(psio::borrow<database> self, uint8_t mode)
   {
      using fn = psio::own<transaction> (*)(psio::borrow<database>, uint8_t);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_start_read), self, mode);
   }

   inline void db_drop(psio::own<database> self)
   {
      using fn = void (*)(psio::own<database>);
      ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_database_drop), self);
   }

   inline auto tx_create_table(psio::borrow<transaction> self, std::string_view name)
   {
      using fn = std::expected<psio::own<table>, error> (*)(
         psio::borrow<transaction>, std::string_view);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_create_table), self, name);
   }

   inline auto tx_open_table(psio::borrow<transaction> self, std::string_view name)
   {
      using fn = std::expected<psio::own<table>, error> (*)(
         psio::borrow<transaction>, std::string_view);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_open_table), self, name);
   }

   inline auto tx_commit(psio::borrow<transaction> self)
   {
      using fn = std::expected<void, error> (*)(psio::borrow<transaction>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_commit), self);
   }

   inline void tx_abort(psio::borrow<transaction> self)
   {
      using fn = void (*)(psio::borrow<transaction>);
      ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_abort), self);
   }

   inline auto tx_list_tables(psio::borrow<transaction> self)
   {
      using fn = std::vector<std::string> (*)(psio::borrow<transaction>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_list_tables), self);
   }

   inline void tx_drop(psio::own<transaction> self)
   {
      using fn = void (*)(psio::own<transaction>);
      ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_transaction_drop), self);
   }

   inline auto tbl_upsert(psio::borrow<table> self, bytes k, bytes v)
   {
      using fn = std::expected<void, error> (*)(psio::borrow<table>, bytes, bytes);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_upsert), self, k, v);
   }

   inline auto tbl_get(psio::borrow<table> self, bytes k,
                        uint32_t offset = 0,
                        std::optional<uint32_t> len = std::nullopt)
   {
      using fn = std::expected<bytes, error> (*)(
         psio::borrow<table>, bytes, uint32_t, std::optional<uint32_t>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_get), self, k, offset, len);
   }

   inline auto tbl_remove(psio::borrow<table> self, bytes k)
   {
      using fn = std::expected<bool, error> (*)(psio::borrow<table>, bytes);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_remove), self, k);
   }

   inline auto tbl_open_cursor(psio::borrow<table> self)
   {
      using fn = psio::own<cursor> (*)(psio::borrow<table>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_open_cursor), self);
   }

   inline auto tbl_get_stats(psio::borrow<table> self)
   {
      using fn = stats (*)(psio::borrow<table>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_get_stats), self);
   }

   inline void tbl_drop(psio::own<table> self)
   {
      using fn = void (*)(psio::own<table>);
      ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_table_drop), self);
   }

   inline auto cur_seek(psio::borrow<cursor> self, bytes k)
   {
      using fn = std::expected<bool, error> (*)(psio::borrow<cursor>, bytes);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_seek), self, k);
   }

   inline auto cur_seek_first(psio::borrow<cursor> self)
   {
      using fn = std::expected<bool, error> (*)(psio::borrow<cursor>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_seek_first), self);
   }

   inline auto cur_next(psio::borrow<cursor> self)
   {
      using fn = std::expected<bool, error> (*)(psio::borrow<cursor>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_next), self);
   }

   inline bool cur_on_row(psio::borrow<cursor> self)
   {
      using fn = bool (*)(psio::borrow<cursor>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_on_row), self);
   }

   inline auto cur_key(psio::borrow<cursor> self)
   {
      using fn = std::expected<bytes, error> (*)(psio::borrow<cursor>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_key), self);
   }

   inline auto cur_value(psio::borrow<cursor> self,
                          uint32_t offset = 0,
                          std::optional<uint32_t> len = std::nullopt)
   {
      using fn = std::expected<bytes, error> (*)(
         psio::borrow<cursor>, uint32_t, std::optional<uint32_t>);
      return ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_value), self, offset, len);
   }

   inline void cur_drop(psio::own<cursor> self)
   {
      using fn = void (*)(psio::own<cursor>);
      ImportProxy::call_impl<fn>(
         reinterpret_cast<raw_import_fn>(&_raw_cursor_drop), self);
   }

   // Helpers
   inline bytes make_bytes(const char* s)
   {
      auto p = reinterpret_cast<const uint8_t*>(s);
      return {p, p + __builtin_strlen(s)};
   }

   inline bytes make_bytes(std::string_view sv)
   {
      auto p = reinterpret_cast<const uint8_t*>(sv.data());
      return {p, p + sv.size()};
   }

}  // namespace db
