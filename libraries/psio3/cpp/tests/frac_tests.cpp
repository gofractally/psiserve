// Phase 8 — fracpack (frac32 / frac16) format tags.

#include <psio3/frac.hpp>
#include <psio3/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct FracPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO3_REFLECT(FracPoint, x, y)

struct FracPerson
{
   std::int32_t id;
   std::string  name;
};
PSIO3_REFLECT(FracPerson, id, name)

struct FracPacket
{
   std::uint16_t             version;
   std::vector<std::int32_t> payload;
   std::string               tag;
};
PSIO3_REFLECT(FracPacket, version, payload, tag)

TEMPLATE_TEST_CASE("frac round-trips primitives", "[frac][primitive]",
                   psio3::frac16, psio3::frac32)
{
   using F = TestType;
   auto b  = psio3::encode(F{}, std::int32_t{-42});
   REQUIRE(b.size() == 4);
   auto v = psio3::decode<std::int32_t>(F{}, std::span<const char>{b});
   REQUIRE(v == -42);
}

TEMPLATE_TEST_CASE("frac round-trips std::vector of fixed elements",
                   "[frac][vector]", psio3::frac16, psio3::frac32)
{
   using F = TestType;
   std::vector<std::uint32_t> v{1, 2, 3, 4};
   auto                       b = psio3::encode(F{}, v);
   auto back = psio3::decode<std::vector<std::uint32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back == v);
}

TEMPLATE_TEST_CASE("frac round-trips std::string inside a record",
                   "[frac][record][string]", psio3::frac16, psio3::frac32)
{
   using F = TestType;
   FracPerson pp{1, "bob"};
   auto       b = psio3::encode(F{}, pp);
   auto back = psio3::decode<FracPerson>(F{}, std::span<const char>{b});
   REQUIRE(back.id == 1);
   REQUIRE(back.name == "bob");
}

TEMPLATE_TEST_CASE("frac round-trips fixed records with correct header",
                   "[frac][record]", psio3::frac16, psio3::frac32)
{
   using F = TestType;
   FracPoint p{10, 20};
   auto      b = psio3::encode(F{}, p);
   // v1 fracpack record header is always u16 regardless of W; fixed
   // region is the two int32 fields. Total = 2 + 8.
   REQUIRE(b.size() == 2u + 8u);
   auto back = psio3::decode<FracPoint>(F{}, std::span<const char>{b});
   REQUIRE(back.x == 10);
   REQUIRE(back.y == 20);
}

TEMPLATE_TEST_CASE("frac round-trips records with multiple variable fields",
                   "[frac][record][variable]", psio3::frac16, psio3::frac32)
{
   using F = TestType;
   FracPacket pk{7, {100, 200, 300}, "hello"};
   auto       b = psio3::encode(F{}, pk);
   auto back = psio3::decode<FracPacket>(F{}, std::span<const char>{b});
   REQUIRE(back.version == 7);
   REQUIRE(back.payload == std::vector<std::int32_t>{100, 200, 300});
   REQUIRE(back.tag == "hello");
}

TEMPLATE_TEST_CASE("frac round-trips std::optional<T>", "[frac][optional]",
                   psio3::frac16, psio3::frac32)
{
   using F = TestType;
   std::optional<std::int32_t> some = 123;
   auto                        b    = psio3::encode(F{}, some);
   auto back = psio3::decode<std::optional<std::int32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back.has_value());
   REQUIRE(*back == 123);

   std::optional<std::int32_t> none;
   auto                        b2 = psio3::encode(F{}, none);
   auto back2 = psio3::decode<std::optional<std::int32_t>>(
      F{}, std::span<const char>{b2});
   REQUIRE(!back2.has_value());
}

TEMPLATE_TEST_CASE("frac size_of agrees with encode output size",
                   "[frac][size_of]", psio3::frac16, psio3::frac32)
{
   using F = TestType;
   FracPacket pk{1, {5, 10}, "x"};
   auto       b = psio3::encode(F{}, pk);
   REQUIRE(psio3::size_of(F{}, pk) == b.size());
}

TEST_CASE("frac scoped sugar works", "[frac][format_tag_base]")
{
   FracPoint p{4, 5};
   auto      a = psio3::encode(psio3::frac32{}, p);
   auto      b = psio3::frac32::encode(p);
   REQUIRE(a == b);

   auto v = psio3::frac32::decode<FracPoint>(std::span<const char>{a});
   REQUIRE(v.x == 4);
   REQUIRE(v.y == 5);
}
