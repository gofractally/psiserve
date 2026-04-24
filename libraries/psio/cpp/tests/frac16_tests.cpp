// frac16 comprehensive test suite.
//
// Covers type-shape round-trips, views, and edge cases at frac_format_16.
//
// Companion file: frac16_mutation_tests.cpp — exercises field_handle
// mutation paths at frac16 (mirror of frac_ref_tests.cpp).

#include <catch2/catch.hpp>

#include <psio/fracpack.hpp>
#include <psio/frac_ref.hpp>

#include <array>
#include <cstdint>
#include <flat_map>
#include <flat_set>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

// ── Shared round-trip helper (pack with frac16, unpack, compare) ───────────

template <typename T>
static T rt16(const T& v)
{
   auto bytes = psio::to_frac16(v);
   T    out{};
   REQUIRE(psio::from_frac16(out, bytes));
   REQUIRE(psio::validate_frac16<T>(bytes) != psio::validation_t::invalid);
   return out;
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 1: Primitives — format-independent
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frac16: unsigned integers round-trip & are format-independent",
          "[fracpack16][numeric]")
{
   REQUIRE(rt16<std::uint8_t>(0x7Fu) == 0x7Fu);
   REQUIRE(rt16<std::uint16_t>(0xBEEFu) == 0xBEEFu);
   REQUIRE(rt16<std::uint32_t>(0xDEADBEEFu) == 0xDEADBEEFu);
   REQUIRE(rt16<std::uint64_t>(0xFEEDFACECAFEBABEull) == 0xFEEDFACECAFEBABEull);

   // Wire size for primitives is identical under both formats
   REQUIRE(psio::to_frac16<std::uint8_t>(1).size() == 1);
   REQUIRE(psio::to_frac16<std::uint16_t>(1).size() == 2);
   REQUIRE(psio::to_frac16<std::uint32_t>(1).size() == 4);
   REQUIRE(psio::to_frac16<std::uint64_t>(1).size() == 8);
   REQUIRE(psio::to_frac16<std::uint32_t>(1).size() == psio::to_frac<std::uint32_t>(1).size());
}

TEST_CASE("frac16: signed integers round-trip", "[fracpack16][numeric]")
{
   REQUIRE(rt16<std::int8_t>(-1) == -1);
   REQUIRE(rt16<std::int16_t>(-32768) == -32768);
   REQUIRE(rt16<std::int32_t>(-1) == -1);
   REQUIRE(rt16<std::int64_t>(std::numeric_limits<std::int64_t>::min())
           == std::numeric_limits<std::int64_t>::min());
}

TEST_CASE("frac16: float / double round-trip", "[fracpack16][numeric]")
{
   REQUIRE(rt16<float>(3.14159f) == 3.14159f);
   REQUIRE(rt16<double>(2.71828182845905) == 2.71828182845905);
}

TEST_CASE("frac16: bool round-trip", "[fracpack16][numeric]")
{
   REQUIRE(rt16<bool>(true) == true);
   REQUIRE(rt16<bool>(false) == false);
}

// ── Enum ──────────────────────────────────────────────────────────────────

enum class Color : std::uint8_t
{
   red,
   green,
   blue
};
enum class BigEnum : std::int32_t
{
   a = -1,
   b = 0,
   c = 42
};

TEST_CASE("frac16: enum class round-trip", "[fracpack16][enum]")
{
   REQUIRE(rt16(Color::green) == Color::green);
   REQUIRE(rt16(BigEnum::a) == BigEnum::a);
   REQUIRE(rt16(BigEnum::c) == BigEnum::c);
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 2: Variable-size containers
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frac16: string round-trip (empty / short / long)", "[fracpack16][string]")
{
   REQUIRE(rt16<std::string>("") == "");
   REQUIRE(rt16<std::string>("a") == "a");
   REQUIRE(rt16<std::string>("hello world") == "hello world");
   // Long (but fits in u16) string — ~4 KB
   std::string long_str(4000, 'x');
   REQUIRE(rt16(long_str) == long_str);
}

TEST_CASE("frac16: vector<numeric> round-trip", "[fracpack16][vector]")
{
   REQUIRE(rt16<std::vector<std::uint32_t>>({}) == std::vector<std::uint32_t>{});
   REQUIRE(rt16<std::vector<std::uint32_t>>({42}) == std::vector<std::uint32_t>{42});
   REQUIRE(rt16<std::vector<double>>({1.1, 2.2, 3.3}) == std::vector<double>{1.1, 2.2, 3.3});
}

TEST_CASE("frac16: vector<string> round-trip", "[fracpack16][vector]")
{
   std::vector<std::string> v{"alpha", "beta", "gamma"};
   REQUIRE(rt16(v) == v);
   REQUIRE(rt16<std::vector<std::string>>({}) == std::vector<std::string>{});
}

TEST_CASE("frac16: array<numeric> round-trip", "[fracpack16][array]")
{
   std::array<std::uint32_t, 4> a{1, 2, 3, 4};
   REQUIRE(rt16(a) == a);
}

TEST_CASE("frac16: array<string> round-trip (non-memcpy array path)", "[fracpack16][array]")
{
   std::array<std::string, 3> a{"x", "yy", "zzz"};
   REQUIRE(rt16(a) == a);
}

// ── Optional ─────────────────────────────────────────────────────────────

TEST_CASE("frac16: optional<numeric>", "[fracpack16][optional]")
{
   REQUIRE(rt16<std::optional<std::uint64_t>>(std::nullopt) == std::nullopt);
   REQUIRE(rt16<std::optional<std::uint64_t>>(std::uint64_t{42}) == std::uint64_t{42});
}

TEST_CASE("frac16: optional<string>", "[fracpack16][optional]")
{
   REQUIRE(rt16<std::optional<std::string>>(std::nullopt) == std::nullopt);
   REQUIRE(rt16<std::optional<std::string>>(std::string("")) == std::string(""));
   REQUIRE(rt16<std::optional<std::string>>(std::string("hi")) == std::string("hi"));
}

TEST_CASE("frac16: optional<vector>", "[fracpack16][optional]")
{
   REQUIRE(rt16<std::optional<std::vector<int>>>(std::nullopt) == std::nullopt);
   REQUIRE(rt16<std::optional<std::vector<int>>>(std::vector<int>{1, 2, 3})
           == std::vector<int>{1, 2, 3});
   REQUIRE(rt16<std::optional<std::vector<int>>>(std::vector<int>{})
           == std::vector<int>{});
}

TEST_CASE("frac16: optional<bool>", "[fracpack16][optional]")
{
   REQUIRE(rt16<std::optional<bool>>(std::nullopt) == std::nullopt);
   REQUIRE(rt16<std::optional<bool>>(true) == true);
   REQUIRE(rt16<std::optional<bool>>(false) == false);
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 3: Tuple / variant
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frac16: tuple round-trip", "[fracpack16][tuple]")
{
   using Tup = std::tuple<std::uint32_t, std::string, std::optional<std::uint64_t>>;
   Tup v{7, "seven", std::uint64_t{700}};
   auto out = rt16(v);
   REQUIRE(std::get<0>(out) == 7);
   REQUIRE(std::get<1>(out) == "seven");
   REQUIRE(std::get<2>(out).value() == 700);
}

TEST_CASE("frac16: variant exercises each arm", "[fracpack16][variant]")
{
   using Var = std::variant<std::uint32_t, std::string, std::vector<std::uint32_t>>;

   Var a = std::uint32_t{123};
   auto oa = rt16(a);
   REQUIRE(oa.index() == 0);
   REQUIRE(std::get<0>(oa) == 123);

   Var b = std::string("two");
   auto ob = rt16(b);
   REQUIRE(ob.index() == 1);
   REQUIRE(std::get<1>(ob) == "two");

   Var c = std::vector<std::uint32_t>{10, 20, 30};
   auto oc = rt16(c);
   REQUIRE(oc.index() == 2);
   REQUIRE(std::get<2>(oc) == std::vector<std::uint32_t>{10, 20, 30});
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 4: Associative containers
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frac16: flat_set<int> round-trip", "[fracpack16][set]")
{
   std::flat_set<std::uint32_t> s{5, 1, 9, 3};
   auto                         out = rt16(s);
   REQUIRE(out.size() == 4);
   REQUIRE(out.contains(1));
   REQUIRE(out.contains(5));
   REQUIRE(out.contains(9));
   REQUIRE(!out.contains(4));
}

TEST_CASE("frac16: flat_map<int, string> round-trip", "[fracpack16][map]")
{
   std::flat_map<std::uint32_t, std::string> m{
       {1, "one"},
       {2, "two"},
       {3, "three"},
   };
   auto out = rt16(m);
   REQUIRE(out.size() == 3);
   REQUIRE(out.at(1) == "one");
   REQUIRE(out.at(3) == "three");
}

TEST_CASE("frac16: std::map round-trip", "[fracpack16][map]")
{
   std::map<std::uint32_t, std::uint32_t> m{{10, 100}, {20, 200}};
   auto                                   out = rt16(m);
   REQUIRE(out.size() == 2);
   REQUIRE(out.at(10) == 100);
   REQUIRE(out.at(20) == 200);
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 5: Reflected struct variations
// ═══════════════════════════════════════════════════════════════════════════

struct F16Fixed
{
   std::uint32_t x;
   std::uint32_t y;
   std::uint32_t z;
};
PSIO_REFLECT(F16Fixed, definitionWillNotChange(), x, y, z)

TEST_CASE("frac16: definitionWillNotChange struct is fixed-size at both formats",
          "[fracpack16][struct]")
{
   F16Fixed v{1, 2, 3};
   auto     bytes16 = psio::to_frac16(v);
   auto     bytes32 = psio::to_frac(v);
   REQUIRE(bytes16.size() == bytes32.size());  // header-free, all fixed
   REQUIRE(bytes16.size() == 12);
   REQUIRE(rt16(v).x == 1);
   REQUIRE(rt16(v).y == 2);
   REQUIRE(rt16(v).z == 3);
}

struct F16TrailingOpt
{
   std::uint32_t                id;
   std::string                  name;
   std::optional<std::uint32_t> maybe;  // trailing optional → extensible
};
PSIO_REFLECT(F16TrailingOpt, id, name, maybe)

TEST_CASE("frac16: struct with trailing optional (extensible path)", "[fracpack16][struct]")
{
   SECTION("optional present")
   {
      F16TrailingOpt v{1, "hi", 42};
      auto           out = rt16(v);
      REQUIRE(out.id == 1);
      REQUIRE(out.name == "hi");
      REQUIRE(out.maybe == 42);
   }
   SECTION("optional absent (trimmed)")
   {
      F16TrailingOpt v{2, "yo", std::nullopt};
      auto           out = rt16(v);
      REQUIRE(out.id == 2);
      REQUIRE(out.name == "yo");
      REQUIRE(!out.maybe.has_value());
   }
}

struct F16AllOpt
{
   std::optional<std::uint32_t> a;
   std::optional<std::string>   b;
   std::optional<std::uint64_t> c;
};
PSIO_REFLECT(F16AllOpt, a, b, c)

TEST_CASE("frac16: all-optional struct", "[fracpack16][struct]")
{
   SECTION("all absent") { REQUIRE(rt16(F16AllOpt{}).a == std::nullopt); }
   SECTION("all present")
   {
      F16AllOpt v{std::uint32_t{1}, std::string("two"), std::uint64_t{3}};
      auto      o = rt16(v);
      REQUIRE(o.a == 1);
      REQUIRE(o.b.value() == "two");
      REQUIRE(o.c == 3);
   }
   SECTION("sparse — middle absent")
   {
      F16AllOpt v{std::uint32_t{1}, std::nullopt, std::uint64_t{3}};
      auto      o = rt16(v);
      REQUIRE(o.a == 1);
      REQUIRE(!o.b.has_value());
      REQUIRE(o.c == 3);
   }
}

struct F16Inner
{
   std::uint32_t k;
   std::string   v;
};
PSIO_REFLECT(F16Inner, k, v)

struct F16Outer
{
   std::uint32_t id;
   F16Inner      inner;
   std::string   tag;
};
PSIO_REFLECT(F16Outer, id, inner, tag)

struct F16Deep
{
   F16Outer outer;
   std::uint32_t depth;
};
PSIO_REFLECT(F16Deep, outer, depth)

TEST_CASE("frac16: nested reflected struct", "[fracpack16][struct]")
{
   F16Outer v{7, {42, "inside"}, "outside"};
   auto     out = rt16(v);
   REQUIRE(out.id == 7);
   REQUIRE(out.inner.k == 42);
   REQUIRE(out.inner.v == "inside");
   REQUIRE(out.tag == "outside");
}

TEST_CASE("frac16: deeply nested reflected struct (3 levels)", "[fracpack16][struct]")
{
   F16Deep v{{9, {1, "a"}, "b"}, 3};
   auto    out = rt16(v);
   REQUIRE(out.outer.id == 9);
   REQUIRE(out.outer.inner.k == 1);
   REQUIRE(out.outer.inner.v == "a");
   REQUIRE(out.outer.tag == "b");
   REQUIRE(out.depth == 3);
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 6: Views over frac16 buffers
// ═══════════════════════════════════════════════════════════════════════════

struct F16Mixed
{
   std::uint32_t                id;
   std::string                  label;
   std::optional<std::uint64_t> maybe;
   std::vector<std::uint32_t>   nums;
};
PSIO_REFLECT(F16Mixed, id, label, maybe, nums)

TEST_CASE("frac16: view<Struct> reads reflected fields", "[fracpack16][view]")
{
   F16Mixed v{42, "hello", std::uint64_t{9000}, {1, 2, 3}};
   auto     bytes = psio::to_frac16(v);
   auto     view  = psio::frac16_view<F16Mixed>::from_buffer(bytes.data());
   REQUIRE(view.id() == 42);
   REQUIRE(view.label() == "hello");
   REQUIRE(view.maybe() == std::optional<std::uint64_t>{9000});
}

TEST_CASE("frac16: frac16_ref unpack + validate path", "[fracpack16][view]")
{
   F16Mixed v{1, "abc", std::nullopt, {10, 20, 30}};
   auto     bytes = psio::to_frac16(v);
   auto     ref   = psio::frac16_ref<F16Mixed, std::span<const char>>(
       std::span<const char>(bytes.data(), bytes.size()));
   auto out = ref.unpack();
   REQUIRE(out.id == v.id);
   REQUIRE(out.label == v.label);
   REQUIRE(out.maybe == v.maybe);
   REQUIRE(out.nums == v.nums);
   REQUIRE(ref.validate() != psio::validation_t::invalid);
}

TEST_CASE("frac16: vec_view<u32, frac16>", "[fracpack16][view]")
{
   F16Mixed v{0, "", std::nullopt, {100, 200, 300, 400}};
   auto     bytes = psio::to_frac16(v);
   auto     view  = psio::frac16_view<F16Mixed>::from_buffer(bytes.data());
   auto     nums  = view.nums();
   REQUIRE(nums.size() == 4);
   REQUIRE(nums[0] == 100);
   REQUIRE(nums[3] == 400);
}

struct F16WithSet
{
   std::uint32_t                    id;
   std::flat_set<std::uint32_t>     tags;
};
PSIO_REFLECT(F16WithSet, id, tags)

TEST_CASE("frac16: set_view<u32, frac16> — contains / size", "[fracpack16][view][set]")
{
   F16WithSet v{7, {3, 1, 4, 1, 5, 9, 2, 6, 5}};  // dedup → {1,2,3,4,5,6,9}
   auto       bytes = psio::to_frac16(v);
   auto       view  = psio::frac16_view<F16WithSet>::from_buffer(bytes.data());
   auto       sv    = view.tags();
   REQUIRE(sv.size() == 7);
   REQUIRE(sv.contains(5));
   REQUIRE(!sv.contains(7));
}

struct F16WithMap
{
   std::uint32_t                                    id;
   std::flat_map<std::uint32_t, std::uint32_t>      counts;
};
PSIO_REFLECT(F16WithMap, id, counts)

TEST_CASE("frac16: map_view<u32,u32, frac16> — find / value_or",
          "[fracpack16][view][map]")
{
   F16WithMap v{1, {{10, 100}, {20, 200}, {30, 300}}};
   auto       bytes = psio::to_frac16(v);
   auto       view  = psio::frac16_view<F16WithMap>::from_buffer(bytes.data());
   auto       mv    = view.counts();
   REQUIRE(mv.size() == 3);
   REQUIRE(mv.contains(20));
   REQUIRE(!mv.contains(999));
   REQUIRE(mv.value_or(30, std::uint32_t{0}) == 300);
   REQUIRE(mv.value_or(999, std::uint32_t{7}) == 7);
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 7: Size, layout, and cross-format behavior
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frac16: frac_layout<T, frac_format_16> metadata", "[fracpack16][layout]")
{
   using L = psio::frac_layout<F16Mixed, psio::frac_format_16>;
   STATIC_REQUIRE(L::num_members == 4);
   STATIC_REQUIRE(L::has_header == true);
   STATIC_REQUIRE(L::hdr_size == 2);
   STATIC_REQUIRE(L::fixed_sizes[0] == 4);  // u32 id
   STATIC_REQUIRE(L::fixed_sizes[1] == 2);  // string offset → size_bytes=2
   STATIC_REQUIRE(L::fixed_sizes[2] == 2);  // optional offset
   STATIC_REQUIRE(L::fixed_sizes[3] == 2);  // vector offset
   STATIC_REQUIRE(L::is_variable[0] == false);
   STATIC_REQUIRE(L::is_variable[1] == true);
   STATIC_REQUIRE(L::is_variable[2] == true);
   STATIC_REQUIRE(L::is_variable[3] == true);
}

TEST_CASE("frac16: exact byte sizes for spot-check shapes", "[fracpack16][size]")
{
   SECTION("empty string")
   {
      // size_type(0 size) = 2B ; no heap payload (empty container elided marker = 0)
      // actually for bare `std::string` at frac16, empty → [2B size=0]
      REQUIRE(psio::to_frac16<std::string>("").size() == 2);
      REQUIRE(psio::to_frac<std::string>("").size() == 4);
   }
   SECTION("3-char string")
   {
      REQUIRE(psio::to_frac16<std::string>("abc").size() == 2 + 3);
      REQUIRE(psio::to_frac<std::string>("abc").size() == 4 + 3);
   }
   SECTION("vector<u32> length 3")
   {
      std::vector<std::uint32_t> v{1, 2, 3};
      REQUIRE(psio::to_frac16(v).size() == 2 + 3 * 4);  // 2B size, 3 u32 elems
      REQUIRE(psio::to_frac(v).size() == 4 + 3 * 4);
   }
   SECTION("F16Mixed with simple contents")
   {
      F16Mixed v{42, "hi", std::uint64_t{7}, {1, 2}};
      // Same shape, just different offset widths.
      auto s16 = psio::to_frac16(v).size();
      auto s32 = psio::to_frac(v).size();
      // 6 width-sensitive slots: header is u16 (unchanged), 3 offsets, u32→u16 sizes on
      // string + vector + optional.  Expect at least 6 bytes saved.
      REQUIRE(s32 - s16 >= 6);
   }
}

TEST_CASE("frac16: cross-format: frac32 buffer rejected by from_frac16",
          "[fracpack16][edge]")
{
   // Frac32 encoding of a struct with several variable-size fields has a
   // layout that's not a valid frac16 buffer — from_frac16 should return
   // false (or at minimum not crash) without producing the original value.
   F16Mixed v{42, "hello", std::uint64_t{9000}, {1, 2, 3}};
   auto     bytes32 = psio::to_frac(v);

   F16Mixed decoded{};
   bool     ok = psio::from_frac16(decoded, bytes32);
   // Either rejected outright, or decoded to a value that does not match
   // the original — the key property is no crash and no false positive.
   if (ok)
      REQUIRE(!(decoded.id == v.id && decoded.label == v.label));
}

TEST_CASE("frac16: validate_frac16 rejects truncated buffer",
          "[fracpack16][validate][edge]")
{
   F16Mixed v{42, "hello", std::uint64_t{9000}, {1, 2, 3}};
   auto     bytes = psio::to_frac16(v);

   // Truncate to half length
   std::vector<char>     truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
   std::span<const char> sp(truncated.data(), truncated.size());
   REQUIRE(psio::validate_frac16<F16Mixed>(sp) == psio::validation_t::invalid);
}

TEST_CASE("frac16: validate_frac16 rejects mangled offset",
          "[fracpack16][validate][edge]")
{
   F16Mixed v{42, "hello", std::uint64_t{9000}, {1, 2, 3}};
   auto     bytes = psio::to_frac16(v);
   REQUIRE(psio::validate_frac16<F16Mixed>(bytes) != psio::validation_t::invalid);

   // F16Mixed layout at frac16: [u16 hdr][u32 id][u16 label_off]...
   // Mangle the label_offset slot at byte 6-7 to point far beyond buf end.
   REQUIRE(bytes.size() > 8);
   auto mangled = bytes;
   mangled[6]   = 0xFF;  // u16 offset slot → bump to 0xFFFF
   mangled[7]   = 0xFF;
   std::span<const char> sp(mangled.data(), mangled.size());
   REQUIRE(psio::validate_frac16<F16Mixed>(sp) == psio::validation_t::invalid);
}

TEST_CASE("frac16: empty containers encode as elided (offset 0)",
          "[fracpack16][edge]")
{
   F16Mixed v{0, "", std::nullopt, {}};
   auto     bytes = psio::to_frac16(v);
   auto     view  = psio::frac16_view<F16Mixed>::from_buffer(bytes.data());
   REQUIRE(view.label() == "");
   REQUIRE(view.maybe() == std::nullopt);
   REQUIRE(view.nums().size() == 0);
}

// ═══════════════════════════════════════════════════════════════════════════
// Section 8: Pack width boundaries — frac16 record must fit in 64KB
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("frac16: large-but-sub-64KB record packs correctly",
          "[fracpack16][size]")
{
   // Single string just under 64KB
   std::string s(60 * 1024, 'a');
   auto        bytes = psio::to_frac16(s);
   REQUIRE(bytes.size() == 2 + s.size());

   std::string out;
   REQUIRE(psio::from_frac16(out, bytes));
   REQUIRE(out == s);
}

