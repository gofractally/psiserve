// db_guest.cpp — WASM guest that exercises psi::db imports.
// Exports run_test() → uint32_t (0 = pass).

#include <psi/db.hpp>
#include <psio/guest_alloc.hpp>
#include <psizam/component_proxy.hpp>

using psizam::flat_val;
using psizam::raw_import_fn;
using psizam::ImportProxy;

// ── Import declarations ────────────────────────────────────────────────────
// Each extern "C" has the correct import_module (WIT-style) and import_name
// matching what PSIO_HOST_MODULE registers in db_host.hpp.

#define DB_RAW_IMPORT(MODULE, NAME)                                           \
   extern "C" [[clang::import_module(MODULE), clang::import_name(#NAME)]]     \
   flat_val _raw_##NAME(                                                       \
      flat_val, flat_val, flat_val, flat_val,                                   \
      flat_val, flat_val, flat_val, flat_val,                                   \
      flat_val, flat_val, flat_val, flat_val,                                   \
      flat_val, flat_val, flat_val, flat_val)

DB_RAW_IMPORT("psi:db/store",       open);
DB_RAW_IMPORT("psi:db/database",    start_write);
DB_RAW_IMPORT("psi:db/database",    database_drop);
DB_RAW_IMPORT("psi:db/transaction", create_table);
DB_RAW_IMPORT("psi:db/transaction", open_table);
DB_RAW_IMPORT("psi:db/transaction", commit);
DB_RAW_IMPORT("psi:db/transaction", abort);
DB_RAW_IMPORT("psi:db/transaction", list_tables);
DB_RAW_IMPORT("psi:db/transaction", transaction_drop);
DB_RAW_IMPORT("psi:db/table",       upsert);
DB_RAW_IMPORT("psi:db/table",       get);
DB_RAW_IMPORT("psi:db/table",       open_cursor);
DB_RAW_IMPORT("psi:db/table",       get_stats);
DB_RAW_IMPORT("psi:db/table",       table_drop);
DB_RAW_IMPORT("psi:db/cursor",      seek_first);
DB_RAW_IMPORT("psi:db/cursor",      next);
DB_RAW_IMPORT("psi:db/cursor",      on_row);
DB_RAW_IMPORT("psi:db/cursor",      key);
DB_RAW_IMPORT("psi:db/cursor",      value);
DB_RAW_IMPORT("psi:db/cursor",      cursor_drop);

// ── Typed call wrappers ────────────────────────────────────────────────────
// Use ImportProxy::call_impl<FnPtrType> to get correct return-type handling.
// FnPtrType determines the return type; argument types come from the caller.

namespace db {
using namespace psi::db;

auto store_open(std::string_view name) {
   using fn = std::expected<psio::own<database>, error>(*)(std::string_view);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_open), name);
}

auto db_start_write(psio::borrow<database> self) {
   using fn = psio::own<transaction>(*)(psio::borrow<database>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_start_write), self);
}

void db_drop(psio::own<database> self) {
   using fn = void(*)(psio::own<database>);
   ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_database_drop), self);
}

auto tx_create_table(psio::borrow<transaction> self, std::string_view name) {
   using fn = std::expected<psio::own<table>, error>(*)(
      psio::borrow<transaction>, std::string_view);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_create_table), self, name);
}

auto tx_open_table(psio::borrow<transaction> self, std::string_view name) {
   using fn = std::expected<psio::own<table>, error>(*)(
      psio::borrow<transaction>, std::string_view);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_open_table), self, name);
}

auto tx_commit(psio::borrow<transaction> self) {
   using fn = std::expected<void, error>(*)(psio::borrow<transaction>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_commit), self);
}

void tx_abort(psio::borrow<transaction> self) {
   using fn = void(*)(psio::borrow<transaction>);
   ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_abort), self);
}

auto tx_list_tables(psio::borrow<transaction> self) {
   using fn = std::vector<std::string>(*)(psio::borrow<transaction>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_list_tables), self);
}

void tx_drop(psio::own<transaction> self) {
   using fn = void(*)(psio::own<transaction>);
   ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_transaction_drop), self);
}

auto tbl_upsert(psio::borrow<table> self, bytes key, bytes val) {
   using fn = std::expected<void, error>(*)(
      psio::borrow<table>, bytes, bytes);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_upsert), self, key, val);
}

auto tbl_get(psio::borrow<table> self, bytes key,
             uint32_t offset, std::optional<uint32_t> len) {
   using fn = std::expected<bytes, error>(*)(
      psio::borrow<table>, bytes, uint32_t, std::optional<uint32_t>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_get), self, key, offset, len);
}

auto tbl_open_cursor(psio::borrow<table> self) {
   using fn = psio::own<cursor>(*)(psio::borrow<table>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_open_cursor), self);
}

auto tbl_get_stats(psio::borrow<table> self) {
   using fn = stats(*)(psio::borrow<table>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_get_stats), self);
}

void tbl_drop(psio::own<table> self) {
   using fn = void(*)(psio::own<table>);
   ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_table_drop), self);
}

auto cur_seek_first(psio::borrow<cursor> self) {
   using fn = std::expected<bool, error>(*)(psio::borrow<cursor>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_seek_first), self);
}

auto cur_next(psio::borrow<cursor> self) {
   using fn = std::expected<bool, error>(*)(psio::borrow<cursor>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_next), self);
}

bool cur_on_row(psio::borrow<cursor> self) {
   using fn = bool(*)(psio::borrow<cursor>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_on_row), self);
}

auto cur_key(psio::borrow<cursor> self) {
   using fn = std::expected<bytes, error>(*)(psio::borrow<cursor>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_key), self);
}

auto cur_value(psio::borrow<cursor> self,
               uint32_t offset, std::optional<uint32_t> len) {
   using fn = std::expected<bytes, error>(*)(
      psio::borrow<cursor>, uint32_t, std::optional<uint32_t>);
   return ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_value), self, offset, len);
}

void cur_drop(psio::own<cursor> self) {
   using fn = void(*)(psio::own<cursor>);
   ImportProxy::call_impl<fn>(
      reinterpret_cast<raw_import_fn>(&_raw_cursor_drop), self);
}

} // namespace db

// ── Helper ─────────────────────────────────────────────────────────────────

static psi::db::bytes make_bytes(const char* s) {
   auto p = reinterpret_cast<const uint8_t*>(s);
   return {p, p + __builtin_strlen(s)};
}

static bool bytes_eq(const psi::db::bytes& a, const char* s) {
   auto n = __builtin_strlen(s);
   if (a.size() != n) return false;
   return __builtin_memcmp(a.data(), s, n) == 0;
}

// ── Test body ──────────────────────────────────────────────────────────────

extern "C" [[clang::export_name("run_test")]]
flat_val run_test(flat_val, flat_val, flat_val, flat_val,
                  flat_val, flat_val, flat_val, flat_val,
                  flat_val, flat_val, flat_val, flat_val,
                  flat_val, flat_val, flat_val, flat_val)
{
   using namespace psi::db;

   // 1. Open the database
   auto db_res = db::store_open("testdb");
   if (!db_res) return 1;
   auto db_handle = db_res->handle;

   // 2. Start a write transaction
   auto tx = db::db_start_write(psio::borrow<database>{db_handle});
   auto tx_h = tx.handle;

   // 3. Create a table
   auto tbl_res = db::tx_create_table(
      psio::borrow<transaction>{tx_h}, "users");
   if (!tbl_res) return 3;
   auto tbl_h = tbl_res->handle;

   // 4. Upsert some rows
   auto r1 = db::tbl_upsert(psio::borrow<table>{tbl_h},
      make_bytes("alice"), make_bytes("admin"));
   if (!r1) return 4;

   auto r2 = db::tbl_upsert(psio::borrow<table>{tbl_h},
      make_bytes("bob"), make_bytes("user"));
   if (!r2) return 5;

   // 5. Get a value back
   auto got = db::tbl_get(psio::borrow<table>{tbl_h},
      make_bytes("alice"), 0, std::nullopt);
   if (!got) return 60;
   if (!bytes_eq(*got, "admin")) return 61;

   // 6. Check stats
   auto st = db::tbl_get_stats(psio::borrow<table>{tbl_h});
   if (st.key_count != 2) return 7;

   // 7. Cursor iteration
   auto cur = db::tbl_open_cursor(psio::borrow<table>{tbl_h});
   auto cur_h = cur.handle;
   auto sf = db::cur_seek_first(psio::borrow<cursor>{cur_h});
   if (!sf || !*sf) return 8;

   // First row should be "alice"
   auto k1 = db::cur_key(psio::borrow<cursor>{cur_h});
   if (!k1 || !bytes_eq(*k1, "alice")) return 9;

   auto v1 = db::cur_value(psio::borrow<cursor>{cur_h}, 0, std::nullopt);
   if (!v1 || !bytes_eq(*v1, "admin")) return 10;

   // Next row should be "bob"
   auto nx = db::cur_next(psio::borrow<cursor>{cur_h});
   if (!nx || !*nx) return 11;

   auto k2 = db::cur_key(psio::borrow<cursor>{cur_h});
   if (!k2 || !bytes_eq(*k2, "bob")) return 12;

   // No more rows
   auto nx2 = db::cur_next(psio::borrow<cursor>{cur_h});
   if (!nx2 || *nx2) return 13;  // should be false

   // 8. Drop cursor and table (writes back)
   db::cur_drop(psio::own<cursor>{cur_h});
   db::tbl_drop(psio::own<table>{tbl_h});

   // 9. Commit
   auto cr = db::tx_commit(psio::borrow<transaction>{tx_h});
   if (!cr) return 14;

   // 10. Drop transaction and database
   db::tx_drop(psio::own<transaction>{tx_h});
   db::db_drop(psio::own<database>{db_handle});

   return 0;
}
