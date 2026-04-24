// v1 ↔ psio3 borsh byte-parity tests.
//
// Borsh wire (MVP scope):
//   primitives raw LE, bool = 1 byte, string = u32 len + bytes,
//   vec = u32 len + contents, array = contents only (no len),
//   optional = u8 tag + value, record = fields concatenated.

#include <psio/bitset.hpp>
#include <psio/ext_int.hpp>
#include <psio/from_borsh.hpp>
#include <psio/reflect.hpp>
#include <psio/to_borsh.hpp>
#include <psio3/borsh.hpp>
#include <psio3/ext_int.hpp>
#include <psio3/reflect.hpp>
#include <psio3/wrappers.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("borsh parity: primitives", "[borsh][parity][primitive]")
{
   REQUIRE(psio::convert_to_borsh(std::uint32_t{0xDEADBEEF}) ==
           psio3::encode(psio3::borsh{}, std::uint32_t{0xDEADBEEF}));
   REQUIRE(psio::convert_to_borsh(std::uint64_t{0x1122334455667788ULL}) ==
           psio3::encode(psio3::borsh{}, std::uint64_t{0x1122334455667788ULL}));
   REQUIRE(psio::convert_to_borsh(true) ==
           psio3::encode(psio3::borsh{}, true));
   REQUIRE(psio::convert_to_borsh(false) ==
           psio3::encode(psio3::borsh{}, false));
}

TEST_CASE("borsh parity: std::array<u32, N>", "[borsh][parity][array]")
{
   std::array<std::uint32_t, 4> a{1, 2, 3, 4};
   REQUIRE(psio::convert_to_borsh(a) == psio3::encode(psio3::borsh{}, a));
}

TEST_CASE("borsh parity: std::vector<u32>", "[borsh][parity][vector]")
{
   std::vector<std::uint32_t> v{10, 20, 30, 40};
   REQUIRE(psio::convert_to_borsh(v) == psio3::encode(psio3::borsh{}, v));

   std::vector<std::uint32_t> empty;
   REQUIRE(psio::convert_to_borsh(empty) ==
           psio3::encode(psio3::borsh{}, empty));
}

TEST_CASE("borsh parity: std::string", "[borsh][parity][string]")
{
   REQUIRE(psio::convert_to_borsh(std::string("hello world")) ==
           psio3::encode(psio3::borsh{}, std::string("hello world")));
   REQUIRE(psio::convert_to_borsh(std::string{}) ==
           psio3::encode(psio3::borsh{}, std::string{}));
}

TEST_CASE("borsh parity: std::optional", "[borsh][parity][optional]")
{
   std::optional<std::uint32_t> n;
   std::optional<std::uint32_t> s = 0xDEADBEEF;
   REQUIRE(psio::convert_to_borsh(n) == psio3::encode(psio3::borsh{}, n));
   REQUIRE(psio::convert_to_borsh(s) == psio3::encode(psio3::borsh{}, s));
}

namespace v1_borsh {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t              version;
      std::vector<std::uint32_t> payload;
      std::string                note;
   };
   PSIO_REFLECT(Blob, version, payload, note)
}
namespace v3_borsh {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO3_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t              version;
      std::vector<std::uint32_t> payload;
      std::string                note;
   };
   PSIO3_REFLECT(Blob, version, payload, note)
}

TEST_CASE("borsh parity: fixed record", "[borsh][parity][record]")
{
   v1_borsh::Point a{3, -7};
   v3_borsh::Point b{3, -7};
   REQUIRE(psio::convert_to_borsh(a) == psio3::encode(psio3::borsh{}, b));
}

TEST_CASE("borsh parity: variable record", "[borsh][parity][record]")
{
   v1_borsh::Blob a{7, {100, 200, 300}, "label"};
   v3_borsh::Blob b{7, {100, 200, 300}, "label"};
   REQUIRE(psio::convert_to_borsh(a) == psio3::encode(psio3::borsh{}, b));
}

TEST_CASE("borsh parity: v1 encode → v3 decode (Blob)",
          "[borsh][parity][record][round-trip]")
{
   v1_borsh::Blob a{42, {1, 2, 3}, "xx"};
   auto           av = psio::convert_to_borsh(a);
   auto           back =
      psio3::decode<v3_borsh::Blob>(psio3::borsh{}, std::span<const char>{av});
   REQUIRE(back.version == 42);
   REQUIRE(back.payload == std::vector<std::uint32_t>{1, 2, 3});
   REQUIRE(back.note == "xx");
}

TEST_CASE("borsh parity: v3 encode → v1 decode (Blob)",
          "[borsh][parity][record][round-trip]")
{
   v3_borsh::Blob b{99, {7, 8}, "yo"};
   auto           bv   = psio3::encode(psio3::borsh{}, b);
   auto           back = psio::convert_from_borsh<v1_borsh::Blob>(bv);
   REQUIRE(back.version == 99);
   REQUIRE(back.payload == std::vector<std::uint32_t>{7, 8});
   REQUIRE(back.note == "yo");
}

TEST_CASE("borsh parity: std::variant", "[borsh][parity][variant]")
{
   using V = std::variant<std::uint32_t, std::string, bool>;
   REQUIRE(psio::convert_to_borsh(V{std::uint32_t{0xDEADBEEF}}) ==
           psio3::encode(psio3::borsh{}, V{std::uint32_t{0xDEADBEEF}}));
   REQUIRE(psio::convert_to_borsh(V{std::string("hello")}) ==
           psio3::encode(psio3::borsh{}, V{std::string("hello")}));
   REQUIRE(psio::convert_to_borsh(V{true}) ==
           psio3::encode(psio3::borsh{}, V{true}));
}

TEST_CASE("borsh round-trip: std::variant",
          "[borsh][parity][variant][round-trip]")
{
   using V = std::variant<std::uint32_t, std::string>;
   V        v1 = std::string("xyz");
   auto     bv = psio3::encode(psio3::borsh{}, v1);
   auto     v2 = psio3::decode<V>(psio3::borsh{}, std::span<const char>{bv});
   REQUIRE(v2.index() == 1);
   REQUIRE(std::get<1>(v2) == "xyz");
}

// ── bitvector / bitlist parity ────────────────────────────────────────

TEST_CASE("borsh parity: bitvector<16>", "[borsh][parity][bitvector]")
{
   psio::bitvector<16>  v1v;
   psio3::bitvector<16> v3v;
   for (std::size_t i : {0, 5, 12})
   {
      v1v.set(i, true);
      v3v.set(i, true);
   }
   REQUIRE(psio::convert_to_borsh(v1v) ==
           psio3::encode(psio3::borsh{}, v3v));
}

TEST_CASE("borsh parity: bitlist<128>", "[borsh][parity][bitlist]")
{
   psio::bitlist<128>  v1l;
   psio3::bitlist<128> v3l;
   for (int i = 0; i < 10; ++i)
   {
      bool on = (i % 3) == 0;
      v1l.push_back(on);
      v3l.push_back(on);
   }
   REQUIRE(psio::convert_to_borsh(v1l) ==
           psio3::encode(psio3::borsh{}, v3l));
}

TEST_CASE("borsh round-trip: bitlist", "[borsh][bitlist][round-trip]")
{
   psio3::bitlist<128> in;
   for (int i = 0; i < 10; ++i)
      in.push_back(i % 2 == 0);
   auto bv = psio3::encode(psio3::borsh{}, in);
   auto out = psio3::decode<psio3::bitlist<128>>(
      psio3::borsh{}, std::span<const char>{bv});
   REQUIRE(out.size() == in.size());
   for (std::size_t i = 0; i < in.size(); ++i)
      REQUIRE(out.test(i) == in.test(i));
}

TEST_CASE("borsh parity: uint128 + uint256", "[borsh][parity][extint]")
{
   psio::uint128  v1u = (static_cast<psio::uint128>(0x1) << 64) | 0x2;
   psio3::uint128 v3u = (static_cast<psio3::uint128>(0x1) << 64) | 0x2;
   REQUIRE(psio::convert_to_borsh(v1u) ==
           psio3::encode(psio3::borsh{}, v3u));

   psio::uint256 v1w; psio3::uint256 v3w;
   for (int i = 0; i < 4; ++i)
   { v1w.limb[i] = 0xB000 + i; v3w.limb[i] = 0xB000 + i; }
   REQUIRE(psio::convert_to_borsh(v1w) ==
           psio3::encode(psio3::borsh{}, v3w));
}
