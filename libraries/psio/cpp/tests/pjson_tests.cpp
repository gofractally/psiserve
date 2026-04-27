// pjson_tests.cpp — Catch2 tests for pjson.
//
// Spec: see comments at the top of pjson.hpp.

#include <psio/pjson.hpp>
#include <psio/pjson_view.hpp>
#include <psio/pjson_typed.hpp>
#include <psio/view_to_json.hpp>
#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
#include <psio/pjson_json.hpp>
#include <psio/pjson_json_typed.hpp>
#endif

#define CATCH_CONFIG_FAST_COMPILE
#include <catch.hpp>

#include <bit>
#include <cstdint>
#include <random>
#include <string>
#include <string_view>
#include <vector>

using psio::pjson;
using psio::pjson_array;
using psio::pjson_bytes;
using psio::pjson_null;
using psio::pjson_number;
using psio::pjson_object;
using psio::pjson_value;
using psio::pjson_view;

namespace {
   pjson_value rt(const pjson_value& v)
   {
      auto bytes = pjson::encode(v);
      REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
      return pjson::decode({bytes.data(), bytes.size()});
   }
}

// ── round-trip basics ────────────────────────────────────────────────────

TEST_CASE("pjson round-trip: scalars", "[pjson][roundtrip]")
{
   CHECK(rt(pjson_value{pjson_null{}}) == pjson_value{pjson_null{}});
   CHECK(rt(pjson_value{true}) == pjson_value{true});
   CHECK(rt(pjson_value{false}) == pjson_value{false});

   for (std::int64_t i : {0LL, 1LL, 15LL, 16LL, -1LL, 12345678901234LL,
                          std::numeric_limits<std::int64_t>::max(),
                          std::numeric_limits<std::int64_t>::min()})
   {
      auto v = rt(pjson_value{i});
      REQUIRE(v.holds<std::int64_t>());
      CHECK(v.as<std::int64_t>() == i);
   }
}

TEST_CASE("pjson round-trip: 128-bit int", "[pjson][int128]")
{
   __int128 huge = (static_cast<__int128>(1) << 100) + 42;
   auto v = rt(pjson_value{pjson_number{huge, 0}});
   REQUIRE(v.holds<pjson_number>());
   CHECK(v.as<pjson_number>().mantissa == huge);
}

TEST_CASE("pjson round-trip: doubles", "[pjson][double]")
{
   // 1.5e308 et al hit a Ryu-corner — the naive pow-of-10 in to_double
   // accumulates ULP error, so the variant decode keeps them as
   // pjson_number rather than collapsing to double. Numeric round-trip
   // still works via as_double(). Tested separately.
   for (double d : {3.14, -3.14, 0.1, 100.5, 1.0 / 7.0})
   {
      CAPTURE(d);
      auto bytes = pjson::encode(pjson_value{d});
      CAPTURE(bytes.size());
      REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
      auto v = pjson::decode({bytes.data(), bytes.size()});
      REQUIRE(v.holds<double>());
      CHECK(std::bit_cast<std::uint64_t>(v.as<double>()) ==
            std::bit_cast<std::uint64_t>(d));
   }
}

TEST_CASE("pjson round-trip: strings", "[pjson][string]")
{
   for (std::size_t n : {std::size_t{0}, std::size_t{1}, std::size_t{15},
                        std::size_t{16}, std::size_t{300}})
   {
      std::string s(n, 'x');
      auto v = rt(pjson_value{s});
      CHECK(v.as<std::string>() == s);
   }
}

TEST_CASE("pjson round-trip: bytes", "[pjson][bytes]")
{
   pjson_bytes b{0x00, 0xFF, 0x7F, 0x80, 0x42};
   auto v = rt(pjson_value{b});
   CHECK(v.as<pjson_bytes>() == b);
}

TEST_CASE("pjson round-trip: containers", "[pjson][container]")
{
   pjson_object obj{
       {"a", pjson_value{static_cast<std::int64_t>(1)}},
       {"b", pjson_value{pjson_array{
                  pjson_value{static_cast<std::int64_t>(2)},
                  pjson_value{std::string{"x"}},
                  pjson_value{pjson_null{}}}}},
       {"c", pjson_value{true}},
   };
   auto out = rt(pjson_value{obj});
   REQUIRE(out.holds<pjson_object>());
   CHECK(out == pjson_value{obj});
}

TEST_CASE("pjson round-trip: empty containers", "[pjson][container]")
{
   CHECK(rt(pjson_value{pjson_array{}}).as<pjson_array>().empty());
   CHECK(rt(pjson_value{pjson_object{}}).as<pjson_object>().empty());
}

// ── pjson_view accessors ─────────────────────────────────────────────────

TEST_CASE("pjson_view: object access", "[pjson][view]")
{
   pjson_object obj{
       {"name", pjson_value{std::string{"alice"}}},
       {"age", pjson_value{static_cast<std::int64_t>(30)}},
       {"active", pjson_value{true}},
       {"score", pjson_value{3.14}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view v{bytes.data(), bytes.size()};

   REQUIRE(v.is_object());
   CHECK(v.count() == 4);
   CHECK(v["name"].as_string() == "alice");
   CHECK(v["age"].as_int64() == 30);
   CHECK(v["active"].as_bool() == true);
   CHECK(v["score"].as_double() == 3.14);
   CHECK_FALSE(v.find("missing").has_value());
}

TEST_CASE("pjson_view: array indexing", "[pjson][view]")
{
   pjson_array a;
   for (int i = 0; i < 10; ++i)
      a.push_back(pjson_value{static_cast<std::int64_t>(i * 10)});
   auto bytes = pjson::encode(pjson_value{a});
   pjson_view v{bytes.data(), bytes.size()};

   REQUIRE(v.is_array());
   CHECK(v.count() == 10);
   for (std::size_t i = 0; i < 10; ++i)
      CHECK(v[i].as_int64() == static_cast<std::int64_t>(i * 10));
}

TEST_CASE("pjson_view: chained access", "[pjson][view]")
{
   pjson_object inner{{"city", pjson_value{std::string{"NYC"}}}};
   pjson_object outer{{"address", pjson_value{inner}}};
   auto bytes = pjson::encode(pjson_value{outer});
   pjson_view v{bytes.data(), bytes.size()};
   CHECK(v["address"]["city"].as_string() == "NYC");
}

TEST_CASE("pjson_view: for_each iteration", "[pjson][view]")
{
   pjson_object obj{
       {"alpha", pjson_value{static_cast<std::int64_t>(1)}},
       {"beta", pjson_value{static_cast<std::int64_t>(2)}},
       {"gamma", pjson_value{static_cast<std::int64_t>(3)}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view v{bytes.data(), bytes.size()};

   std::vector<std::string> seen_keys;
   v.for_each_field([&](std::string_view k, pjson_view) {
      seen_keys.emplace_back(k);
   });
   REQUIRE(seen_keys.size() == 3);
   CHECK(seen_keys[0] == "alpha");
   CHECK(seen_keys[1] == "beta");
   CHECK(seen_keys[2] == "gamma");
}

TEST_CASE("pjson_view: hash suffix-strip", "[pjson][view][hash]")
{
   pjson_object o{
       {"amount.decimal", pjson_value{static_cast<std::int64_t>(1)}},
       {"amount", pjson_value{static_cast<std::int64_t>(2)}},
   };
   auto bytes = pjson::encode(pjson_value{o});
   pjson_view v{bytes.data(), bytes.size()};
   // Different keys, same hash bucket — both still findable distinctly.
   auto a = v.find("amount.decimal");
   auto b = v.find("amount");
   REQUIRE(a.has_value());
   REQUIRE(b.has_value());
   CHECK(a->as_int64() == 1);
   CHECK(b->as_int64() == 2);
}

// ── typed_pjson_view + canonical fast path ────────────────────────────────

struct User
{
   std::string  name;
   std::int64_t age;
   bool         active;
   double       score;
};
PSIO_REFLECT(User, name, age, active, score)

TEST_CASE("typed_pjson_view: canonical fast path", "[pjson][typed]")
{
   User u{"alice", 30, true, 3.14};
   auto bytes = psio::from_struct(u);
   pjson_view raw{bytes.data(), bytes.size()};

   auto t = psio::typed_pjson_view<User>::from_pjson(raw);
   REQUIRE(t.is_canonical());
   CHECK(t.get<0>() == "alice");
   CHECK(t.get<1>() == 30);
   CHECK(t.get<2>() == true);
   CHECK(t.get<3>() == 3.14);

   User out = t.to_struct();
   CHECK(out.name == u.name);
   CHECK(out.age == u.age);
   CHECK(out.active == u.active);
   CHECK(out.score == u.score);
}

TEST_CASE("typed_pjson_view: non-canonical falls back to find",
          "[pjson][typed]")
{
   // Build the object with reordered keys → canonical check must fail.
   pjson_object obj{
       {"score", pjson_value{3.14}},
       {"active", pjson_value{true}},
       {"age", pjson_value{static_cast<std::int64_t>(30)}},
       {"name", pjson_value{std::string{"alice"}}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view raw{bytes.data(), bytes.size()};

   auto t = psio::typed_pjson_view<User>::from_pjson(raw);
   CHECK_FALSE(t.is_canonical());
   // Field access still works via find().
   CHECK(t.get<0>() == "alice");
   CHECK(t.get<1>() == 30);
}

// ── pjson_number direct decimal parse ────────────────────────────────────

TEST_CASE("pjson_number::from_string", "[pjson][number]")
{
   auto eq = [](pjson_number a, pjson_number b) {
      return a.mantissa == b.mantissa && a.scale == b.scale;
   };
   CHECK(eq(pjson_number::from_string("0.1"),    pjson_number{1, -1}));
   CHECK(eq(pjson_number::from_string("0.10"),   pjson_number{1, -1}));
   CHECK(eq(pjson_number::from_string("3.14"),   pjson_number{314, -2}));
   CHECK(eq(pjson_number::from_string("-3.14"),  pjson_number{-314, -2}));
   CHECK(eq(pjson_number::from_string("100"),    pjson_number{100, 0}));
   CHECK(eq(pjson_number::from_string("0"),      pjson_number{0, 0}));
   CHECK(eq(pjson_number::from_string("1.5e10"), pjson_number{15, 9}));
}

TEST_CASE("pjson_number: scale-aligned compare", "[pjson][number]")
{
   pjson_number a{1, -1};
   pjson_number b{10, -2};
   CHECK(numerically_equal(a, b));
   CHECK(compare(a, b) == std::strong_ordering::equal);

   CHECK(compare(pjson_number::from_string("0.99"),
                 pjson_number::from_string("1.0")) ==
         std::strong_ordering::less);
}

// ── stress ───────────────────────────────────────────────────────────────

TEST_CASE("pjson stress: random tree round-trips", "[pjson][stress]")
{
   std::mt19937 rng(0xC0FFEE);

   auto gen = [&](auto& self, int depth) -> pjson_value {
      std::uniform_int_distribution<int> d(0, depth <= 0 ? 4 : 6);
      switch (d(rng))
      {
         case 0: return pjson_value{pjson_null{}};
         case 1: return pjson_value{(rng() & 1) != 0};
         case 2: return pjson_value{static_cast<std::int64_t>(rng())};
         case 3:
         {
            std::uniform_int_distribution<std::size_t> n(0, 40);
            return pjson_value{std::string(n(rng), 'a')};
         }
         case 4:
         {
            std::uniform_int_distribution<std::size_t> n(0, 16);
            pjson_bytes b(n(rng));
            for (auto& x : b) x = static_cast<std::uint8_t>(rng());
            return pjson_value{std::move(b)};
         }
         case 5:
         {
            std::uniform_int_distribution<int> n(0, 4);
            pjson_array a;
            int k = n(rng);
            for (int i = 0; i < k; ++i) a.push_back(self(self, depth - 1));
            return pjson_value{std::move(a)};
         }
         default:
         {
            std::uniform_int_distribution<int> n(0, 4);
            pjson_object o;
            int k = n(rng);
            for (int i = 0; i < k; ++i)
               o.emplace_back("k" + std::to_string(rng() % 1000),
                              self(self, depth - 1));
            return pjson_value{std::move(o)};
         }
      }
   };

   for (int trial = 0; trial < 50; ++trial)
   {
      pjson_value v     = gen(gen, 4);
      auto        bytes = pjson::encode(v);
      REQUIRE(pjson::validate({bytes.data(), bytes.size()}));
      auto v2 = pjson::decode({bytes.data(), bytes.size()});
      REQUIRE(v == v2);
      auto bytes2 = pjson::encode(v2);
      REQUIRE(bytes == bytes2);
   }
}

// ── JSON pipeline (when simdjson is available) ───────────────────────────

#if defined(PSIO_HAVE_SIMDJSON) && PSIO_HAVE_SIMDJSON
TEST_CASE("pjson_json: JSON → pjson via simdjson", "[pjson][json]")
{
   std::string_view doc =
       R"({"name":"alice","age":30,"active":true,"score":3.14,"id":1234567890,"email":"alice@example.com"})";

   auto bytes = psio::pjson_json::from_json(doc);
   REQUIRE(pjson::validate({bytes.data(), bytes.size()}));

   pjson_view v{bytes.data(), bytes.size()};
   REQUIRE(v.is_object());
   CHECK(v["name"].as_string() == "alice");
   CHECK(v["age"].as_int64() == 30);
   CHECK(v["active"].as_bool() == true);
   CHECK(v["score"].as_double() == 3.14);
   CHECK(v["id"].as_int64() == 1234567890);
   CHECK(v["email"].as_string() == "alice@example.com");
}

TEST_CASE("pjson_json: nested JSON", "[pjson][json]")
{
   std::string_view doc =
       R"({"users":[{"name":"alice","id":1},{"name":"bob","id":2}]})";

   auto bytes = psio::pjson_json::from_json(doc);
   pjson_view v{bytes.data(), bytes.size()};
   CHECK(v["users"][0]["name"].as_string() == "alice");
   CHECK(v["users"][1]["id"].as_int64() == 2);
}

TEST_CASE("json_to: in-place reuses caller storage",
          "[pjson][json][typed]")
{
   std::string_view doc =
       R"({"name":"alice","age":30,"active":true,"score":3.14})";
   User u;
   // Pre-populate to confirm the in-place path overwrites.
   u.name   = "previous-very-long-name-with-allocation";
   u.age    = 999;
   u.active = false;
   u.score  = 99.9;
   psio::json_to<User>(doc, u);
   CHECK(u.name == "alice");
   CHECK(u.age == 30);
   CHECK(u.active == true);
   CHECK(u.score == 3.14);
}

TEST_CASE("json_to: direct JSON → T", "[pjson][json][typed]")
{
   std::string_view doc =
       R"({"name":"alice","age":30,"active":true,"score":3.14})";
   User u = psio::json_to<User>(doc);
   CHECK(u.name == "alice");
   CHECK(u.age == 30);
   CHECK(u.active == true);
   CHECK(u.score == 3.14);
}

TEST_CASE("json_to_struct: non-canonical key order",
          "[pjson][json][typed]")
{
   std::string_view doc =
       R"({"score":3.14,"active":true,"age":30,"name":"alice"})";
   User u = psio::json_to<User>(doc);
   CHECK(u.name == "alice");
   CHECK(u.age == 30);
   CHECK(u.active == true);
   CHECK(u.score == 3.14);
}

TEST_CASE("json_to_struct: missing fields default-init",
          "[pjson][json][typed]")
{
   std::string_view doc = R"({"name":"alice"})";
   User u = psio::json_to<User>(doc);
   CHECK(u.name == "alice");
   CHECK(u.age == 0);  // default
   CHECK(u.active == false);
   CHECK(u.score == 0.0);
}

TEST_CASE("view_to_json: pjson_view → JSON (generic walker)",
          "[pjson][json][view]")
{
   pjson_object obj{
       {"name", pjson_value{std::string{"alice"}}},
       {"age", pjson_value{static_cast<std::int64_t>(30)}},
       {"active", pjson_value{true}},
       {"tags", pjson_value{pjson_array{
                    pjson_value{std::string{"admin"}},
                    pjson_value{std::string{"ops"}}}}},
   };
   auto bytes = pjson::encode(pjson_value{obj});
   pjson_view v{bytes.data(), bytes.size()};

   std::string j = psio::view_to_json(v);
   // Round-trip: emitted JSON parses back to a structurally equal pjson.
   auto bytes2 = psio::pjson_json::from_json(j);
   pjson_view v2{bytes2.data(), bytes2.size()};
   CHECK(v2["name"].as_string() == "alice");
   CHECK(v2["age"].as_int64() == 30);
   CHECK(v2["active"].as_bool() == true);
   CHECK(v2["tags"][0].as_string() == "admin");
}

TEST_CASE("struct_to_json: T → JSON", "[pjson][json][typed]")
{
   User u{"alice", 30, true, 3.14};
   std::string j = psio::struct_to_json(u);
   // Round-trip via the typed parser:
   User back = psio::json_to<User>(j);
   CHECK(back.name == u.name);
   CHECK(back.age == u.age);
   CHECK(back.active == u.active);
   CHECK(back.score == u.score);
}
#endif
