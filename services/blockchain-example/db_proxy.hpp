#pragma once

// db_proxy.hpp — Guest-side proxy imports for psi:db/* resources.
//
// Generates import stubs from psi/db.hpp's reflected resource types
// so that own<database>, own<transaction>, own<table>, own<cursor>
// proxy calls work directly:
//
//   auto db  = psi::db::store::open("blockchain");
//   auto tx  = db->start_write();         // own<database> → start_write()
//   auto tbl = tx->open_table("balances"); // own<transaction> → open_table()
//   tbl->upsert(key, value);              // own<table> → upsert()
//   tx->commit();

#include <psi/db.hpp>
#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

// Generate raw import thunks + ImportProxy call bodies for each
// resource interface's methods.

// psi:db/store
PSIO_IMPORT_IMPL(psi::db::store, open)

psi::db::store::open_result psi::db::store::open(std::string name)
   PSIO_IMPORT_IMPL_BODY(psi::db::store, open, name)

// psi:db/database
PSIO_IMPORT_IMPL(psi::db::database, start_write, start_read, database_drop)

psio::own<psi::db::transaction> psi::db::database::start_write()
   PSIO_IMPORT_IMPL_BODY(psi::db::database, start_write)

psio::own<psi::db::transaction> psi::db::database::start_read(psi::db::read_mode mode)
   PSIO_IMPORT_IMPL_BODY(psi::db::database, start_read, mode)

// psi:db/transaction
PSIO_IMPORT_IMPL(psi::db::transaction, commit, abort, start_sub,
                 open_table, create_table, drop_table, list_tables,
                 transaction_drop)

std::expected<void, psi::db::error> psi::db::transaction::commit()
   PSIO_IMPORT_IMPL_BODY(psi::db::transaction, commit)

void psi::db::transaction::abort()
   PSIO_IMPORT_IMPL_BODY(psi::db::transaction, abort)

psio::own<psi::db::transaction> psi::db::transaction::start_sub()
   PSIO_IMPORT_IMPL_BODY(psi::db::transaction, start_sub)

std::expected<psio::own<psi::db::table>, psi::db::error>
psi::db::transaction::open_table(std::string name)
   PSIO_IMPORT_IMPL_BODY(psi::db::transaction, open_table, name)

std::expected<psio::own<psi::db::table>, psi::db::error>
psi::db::transaction::create_table(std::string name)
   PSIO_IMPORT_IMPL_BODY(psi::db::transaction, create_table, name)

std::expected<void, psi::db::error>
psi::db::transaction::drop_table(std::string name)
   PSIO_IMPORT_IMPL_BODY(psi::db::transaction, drop_table, name)

std::vector<std::string> psi::db::transaction::list_tables()
   PSIO_IMPORT_IMPL_BODY(psi::db::transaction, list_tables)

// psi:db/table
PSIO_IMPORT_IMPL(psi::db::table, get, upsert, remove, remove_range,
                 open_cursor, get_stats, table_drop)

std::expected<psi::db::bytes, psi::db::error>
psi::db::table::get(psi::db::bytes key, std::uint32_t offset,
                    std::optional<std::uint32_t> len)
   PSIO_IMPORT_IMPL_BODY(psi::db::table, get, key, offset, len)

std::expected<void, psi::db::error>
psi::db::table::upsert(psi::db::bytes key, psi::db::bytes value)
   PSIO_IMPORT_IMPL_BODY(psi::db::table, upsert, key, value)

std::expected<bool, psi::db::error>
psi::db::table::remove(psi::db::bytes key)
   PSIO_IMPORT_IMPL_BODY(psi::db::table, remove, key)

std::expected<std::uint32_t, psi::db::error>
psi::db::table::remove_range(psi::db::bytes low, psi::db::bytes high)
   PSIO_IMPORT_IMPL_BODY(psi::db::table, remove_range, low, high)

psio::own<psi::db::cursor> psi::db::table::open_cursor()
   PSIO_IMPORT_IMPL_BODY(psi::db::table, open_cursor)

psi::db::stats psi::db::table::get_stats()
   PSIO_IMPORT_IMPL_BODY(psi::db::table, get_stats)

// psi:db/cursor
PSIO_IMPORT_IMPL(psi::db::cursor, seek, seek_first, seek_last,
                 lower_bound, upper_bound, next, prev, on_row,
                 key, value, cursor_drop)

std::expected<bool, psi::db::error>
psi::db::cursor::seek(psi::db::bytes key)
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, seek, key)

std::expected<bool, psi::db::error> psi::db::cursor::seek_first()
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, seek_first)

std::expected<bool, psi::db::error> psi::db::cursor::seek_last()
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, seek_last)

std::expected<bool, psi::db::error>
psi::db::cursor::lower_bound(psi::db::bytes key)
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, lower_bound, key)

std::expected<bool, psi::db::error>
psi::db::cursor::upper_bound(psi::db::bytes key)
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, upper_bound, key)

std::expected<bool, psi::db::error> psi::db::cursor::next()
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, next)

std::expected<bool, psi::db::error> psi::db::cursor::prev()
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, prev)

bool psi::db::cursor::on_row()
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, on_row)

std::expected<psi::db::bytes, psi::db::error> psi::db::cursor::key()
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, key)

std::expected<psi::db::bytes, psi::db::error>
psi::db::cursor::value(std::uint32_t offset, std::optional<std::uint32_t> len)
   PSIO_IMPORT_IMPL_BODY(psi::db::cursor, value, offset, len)
