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
   f.host.database_drop(std::move(r).value());
}

TEST_CASE("db_host create table, upsert, get, commit, re-read", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();

   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
   REQUIRE(tx.handle != psizam::handle_table<psiserve::transaction_impl>::invalid_handle);

   auto tbl = std::move(f.host.create_table(psio1::borrow<psi::db::transaction>{tx.handle}, "users")).value();

   auto upsert_r = f.host.upsert(
       psio1::borrow<psi::db::table>{tbl.handle},
       to_bytes("alice"), to_bytes("eng"));
   REQUIRE(upsert_r.has_value());

   auto get_r = f.host.get(
       psio1::borrow<psi::db::table>{tbl.handle},
       to_bytes("alice"), 0, std::nullopt);
   REQUIRE(get_r.has_value());
   CHECK(from_bytes(get_r.value()) == "eng");

   f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});

   auto commit_r = f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   REQUIRE(commit_r.has_value());
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});

   auto tx2 = f.host.start_read(psio1::borrow<psi::db::database>{db.handle}, 0);
   auto tbl2 = std::move(f.host.open_table(
       psio1::borrow<psi::db::transaction>{tx2.handle}, "users")).value();

   auto get2_r = f.host.get(
       psio1::borrow<psi::db::table>{tbl2.handle},
       to_bytes("alice"), 0, std::nullopt);
   REQUIRE(get2_r.has_value());
   CHECK(from_bytes(get2_r.value()) == "eng");

   f.host.table_drop(psio1::own<psi::db::table>{tbl2.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx2.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});

   CHECK(f.host.databases.live_count() == 0);
   CHECK(f.host.transactions.live_count() == 0);
   CHECK(f.host.tables.live_count() == 0);
   CHECK(f.host.cursors.live_count() == 0);
}

TEST_CASE("db_host cursor iteration", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();
   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
   auto tbl = std::move(f.host.create_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "kv")).value();
   auto b = psio1::borrow<psi::db::table>{tbl.handle};

   f.host.upsert(b, to_bytes("a"), to_bytes("1"));
   f.host.upsert(b, to_bytes("b"), to_bytes("2"));
   f.host.upsert(b, to_bytes("c"), to_bytes("3"));

   auto cur = f.host.open_cursor(b);
   auto cb = psio1::borrow<psi::db::cursor>{cur.handle};

   auto sf = f.host.seek_first(cb);
   REQUIRE(sf.has_value());
   CHECK(sf.value() == true);

   auto k = f.host.key(cb);
   REQUIRE(k.has_value());
   CHECK(from_bytes(k.value()) == "a");

   auto v = f.host.value(cb, 0, std::nullopt);
   REQUIRE(v.has_value());
   CHECK(from_bytes(v.value()) == "1");

   auto n1 = f.host.next(cb);
   REQUIRE(n1.has_value());
   CHECK(n1.value() == true);
   CHECK(from_bytes(f.host.key(cb).value()) == "b");

   auto n2 = f.host.next(cb);
   REQUIRE(n2.has_value());
   CHECK(n2.value() == true);
   CHECK(from_bytes(f.host.key(cb).value()) == "c");

   auto n3 = f.host.next(cb);
   REQUIRE(n3.has_value());
   CHECK(n3.value() == false);
   CHECK(f.host.on_row(cb) == false);

   f.host.cursor_drop(psio1::own<psi::db::cursor>{cur.handle});
   f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});
   f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}

TEST_CASE("db_host list_tables", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();
   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});

   auto alpha = std::move(f.host.create_table(psio1::borrow<psi::db::transaction>{tx.handle}, "alpha")).value();
   auto beta  = std::move(f.host.create_table(psio1::borrow<psi::db::transaction>{tx.handle}, "beta")).value();

   f.host.upsert(psio1::borrow<psi::db::table>{alpha.handle}, to_bytes("k1"), to_bytes("v1"));
   f.host.upsert(psio1::borrow<psi::db::table>{beta.handle}, to_bytes("k2"), to_bytes("v2"));

   f.host.table_drop(psio1::own<psi::db::table>{alpha.handle});
   f.host.table_drop(psio1::own<psi::db::table>{beta.handle});

   auto names = f.host.list_tables(psio1::borrow<psi::db::transaction>{tx.handle});
   std::sort(names.begin(), names.end());
   REQUIRE(names.size() == 2);
   CHECK(names[0] == "alpha");
   CHECK(names[1] == "beta");

   f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}

TEST_CASE("db_host remove and remove_range", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();
   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
   auto tbl = std::move(f.host.create_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   auto b = psio1::borrow<psi::db::table>{tbl.handle};

   f.host.upsert(b, to_bytes("a"), to_bytes("1"));
   f.host.upsert(b, to_bytes("b"), to_bytes("2"));
   f.host.upsert(b, to_bytes("c"), to_bytes("3"));
   f.host.upsert(b, to_bytes("d"), to_bytes("4"));

   auto rm = f.host.remove(b, to_bytes("b"));
   REQUIRE(rm.has_value());
   CHECK(rm.value() == true);

   auto rm2 = f.host.remove(b, to_bytes("zzz"));
   REQUIRE(rm2.has_value());
   CHECK(rm2.value() == false);

   auto rr = f.host.remove_range(b, to_bytes("c"), to_bytes("e"));
   REQUIRE(rr.has_value());
   CHECK(rr.value() == 2);

   auto stats = f.host.get_stats(b);
   CHECK(stats.key_count == 1);

   f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});
   f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}

TEST_CASE("db_host slice reads", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();
   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
   auto tbl = std::move(f.host.create_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   auto b = psio1::borrow<psi::db::table>{tbl.handle};

   f.host.upsert(b, to_bytes("k"), to_bytes("hello world"));

   auto full = f.host.get(b, to_bytes("k"), 0, std::nullopt);
   REQUIRE(full.has_value());
   CHECK(from_bytes(full.value()) == "hello world");

   auto slice = f.host.get(b, to_bytes("k"), 6, uint32_t{5});
   REQUIRE(slice.has_value());
   CHECK(from_bytes(slice.value()) == "world");

   auto past_end = f.host.get(b, to_bytes("k"), 100, std::nullopt);
   REQUIRE(past_end.has_value());
   CHECK(past_end.value().empty());

   f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});
   f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}

TEST_CASE("db_host sub-transaction commit propagates to parent", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();
   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
   auto tbl = std::move(f.host.create_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   f.host.upsert(psio1::borrow<psi::db::table>{tbl.handle},
                  to_bytes("a"), to_bytes("1"));
   f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});

   auto sub = f.host.start_sub(psio1::borrow<psi::db::transaction>{tx.handle});
   REQUIRE(sub.handle != psizam::handle_table<psiserve::transaction_impl>::invalid_handle);

   auto tbl2 = std::move(f.host.open_table(
       psio1::borrow<psi::db::transaction>{sub.handle}, "t")).value();
   f.host.upsert(psio1::borrow<psi::db::table>{tbl2.handle},
                  to_bytes("b"), to_bytes("2"));
   f.host.table_drop(psio1::own<psi::db::table>{tbl2.handle});

   auto commit_sub = f.host.commit(psio1::borrow<psi::db::transaction>{sub.handle});
   REQUIRE(commit_sub.has_value());
   f.host.transaction_drop(psio1::own<psi::db::transaction>{sub.handle});

   auto tbl3 = std::move(f.host.open_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   auto b_val = f.host.get(psio1::borrow<psi::db::table>{tbl3.handle},
                           to_bytes("b"), 0, std::nullopt);
   REQUIRE(b_val.has_value());
   CHECK(from_bytes(b_val.value()) == "2");
   f.host.table_drop(psio1::own<psi::db::table>{tbl3.handle});

   f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}

TEST_CASE("db_host sub-transaction abort discards changes", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();
   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
   auto tbl = std::move(f.host.create_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   f.host.upsert(psio1::borrow<psi::db::table>{tbl.handle},
                  to_bytes("a"), to_bytes("1"));
   f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});

   {
      auto sub = f.host.start_sub(psio1::borrow<psi::db::transaction>{tx.handle});
      auto tbl2 = std::move(f.host.open_table(
          psio1::borrow<psi::db::transaction>{sub.handle}, "t")).value();
      f.host.upsert(psio1::borrow<psi::db::table>{tbl2.handle},
                     to_bytes("b"), to_bytes("BAD"));
      f.host.table_drop(psio1::own<psi::db::table>{tbl2.handle});
      f.host.abort(psio1::borrow<psi::db::transaction>{sub.handle});
      f.host.transaction_drop(psio1::own<psi::db::transaction>{sub.handle});
   }

   auto tbl3 = std::move(f.host.open_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   auto b_val = f.host.get(psio1::borrow<psi::db::table>{tbl3.handle},
                           to_bytes("b"), 0, std::nullopt);
   CHECK(!b_val.has_value());
   CHECK(b_val.error() == error::not_found);

   auto a_val = f.host.get(psio1::borrow<psi::db::table>{tbl3.handle},
                           to_bytes("a"), 0, std::nullopt);
   REQUIRE(a_val.has_value());
   CHECK(from_bytes(a_val.value()) == "1");

   f.host.table_drop(psio1::own<psi::db::table>{tbl3.handle});
   f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}

TEST_CASE("db_host nested sub-transactions", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();
   auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
   auto tbl = std::move(f.host.create_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   f.host.upsert(psio1::borrow<psi::db::table>{tbl.handle},
                  to_bytes("a"), to_bytes("1"));
   f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});

   auto sub1 = f.host.start_sub(psio1::borrow<psi::db::transaction>{tx.handle});
   {
      auto t1 = std::move(f.host.open_table(
          psio1::borrow<psi::db::transaction>{sub1.handle}, "t")).value();
      f.host.upsert(psio1::borrow<psi::db::table>{t1.handle},
                     to_bytes("b"), to_bytes("2"));
      f.host.table_drop(psio1::own<psi::db::table>{t1.handle});
   }

   auto sub2 = f.host.start_sub(psio1::borrow<psi::db::transaction>{sub1.handle});
   {
      auto t2 = std::move(f.host.open_table(
          psio1::borrow<psi::db::transaction>{sub2.handle}, "t")).value();
      f.host.upsert(psio1::borrow<psi::db::table>{t2.handle},
                     to_bytes("c"), to_bytes("3"));
      f.host.table_drop(psio1::own<psi::db::table>{t2.handle});
   }
   f.host.abort(psio1::borrow<psi::db::transaction>{sub2.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{sub2.handle});

   f.host.commit(psio1::borrow<psi::db::transaction>{sub1.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{sub1.handle});

   auto tbl_chk = std::move(f.host.open_table(
       psio1::borrow<psi::db::transaction>{tx.handle}, "t")).value();
   auto b_val = f.host.get(psio1::borrow<psi::db::table>{tbl_chk.handle},
                           to_bytes("b"), 0, std::nullopt);
   REQUIRE(b_val.has_value());
   CHECK(from_bytes(b_val.value()) == "2");

   auto c_val = f.host.get(psio1::borrow<psi::db::table>{tbl_chk.handle},
                           to_bytes("c"), 0, std::nullopt);
   CHECK(!c_val.has_value());

   f.host.table_drop(psio1::own<psi::db::table>{tbl_chk.handle});
   f.host.commit(psio1::borrow<psi::db::transaction>{tx.handle});
   f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}

TEST_CASE("db_host abort discards writes", "[db_host]")
{
   test_fixture f;

   auto db = std::move(f.host.open("testdb")).value();

   {
      auto tx = f.host.start_write(psio1::borrow<psi::db::database>{db.handle});
      auto tbl = std::move(f.host.create_table(
          psio1::borrow<psi::db::transaction>{tx.handle}, "ephemeral")).value();
      f.host.upsert(
          psio1::borrow<psi::db::table>{tbl.handle},
          to_bytes("x"), to_bytes("y"));
      f.host.table_drop(psio1::own<psi::db::table>{tbl.handle});
      f.host.abort(psio1::borrow<psi::db::transaction>{tx.handle});
      f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   }

   {
      auto tx = f.host.start_read(psio1::borrow<psi::db::database>{db.handle}, 0);
      auto tbl_r = f.host.open_table(
          psio1::borrow<psi::db::transaction>{tx.handle}, "ephemeral");
      CHECK(!tbl_r.has_value());
      CHECK(tbl_r.error() == error::not_found);
      f.host.transaction_drop(psio1::own<psi::db::transaction>{tx.handle});
   }

   f.host.database_drop(psio1::own<psi::db::database>{db.handle});
}
