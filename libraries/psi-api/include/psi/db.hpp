#pragma once

/*
 * psi/db.hpp — psi database API, code-first (WIT derived by reflection).
 *
 * Authoritative declaration of the psi database interface. Shared by
 * host and guest:
 *
 *   - Guest:  includes this header, calls methods on own<database>
 *             (and friends) as if they were native objects. The
 *             PSIO1_REFLECT proxy generates Canonical ABI stubs that
 *             marshal to the imported component.
 *
 *   - Host:   reflects the same declarations to discover the import
 *             surface; provides concrete subclasses that implement
 *             the methods natively (handle-table entries).
 *
 * The WIT binary is derived mechanically by
 * psio1::generate_wit_binary<store>(...). No hand-authored .wit file,
 * no handwritten stub layer.
 *
 * ── Transaction scoping ──────────────────────────────────────────────
 *
 * A virtual database corresponds to a single top-root in the psitri
 * trie. That top-root is itself a keyed map of `{table-name →
 * subtree-root}`. Writing into a table means mutating its subtree,
 * which produces a new subtree root, which must then be written back
 * into the top-root's entry for that table name.
 *
 * Because commit has to atomically advance the top-root entry for
 * every table the transaction touched (DDL and DML alike), write
 * transactions are scoped to the *database* (top-root), not to a
 * single table. Tables are obtained as typed lenses *inside* a
 * transaction: `tx.open_table(name) -> own<table>`. The table handle
 * is valid only for the lifetime of its parent transaction.
 *
 * Read transactions have the same shape for symmetry — open tables
 * from a read-tx to get a consistent view across all of them.
 *
 * Single-type transactions for now: write operations on a table
 * obtained from a read-tx return error::read_only_tx. A future split
 * into read_transaction / write_transaction resources would lift
 * that into the type system.
 *
 * ── Ownership ────────────────────────────────────────────────────────
 *
 * Factories return own<T>; cross-call refs use borrow<T>. Dropping
 * own<transaction> without calling commit() aborts the tx host-side.
 *
 * ── Slice reads ──────────────────────────────────────────────────────
 *
 * `table::get` and `cursor::value` take (offset, optional<len>).
 * len = nullopt reads to end-of-value. Zero-byte return = EOF or
 * empty value. For large-value streaming without per-call
 * cabi_realloc allocations, see psi/db_raw.hpp — raw byte-blit
 * imports that write into a caller-owned buffer.
 */

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <vector>

#include <psio1/reflect.hpp>
#include <psio1/wit_resource.hpp>

namespace psi::db
{

   // ── shared value types ────────────────────────────────────────────

   /* Arbitrary byte sequences — keys and values are not UTF-8. */
   using bytes = std::vector<std::uint8_t>;

   /* Read-only visibility hint for snapshot reads. */
   enum class read_mode : std::uint8_t
   {
      latest    = 0,  /* read-your-writes + uncommitted */
      committed = 1,  /* only merged (trie) data */
      fresh     = 2,  /* stable snapshot pinned for this cursor */
   };

   /* Per-table counters. Eventually consistent — reflects committed
    * writes only, not uncommitted tx state. */
   struct stats
   {
      std::uint64_t key_count;
      std::uint64_t total_key_bytes;
      std::uint64_t total_val_bytes;
   };
   PSIO1_REFLECT(stats, key_count, total_key_bytes, total_val_bytes)

   /* Domain error codes. Methods that can fail return
    * std::expected<T, error>. */
   enum class error : std::uint8_t
   {
      not_found        = 0,
      already_exists   = 1,
      access_denied    = 2,
      invalid_handle   = 3,
      invalid_arg      = 4,
      tx_aborted       = 5,
      tx_conflict      = 6,
      read_only_tx     = 7,  /* write op on a read-tx's table */
      snapshot_missing = 8,
   };

   // ── forward declarations ──────────────────────────────────────────

   struct database;
   struct table;
   struct transaction;
   struct cursor;
   struct snapshots;
   struct snapshot_cursor;

   // ── cursor ────────────────────────────────────────────────────────

   /* Positioned read cursor over a table. Cursor is scoped to the
    * table that opened it, which in turn is scoped to its parent
    * transaction. Valid while the tx is uncommitted and the table
    * handle is alive. */
   struct cursor : psio1::wit_resource
   {
      std::expected<bool, error> seek(bytes key);
      std::expected<bool, error> seek_first();
      std::expected<bool, error> seek_last();
      std::expected<bool, error> lower_bound(bytes key);
      std::expected<bool, error> upper_bound(bytes key);
      std::expected<bool, error> next();
      std::expected<bool, error> prev();
      bool                       on_row();

      /* Key of the current row (no slicing — keys are bounded). */
      std::expected<bytes, error> key();

      /* Slice of the current row's value. offset past end → 0-length
       * return. len = nullopt means "to end of value". */
      std::expected<bytes, error> value(std::uint32_t                offset = 0,
                                        std::optional<std::uint32_t> len    = std::nullopt);
   };
   PSIO1_REFLECT(cursor,
                method(seek, key),
                method(seek_first),
                method(seek_last),
                method(lower_bound, key),
                method(upper_bound, key),
                method(next),
                method(prev),
                method(on_row),
                method(key),
                method(value, offset, len))

   // ── table ─────────────────────────────────────────────────────────

   /* Typed lens into a specific table within its parent transaction's
    * view. Obtained via transaction::open_table or create_table; the
    * handle is invalidated when the parent tx commits or aborts.
    *
    * Read ops (get, open_cursor, get_stats) work on tables from both
    * read-tx and write-tx. Mutating ops (upsert, remove, remove_range)
    * on a table from a read-tx return error::read_only_tx. */
   struct table : psio1::wit_resource
   {
      /* Read ops. */
      std::expected<bytes, error> get(bytes                        key,
                                      std::uint32_t                offset = 0,
                                      std::optional<std::uint32_t> len    = std::nullopt);
      psio1::own<cursor>           open_cursor();
      stats                       get_stats();

      /* Write ops. */
      std::expected<void, error>          upsert(bytes key, bytes value);
      std::expected<bool, error>          remove(bytes key);         /* true=removed */
      std::expected<std::uint32_t, error> remove_range(bytes low, bytes high);
   };
   PSIO1_REFLECT(table,
                method(get, key, offset, len),
                method(open_cursor),
                method(get_stats),
                method(upsert, key, value),
                method(remove, key),
                method(remove_range, low, high))

   // ── transaction ───────────────────────────────────────────────────

   /* A transaction spans the entire virtual DB (top-root). Tables are
    * opened as lenses into the tx's view — DDL and DML in the same
    * transaction commit atomically against the top-root.
    *
    * Dropping an uncommitted transaction aborts it via [resource-drop].
    * A read-tx rejects DDL (create_table, drop_table) with
    * error::read_only_tx. */
   struct transaction : psio1::wit_resource
   {
      std::expected<void, error> commit();
      void                       abort();

      psio1::own<transaction> start_sub();

      /* Table acquisition (lives within this tx). */
      std::expected<psio1::own<table>, error> open_table(std::string name);
      std::expected<psio1::own<table>, error> create_table(std::string name);
      std::expected<void, error>             drop_table(std::string name);

      /* Snapshot of the top-root's table directory as seen by this
       * tx. A read-tx sees the committed state; a write-tx
       * additionally reflects its own create/drop operations. */
      std::vector<std::string> list_tables();
   };
   PSIO1_REFLECT(transaction,
                method(commit),
                method(abort),
                method(start_sub),
                method(open_table, name),
                method(create_table, name),
                method(drop_table, name),
                method(list_tables))

   // ── snapshot_cursor ───────────────────────────────────────────────

   /* Positioned iterator over the snapshot name-space. Snapshots can
    * number in the thousands (e.g. one-per-block history), so the
    * snapshot directory is navigated with a stateful cursor rather
    * than a bulk list. Scoped to its parent `snapshots` sub-resource;
    * valid while that handle (and the owning database) is alive. */
   struct snapshot_cursor : psio1::wit_resource
   {
      std::expected<bool, error> seek(std::string name);   /* lower_bound */
      std::expected<bool, error> seek_first();
      std::expected<bool, error> seek_last();
      std::expected<bool, error> next();
      std::expected<bool, error> prev();
      bool                       on_row();

      /* Name of the current snapshot. Error if !on_row. */
      std::expected<std::string, error> name();
   };
   PSIO1_REFLECT(snapshot_cursor,
                method(seek, name),
                method(seek_first),
                method(seek_last),
                method(next),
                method(prev),
                method(on_row),
                method(name))

   // ── snapshots (collection sub-resource) ───────────────────────────

   /* Host-managed collection of named snapshots, scoped to the owning
    * database. See DesignTodo.md on why the guest cannot construct
    * snapshot handles directly (prevents subtree cycle attacks).
    *
    * `create` both registers a new named snapshot and returns a handle
    * to it — same return shape as `open`, so the caller can use it
    * immediately without a redundant open. */
   struct snapshots : psio1::wit_resource
   {
      std::expected<psio1::own<database>, error> create(std::string name);
      std::expected<psio1::own<database>, error> open(std::string name);
      std::expected<void, error>                release(std::string name);

      /* Iterate the snapshot name-space in sorted order. */
      psio1::own<snapshot_cursor> cursor();
   };
   PSIO1_REFLECT(snapshots,
                method(create, name),
                method(open, name),
                method(release, name),
                method(cursor))

   // ── database (virtual DB / top root) ──────────────────────────────

   struct database : psio1::wit_resource
   {
      /* Transactions are top-root-scoped. */
      psio1::own<transaction> start_read(read_mode mode = read_mode::latest);
      psio1::own<transaction> start_write();

      /* Snapshot directory for this database. */
      psio1::own<snapshots> snapshots();

      /* Atomically replace live state with a snapshot's contents. */
      std::expected<void, error> restore_to(psio1::borrow<database> snapshot);
   };
   PSIO1_REFLECT(database,
                method(start_read, mode),
                method(start_write),
                method(snapshots),
                method(restore_to, snapshot))

   // ── interface (top-level imports) ─────────────────────────────────

   /* `store` is the top-level import surface: a factory that hands
    * out `database` handles by name. It's intentionally named for the
    * capability, not the provider — a guest's `psi:db/store` import
    * can be satisfied by the runtime host, a virtualization shim, or
    * another component, and the guest need not care which.
    *
    * psio1::generate_wit_binary<store> produces the Component Model
    * binary. */
   struct store
   {
      std::expected<psio1::own<database>, error> open(std::string name);
   };
   PSIO1_REFLECT(store, method(open, name))

}  // namespace psi::db
