// Phase 14c — transcode free function.
//
// Exercises the compile-time T path across every pair of currently-
// implemented formats (ssz, pssz16, pssz32, json, frac16, frac32, bin).

#include <psio3/bin.hpp>
#include <psio3/frac.hpp>
#include <psio3/json.hpp>
#include <psio3/pssz.hpp>
#include <psio3/reflect.hpp>
#include <psio3/ssz.hpp>
#include <psio3/transcode.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

struct XcodePoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO3_REFLECT(XcodePoint, x, y)

struct XcodePerson
{
   std::string                name;
   std::int32_t               age;
   std::vector<std::uint32_t> scores;
};
PSIO3_REFLECT(XcodePerson, name, age, scores)

TEST_CASE("transcode<T> ssz → json produces equivalent JSON",
          "[transcode][ssz][json]")
{
   XcodePoint p{7, 9};
   auto       src = psio3::encode(psio3::ssz{}, p);

   auto json_out = psio3::transcode<XcodePoint>(
      psio3::ssz{}, std::span<const char>{src}, psio3::json{});

   REQUIRE(json_out == R"({"x":7,"y":9})");
}

TEST_CASE("transcode<T> json → ssz round-trips through T",
          "[transcode][json][ssz]")
{
   std::string src_json = R"({"x":11,"y":-5})";
   auto        ssz_out  = psio3::transcode<XcodePoint>(
      psio3::json{}, std::span<const char>{src_json}, psio3::ssz{});

   auto back = psio3::decode<XcodePoint>(psio3::ssz{},
                                          std::span<const char>{ssz_out});
   REQUIRE(back.x == 11);
   REQUIRE(back.y == -5);
}

TEST_CASE("transcode<T> frac → pssz preserves the value",
          "[transcode][frac][pssz]")
{
   XcodePerson pp{"carol", 40, {9, 8, 7}};
   auto        src = psio3::encode(psio3::frac32{}, pp);
   auto        dst = psio3::transcode<XcodePerson>(
      psio3::frac32{}, std::span<const char>{src}, psio3::pssz16{});

   auto back =
      psio3::decode<XcodePerson>(psio3::pssz16{}, std::span<const char>{dst});
   REQUIRE(back.name == "carol");
   REQUIRE(back.age == 40);
   REQUIRE(back.scores == std::vector<std::uint32_t>{9, 8, 7});
}

TEST_CASE("transcode<T> through every format pair preserves the record",
          "[transcode][cross]")
{
   XcodePerson pp{"dave", 32, {1, 2, 3}};

   // Original SSZ encoding.
   auto ssz_src = psio3::encode(psio3::ssz{}, pp);

   // SSZ → bin → frac32 → pssz32 → json and back to a value.
   auto bin_bytes = psio3::transcode<XcodePerson>(
      psio3::ssz{}, std::span<const char>{ssz_src}, psio3::bin{});
   auto frac_bytes = psio3::transcode<XcodePerson>(
      psio3::bin{}, std::span<const char>{bin_bytes}, psio3::frac32{});
   auto pssz_bytes = psio3::transcode<XcodePerson>(
      psio3::frac32{}, std::span<const char>{frac_bytes}, psio3::pssz32{});
   auto json_str = psio3::transcode<XcodePerson>(
      psio3::pssz32{}, std::span<const char>{pssz_bytes}, psio3::json{});

   auto back = psio3::decode<XcodePerson>(psio3::json{},
                                           std::span<const char>{json_str});
   REQUIRE(back.name == "dave");
   REQUIRE(back.age == 32);
   REQUIRE(back.scores == std::vector<std::uint32_t>{1, 2, 3});
}

TEST_CASE("transcode_via_json round-trips a dynamic value through itself",
          "[transcode][dynamic]")
{
   std::string src = R"({"x":42,"y":99})";
   auto        sc  = psio3::schema_of<XcodePoint>();
   auto        out = psio3::transcode_via_json(sc, std::span<const char>{src});
   REQUIRE(out == R"({"x":42,"y":99})");
}

TEST_CASE("transcode_ssz_to_json converts SSZ → JSON via runtime schema",
          "[transcode][dynamic][ssz][json]")
{
   XcodePoint p{101, -7};
   auto       ssz_bytes = psio3::encode(psio3::ssz{}, p);

   auto sc       = psio3::schema_of<XcodePoint>();
   auto json_str = psio3::transcode_ssz_to_json(
      sc, std::span<const char>{ssz_bytes});

   REQUIRE(json_str == R"({"x":101,"y":-7})");
}

TEST_CASE("transcode_json_to_ssz converts JSON → SSZ via runtime schema",
          "[transcode][dynamic][ssz][json]")
{
   std::string src   = R"({"x":55,"y":-33})";
   auto        sc    = psio3::schema_of<XcodePoint>();
   auto        bytes = psio3::transcode_json_to_ssz(
      sc, std::span<const char>{src});

   auto back = psio3::decode<XcodePoint>(psio3::ssz{},
                                          std::span<const char>{bytes});
   REQUIRE(back.x == 55);
   REQUIRE(back.y == -33);
}

TEST_CASE("runtime-schema SSZ↔JSON round-trips a record with variable fields",
          "[transcode][dynamic][ssz][json][variable]")
{
   XcodePerson pp{"eve", 29, {7, 11, 13}};

   auto ssz_bytes = psio3::encode(psio3::ssz{}, pp);
   auto sc        = psio3::schema_of<XcodePerson>();
   auto as_json   = psio3::transcode_ssz_to_json(
      sc, std::span<const char>{ssz_bytes});
   auto ssz_again = psio3::transcode_json_to_ssz(
      sc, std::span<const char>{as_json});

   auto back =
      psio3::decode<XcodePerson>(psio3::ssz{},
                                  std::span<const char>{ssz_again});
   REQUIRE(back.name == "eve");
   REQUIRE(back.age == 29);
   REQUIRE(back.scores == std::vector<std::uint32_t>{7, 11, 13});
}

TEST_CASE("generic psio3::transcode(ssz → json) dispatches via CPOs",
          "[transcode][cpo][dynamic]")
{
   XcodePoint p{12, 34};
   auto       ssz_bytes = psio3::encode(psio3::ssz{}, p);
   auto       sc        = psio3::schema_of<XcodePoint>();

   auto js = psio3::transcode(psio3::ssz{}, sc,
                              std::span<const char>{ssz_bytes},
                              psio3::json{});
   REQUIRE(js == R"({"x":12,"y":34})");
}

TEST_CASE("generic psio3::transcode(json → ssz) dispatches via CPOs",
          "[transcode][cpo][dynamic]")
{
   std::string src = R"({"x":44,"y":-11})";
   auto        sc  = psio3::schema_of<XcodePoint>();

   auto bytes = psio3::transcode(psio3::json{}, sc,
                                 std::span<const char>{src}, psio3::ssz{});
   auto back  = psio3::decode<XcodePoint>(psio3::ssz{},
                                           std::span<const char>{bytes});
   REQUIRE(back.x == 44);
   REQUIRE(back.y == -11);
}

TEST_CASE("generic transcode chains ssz → json → ssz via CPOs",
          "[transcode][cpo][dynamic][variable]")
{
   XcodePerson pp{"frank", 41, {100, 200}};
   auto        src = psio3::encode(psio3::ssz{}, pp);
   auto        sc  = psio3::schema_of<XcodePerson>();

   auto js = psio3::transcode(psio3::ssz{}, sc,
                              std::span<const char>{src}, psio3::json{});
   auto back_bytes = psio3::transcode(psio3::json{}, sc,
                                       std::span<const char>{js},
                                       psio3::ssz{});
   auto back =
      psio3::decode<XcodePerson>(psio3::ssz{},
                                  std::span<const char>{back_bytes});

   REQUIRE(back.name == "frank");
   REQUIRE(back.age == 41);
   REQUIRE(back.scores == std::vector<std::uint32_t>{100, 200});
}

TEST_CASE("generic transcode works for bin", "[transcode][cpo][dynamic][bin]")
{
   XcodePerson pp{"grace", 52, {10, 20, 30}};
   auto        bin_bytes = psio3::encode(psio3::bin{}, pp);
   auto        sc        = psio3::schema_of<XcodePerson>();

   auto as_json  = psio3::transcode(psio3::bin{}, sc,
                                    std::span<const char>{bin_bytes},
                                    psio3::json{});
   auto as_ssz   = psio3::transcode(psio3::json{}, sc,
                                    std::span<const char>{as_json},
                                    psio3::ssz{});
   auto as_bin2  = psio3::transcode(psio3::ssz{}, sc,
                                    std::span<const char>{as_ssz},
                                    psio3::bin{});

   auto back = psio3::decode<XcodePerson>(psio3::bin{},
                                           std::span<const char>{as_bin2});
   REQUIRE(back.name == "grace");
   REQUIRE(back.age == 52);
   REQUIRE(back.scores == std::vector<std::uint32_t>{10, 20, 30});
}

TEST_CASE("generic transcode works for frac32", "[transcode][cpo][dynamic][frac]")
{
   XcodePerson pp{"heidi", 28, {1, 2}};
   auto        sc = psio3::schema_of<XcodePerson>();

   // frac32 → json → frac32 round-trip.
   auto frac_bytes = psio3::encode(psio3::frac32{}, pp);
   auto json_str   = psio3::transcode(psio3::frac32{}, sc,
                                      std::span<const char>{frac_bytes},
                                      psio3::json{});
   auto frac_back  = psio3::transcode(psio3::json{}, sc,
                                      std::span<const char>{json_str},
                                      psio3::frac32{});

   auto back = psio3::decode<XcodePerson>(
      psio3::frac32{}, std::span<const char>{frac_back});
   REQUIRE(back.name == "heidi");
   REQUIRE(back.age == 28);
   REQUIRE(back.scores == std::vector<std::uint32_t>{1, 2});
}

TEST_CASE("generic transcode chains every format pair via dynamic",
          "[transcode][cpo][dynamic][cross]")
{
   XcodePerson pp{"ivan", 60, {99}};
   auto        sc = psio3::schema_of<XcodePerson>();

   // Start with pssz32 bytes, route through json, ssz, bin, frac32,
   // and back to pssz32 — every format's dynamic codec in the mix.
   auto a = psio3::encode(psio3::pssz32{}, pp);
   auto b = psio3::transcode(psio3::pssz32{}, sc, std::span<const char>{a},
                             psio3::json{});
   auto c = psio3::transcode(psio3::json{}, sc, std::span<const char>{b},
                             psio3::ssz{});
   auto d = psio3::transcode(psio3::ssz{}, sc, std::span<const char>{c},
                             psio3::bin{});
   auto e = psio3::transcode(psio3::bin{}, sc, std::span<const char>{d},
                             psio3::frac32{});
   auto f = psio3::transcode(psio3::frac32{}, sc, std::span<const char>{e},
                             psio3::pssz32{});

   auto back = psio3::decode<XcodePerson>(psio3::pssz32{},
                                           std::span<const char>{f});
   REQUIRE(back.name == "ivan");
   REQUIRE(back.age == 60);
   REQUIRE(back.scores == std::vector<std::uint32_t>{99});
}
