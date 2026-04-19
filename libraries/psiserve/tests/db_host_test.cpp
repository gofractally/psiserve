#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiserve/db_host.hpp>

#include <cstdint>
#include <filesystem>
#include <string>

namespace
{
   namespace fs = std::filesystem;
   using psi::db::error;
   using psi::db::bytes;

   bytes to_bytes(std::string_view s)
   {
      auto p = reinterpret_cast<const uint8_t*>(s.data());
      return {p, p + s.size()};
   }

   std::string from_bytes(const bytes& b)
   {
      return {reinterpret_cast<const char*>(b.data()), b.size()};
   }

   struct test_fixture
   {
      fs::path                        dir;
      psiserve::DbHost<>              host;

      test_fixture()
      {
         dir = fs::temp_directory_path() / "db_host_test";
         fs::remove_all(dir);
         host.db = psitri::database::open(dir);
         host.ws = host.db->start_write_session();
         host.name_to_root["testdb"] = 0;
      }

      ~test_fixture()
      {
         host.cursors.destroy_all();
         host.tables.destroy_all();
         host.transactions.destroy_all();
         host.databases.destroy_all();
         host.ws.reset();
         host.db.reset();
         fs::remove_all(dir);
      }
   };

}  // namespace

TEST_CASE("db_host open unknown database returns not_found", "[db_host]")
{
   test_fixture f;
   auto r = f.host.open("no_such_db");
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::not_found);
}

TEST_CASE("db_host open known database succeeds", "[db_host]")
{
   test_fixture f;
   auto r = f.host.open("testdb");
   REQUIRE(r.has_value());
   f.host.database_drop(r.value());
}

TEST_CASE("db_host create table, upsert, get, commit, re-read", "[db_host]")
{
   test_fixture f;

   auto db_r = f.host.open("testdb");
   REQUIRE(db_r.has_value());
   auto db = db_r.value();

   auto tx = f.host.start_write(psio::borrow<psi::db::database>{db.handle});
   REQUIRE(tx.handle != psizam::handle_table<psiserve::transaction_impl>::invalid_handle);

   auto tbl_r = f.host.create_table(psio::borrow<psi::db::transaction>{tx.handle}, "users");
   REQUIRE(tbl_r.has_value());
   auto tbl = tbl_r.value();

   auto upsert_r = f.host.table_upsert(
       psio::borrow<psi::db::table>{tbl.handle},
       to_bytes("alice"), to_bytes("eng"));
   REQUIRE(upsert_r.has_value());

   auto get_r = f.host.table_get(
       psio::borrow<psi::db::table>{tbl.handle},
       to_bytes("alice"), 0, std::nullopt);
   REQUIRE(get_r.has_value());
   CHECK(from_bytes(get_r.value()) == "eng");

   f.host.table_drop(psio::own<psi::db::table>{tbl.handle});

   auto commit_r = f.host.commit(psio::borrow<psi::db::transaction>{tx.handle});
   REQUIRE(commit_r.has_value());
   f.host.transaction_drop(psio::own<psi::db::transaction>{tx.handle});

   auto tx2 = f.host.start_read(psio::borrow<psi::db::database>{db.handle}, 0);
   auto tbl2_r = f.host.open_table(
       psio::borrow<psi::db::transaction>{tx2.handle}, "users");
   REQUIRE(tbl2_r.has_value());
   auto tbl2 = tbl2_r.value();

   auto get2_r = f.host.table_get(
       psio::borrow<psi::db::table>{tbl2.handle},
       to_bytes("alice"), 0, std::nullopt);
   REQUIRE(get2_r.has_value());
   CHECK(from_bytes(get2_r.value()) == "eng");

   f.host.table_drop(psio::own<psi::db::table>{tbl2.handle});
   f.host.transaction_drop(psio::own<psi::db::transaction>{tx2.handle});
   f.host.database_drop(db);

   CHECK(f.host.databases.live_count() == 0);
   CHECK(f.host.transactions.live_count() == 0);
   CHECK(f.host.tables.live_count() == 0);
   CHECK(f.host.cursors.live_count() == 0);
}

TEST_CASE("db_host cursor iteration", "[db_host]")
{
   test_fixture f;

   auto db = f.host.open("testdb").value();
   auto tx = f.host.start_write(psio::borrow<psi::db::database>{db.handle});
   auto tbl = f.host.create_table(
       psio::borrow<psi::db::transaction>{tx.handle}, "kv").value();
   auto b = psio::borrow<psi::db::table>{tbl.handle};

   f.host.table_upsert(b, to_bytes("a"), to_bytes("1"));
   f.host.table_upsert(b, to_bytes("b"), to_bytes("2"));
   f.host.table_upsert(b, to_bytes("c"), to_bytes("3"));

   auto cur = f.host.table_open_cursor(b);
   auto cb = psio::borrow<psi::db::cursor>{cur.handle};

   auto sf = f.host.cursor_seek_first(cb);
   REQUIRE(sf.has_value());
   CHECK(sf.value() == true);

   auto k = f.host.cursor_key(cb);
   REQUIRE(k.has_value());
   CHECK(from_bytes(k.value()) == "a");

   auto v = f.host.cursor_value(cb, 0, std::nullopt);
   REQUIRE(v.has_value());
   CHECK(from_bytes(v.value()) == "1");

   auto n1 = f.host.cursor_next(cb);
   REQUIRE(n1.has_value());
   CHECK(n1.value() == true);
   CHECK(from_bytes(f.host.cursor_key(cb).value()) == "b");

   auto n2 = f.host.cursor_next(cb);
   REQUIRE(n2.has_value());
   CHECK(n2.value() == true);
   CHECK(from_bytes(f.host.cursor_key(cb).value()) == "c");

   auto n3 = f.host.cursor_next(cb);
   REQUIRE(n3.has_value());
   CHECK(n3.value() == false);
   CHECK(f.host.cursor_on_row(cb) == false);

   f.host.cursor_drop(psio::own<psi::db::cursor>{cur.handle});
   f.host.table_drop(psio::own<psi::db::table>{tbl.handle});
   f.host.commit(psio::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(db);
}

TEST_CASE("db_host list_tables", "[db_host]")
{
   test_fixture f;

   auto db = f.host.open("testdb").value();
   auto tx = f.host.start_write(psio::borrow<psi::db::database>{db.handle});

   auto alpha = f.host.create_table(psio::borrow<psi::db::transaction>{tx.handle}, "alpha").value();
   auto beta  = f.host.create_table(psio::borrow<psi::db::transaction>{tx.handle}, "beta").value();

   f.host.table_upsert(psio::borrow<psi::db::table>{alpha.handle}, to_bytes("k1"), to_bytes("v1"));
   f.host.table_upsert(psio::borrow<psi::db::table>{beta.handle}, to_bytes("k2"), to_bytes("v2"));

   f.host.table_drop(alpha);
   f.host.table_drop(beta);

   auto names = f.host.list_tables(psio::borrow<psi::db::transaction>{tx.handle});
   std::sort(names.begin(), names.end());
   REQUIRE(names.size() == 2);
   CHECK(names[0] == "alpha");
   CHECK(names[1] == "beta");

   f.host.commit(psio::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(db);
}

TEST_CASE("db_host remove and remove_range", "[db_host]")
{
   test_fixture f;

   auto db = f.host.open("testdb").value();
   auto tx = f.host.start_write(psio::borrow<psi::db::database>{db.handle});
   auto tbl = f.host.create_table(
       psio::borrow<psi::db::transaction>{tx.handle}, "t").value();
   auto b = psio::borrow<psi::db::table>{tbl.handle};

   f.host.table_upsert(b, to_bytes("a"), to_bytes("1"));
   f.host.table_upsert(b, to_bytes("b"), to_bytes("2"));
   f.host.table_upsert(b, to_bytes("c"), to_bytes("3"));
   f.host.table_upsert(b, to_bytes("d"), to_bytes("4"));

   auto rm = f.host.table_remove(b, to_bytes("b"));
   REQUIRE(rm.has_value());
   CHECK(rm.value() == true);

   auto rm2 = f.host.table_remove(b, to_bytes("zzz"));
   REQUIRE(rm2.has_value());
   CHECK(rm2.value() == false);

   auto rr = f.host.table_remove_range(b, to_bytes("c"), to_bytes("e"));
   REQUIRE(rr.has_value());
   CHECK(rr.value() == 2);

   auto stats = f.host.table_get_stats(b);
   CHECK(stats.key_count == 1);

   f.host.table_drop(psio::own<psi::db::table>{tbl.handle});
   f.host.commit(psio::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(db);
}

TEST_CASE("db_host slice reads", "[db_host]")
{
   test_fixture f;

   auto db = f.host.open("testdb").value();
   auto tx = f.host.start_write(psio::borrow<psi::db::database>{db.handle});
   auto tbl = f.host.create_table(
       psio::borrow<psi::db::transaction>{tx.handle}, "t").value();
   auto b = psio::borrow<psi::db::table>{tbl.handle};

   f.host.table_upsert(b, to_bytes("k"), to_bytes("hello world"));

   auto full = f.host.table_get(b, to_bytes("k"), 0, std::nullopt);
   REQUIRE(full.has_value());
   CHECK(from_bytes(full.value()) == "hello world");

   auto slice = f.host.table_get(b, to_bytes("k"), 6, uint32_t{5});
   REQUIRE(slice.has_value());
   CHECK(from_bytes(slice.value()) == "world");

   auto past_end = f.host.table_get(b, to_bytes("k"), 100, std::nullopt);
   REQUIRE(past_end.has_value());
   CHECK(past_end.value().empty());

   f.host.table_drop(psio::own<psi::db::table>{tbl.handle});
   f.host.commit(psio::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(db);
}

TEST_CASE("db_host abort discards writes", "[db_host]")
{
   test_fixture f;

   auto db = f.host.open("testdb").value();

   {
      auto tx = f.host.start_write(psio::borrow<psi::db::database>{db.handle});
      auto tbl = f.host.create_table(
          psio::borrow<psi::db::transaction>{tx.handle}, "ephemeral").value();
      f.host.table_upsert(
          psio::borrow<psi::db::table>{tbl.handle},
          to_bytes("x"), to_bytes("y"));
      f.host.table_drop(psio::own<psi::db::table>{tbl.handle});
      f.host.abort(psio::borrow<psi::db::transaction>{tx.handle});
      f.host.transaction_drop(psio::own<psi::db::transaction>{tx.handle});
   }

   {
      auto tx = f.host.start_read(psio::borrow<psi::db::database>{db.handle}, 0);
      auto tbl_r = f.host.open_table(
          psio::borrow<psi::db::transaction>{tx.handle}, "ephemeral");
      CHECK(!tbl_r.has_value());
      CHECK(tbl_r.error() == error::not_found);
      f.host.transaction_drop(psio::own<psi::db::transaction>{tx.handle});
   }

   f.host.database_drop(db);
}
