#include <catch2/catch.hpp>
#include <psio1/from_bin.hpp>
#include <psio1/to_bin.hpp>
#include <psio1/from_avro.hpp>
#include <psio1/to_avro.hpp>
#include <psio1/from_bincode.hpp>
#include <psio1/to_bincode.hpp>
#include <psio1/from_borsh.hpp>
#include <psio1/to_borsh.hpp>
#include <psio1/fracpack.hpp>

#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <variant>
#include <vector>

// ── Test types ───────────────────────────────────────────────────────────────

struct MfPoint
{
   int32_t x;
   int32_t y;
};
PSIO1_REFLECT(MfPoint, definitionWillNotChange(), x, y)

inline bool operator==(const MfPoint& a, const MfPoint& b)
{
   return a.x == b.x && a.y == b.y;
}

struct MfPerson
{
   std::string              name;
   uint32_t                 age;
   bool                     active;
   std::vector<std::string> tags;
   std::optional<uint32_t>  score;
};
PSIO1_REFLECT(MfPerson, name, age, active, tags, score)

inline bool operator==(const MfPerson& a, const MfPerson& b)
{
   return a.name == b.name && a.age == b.age && a.active == b.active && a.tags == b.tags &&
          a.score == b.score;
}

struct MfNested
{
   MfPoint              origin;
   std::vector<MfPoint> points;
};
PSIO1_REFLECT(MfNested, origin, points)

inline bool operator==(const MfNested& a, const MfNested& b)
{
   return a.origin == b.origin && a.points == b.points;
}

enum class Color : uint8_t
{
   Red   = 0,
   Green = 1,
   Blue  = 2
};

struct MfWithEnum
{
   Color   color;
   int32_t value;
};
PSIO1_REFLECT(MfWithEnum, color, value)

inline bool operator==(const MfWithEnum& a, const MfWithEnum& b)
{
   return a.color == b.color && a.value == b.value;
}

using MfVariant = std::variant<int32_t, std::string, MfPoint>;

struct MfWithVariant
{
   std::string name;
   MfVariant   data;
};
PSIO1_REFLECT(MfWithVariant, name, data)

inline bool operator==(const MfWithVariant& a, const MfWithVariant& b)
{
   return a.name == b.name && a.data == b.data;
}

// ── Helper: round-trip test macro ────────────────────────────────────────────

#define ROUND_TRIP_BIN(type, value)                     \
   {                                                    \
      type orig = value;                                \
      auto data = psio1::convert_to_bin(orig);           \
      auto back = psio1::convert_from_bin<type>(data);   \
      REQUIRE(back == orig);                            \
   }

#define ROUND_TRIP_AVRO(type, value)                    \
   {                                                    \
      type orig = value;                                \
      auto data = psio1::convert_to_avro(orig);          \
      auto back = psio1::convert_from_avro<type>(data);  \
      REQUIRE(back == orig);                            \
   }

#define ROUND_TRIP_BINCODE(type, value)                    \
   {                                                      \
      type orig = value;                                  \
      auto data = psio1::convert_to_bincode(orig);         \
      auto back = psio1::convert_from_bincode<type>(data); \
      REQUIRE(back == orig);                              \
   }

#define ROUND_TRIP_BORSH(type, value)                      \
   {                                                      \
      type orig = value;                                  \
      auto data = psio1::convert_to_borsh(orig);           \
      auto back = psio1::convert_from_borsh<type>(data);   \
      REQUIRE(back == orig);                              \
   }

// ============================================================================
//  PSIO BIN FORMAT TESTS
// ============================================================================

TEST_CASE("bin: scalar round-trip", "[bin]")
{
   ROUND_TRIP_BIN(uint8_t, 0);
   ROUND_TRIP_BIN(uint8_t, 255);
   ROUND_TRIP_BIN(int8_t, -128);
   ROUND_TRIP_BIN(int8_t, 127);
   ROUND_TRIP_BIN(uint16_t, 0);
   ROUND_TRIP_BIN(uint16_t, 65535);
   ROUND_TRIP_BIN(int32_t, 0);
   ROUND_TRIP_BIN(int32_t, -1);
   ROUND_TRIP_BIN(int32_t, 2147483647);
   ROUND_TRIP_BIN(int32_t, -2147483648);
   ROUND_TRIP_BIN(uint32_t, 0);
   ROUND_TRIP_BIN(uint32_t, 4294967295u);
   ROUND_TRIP_BIN(int64_t, 0);
   ROUND_TRIP_BIN(int64_t, -1);
   ROUND_TRIP_BIN(uint64_t, 0);
   ROUND_TRIP_BIN(uint64_t, 18446744073709551615ull);
   ROUND_TRIP_BIN(bool, true);
   ROUND_TRIP_BIN(bool, false);
}

TEST_CASE("bin: float round-trip", "[bin]")
{
   {
      float orig = 3.14f;
      auto  data = psio1::convert_to_bin(orig);
      auto  back = psio1::convert_from_bin<float>(data);
      REQUIRE(back == Approx(orig));
   }
   {
      double orig = 2.718281828459045;
      auto   data = psio1::convert_to_bin(orig);
      auto   back = psio1::convert_from_bin<double>(data);
      REQUIRE(back == Approx(orig));
   }
}

TEST_CASE("bin: string round-trip", "[bin]")
{
   ROUND_TRIP_BIN(std::string, "");
   ROUND_TRIP_BIN(std::string, "hello");
   ROUND_TRIP_BIN(std::string, "The quick brown fox jumps over the lazy dog");
}

TEST_CASE("bin: vector round-trip", "[bin]")
{
   ROUND_TRIP_BIN(std::vector<int32_t>, (std::vector<int32_t>{}));
   ROUND_TRIP_BIN(std::vector<int32_t>, (std::vector<int32_t>{1, 2, 3}));
   ROUND_TRIP_BIN(std::vector<std::string>, (std::vector<std::string>{"a", "bb", "ccc"}));
}

TEST_CASE("bin: optional round-trip", "[bin]")
{
   ROUND_TRIP_BIN(std::optional<int32_t>, std::nullopt);
   ROUND_TRIP_BIN(std::optional<int32_t>, 42);
   ROUND_TRIP_BIN(std::optional<std::string>, std::nullopt);
   ROUND_TRIP_BIN(std::optional<std::string>, std::string("hello"));
}

TEST_CASE("bin: struct round-trip", "[bin]")
{
   ROUND_TRIP_BIN(MfPoint, (MfPoint{10, 20}));
   ROUND_TRIP_BIN(MfPoint, (MfPoint{-100, 200}));
   {
      MfPerson orig{"Alice", 30, true, {"eng", "rust"}, 95};
      auto     data = psio1::convert_to_bin(orig);
      auto     back = psio1::convert_from_bin<MfPerson>(data);
      REQUIRE(back == orig);
   }
   {
      MfPerson orig{"Bob", 25, false, {}, std::nullopt};
      auto     data = psio1::convert_to_bin(orig);
      auto     back = psio1::convert_from_bin<MfPerson>(data);
      REQUIRE(back == orig);
   }
}

TEST_CASE("bin: nested struct round-trip", "[bin]")
{
   MfNested n{MfPoint{1, 2}, {MfPoint{3, 4}, MfPoint{5, 6}}};
   ROUND_TRIP_BIN(MfNested, n);
}

TEST_CASE("bin: variant round-trip", "[bin]")
{
   ROUND_TRIP_BIN(MfVariant, MfVariant(int32_t(42)));
   ROUND_TRIP_BIN(MfVariant, MfVariant(std::string("hello")));
   ROUND_TRIP_BIN(MfVariant, MfVariant(MfPoint{1, 2}));
}

TEST_CASE("bin: tuple round-trip", "[bin]")
{
   using T = std::tuple<int32_t, std::string, bool>;
   ROUND_TRIP_BIN(T, (T{42, "hello", true}));
}

TEST_CASE("bin: array round-trip", "[bin]")
{
   using A = std::array<int32_t, 3>;
   ROUND_TRIP_BIN(A, (A{10, 20, 30}));
}

// ============================================================================
//  AVRO BINARY FORMAT TESTS
// ============================================================================

TEST_CASE("avro: scalar round-trip", "[avro]")
{
   ROUND_TRIP_AVRO(uint8_t, 0);
   ROUND_TRIP_AVRO(uint8_t, 255);
   ROUND_TRIP_AVRO(int8_t, -128);
   ROUND_TRIP_AVRO(int8_t, 127);
   ROUND_TRIP_AVRO(int32_t, 0);
   ROUND_TRIP_AVRO(int32_t, -1);
   ROUND_TRIP_AVRO(int32_t, 2147483647);
   ROUND_TRIP_AVRO(int32_t, -2147483648);
   ROUND_TRIP_AVRO(uint32_t, 0);
   ROUND_TRIP_AVRO(uint32_t, 4294967295u);
   ROUND_TRIP_AVRO(int64_t, 0);
   ROUND_TRIP_AVRO(int64_t, -1);
   ROUND_TRIP_AVRO(uint64_t, 0);
   ROUND_TRIP_AVRO(uint64_t, 18446744073709551615ull);
   ROUND_TRIP_AVRO(bool, true);
   ROUND_TRIP_AVRO(bool, false);
}

TEST_CASE("avro: float round-trip", "[avro]")
{
   {
      float orig = 3.14f;
      auto  data = psio1::convert_to_avro(orig);
      REQUIRE(data.size() == 4);  // raw 4 bytes
      auto back = psio1::convert_from_avro<float>(data);
      REQUIRE(back == Approx(orig));
   }
   {
      double orig = 2.718281828459045;
      auto   data = psio1::convert_to_avro(orig);
      REQUIRE(data.size() == 8);  // raw 8 bytes
      auto back = psio1::convert_from_avro<double>(data);
      REQUIRE(back == Approx(orig));
   }
}

TEST_CASE("avro: zig-zag encoding known values", "[avro]")
{
   // Avro spec examples: 0→0, -1→1, 1→2, -2→3, 2→4, ...
   // Zig-zag of 0 = 0x00 (1 byte varint)
   auto d0 = psio1::convert_to_avro(int32_t(0));
   REQUIRE(d0.size() == 1);
   REQUIRE(static_cast<uint8_t>(d0[0]) == 0x00);

   // Zig-zag of -1 = 1 → 0x01
   auto dm1 = psio1::convert_to_avro(int32_t(-1));
   REQUIRE(dm1.size() == 1);
   REQUIRE(static_cast<uint8_t>(dm1[0]) == 0x01);

   // Zig-zag of 1 = 2 → 0x02
   auto d1 = psio1::convert_to_avro(int32_t(1));
   REQUIRE(d1.size() == 1);
   REQUIRE(static_cast<uint8_t>(d1[0]) == 0x02);

   // Zig-zag of -2 = 3 → 0x03
   auto dm2 = psio1::convert_to_avro(int32_t(-2));
   REQUIRE(dm2.size() == 1);
   REQUIRE(static_cast<uint8_t>(dm2[0]) == 0x03);

   // Zig-zag of 64 = 128 → 0x80 0x01
   auto d64 = psio1::convert_to_avro(int32_t(64));
   REQUIRE(d64.size() == 2);
   REQUIRE(static_cast<uint8_t>(d64[0]) == 0x80);
   REQUIRE(static_cast<uint8_t>(d64[1]) == 0x01);
}

TEST_CASE("avro: string round-trip", "[avro]")
{
   ROUND_TRIP_AVRO(std::string, "");
   ROUND_TRIP_AVRO(std::string, "hello");
   ROUND_TRIP_AVRO(std::string, "The quick brown fox");
}

TEST_CASE("avro: vector round-trip", "[avro]")
{
   ROUND_TRIP_AVRO(std::vector<int32_t>, (std::vector<int32_t>{}));
   ROUND_TRIP_AVRO(std::vector<int32_t>, (std::vector<int32_t>{1, 2, 3}));
   ROUND_TRIP_AVRO(std::vector<std::string>, (std::vector<std::string>{"a", "bb"}));
}

TEST_CASE("avro: optional as union{null,T}", "[avro]")
{
   // None → branch 0 (zig-zag 0 = 0x00)
   auto none_data = psio1::convert_to_avro(std::optional<int32_t>{});
   REQUIRE(static_cast<uint8_t>(none_data[0]) == 0x00);

   // Some(42) → branch 1 (zig-zag 1 = 0x02) + zig-zag(42) = 84 = 0x54
   auto some_data = psio1::convert_to_avro(std::optional<int32_t>{42});
   REQUIRE(static_cast<uint8_t>(some_data[0]) == 0x02);  // branch index 1, zig-zag encoded

   ROUND_TRIP_AVRO(std::optional<int32_t>, std::nullopt);
   ROUND_TRIP_AVRO(std::optional<int32_t>, 42);
   ROUND_TRIP_AVRO(std::optional<std::string>, std::nullopt);
   ROUND_TRIP_AVRO(std::optional<std::string>, std::string("hello"));
}

TEST_CASE("avro: struct round-trip", "[avro]")
{
   ROUND_TRIP_AVRO(MfPoint, (MfPoint{10, 20}));
   ROUND_TRIP_AVRO(MfPoint, (MfPoint{-100, 200}));
   {
      MfPerson orig{"Alice", 30, true, {"eng", "rust"}, 95};
      auto     data = psio1::convert_to_avro(orig);
      auto     back = psio1::convert_from_avro<MfPerson>(data);
      REQUIRE(back == orig);
   }
   {
      MfPerson orig{"Bob", 25, false, {}, std::nullopt};
      auto     data = psio1::convert_to_avro(orig);
      auto     back = psio1::convert_from_avro<MfPerson>(data);
      REQUIRE(back == orig);
   }
}

TEST_CASE("avro: nested struct round-trip", "[avro]")
{
   MfNested n{MfPoint{1, 2}, {MfPoint{3, 4}, MfPoint{5, 6}}};
   ROUND_TRIP_AVRO(MfNested, n);
}

TEST_CASE("avro: variant round-trip", "[avro]")
{
   ROUND_TRIP_AVRO(MfVariant, MfVariant(int32_t(42)));
   ROUND_TRIP_AVRO(MfVariant, MfVariant(std::string("hello")));
   ROUND_TRIP_AVRO(MfVariant, MfVariant(MfPoint{1, 2}));
}

TEST_CASE("avro: tuple round-trip", "[avro]")
{
   using T = std::tuple<int32_t, std::string, bool>;
   ROUND_TRIP_AVRO(T, (T{42, "hello", true}));
}

TEST_CASE("avro: fixed-length byte array as avro fixed", "[avro]")
{
   using A = std::array<uint8_t, 4>;
   A orig{0xDE, 0xAD, 0xBE, 0xEF};
   auto data = psio1::convert_to_avro(orig);
   REQUIRE(data.size() == 4);  // no length prefix for byte arrays (Avro fixed)
   auto back = psio1::convert_from_avro<A>(data);
   REQUIRE(back == orig);
}

TEST_CASE("avro: non-byte array as avro array", "[avro]")
{
   using A = std::array<int32_t, 3>;
   ROUND_TRIP_AVRO(A, (A{10, 20, 30}));
}

// ============================================================================
//  BINCODE FORMAT TESTS
// ============================================================================

TEST_CASE("bincode: scalar round-trip", "[bincode]")
{
   ROUND_TRIP_BINCODE(uint8_t, 0);
   ROUND_TRIP_BINCODE(uint8_t, 255);
   ROUND_TRIP_BINCODE(int8_t, -128);
   ROUND_TRIP_BINCODE(int8_t, 127);
   ROUND_TRIP_BINCODE(int32_t, 0);
   ROUND_TRIP_BINCODE(int32_t, -1);
   ROUND_TRIP_BINCODE(int32_t, 2147483647);
   ROUND_TRIP_BINCODE(int32_t, -2147483648);
   ROUND_TRIP_BINCODE(uint32_t, 0);
   ROUND_TRIP_BINCODE(uint32_t, 4294967295u);
   ROUND_TRIP_BINCODE(int64_t, 0);
   ROUND_TRIP_BINCODE(int64_t, -1);
   ROUND_TRIP_BINCODE(uint64_t, 0);
   ROUND_TRIP_BINCODE(uint64_t, 18446744073709551615ull);
   ROUND_TRIP_BINCODE(bool, true);
   ROUND_TRIP_BINCODE(bool, false);
}

TEST_CASE("bincode: fixed-width encoding verification", "[bincode]")
{
   // uint32_t should be exactly 4 bytes LE
   auto d = psio1::convert_to_bincode(uint32_t(0x12345678));
   REQUIRE(d.size() == 4);
   REQUIRE(static_cast<uint8_t>(d[0]) == 0x78);
   REQUIRE(static_cast<uint8_t>(d[1]) == 0x56);
   REQUIRE(static_cast<uint8_t>(d[2]) == 0x34);
   REQUIRE(static_cast<uint8_t>(d[3]) == 0x12);

   // bool should be exactly 1 byte
   auto bt = psio1::convert_to_bincode(true);
   REQUIRE(bt.size() == 1);
   REQUIRE(static_cast<uint8_t>(bt[0]) == 0x01);

   auto bf = psio1::convert_to_bincode(false);
   REQUIRE(bf.size() == 1);
   REQUIRE(static_cast<uint8_t>(bf[0]) == 0x00);
}

TEST_CASE("bincode: float round-trip", "[bincode]")
{
   {
      float orig = 3.14f;
      auto  data = psio1::convert_to_bincode(orig);
      REQUIRE(data.size() == 4);
      auto back = psio1::convert_from_bincode<float>(data);
      REQUIRE(back == Approx(orig));
   }
   {
      double orig = 2.718281828459045;
      auto   data = psio1::convert_to_bincode(orig);
      REQUIRE(data.size() == 8);
      auto back = psio1::convert_from_bincode<double>(data);
      REQUIRE(back == Approx(orig));
   }
}

TEST_CASE("bincode: string round-trip with u64 length", "[bincode]")
{
   // Empty string: 8 bytes length (u64=0) + 0 bytes data
   auto empty = psio1::convert_to_bincode(std::string(""));
   REQUIRE(empty.size() == 8);

   // "hello": 8 bytes length + 5 bytes data
   auto hello = psio1::convert_to_bincode(std::string("hello"));
   REQUIRE(hello.size() == 13);

   ROUND_TRIP_BINCODE(std::string, "");
   ROUND_TRIP_BINCODE(std::string, "hello");
   ROUND_TRIP_BINCODE(std::string, "The quick brown fox");
}

TEST_CASE("bincode: vector round-trip with u64 length", "[bincode]")
{
   // Empty vector: 8 bytes length only
   auto empty = psio1::convert_to_bincode(std::vector<int32_t>{});
   REQUIRE(empty.size() == 8);

   // 3-element i32 vector: 8 bytes length + 3*4 bytes data
   auto v = psio1::convert_to_bincode(std::vector<int32_t>{1, 2, 3});
   REQUIRE(v.size() == 8 + 12);

   ROUND_TRIP_BINCODE(std::vector<int32_t>, (std::vector<int32_t>{}));
   ROUND_TRIP_BINCODE(std::vector<int32_t>, (std::vector<int32_t>{1, 2, 3}));
   ROUND_TRIP_BINCODE(std::vector<std::string>, (std::vector<std::string>{"a", "bb"}));
}

TEST_CASE("bincode: optional round-trip", "[bincode]")
{
   // None: u8(0) = 1 byte
   auto none = psio1::convert_to_bincode(std::optional<int32_t>{});
   REQUIRE(none.size() == 1);
   REQUIRE(static_cast<uint8_t>(none[0]) == 0x00);

   // Some(42): u8(1) + i32(42) = 5 bytes
   auto some = psio1::convert_to_bincode(std::optional<int32_t>{42});
   REQUIRE(some.size() == 5);
   REQUIRE(static_cast<uint8_t>(some[0]) == 0x01);

   ROUND_TRIP_BINCODE(std::optional<int32_t>, std::nullopt);
   ROUND_TRIP_BINCODE(std::optional<int32_t>, 42);
   ROUND_TRIP_BINCODE(std::optional<std::string>, std::nullopt);
   ROUND_TRIP_BINCODE(std::optional<std::string>, std::string("hello"));
}

TEST_CASE("bincode: struct round-trip", "[bincode]")
{
   // Point is all fixed-size scalars: should be exactly 8 bytes
   auto pd = psio1::convert_to_bincode(MfPoint{10, 20});
   REQUIRE(pd.size() == 8);

   ROUND_TRIP_BINCODE(MfPoint, (MfPoint{10, 20}));
   ROUND_TRIP_BINCODE(MfPoint, (MfPoint{-100, 200}));
   {
      MfPerson orig{"Alice", 30, true, {"eng", "rust"}, 95};
      auto     data = psio1::convert_to_bincode(orig);
      auto     back = psio1::convert_from_bincode<MfPerson>(data);
      REQUIRE(back == orig);
   }
   {
      MfPerson orig{"Bob", 25, false, {}, std::nullopt};
      auto     data = psio1::convert_to_bincode(orig);
      auto     back = psio1::convert_from_bincode<MfPerson>(data);
      REQUIRE(back == orig);
   }
}

TEST_CASE("bincode: nested struct round-trip", "[bincode]")
{
   MfNested n{MfPoint{1, 2}, {MfPoint{3, 4}, MfPoint{5, 6}}};
   ROUND_TRIP_BINCODE(MfNested, n);
}

TEST_CASE("bincode: variant round-trip with u32 index", "[bincode]")
{
   // int32_t variant: u32(0) + i32(42) = 8 bytes
   auto vi = psio1::convert_to_bincode(MfVariant(int32_t(42)));
   REQUIRE(vi.size() == 8);
   REQUIRE(static_cast<uint8_t>(vi[0]) == 0x00);  // index 0

   // string variant: u32(1) + u64(5) + "hello"
   auto vs = psio1::convert_to_bincode(MfVariant(std::string("hello")));
   REQUIRE(static_cast<uint8_t>(vs[0]) == 0x01);  // index 1

   ROUND_TRIP_BINCODE(MfVariant, MfVariant(int32_t(42)));
   ROUND_TRIP_BINCODE(MfVariant, MfVariant(std::string("hello")));
   ROUND_TRIP_BINCODE(MfVariant, MfVariant(MfPoint{1, 2}));
}

TEST_CASE("bincode: tuple round-trip", "[bincode]")
{
   using T = std::tuple<int32_t, std::string, bool>;
   ROUND_TRIP_BINCODE(T, (T{42, "hello", true}));
}

TEST_CASE("bincode: fixed-length array (no length prefix)", "[bincode]")
{
   using A = std::array<int32_t, 3>;
   A    orig{10, 20, 30};
   auto data = psio1::convert_to_bincode(orig);
   REQUIRE(data.size() == 12);  // 3 * 4 bytes, no length prefix
   auto back = psio1::convert_from_bincode<A>(data);
   REQUIRE(back == orig);
}

// ============================================================================
//  BORSH FORMAT TESTS
// ============================================================================

TEST_CASE("borsh: scalar round-trip", "[borsh]")
{
   ROUND_TRIP_BORSH(uint8_t, 0);
   ROUND_TRIP_BORSH(uint8_t, 255);
   ROUND_TRIP_BORSH(int8_t, -128);
   ROUND_TRIP_BORSH(int8_t, 127);
   ROUND_TRIP_BORSH(int32_t, 0);
   ROUND_TRIP_BORSH(int32_t, -1);
   ROUND_TRIP_BORSH(int32_t, 2147483647);
   ROUND_TRIP_BORSH(int32_t, -2147483648);
   ROUND_TRIP_BORSH(uint32_t, 0);
   ROUND_TRIP_BORSH(uint32_t, 4294967295u);
   ROUND_TRIP_BORSH(int64_t, 0);
   ROUND_TRIP_BORSH(int64_t, -1);
   ROUND_TRIP_BORSH(uint64_t, 0);
   ROUND_TRIP_BORSH(uint64_t, 18446744073709551615ull);
   ROUND_TRIP_BORSH(bool, true);
   ROUND_TRIP_BORSH(bool, false);
}

TEST_CASE("borsh: fixed-width encoding verification", "[borsh]")
{
   // uint32_t: 4 bytes LE
   auto d = psio1::convert_to_borsh(uint32_t(0x12345678));
   REQUIRE(d.size() == 4);
   REQUIRE(static_cast<uint8_t>(d[0]) == 0x78);
   REQUIRE(static_cast<uint8_t>(d[1]) == 0x56);
   REQUIRE(static_cast<uint8_t>(d[2]) == 0x34);
   REQUIRE(static_cast<uint8_t>(d[3]) == 0x12);

   // bool: 1 byte
   auto bt = psio1::convert_to_borsh(true);
   REQUIRE(bt.size() == 1);
   REQUIRE(static_cast<uint8_t>(bt[0]) == 0x01);

   auto bf = psio1::convert_to_borsh(false);
   REQUIRE(bf.size() == 1);
   REQUIRE(static_cast<uint8_t>(bf[0]) == 0x00);
}

TEST_CASE("borsh: string round-trip with u32 length", "[borsh]")
{
   // Empty string: 4-byte u32 length + 0 bytes
   auto empty = psio1::convert_to_borsh(std::string(""));
   REQUIRE(empty.size() == 4);

   // "hello": 4 bytes length + 5 bytes data (vs bincode's 13)
   auto hello = psio1::convert_to_borsh(std::string("hello"));
   REQUIRE(hello.size() == 9);

   ROUND_TRIP_BORSH(std::string, "");
   ROUND_TRIP_BORSH(std::string, "hello");
   ROUND_TRIP_BORSH(std::string, "The quick brown fox");
}

TEST_CASE("borsh: vector round-trip with u32 length", "[borsh]")
{
   // Empty vector: 4-byte length only
   auto empty = psio1::convert_to_borsh(std::vector<int32_t>{});
   REQUIRE(empty.size() == 4);

   // 3-element i32 vector: 4 + 12 bytes
   auto v = psio1::convert_to_borsh(std::vector<int32_t>{1, 2, 3});
   REQUIRE(v.size() == 4 + 12);

   ROUND_TRIP_BORSH(std::vector<int32_t>, (std::vector<int32_t>{}));
   ROUND_TRIP_BORSH(std::vector<int32_t>, (std::vector<int32_t>{1, 2, 3}));
   ROUND_TRIP_BORSH(std::vector<std::string>, (std::vector<std::string>{"a", "bb"}));
}

TEST_CASE("borsh: optional round-trip", "[borsh]")
{
   // None: u8(0) = 1 byte
   auto none = psio1::convert_to_borsh(std::optional<int32_t>{});
   REQUIRE(none.size() == 1);
   REQUIRE(static_cast<uint8_t>(none[0]) == 0x00);

   // Some(42): u8(1) + i32(42) = 5 bytes
   auto some = psio1::convert_to_borsh(std::optional<int32_t>{42});
   REQUIRE(some.size() == 5);
   REQUIRE(static_cast<uint8_t>(some[0]) == 0x01);

   ROUND_TRIP_BORSH(std::optional<int32_t>, std::nullopt);
   ROUND_TRIP_BORSH(std::optional<int32_t>, 42);
   ROUND_TRIP_BORSH(std::optional<std::string>, std::nullopt);
   ROUND_TRIP_BORSH(std::optional<std::string>, std::string("hello"));
}

TEST_CASE("borsh: struct round-trip", "[borsh]")
{
   // Point: 8 bytes (two i32, no headers)
   auto pd = psio1::convert_to_borsh(MfPoint{10, 20});
   REQUIRE(pd.size() == 8);

   ROUND_TRIP_BORSH(MfPoint, (MfPoint{10, 20}));
   ROUND_TRIP_BORSH(MfPoint, (MfPoint{-100, 200}));
   {
      MfPerson orig{"Alice", 30, true, {"eng", "rust"}, 95};
      auto     data = psio1::convert_to_borsh(orig);
      auto     back = psio1::convert_from_borsh<MfPerson>(data);
      REQUIRE(back == orig);
   }
   {
      MfPerson orig{"Bob", 25, false, {}, std::nullopt};
      auto     data = psio1::convert_to_borsh(orig);
      auto     back = psio1::convert_from_borsh<MfPerson>(data);
      REQUIRE(back == orig);
   }
}

TEST_CASE("borsh: nested struct round-trip", "[borsh]")
{
   MfNested n{MfPoint{1, 2}, {MfPoint{3, 4}, MfPoint{5, 6}}};
   ROUND_TRIP_BORSH(MfNested, n);
}

TEST_CASE("borsh: variant round-trip with u8 discriminant", "[borsh]")
{
   // int32_t variant: u8(0) + i32(42) = 5 bytes (vs bincode's 8)
   auto vi = psio1::convert_to_borsh(MfVariant(int32_t(42)));
   REQUIRE(vi.size() == 5);
   REQUIRE(static_cast<uint8_t>(vi[0]) == 0x00);

   // string variant: u8(1) + u32(5) + "hello" = 10 bytes
   auto vs = psio1::convert_to_borsh(MfVariant(std::string("hello")));
   REQUIRE(vs.size() == 10);
   REQUIRE(static_cast<uint8_t>(vs[0]) == 0x01);

   ROUND_TRIP_BORSH(MfVariant, MfVariant(int32_t(42)));
   ROUND_TRIP_BORSH(MfVariant, MfVariant(std::string("hello")));
   ROUND_TRIP_BORSH(MfVariant, MfVariant(MfPoint{1, 2}));
}

TEST_CASE("borsh: tuple round-trip", "[borsh]")
{
   using T = std::tuple<int32_t, std::string, bool>;
   ROUND_TRIP_BORSH(T, (T{42, "hello", true}));
}

TEST_CASE("borsh: fixed-length array (no length prefix)", "[borsh]")
{
   using A = std::array<int32_t, 3>;
   A    orig{10, 20, 30};
   auto data = psio1::convert_to_borsh(orig);
   REQUIRE(data.size() == 12);  // 3*4 bytes, no length prefix
   auto back = psio1::convert_from_borsh<A>(data);
   REQUIRE(back == orig);
}

// bincode vs borsh wire-size sanity check
TEST_CASE("borsh vs bincode: wire size differences", "[borsh][cross]")
{
   // String "hi": borsh u32 = 4+2 = 6, bincode u64 = 8+2 = 10
   auto s_borsh   = psio1::convert_to_borsh(std::string("hi"));
   auto s_bincode = psio1::convert_to_bincode(std::string("hi"));
   REQUIRE(s_borsh.size() == 6);
   REQUIRE(s_bincode.size() == 10);

   // Variant index: borsh u8 = 1, bincode u32 = 4
   auto v_borsh   = psio1::convert_to_borsh(MfVariant(int32_t(0)));
   auto v_bincode = psio1::convert_to_bincode(MfVariant(int32_t(0)));
   REQUIRE(v_borsh.size() == 5);    // 1 (tag) + 4 (i32)
   REQUIRE(v_bincode.size() == 8);  // 4 (tag) + 4 (i32)
}

// ============================================================================
//  CROSS-FORMAT TESTS
// ============================================================================

TEST_CASE("cross-format: same types, different encodings", "[cross]")
{
   MfPerson alice;
   alice.name   = "Alice";
   alice.age    = 30;
   alice.active = true;
   alice.tags   = {"engineer"};
   alice.score  = 95;

   auto bin_data     = psio1::convert_to_bin(alice);
   auto avro_data    = psio1::convert_to_avro(alice);
   auto bincode_data = psio1::convert_to_bincode(alice);

   // All three should round-trip to the same logical value
   auto from_bin     = psio1::convert_from_bin<MfPerson>(bin_data);
   auto from_avro    = psio1::convert_from_avro<MfPerson>(avro_data);
   auto from_bincode = psio1::convert_from_bincode<MfPerson>(bincode_data);

   REQUIRE(from_bin == alice);
   REQUIRE(from_avro == alice);
   REQUIRE(from_bincode == alice);

   // But the byte representations should differ
   REQUIRE(bin_data != avro_data);
   REQUIRE(bin_data != bincode_data);

   INFO("bin size: " << bin_data.size());
   INFO("avro size: " << avro_data.size());
   INFO("bincode size: " << bincode_data.size());

   // Avro should generally be more compact for small integers (zig-zag varints)
   // Bincode uses u64 for lengths, so it's larger for string/vec-heavy data
}

TEST_CASE("cross-format: Point struct all formats", "[cross]")
{
   MfPoint p{-42, 100};

   auto bin_data     = psio1::convert_to_bin(p);
   auto avro_data    = psio1::convert_to_avro(p);
   auto bincode_data = psio1::convert_to_bincode(p);

   // Bin: raw 8 bytes (two int32_t, bitwise serialization)
   REQUIRE(bin_data.size() == 8);

   // Avro: two zig-zag varints (variable size)
   // -42 → zig-zag 83 → varint [0x53] (1 byte), 100 → zig-zag 200 → [0xC8, 0x01] (2 bytes)
   // Total: 3 bytes — more compact than fixed-width
   REQUIRE(avro_data.size() < bin_data.size());

   // Bincode: raw 8 bytes (same as bin for fixed-width integers)
   REQUIRE(bincode_data.size() == 8);

   REQUIRE(psio1::convert_from_bin<MfPoint>(bin_data) == p);
   REQUIRE(psio1::convert_from_avro<MfPoint>(avro_data) == p);
   REQUIRE(psio1::convert_from_bincode<MfPoint>(bincode_data) == p);
}

TEST_CASE("cross-format: empty optional all formats", "[cross]")
{
   std::optional<int32_t> empty;

   auto bin_data     = psio1::convert_to_bin(empty);
   auto avro_data    = psio1::convert_to_avro(empty);
   auto bincode_data = psio1::convert_to_bincode(empty);

   // Bin: bool(false) = 1 byte
   REQUIRE(bin_data.size() == 1);

   // Avro: union index 0 (zig-zag 0 = 1 byte)
   REQUIRE(avro_data.size() == 1);

   // Bincode: u8(0) = 1 byte
   REQUIRE(bincode_data.size() == 1);

   REQUIRE(psio1::convert_from_bin<std::optional<int32_t>>(bin_data) == empty);
   REQUIRE(psio1::convert_from_avro<std::optional<int32_t>>(avro_data) == empty);
   REQUIRE(psio1::convert_from_bincode<std::optional<int32_t>>(bincode_data) == empty);
}

TEST_CASE("cross-format: nested structs all formats", "[cross]")
{
   MfNested n{MfPoint{-1, 1}, {MfPoint{0, 0}, MfPoint{100, -100}}};

   auto from_bin     = psio1::convert_from_bin<MfNested>(psio1::convert_to_bin(n));
   auto from_avro    = psio1::convert_from_avro<MfNested>(psio1::convert_to_avro(n));
   auto from_bincode = psio1::convert_from_bincode<MfNested>(psio1::convert_to_bincode(n));

   REQUIRE(from_bin == n);
   REQUIRE(from_avro == n);
   REQUIRE(from_bincode == n);
}

TEST_CASE("cross-format: variant all formats", "[cross]")
{
   MfWithVariant wv{"test", MfVariant(std::string("payload"))};

   auto from_bin     = psio1::convert_from_bin<MfWithVariant>(psio1::convert_to_bin(wv));
   auto from_avro    = psio1::convert_from_avro<MfWithVariant>(psio1::convert_to_avro(wv));
   auto from_bincode = psio1::convert_from_bincode<MfWithVariant>(psio1::convert_to_bincode(wv));

   REQUIRE(from_bin == wv);
   REQUIRE(from_avro == wv);
   REQUIRE(from_bincode == wv);
}
