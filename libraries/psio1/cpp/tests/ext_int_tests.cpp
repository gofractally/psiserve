#include <catch2/catch.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/from_bin.hpp>
#include <psio1/to_bin.hpp>
#include <psio1/from_bincode.hpp>
#include <psio1/to_bincode.hpp>
#include <psio1/from_borsh.hpp>
#include <psio1/to_borsh.hpp>
#include <psio1/fracpack.hpp>

#include <array>
#include <cstring>
#include <vector>

// ── Layout sanity ─────────────────────────────────────────────────────────────

static_assert(sizeof(psio1::uint128) == 16);
static_assert(sizeof(psio1::int128)  == 16);
static_assert(sizeof(psio1::uint256) == 32);
static_assert(alignof(psio1::uint256) == alignof(std::uint64_t));
static_assert(psio1::has_bitwise_serialization<psio1::uint128>());
static_assert(psio1::has_bitwise_serialization<psio1::int128>());
static_assert(psio1::has_bitwise_serialization<psio1::uint256>());

TEST_CASE("ext_int: uint256 construction", "[ext_int]")
{
   psio1::uint256 zero;
   REQUIRE(zero.limb[0] == 0);
   REQUIRE(zero.limb[1] == 0);
   REQUIRE(zero.limb[2] == 0);
   REQUIRE(zero.limb[3] == 0);

   psio1::uint256 from_u64{42ULL};
   REQUIRE(from_u64.limb[0] == 42);
   REQUIRE(from_u64.limb[1] == 0);
   REQUIRE(from_u64.limb[2] == 0);
   REQUIRE(from_u64.limb[3] == 0);

   psio1::uint128 big = (psio1::uint128{1} << 64) | psio1::uint128{0xDEADBEEF};
   psio1::uint256 from_u128{big};
   REQUIRE(from_u128.limb[0] == 0xDEADBEEF);
   REQUIRE(from_u128.limb[1] == 1);
   REQUIRE(from_u128.limb[2] == 0);
   REQUIRE(from_u128.limb[3] == 0);
}

TEST_CASE("ext_int: uint256 equality / ordering", "[ext_int]")
{
   psio1::uint256 a;
   a.limb[0] = 1;
   psio1::uint256 b;
   b.limb[0] = 2;
   REQUIRE(a != b);
   REQUIRE(a < b);

   psio1::uint256 a2 = a;
   REQUIRE(a == a2);
}

// ── Fracpack round-trip ───────────────────────────────────────────────────────

struct ExtIntStruct
{
   psio1::uint128 u128;
   psio1::int128  i128;
   psio1::uint256 u256;
};
PSIO1_REFLECT(ExtIntStruct, definitionWillNotChange(), u128, i128, u256)

inline bool operator==(const ExtIntStruct& a, const ExtIntStruct& b)
{
   return a.u128 == b.u128 && a.i128 == b.i128 && a.u256 == b.u256;
}

TEST_CASE("ext_int: fracpack round-trip", "[ext_int][fracpack]")
{
   ExtIntStruct orig{};
   orig.u128 = (psio1::uint128{0xAABBCCDDEEFF1122} << 64) | psio1::uint128{0x3344556677889900};
   orig.i128 = -psio1::int128{12345};
   orig.u256.limb[0] = 0x1111111111111111;
   orig.u256.limb[1] = 0x2222222222222222;
   orig.u256.limb[2] = 0x3333333333333333;
   orig.u256.limb[3] = 0x4444444444444444;

   auto bytes = psio1::to_frac(orig);
   REQUIRE(bytes.size() == 16 + 16 + 32);  // DWNC, no header
   auto back = psio1::from_frac<ExtIntStruct>(std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back == orig);
}

// ── psio pack_bin (to_bin) round-trip ────────────────────────────────────────

TEST_CASE("ext_int: pack_bin round-trip", "[ext_int][bin]")
{
   ExtIntStruct orig{};
   orig.u128 = psio1::uint128{99999};
   orig.i128 = psio1::int128{-42};
   orig.u256.limb[0] = 0xDEADBEEFCAFEBABE;
   orig.u256.limb[3] = 0xFFFFFFFFFFFFFFFF;

   auto bytes = psio1::convert_to_bin(orig);
   auto back  = psio1::convert_from_bin<ExtIntStruct>(bytes);
   REQUIRE(back == orig);
}

// ── Bincode round-trip ────────────────────────────────────────────────────────

TEST_CASE("ext_int: bincode round-trip uint128", "[ext_int][bincode]")
{
   psio1::uint128 val = (psio1::uint128{0xAABBCCDDEEFF1122} << 64) |
                       psio1::uint128{0x3344556677889900};
   auto data = psio1::convert_to_bincode(val);
   REQUIRE(data.size() == 16);
   auto back = psio1::convert_from_bincode<psio1::uint128>(data);
   REQUIRE(back == val);
}

TEST_CASE("ext_int: bincode round-trip int128", "[ext_int][bincode]")
{
   psio1::int128 val = -(psio1::int128{1} << 100);
   auto data = psio1::convert_to_bincode(val);
   REQUIRE(data.size() == 16);
   auto back = psio1::convert_from_bincode<psio1::int128>(data);
   REQUIRE(back == val);
}

TEST_CASE("ext_int: bincode round-trip uint256", "[ext_int][bincode]")
{
   psio1::uint256 val;
   val.limb[0] = 0x1111111111111111;
   val.limb[1] = 0x2222222222222222;
   val.limb[2] = 0x3333333333333333;
   val.limb[3] = 0x4444444444444444;
   auto data = psio1::convert_to_bincode(val);
   REQUIRE(data.size() == 32);

   // Verify LE byte layout: first 8 bytes are limb[0] little-endian
   REQUIRE(static_cast<std::uint8_t>(data[0]) == 0x11);
   REQUIRE(static_cast<std::uint8_t>(data[7]) == 0x11);
   REQUIRE(static_cast<std::uint8_t>(data[8]) == 0x22);
   REQUIRE(static_cast<std::uint8_t>(data[24]) == 0x44);

   auto back = psio1::convert_from_bincode<psio1::uint256>(data);
   REQUIRE(back == val);
}

// ── Borsh round-trip ──────────────────────────────────────────────────────────

TEST_CASE("ext_int: borsh round-trip uint128", "[ext_int][borsh]")
{
   psio1::uint128 val = (psio1::uint128{7} << 80) | psio1::uint128{0x0123456789ABCDEF};
   auto data = psio1::convert_to_borsh(val);
   REQUIRE(data.size() == 16);
   auto back = psio1::convert_from_borsh<psio1::uint128>(data);
   REQUIRE(back == val);
}

TEST_CASE("ext_int: borsh round-trip uint256", "[ext_int][borsh]")
{
   psio1::uint256 val;
   val.limb[0] = 0xAAAAAAAAAAAAAAAA;
   val.limb[1] = 0xBBBBBBBBBBBBBBBB;
   val.limb[2] = 0xCCCCCCCCCCCCCCCC;
   val.limb[3] = 0xDDDDDDDDDDDDDDDD;
   auto data = psio1::convert_to_borsh(val);
   REQUIRE(data.size() == 32);
   auto back = psio1::convert_from_borsh<psio1::uint256>(data);
   REQUIRE(back == val);
}

// ── Vector of ext ints: memcpy bulk path ──────────────────────────────────────

TEST_CASE("ext_int: vector<uint256> bulk memcpy path", "[ext_int][fracpack]")
{
   std::vector<psio1::uint256> vec(4);
   for (std::size_t i = 0; i < 4; ++i)
   {
      vec[i].limb[0] = i;
      vec[i].limb[3] = i * 1000;
   }

   auto bytes = psio1::convert_to_bincode(vec);
   // u64 length prefix + 4 × 32 bytes
   REQUIRE(bytes.size() == 8 + 4 * 32);

   auto back = psio1::convert_from_bincode<std::vector<psio1::uint256>>(bytes);
   REQUIRE(back.size() == 4);
   for (std::size_t i = 0; i < 4; ++i)
   {
      REQUIRE(back[i] == vec[i]);
   }
}

// ── Run-batching with surrounding fixed fields ────────────────────────────────

struct ExtIntRun
{
   std::uint32_t tag;
   psio1::uint128 big;
   psio1::uint256 bigger;
   std::uint64_t trailer;
};
PSIO1_REFLECT(ExtIntRun, definitionWillNotChange(), tag, big, bigger, trailer)

inline bool operator==(const ExtIntRun& a, const ExtIntRun& b)
{
   return a.tag == b.tag && a.big == b.big && a.bigger == b.bigger && a.trailer == b.trailer;
}

TEST_CASE("ext_int: run-batching over ext-int fields", "[ext_int][fracpack]")
{
   ExtIntRun orig{};
   orig.tag = 0xDEADBEEF;
   orig.big = psio1::uint128{42};
   orig.bigger.limb[0] = 123;
   orig.bigger.limb[2] = 456;
   orig.trailer = 0xCAFEBABE;

   auto bytes = psio1::to_frac(orig);
   // DWNC: 4 + 16 + 32 + 8 = 60 bytes, no offsets, should memcpy as one blob
   REQUIRE(bytes.size() == 60);
   auto back = psio1::from_frac<ExtIntRun>(std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back == orig);
}
