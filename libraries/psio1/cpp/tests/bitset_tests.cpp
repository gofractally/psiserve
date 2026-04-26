#include <catch2/catch.hpp>
#include <psio1/bitset.hpp>
#include <psio1/from_bin.hpp>
#include <psio1/to_bin.hpp>
#include <psio1/from_bincode.hpp>
#include <psio1/to_bincode.hpp>
#include <psio1/from_borsh.hpp>
#include <psio1/to_borsh.hpp>
#include <psio1/fracpack.hpp>
#include <psio1/schema.hpp>

#include <cstring>

// ── bitvector<N> basic ops ────────────────────────────────────────────────────

static_assert(sizeof(psio1::bitvector<8>)   == 1);
static_assert(sizeof(psio1::bitvector<16>)  == 2);
static_assert(sizeof(psio1::bitvector<17>)  == 3);
static_assert(sizeof(psio1::bitvector<512>) == 64);
static_assert(std::is_trivially_copyable_v<psio1::bitvector<7>>);
static_assert(psio1::has_bitwise_serialization<psio1::bitvector<7>>());

TEST_CASE("bitvector: default is all-zero", "[bitset][bitvector]")
{
   psio1::bitvector<17> v;
   REQUIRE(v.size() == 17);
   REQUIRE(v.count() == 0);
   REQUIRE(v.none());
   REQUIRE(!v.any());
   REQUIRE(!v.all());
}

TEST_CASE("bitvector: set/test/reset/flip", "[bitset][bitvector]")
{
   psio1::bitvector<10> v;
   v.set(0).set(3).set(9);
   REQUIRE(v.test(0));
   REQUIRE(!v.test(1));
   REQUIRE(v.test(3));
   REQUIRE(v.test(9));
   REQUIRE(v.count() == 3);

   v.reset(3);
   REQUIRE(!v.test(3));
   REQUIRE(v.count() == 2);

   v.flip(1);
   REQUIRE(v.test(1));
}

TEST_CASE("bitvector: all() with non-multiple-of-8", "[bitset][bitvector]")
{
   psio1::bitvector<10> v;
   for (std::size_t i = 0; i < 10; ++i)
      v.set(i);
   REQUIRE(v.all());
   REQUIRE(v.count() == 10);
}

TEST_CASE("bitvector: bitwise ops", "[bitset][bitvector]")
{
   psio1::bitvector<16> a;
   psio1::bitvector<16> b;
   a.set(0).set(2).set(4);
   b.set(2).set(4).set(6);

   auto c = a;
   c &= b;
   REQUIRE(c.count() == 2);
   REQUIRE(c.test(2));
   REQUIRE(c.test(4));

   auto d = a;
   d |= b;
   REQUIRE(d.count() == 4);
}

// ── bitlist<N> basic ops ──────────────────────────────────────────────────────

TEST_CASE("bitlist: default empty", "[bitset][bitlist]")
{
   psio1::bitlist<100> v;
   REQUIRE(v.size() == 0);
   REQUIRE(v.empty());
   REQUIRE(v.byte_count() == 0);
}

TEST_CASE("bitlist: push_back packs LSB-first", "[bitset][bitlist]")
{
   psio1::bitlist<16> v;
   v.push_back(true);   // bit 0 set
   v.push_back(false);
   v.push_back(true);   // bit 2 set
   v.push_back(true);   // bit 3 set

   REQUIRE(v.size() == 4);
   REQUIRE(v.test(0));
   REQUIRE(!v.test(1));
   REQUIRE(v.test(2));
   REQUIRE(v.test(3));
   REQUIRE(v.count() == 3);
   REQUIRE(v.byte_count() == 1);

   // First byte should be 0b00001101 = 0x0D
   REQUIRE(v.bytes()[0] == 0x0D);
}

TEST_CASE("bitlist: overflow throws", "[bitset][bitlist]")
{
   psio1::bitlist<3> v;
   v.push_back(true);
   v.push_back(false);
   v.push_back(true);
   REQUIRE_THROWS(v.push_back(false));
}

TEST_CASE("bitlist: init list", "[bitset][bitlist]")
{
   psio1::bitlist<10> v{true, false, true, true, false};
   REQUIRE(v.size() == 5);
   REQUIRE(v.test(0));
   REQUIRE(!v.test(1));
   REQUIRE(v.test(2));
   REQUIRE(v.test(3));
   REQUIRE(!v.test(4));
}

// ── Fracpack round-trip ──────────────────────────────────────────────────────

TEST_CASE("bitvector: fracpack round-trip (memcpy path)", "[bitset][bitvector][fracpack]")
{
   psio1::bitvector<64> v;
   for (std::size_t i = 0; i < 64; i += 3)
      v.set(i);

   auto bytes = psio1::to_frac(v);
   REQUIRE(bytes.size() == 8);  // fixed 64-bit bitvector = 8 bytes

   auto back = psio1::from_frac<psio1::bitvector<64>>(
       std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back == v);
}

TEST_CASE("bitlist: fracpack round-trip", "[bitset][bitlist][fracpack]")
{
   psio1::bitlist<100> v;
   for (std::size_t i = 0; i < 37; ++i)
      if (i % 5 == 0)
         v.push_back(true);
      else
         v.push_back(false);
   REQUIRE(v.size() == 37);

   auto bytes = psio1::to_frac(v);
   auto back  = psio1::from_frac<psio1::bitlist<100>>(
       std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back.size() == v.size());
   for (std::size_t i = 0; i < v.size(); ++i)
      REQUIRE(back.test(i) == v.test(i));
}

TEST_CASE("bitlist: empty fracpack", "[bitset][bitlist][fracpack]")
{
   psio1::bitlist<100> empty;
   auto               bytes = psio1::to_frac(empty);
   auto               back  = psio1::from_frac<psio1::bitlist<100>>(
       std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back.empty());
}

// ── Fracpack sentinel verification ────────────────────────────────────────────

struct BitsInStruct
{
   std::uint32_t       tag;
   psio1::bitvector<64> flags;
   psio1::bitlist<255>  extras;
   std::uint32_t       trailer;
};
PSIO1_REFLECT(BitsInStruct, definitionWillNotChange(), tag, flags, extras, trailer)

inline bool operator==(const BitsInStruct& a, const BitsInStruct& b)
{
   return a.tag == b.tag && a.flags == b.flags && a.extras == b.extras &&
          a.trailer == b.trailer;
}

TEST_CASE("bitlist: empty embeds as offset=0 sentinel", "[bitset][bitlist][sentinel]")
{
   BitsInStruct s{};
   s.tag     = 0xCAFE;
   s.trailer = 0xBEEF;
   // flags is all-zero (bitvector default), extras is empty.

   auto bytes = psio1::to_frac(s);
   // tag(4) + flags(8) + off_extras(4) + trailer(4) = 20 bytes fixed region
   // extras is empty → offset sentinel = 0, no tail data.
   REQUIRE(bytes.size() == 20);

   std::uint32_t off_extras;
   std::memcpy(&off_extras, bytes.data() + 4 + 8, 4);
   REQUIRE(off_extras == 0);  // sentinel

   auto back = psio1::from_frac<BitsInStruct>(std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back == s);
}

TEST_CASE("bitlist: non-empty consumes heap", "[bitset][bitlist][sentinel]")
{
   BitsInStruct s{};
   s.tag = 0;
   for (std::size_t i = 0; i < 10; ++i)
      s.extras.push_back(i & 1);

   auto bytes = psio1::to_frac(s);
   // fixed region = 20 bytes; tail = bitlist [u8 count=10][ceil(10/8)=2 bytes] = 3 bytes
   REQUIRE(bytes.size() == 23);

   std::uint32_t off_extras;
   std::memcpy(&off_extras, bytes.data() + 4 + 8, 4);
   REQUIRE(off_extras != 0);

   auto back = psio1::from_frac<BitsInStruct>(std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back.extras.size() == 10);
   for (std::size_t i = 0; i < 10; ++i)
      REQUIRE(back.extras.test(i) == static_cast<bool>(i & 1));
}

// ── Bincode / Borsh / pack_bin round-trip ─────────────────────────────────────

TEST_CASE("bitvector: bincode round-trip", "[bitset][bitvector][bincode]")
{
   psio1::bitvector<32> v;
   v.set(0).set(5).set(16).set(31);
   auto data = psio1::convert_to_bincode(v);
   REQUIRE(data.size() == 4);
   auto back = psio1::convert_from_bincode<psio1::bitvector<32>>(data);
   REQUIRE(back == v);
}

TEST_CASE("bitlist: bincode round-trip", "[bitset][bitlist][bincode]")
{
   psio1::bitlist<50> v;
   for (std::size_t i = 0; i < 13; ++i)
      v.push_back(i % 2 == 0);

   auto data = psio1::convert_to_bincode(v);
   auto back = psio1::convert_from_bincode<psio1::bitlist<50>>(data);
   REQUIRE(back.size() == v.size());
   for (std::size_t i = 0; i < v.size(); ++i)
      REQUIRE(back.test(i) == v.test(i));
}

TEST_CASE("bitvector: borsh round-trip", "[bitset][bitvector][borsh]")
{
   psio1::bitvector<64> v;
   v.set(0).set(63);
   auto data = psio1::convert_to_borsh(v);
   auto back = psio1::convert_from_borsh<psio1::bitvector<64>>(data);
   REQUIRE(back == v);
}

TEST_CASE("bitlist: borsh round-trip", "[bitset][bitlist][borsh]")
{
   psio1::bitlist<100> v;
   for (std::size_t i = 0; i < 25; ++i)
      v.push_back(i % 3 == 0);
   auto data = psio1::convert_to_borsh(v);
   auto back = psio1::convert_from_borsh<psio1::bitlist<100>>(data);
   REQUIRE(back.size() == v.size());
}

TEST_CASE("bitvector: pack_bin round-trip", "[bitset][bitvector][bin]")
{
   psio1::bitvector<16> v;
   v.set(1).set(7).set(8).set(15);
   auto data = psio1::convert_to_bin(v);
   auto back = psio1::convert_from_bin<psio1::bitvector<16>>(data);
   REQUIRE(back == v);
}

TEST_CASE("bitlist: pack_bin round-trip", "[bitset][bitlist][bin]")
{
   psio1::bitlist<100> v;
   for (std::size_t i = 0; i < 17; ++i)
      v.push_back(i % 4 == 1);
   auto data = psio1::convert_to_bin(v);
   auto back = psio1::convert_from_bin<psio1::bitlist<100>>(data);
   REQUIRE(back.size() == v.size());
}

// ── Bound enforcement on decode ───────────────────────────────────────────────

TEST_CASE("bitlist: bincode rejects oversize bit count", "[bitset][bitlist][bincode]")
{
   psio1::bitlist<100> big;
   for (std::size_t i = 0; i < 80; ++i)
      big.push_back(true);
   auto data = psio1::convert_to_bincode(big);
   REQUIRE_THROWS(psio1::convert_from_bincode<psio1::bitlist<50>>(data));
}

// ── Schema generation ────────────────────────────────────────────────────────

TEST_CASE("bitvector: schema emits Custom{Array<u8>, \"bitvector:N\"}",
          "[bitset][schema]")
{
   namespace S  = psio1::schema_types;
   auto schema  = S::SchemaBuilder{}.insert<BitsInStruct>("BitsInStruct").build();

   const auto* resolved = schema.get("BitsInStruct")->resolve(schema);
   auto*       obj      = std::get_if<S::Struct>(&resolved->value);
   REQUIRE(obj != nullptr);

   // flags: bitvector<64> → Custom{Array<u8,8>, "bitvector:64"}
   const auto* flags_r = obj->members[1].type.resolve(schema);
   auto*       custom  = std::get_if<S::Custom>(&flags_r->value);
   REQUIRE(custom != nullptr);
   REQUIRE(custom->id == "bitvector:64");

   auto* arr = std::get_if<S::Array>(&custom->type->value);
   REQUIRE(arr != nullptr);
   REQUIRE(arr->len == 8);
}

// ── std::bitset<N> & std::vector<bool> ────────────────────────────────────────

TEST_CASE("std::bitset: layout probe detects canonical form", "[bitset][stdbitset]")
{
   // Most platforms (libc++/libstdc++ on little-endian x86_64/aarch64) should
   // report canonical. Just verify the probe returns SOMETHING consistent and
   // doesn't crash.
   constexpr bool canonical_8  = psio1::bitset_layout_is_canonical_v<8>;
   constexpr bool canonical_64 = psio1::bitset_layout_is_canonical_v<64>;
   constexpr bool canonical_512 = psio1::bitset_layout_is_canonical_v<512>;
   INFO("canonical<8>=" << canonical_8
        << " canonical<64>=" << canonical_64
        << " canonical<512>=" << canonical_512);
   // On our supported platforms we expect all three to be true.
   REQUIRE(canonical_8);
   REQUIRE(canonical_64);
   REQUIRE(canonical_512);
}

TEST_CASE("std::bitset: pack/unpack round-trip", "[bitset][stdbitset]")
{
   std::bitset<100> bs;
   bs.set(0);
   bs.set(7);
   bs.set(8);
   bs.set(63);
   bs.set(99);

   std::uint8_t buf[13];  // ceil(100/8)
   psio1::pack_bitset_bytes(bs, buf);

   std::bitset<100> back;
   psio1::unpack_bitset_bytes(buf, back);
   REQUIRE(back == bs);
}

TEST_CASE("std::bitset: bincode round-trip", "[bitset][stdbitset][bincode]")
{
   std::bitset<64> bs;
   for (std::size_t i = 0; i < 64; i += 3)
      bs.set(i);

   auto data = psio1::convert_to_bincode(bs);
   REQUIRE(data.size() == 8);
   auto back = psio1::convert_from_bincode<std::bitset<64>>(data);
   REQUIRE(back == bs);
}

TEST_CASE("std::bitset<512>: large bitset round-trip", "[bitset][stdbitset][bincode]")
{
   std::bitset<512> bs;
   for (std::size_t i = 0; i < 512; i += 7)
      bs.set(i);

   auto data = psio1::convert_to_bincode(bs);
   REQUIRE(data.size() == 64);
   auto back = psio1::convert_from_bincode<std::bitset<512>>(data);
   REQUIRE(back == bs);
}

TEST_CASE("std::bitset: wire format matches psio1::bitvector with same N",
          "[bitset][stdbitset]")
{
   // A bitset and bitvector with the same bit pattern must produce identical
   // bytes across all formats.
   std::bitset<64>      bs;
   psio1::bitvector<64>  bv;
   for (std::size_t i = 0; i < 64; ++i)
   {
      bool v = (i % 5 == 0);
      bs.set(i, v);
      bv.set(i, v);
   }

   auto a = psio1::convert_to_bincode(bs);
   auto b = psio1::convert_to_bincode(bv);
   REQUIRE(a == b);

   auto c = psio1::convert_to_borsh(bs);
   auto d = psio1::convert_to_borsh(bv);
   REQUIRE(c == d);
}

TEST_CASE("std::bitset: fracpack round-trip", "[bitset][stdbitset][fracpack]")
{
   std::bitset<33> bs;
   bs.set(0);
   bs.set(15);
   bs.set(32);

   auto bytes = psio1::to_frac(bs);
   REQUIRE(bytes.size() == 5);  // ceil(33/8)
   auto back = psio1::from_frac<std::bitset<33>>(
       std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back == bs);
}

// ── Conversions ──────────────────────────────────────────────────────────────

TEST_CASE("convert: std::bitset ↔ psio1::bitvector", "[bitset][convert]")
{
   psio1::bitvector<100> bv;
   bv.set(0).set(7).set(99);

   auto bs = psio1::to_bitset(bv);
   REQUIRE(bs.test(0));
   REQUIRE(bs.test(7));
   REQUIRE(bs.test(99));
   REQUIRE(!bs.test(50));

   auto bv2 = psio1::to_bitvector(bs);
   REQUIRE(bv2 == bv);
}

// ── std::vector<bool> ────────────────────────────────────────────────────────

TEST_CASE("std::vector<bool>: bincode round-trip", "[bitset][vecbool][bincode]")
{
   std::vector<bool> v{true, false, true, true, false, false, true};
   auto              data = psio1::convert_to_bincode(v);
   auto              back = psio1::convert_from_bincode<std::vector<bool>>(data);
   REQUIRE(back.size() == v.size());
   for (std::size_t i = 0; i < v.size(); ++i)
      REQUIRE(back[i] == v[i]);
}

TEST_CASE("std::vector<bool>: borsh round-trip", "[bitset][vecbool][borsh]")
{
   std::vector<bool> v;
   for (int i = 0; i < 100; ++i)
      v.push_back(i % 3 == 0);
   auto data = psio1::convert_to_borsh(v);
   auto back = psio1::convert_from_borsh<std::vector<bool>>(data);
   REQUIRE(back.size() == v.size());
   for (std::size_t i = 0; i < v.size(); ++i)
      REQUIRE(back[i] == v[i]);
}

TEST_CASE("std::vector<bool>: pack_bin round-trip", "[bitset][vecbool][bin]")
{
   std::vector<bool> v{false, true, false, true, true, false, true, false, true};
   auto              data = psio1::convert_to_bin(v);
   auto              back = psio1::convert_from_bin<std::vector<bool>>(data);
   REQUIRE(back.size() == v.size());
   for (std::size_t i = 0; i < v.size(); ++i)
      REQUIRE(back[i] == v[i]);
}

TEST_CASE("std::vector<bool>: fracpack round-trip", "[bitset][vecbool][fracpack]")
{
   std::vector<bool> v;
   for (int i = 0; i < 23; ++i)
      v.push_back(i % 4 == 1);

   auto bytes = psio1::to_frac(v);
   auto back  = psio1::from_frac<std::vector<bool>>(
       std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back.size() == v.size());
   for (std::size_t i = 0; i < v.size(); ++i)
      REQUIRE(back[i] == v[i]);
}

TEST_CASE("std::vector<bool>: empty fracpack", "[bitset][vecbool][fracpack]")
{
   std::vector<bool> empty;
   auto              bytes = psio1::to_frac(empty);
   auto              back  = psio1::from_frac<std::vector<bool>>(
       std::span<const char>(bytes.data(), bytes.size()));
   REQUIRE(back.empty());
}

TEST_CASE("bitlist: schema emits Custom{BoundedList<u8>, \"bitlist:N\"}",
          "[bitset][schema]")
{
   namespace S  = psio1::schema_types;
   auto schema  = S::SchemaBuilder{}.insert<BitsInStruct>("BitsInStruct").build();

   const auto* resolved = schema.get("BitsInStruct")->resolve(schema);
   auto*       obj      = std::get_if<S::Struct>(&resolved->value);
   REQUIRE(obj != nullptr);

   // extras: bitlist<255> → Custom{BoundedList<u8, 32>, "bitlist:255"}
   const auto* extras_r = obj->members[2].type.resolve(schema);
   auto*       custom   = std::get_if<S::Custom>(&extras_r->value);
   REQUIRE(custom != nullptr);
   REQUIRE(custom->id == "bitlist:255");

   auto* blist = std::get_if<S::BoundedList>(&custom->type->value);
   REQUIRE(blist != nullptr);
   REQUIRE(blist->maxCount == 32);  // ceil(255 / 8)
}
