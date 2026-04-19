#pragma once

#include <psi/db.hpp>
#include <psio/structural.hpp>
#include <psizam/handle_table.hpp>
#include <psitri/database_impl.hpp>
#include <psitri/read_session_impl.hpp>
#include <psitri/transaction.hpp>
#include <psitri/write_cursor.hpp>
#include <psitri/write_session_impl.hpp>

#include <algorithm>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace psiserve
{

namespace db_detail
{
   inline psitri::key_view to_kv(const psi::db::bytes& b)
   {
      return {reinterpret_cast<const char*>(b.data()), b.size()};
   }

   inline psi::db::bytes from_kv(psitri::key_view kv)
   {
      auto p = reinterpret_cast<const uint8_t*>(kv.data());
      return {p, p + kv.size()};
   }
}  // namespace db_detail

struct database_impl
{
   std::string name;
   uint32_t    root_index;
   bool        read_only;
};

struct table_impl;

struct transaction_impl
{
   uint32_t                          db_handle;
   bool                              is_write;
   std::optional<psitri::transaction> write_tx;
   sal::smart_ptr<sal::alloc_header> read_root;
   std::vector<uint32_t>             open_table_handles;
   bool                              ended = false;
};

struct table_impl
{
   uint32_t                                tx_handle;
   std::string                             table_name;
   bool                                    is_write;
   std::optional<psitri::write_cursor>     subtree_wc;
   sal::smart_ptr<sal::alloc_header>       subtree_root;
   std::vector<uint32_t>                   open_cursor_handles;
   bool                                    dirty   = false;
   bool                                    created = false;
};

struct cursor_impl
{
   uint32_t       table_handle;
   psitri::cursor inner;

   cursor_impl(uint32_t th, psitri::cursor c) : table_handle(th), inner(std::move(c)) {}
};

template <typename LP = psitri::std_lock_policy>
struct DbHost
{
   using database_type      = psitri::basic_database<LP>;
   using write_session_type = psitri::basic_write_session<LP>;

   std::shared_ptr<database_type>              db;
   std::shared_ptr<write_session_type>         ws;
   std::unordered_map<std::string, uint32_t>   name_to_root;

   psizam::handle_table<database_impl, 32>     databases{32};
   psizam::handle_table<transaction_impl, 16>  transactions{16};
   psizam::handle_table<table_impl, 64>        tables{64};
   psizam::handle_table<cursor_impl, 128>      cursors{128};

   // ── store ────────────────────────────────────────────────────────

   std::expected<psio::own<psi::db::database>, psi::db::error>
   open(std::string name)
   {
      auto it = name_to_root.find(name);
      if (it == name_to_root.end())
         return std::unexpected(psi::db::error::not_found);

      auto h = databases.create(database_impl{
          .name       = std::move(name),
          .root_index = it->second,
          .read_only  = false,
      });
      if (h == psizam::handle_table<database_impl, 32>::invalid_handle)
         return std::unexpected(psi::db::error::access_denied);

      return psio::own<psi::db::database>{h};
   }

   // ── database ─────────────────────────────────────────────────────

   psio::own<psi::db::transaction>
   start_write(psio::borrow<psi::db::database> self)
   {
      auto* d = databases.get(self.handle);
      if (!d)
         return psio::own<psi::db::transaction>{psizam::handle_table<transaction_impl>::invalid_handle};
      if (d->read_only)
         return psio::own<psi::db::transaction>{psizam::handle_table<transaction_impl>::invalid_handle};

      auto tx = ws->start_transaction(d->root_index);
      auto h  = transactions.create(transaction_impl{
           .db_handle = self.handle,
           .is_write  = true,
           .write_tx  = std::move(tx),
      });
      return psio::own<psi::db::transaction>{h};
   }

   psio::own<psi::db::transaction>
   start_read(psio::borrow<psi::db::database> self, uint8_t mode)
   {
      auto* d = databases.get(self.handle);
      if (!d)
         return psio::own<psi::db::transaction>{psizam::handle_table<transaction_impl>::invalid_handle};

      auto root = ws->get_root(d->root_index);
      auto h    = transactions.create(transaction_impl{
             .db_handle = self.handle,
             .is_write  = false,
             .read_root = std::move(root),
      });
      return psio::own<psi::db::transaction>{h};
   }

   void database_drop(psio::own<psi::db::database> self)
   {
      databases.destroy(self.handle);
   }

   // ── transaction ──────────────────────────────────────────────────

   void write_back_dirty_tables(transaction_impl& tx)
   {
      for (auto th : tx.open_table_handles)
      {
         auto* t = tables.get(th);
         if (!t || !t->is_write)
            continue;
         if (t->dirty && t->subtree_wc && tx.write_tx)
         {
            auto root = t->subtree_wc->take_root();
            if (root)
               tx.write_tx->upsert(t->table_name, std::move(root));
            t->dirty = false;
         }
      }
   }

   void destroy_transaction_children(transaction_impl& tx)
   {
      for (auto th : tx.open_table_handles)
      {
         auto* t = tables.get(th);
         if (t)
         {
            for (auto ch : t->open_cursor_handles)
               cursors.destroy(ch);
            t->open_cursor_handles.clear();
         }
         tables.destroy(th);
      }
      tx.open_table_handles.clear();
   }

   std::expected<void, psi::db::error>
   commit(psio::borrow<psi::db::transaction> self)
   {
      auto* tx = transactions.get(self.handle);
      if (!tx || tx->ended)
         return std::unexpected(psi::db::error::tx_aborted);
      if (!tx->is_write || !tx->write_tx)
         return std::unexpected(psi::db::error::read_only_tx);

      write_back_dirty_tables(*tx);
      tx->write_tx->commit();
      tx->ended = true;
      destroy_transaction_children(*tx);
      return {};
   }

   void abort(psio::borrow<psi::db::transaction> self)
   {
      auto* tx = transactions.get(self.handle);
      if (!tx || tx->ended)
         return;

      if (tx->write_tx)
         tx->write_tx->abort();
      tx->ended = true;
      destroy_transaction_children(*tx);
   }

   void transaction_drop(psio::own<psi::db::transaction> self)
   {
      auto* tx = transactions.get(self.handle);
      if (tx && !tx->ended)
      {
         if (tx->write_tx)
            tx->write_tx->abort();
         destroy_transaction_children(*tx);
      }
      transactions.destroy(self.handle);
   }

   std::expected<psio::own<psi::db::table>, psi::db::error>
   open_table(psio::borrow<psi::db::transaction> self, std::string name)
   {
      auto* tx = transactions.get(self.handle);
      if (!tx || tx->ended)
         return std::unexpected(psi::db::error::invalid_handle);

      if (tx->is_write && tx->write_tx)
      {
         if (!tx->write_tx->is_subtree(name))
            return std::unexpected(psi::db::error::not_found);

         auto wc = tx->write_tx->get_subtree_cursor(name);
         auto h  = tables.create(table_impl{
             .tx_handle  = self.handle,
             .table_name = name,
             .is_write   = true,
             .subtree_wc = std::move(wc),
         });
         if (h == psizam::handle_table<table_impl>::invalid_handle)
            return std::unexpected(psi::db::error::access_denied);
         tx->open_table_handles.push_back(h);
         return psio::own<psi::db::table>{h};
      }
      else
      {
         if (!tx->read_root)
            return std::unexpected(psi::db::error::not_found);
         psitri::cursor rc(tx->read_root);
         if (!rc.seek(name))
            return std::unexpected(psi::db::error::not_found);
         if (!rc.is_subtree())
            return std::unexpected(psi::db::error::not_found);
         auto sub = rc.subtree();
         auto h   = tables.create(table_impl{
               .tx_handle    = self.handle,
               .table_name   = name,
               .is_write     = false,
               .subtree_root = std::move(sub),
         });
         if (h == psizam::handle_table<table_impl>::invalid_handle)
            return std::unexpected(psi::db::error::access_denied);
         tx->open_table_handles.push_back(h);
         return psio::own<psi::db::table>{h};
      }
   }

   std::expected<psio::own<psi::db::table>, psi::db::error>
   create_table(psio::borrow<psi::db::transaction> self, std::string name)
   {
      auto* tx = transactions.get(self.handle);
      if (!tx || tx->ended)
         return std::unexpected(psi::db::error::invalid_handle);
      if (!tx->is_write || !tx->write_tx)
         return std::unexpected(psi::db::error::read_only_tx);

      if (tx->write_tx->is_subtree(name))
         return std::unexpected(psi::db::error::already_exists);

      auto wc_ptr = ws->create_write_cursor();
      auto h      = tables.create(table_impl{
          .tx_handle  = self.handle,
          .table_name = name,
          .is_write   = true,
          .subtree_wc = std::move(*wc_ptr),
          .created    = true,
      });
      if (h == psizam::handle_table<table_impl>::invalid_handle)
         return std::unexpected(psi::db::error::access_denied);
      tx->open_table_handles.push_back(h);
      return psio::own<psi::db::table>{h};
   }

   std::expected<void, psi::db::error>
   drop_table(psio::borrow<psi::db::transaction> self, std::string name)
   {
      auto* tx = transactions.get(self.handle);
      if (!tx || tx->ended)
         return std::unexpected(psi::db::error::invalid_handle);
      if (!tx->is_write || !tx->write_tx)
         return std::unexpected(psi::db::error::read_only_tx);

      int r = tx->write_tx->remove(name);
      if (r < 0)
         return std::unexpected(psi::db::error::not_found);
      return {};
   }

   std::vector<std::string>
   list_tables(psio::borrow<psi::db::transaction> self)
   {
      auto* tx = transactions.get(self.handle);
      if (!tx || tx->ended)
         return {};

      std::vector<std::string> result;
      psitri::cursor           rc = tx->is_write && tx->write_tx
                                        ? tx->write_tx->read_cursor()
                                        : psitri::cursor(tx->read_root);
      if (rc.seek_begin())
      {
         do
         {
            if (rc.is_subtree())
               result.emplace_back(rc.key());
         } while (rc.next());
      }
      return result;
   }

   // ── table ────────────────────────────────────────────────────────

   void write_back_table(table_impl& t)
   {
      if (!t.is_write || !(t.dirty || t.created) || !t.subtree_wc)
         return;
      auto* tx = transactions.get(t.tx_handle);
      if (!tx || !tx->write_tx || tx->ended)
         return;
      auto root = t.subtree_wc->root();
      if (root)
         tx->write_tx->upsert(t.table_name, std::move(root));
      t.dirty = false;
   }

   void table_drop(psio::own<psi::db::table> self)
   {
      auto* t = tables.get(self.handle);
      if (t)
      {
         if (t->is_write && (t->dirty || t->created))
            write_back_table(*t);

         for (auto ch : t->open_cursor_handles)
            cursors.destroy(ch);

         auto* tx = transactions.get(t->tx_handle);
         if (tx)
         {
            auto& v = tx->open_table_handles;
            v.erase(std::remove(v.begin(), v.end(), self.handle), v.end());
         }
      }
      tables.destroy(self.handle);
   }

   std::expected<psi::db::bytes, psi::db::error>
   get(psio::borrow<psi::db::table> self, psi::db::bytes key,
             uint32_t offset, std::optional<uint32_t> len)
   {
      auto* t = tables.get(self.handle);
      if (!t)
         return std::unexpected(psi::db::error::invalid_handle);

      auto kv = db_detail::to_kv(key);
      std::optional<std::string> val;

      if (t->is_write && t->subtree_wc)
         val = t->subtree_wc->get<std::string>(kv);
      else if (t->subtree_root)
      {
         psitri::cursor rc(t->subtree_root);
         val = rc.get<std::string>(kv);
      }

      if (!val)
         return std::unexpected(psi::db::error::not_found);

      uint32_t vsize = static_cast<uint32_t>(val->size());
      if (offset >= vsize)
         return psi::db::bytes{};

      uint32_t avail = vsize - offset;
      uint32_t count = len ? std::min(*len, avail) : avail;
      auto     p     = reinterpret_cast<const uint8_t*>(val->data()) + offset;
      return psi::db::bytes(p, p + count);
   }

   std::expected<void, psi::db::error>
   upsert(psio::borrow<psi::db::table> self, psi::db::bytes key, psi::db::bytes value)
   {
      auto* t = tables.get(self.handle);
      if (!t)
         return std::unexpected(psi::db::error::invalid_handle);
      if (!t->is_write || !t->subtree_wc)
         return std::unexpected(psi::db::error::read_only_tx);

      t->subtree_wc->upsert(db_detail::to_kv(key), db_detail::to_kv(value));
      t->dirty = true;
      return {};
   }

   std::expected<bool, psi::db::error>
   remove(psio::borrow<psi::db::table> self, psi::db::bytes key)
   {
      auto* t = tables.get(self.handle);
      if (!t)
         return std::unexpected(psi::db::error::invalid_handle);
      if (!t->is_write || !t->subtree_wc)
         return std::unexpected(psi::db::error::read_only_tx);

      int r = t->subtree_wc->remove(db_detail::to_kv(key));
      if (r >= 0)
         t->dirty = true;
      return r >= 0;
   }

   std::expected<uint32_t, psi::db::error>
   remove_range(psio::borrow<psi::db::table> self, psi::db::bytes low, psi::db::bytes high)
   {
      auto* t = tables.get(self.handle);
      if (!t)
         return std::unexpected(psi::db::error::invalid_handle);
      if (!t->is_write || !t->subtree_wc)
         return std::unexpected(psi::db::error::read_only_tx);

      uint64_t n = t->subtree_wc->remove_range(db_detail::to_kv(low), db_detail::to_kv(high));
      if (n > 0)
         t->dirty = true;
      return static_cast<uint32_t>(n);
   }

   psi::db::stats get_stats(psio::borrow<psi::db::table> self)
   {
      auto* t = tables.get(self.handle);
      if (!t)
         return {};

      uint64_t count = 0;
      if (t->is_write && t->subtree_wc)
         count = t->subtree_wc->count_keys();
      else if (t->subtree_root)
      {
         psitri::cursor rc(t->subtree_root);
         count = rc.count_keys();
      }
      return psi::db::stats{.key_count = count, .total_key_bytes = 0, .total_val_bytes = 0};
   }

   psio::own<psi::db::cursor>
   open_cursor(psio::borrow<psi::db::table> self)
   {
      auto* t = tables.get(self.handle);
      if (!t)
         return psio::own<psi::db::cursor>{psizam::handle_table<cursor_impl>::invalid_handle};

      psitri::cursor rc = t->is_write && t->subtree_wc
                              ? t->subtree_wc->read_cursor()
                              : psitri::cursor(t->subtree_root);

      auto h = cursors.create(self.handle, std::move(rc));
      if (h != psizam::handle_table<cursor_impl>::invalid_handle)
         t->open_cursor_handles.push_back(h);
      return psio::own<psi::db::cursor>{h};
   }

   // ── cursor ───────────────────────────────────────────────────────

   std::expected<bool, psi::db::error>
   seek(psio::borrow<psi::db::cursor> self, psi::db::bytes key)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      return c->inner.seek(db_detail::to_kv(key));
   }

   std::expected<bool, psi::db::error>
   seek_first(psio::borrow<psi::db::cursor> self)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      bool has = c->inner.seek_begin();
      return has;
   }

   std::expected<bool, psi::db::error>
   seek_last(psio::borrow<psi::db::cursor> self)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      bool has = c->inner.seek_last();
      return has;
   }

   std::expected<bool, psi::db::error>
   lower_bound(psio::borrow<psi::db::cursor> self, psi::db::bytes key)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      return c->inner.lower_bound(db_detail::to_kv(key));
   }

   std::expected<bool, psi::db::error>
   upper_bound(psio::borrow<psi::db::cursor> self, psi::db::bytes key)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      return c->inner.upper_bound(db_detail::to_kv(key));
   }

   std::expected<bool, psi::db::error>
   next(psio::borrow<psi::db::cursor> self)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      return c->inner.next();
   }

   std::expected<bool, psi::db::error>
   prev(psio::borrow<psi::db::cursor> self)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      return c->inner.prev();
   }

   bool on_row(psio::borrow<psi::db::cursor> self)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return false;
      return !c->inner.is_end() && !c->inner.is_rend();
   }

   std::expected<psi::db::bytes, psi::db::error>
   key(psio::borrow<psi::db::cursor> self)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      if (c->inner.is_end() || c->inner.is_rend())
         return std::unexpected(psi::db::error::not_found);
      return db_detail::from_kv(c->inner.key());
   }

   std::expected<psi::db::bytes, psi::db::error>
   value(psio::borrow<psi::db::cursor> self,
                uint32_t offset, std::optional<uint32_t> len)
   {
      auto* c = cursors.get(self.handle);
      if (!c)
         return std::unexpected(psi::db::error::invalid_handle);
      if (c->inner.is_end() || c->inner.is_rend())
         return std::unexpected(psi::db::error::not_found);

      auto val = c->inner.value<std::string>();
      if (!val)
         return std::unexpected(psi::db::error::not_found);

      uint32_t vsize = static_cast<uint32_t>(val->size());
      if (offset >= vsize)
         return psi::db::bytes{};

      uint32_t avail = vsize - offset;
      uint32_t count = len ? std::min(*len, avail) : avail;
      auto     p     = reinterpret_cast<const uint8_t*>(val->data()) + offset;
      return psi::db::bytes(p, p + count);
   }

   void cursor_drop(psio::own<psi::db::cursor> self)
   {
      auto* c = cursors.get(self.handle);
      if (c)
      {
         auto* t = tables.get(c->table_handle);
         if (t)
         {
            auto& v = t->open_cursor_handles;
            v.erase(std::remove(v.begin(), v.end(), self.handle), v.end());
         }
      }
      cursors.destroy(self.handle);
   }
};

   using db_host = DbHost<psitri::std_lock_policy>;

}  // namespace psiserve

// ── interface_info specializations (host-side module names) ──────────

namespace psio::detail
{
   template <>
   struct interface_info<psi::db::store>
   {
      static constexpr ::psio::FixedString name = "psi:db/store";
   };
   template <>
   struct interface_info<psi::db::database>
   {
      static constexpr ::psio::FixedString name = "psi:db/database";
   };
   template <>
   struct interface_info<psi::db::transaction>
   {
      static constexpr ::psio::FixedString name = "psi:db/transaction";
   };
   template <>
   struct interface_info<psi::db::table>
   {
      static constexpr ::psio::FixedString name = "psi:db/table";
   };
   template <>
   struct interface_info<psi::db::cursor>
   {
      static constexpr ::psio::FixedString name = "psi:db/cursor";
   };
}  // namespace psio::detail

// ── Host module registration ────────────────────────────────────────

PSIO_HOST_MODULE(psiserve::db_host,
   interface(psi::db::store,
             open),
   interface(psi::db::database,
             start_write, start_read, database_drop),
   interface(psi::db::transaction,
             commit, abort, open_table, create_table,
             drop_table, list_tables, transaction_drop),
   interface(psi::db::table,
             get, upsert, remove, remove_range,
             get_stats, open_cursor, table_drop),
   interface(psi::db::cursor,
             seek, seek_first, seek_last, lower_bound,
             upper_bound, next, prev, on_row, key,
             value, cursor_drop))
