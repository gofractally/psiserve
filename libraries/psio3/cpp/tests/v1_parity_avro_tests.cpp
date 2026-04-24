// v1 ↔ psio3 avro byte-parity tests.
//
// Avro wire (MVP scope):
//   integers:           zig-zag varint (Avro "long")
//   bool:               1 byte
//   float/double:       raw LE
//   string:             varint(len) + utf-8
//   vector<T>:          varint(count) + items, then varint(0)
//   array<byte,N>:      raw N bytes
//   array<non-byte,N>:  varint(N) + items, then varint(0)
//   optional:           varint(0)=null or varint(1)+value
//   record:             fields concatenated

#include <psio/from_avro.hpp>
#include <psio/reflect.hpp>
#include <psio/to_avro.hpp>
#include <psio3/avro.hpp>
#include <psio3/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

TEST_CASE("avro parity: integers", "[avro][parity][primitive]")
{
   REQUIRE(psio::convert_to_avro(std::int32_t{-123}) ==
           psio3::encode(psio3::avro{}, std::int32_t{-123}));
   REQUIRE(psio::convert_to_avro(std::int32_t{0}) ==
           psio3::encode(psio3::avro{}, std::int32_t{0}));
   REQUIRE(psio::convert_to_avro(std::int64_t{1LL << 40}) ==
           psio3::encode(psio3::avro{}, std::int64_t{1LL << 40}));
   REQUIRE(psio::convert_to_avro(std::uint32_t{0xDEADBEEF}) ==
           psio3::encode(psio3::avro{}, std::uint32_t{0xDEADBEEF}));
}

TEST_CASE("avro parity: bool/float/double", "[avro][parity][primitive]")
{
   REQUIRE(psio::convert_to_avro(true) ==
           psio3::encode(psio3::avro{}, true));
   REQUIRE(psio::convert_to_avro(false) ==
           psio3::encode(psio3::avro{}, false));
   REQUIRE(psio::convert_to_avro(3.14159f) ==
           psio3::encode(psio3::avro{}, 3.14159f));
   REQUIRE(psio::convert_to_avro(2.718281828459045) ==
           psio3::encode(psio3::avro{}, 2.718281828459045));
}

TEST_CASE("avro parity: std::string", "[avro][parity][string]")
{
   REQUIRE(psio::convert_to_avro(std::string("hello world")) ==
           psio3::encode(psio3::avro{}, std::string("hello world")));
   REQUIRE(psio::convert_to_avro(std::string{}) ==
           psio3::encode(psio3::avro{}, std::string{}));
}

TEST_CASE("avro parity: std::vector<int32>", "[avro][parity][vector]")
{
   std::vector<std::int32_t> v{-10, 20, -30, 40};
   REQUIRE(psio::convert_to_avro(v) == psio3::encode(psio3::avro{}, v));
   std::vector<std::int32_t> empty;
   REQUIRE(psio::convert_to_avro(empty) ==
           psio3::encode(psio3::avro{}, empty));
}

TEST_CASE("avro parity: std::array<u8, N> (fixed)", "[avro][parity][array]")
{
   std::array<std::uint8_t, 4> a{0x11, 0x22, 0x33, 0x44};
   REQUIRE(psio::convert_to_avro(a) == psio3::encode(psio3::avro{}, a));
}

TEST_CASE("avro parity: std::optional", "[avro][parity][optional]")
{
   std::optional<std::int32_t> n;
   std::optional<std::int32_t> s = -99;
   REQUIRE(psio::convert_to_avro(n) == psio3::encode(psio3::avro{}, n));
   REQUIRE(psio::convert_to_avro(s) == psio3::encode(psio3::avro{}, s));
}

namespace v1_avro {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t             version;
      std::vector<std::int32_t> payload;
      std::string               note;
   };
   PSIO_REFLECT(Blob, version, payload, note)
}
namespace v3_avro {
   struct Point { std::int32_t x; std::int32_t y; };
   PSIO3_REFLECT(Point, x, y)

   struct Blob {
      std::uint16_t             version;
      std::vector<std::int32_t> payload;
      std::string               note;
   };
   PSIO3_REFLECT(Blob, version, payload, note)
}

TEST_CASE("avro parity: fixed record", "[avro][parity][record]")
{
   v1_avro::Point a{3, -7};
   v3_avro::Point b{3, -7};
   REQUIRE(psio::convert_to_avro(a) == psio3::encode(psio3::avro{}, b));
}

TEST_CASE("avro parity: variable record", "[avro][parity][record]")
{
   v1_avro::Blob a{7, {100, -200, 300}, "label"};
   v3_avro::Blob b{7, {100, -200, 300}, "label"};
   REQUIRE(psio::convert_to_avro(a) == psio3::encode(psio3::avro{}, b));
}

TEST_CASE("avro round-trip: v3 encode → v3 decode",
          "[avro][parity][round-trip]")
{
   v3_avro::Blob b{42, {1, -2, 3}, "hi"};
   auto          bv   = psio3::encode(psio3::avro{}, b);
   auto          back =
      psio3::decode<v3_avro::Blob>(psio3::avro{}, std::span<const char>{bv});
   REQUIRE(back.version == 42);
   REQUIRE(back.payload == std::vector<std::int32_t>{1, -2, 3});
   REQUIRE(back.note == "hi");
}

TEST_CASE("avro parity: std::variant", "[avro][parity][variant]")
{
   using V = std::variant<std::int32_t, std::string>;
   REQUIRE(psio::convert_to_avro(V{std::int32_t{-99}}) ==
           psio3::encode(psio3::avro{}, V{std::int32_t{-99}}));
   REQUIRE(psio::convert_to_avro(V{std::string("payload")}) ==
           psio3::encode(psio3::avro{}, V{std::string("payload")}));
}

TEST_CASE("avro round-trip: std::variant",
          "[avro][parity][variant][round-trip]")
{
   using V = std::variant<std::int32_t, std::string>;
   V      in = std::int32_t{-42};
   auto   bv = psio3::encode(psio3::avro{}, in);
   auto   out =
      psio3::decode<V>(psio3::avro{}, std::span<const char>{bv});
   REQUIRE(out.index() == 0);
   REQUIRE(std::get<0>(out) == -42);
}

// v1 avro has no built-in uint256 wire; psio3 treats it as Avro "fixed"
// (raw 32 bytes). Round-trip only — no v1 parity check.

#include <psio3/ext_int.hpp>

TEST_CASE("avro round-trip: uint256", "[avro][extint][round-trip]")
{
   psio3::uint256 in;
   for (int i = 0; i < 4; ++i) in.limb[i] = 0xE000 + i;
   auto bv = psio3::encode(psio3::avro{}, in);
   auto out = psio3::decode<psio3::uint256>(psio3::avro{},
                                            std::span<const char>{bv});
   REQUIRE(out == in);
}
