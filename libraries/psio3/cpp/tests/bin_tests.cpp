// Phase 10 — bin format (simplest binary).

#include <psio3/bin.hpp>
#include <psio3/reflect.hpp>

#include <catch.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct BinPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO3_REFLECT(BinPoint, x, y)

struct BinPerson
{
   std::string  name;
   std::int32_t age;
};
PSIO3_REFLECT(BinPerson, name, age)

TEST_CASE("bin round-trips primitives", "[bin][primitive]")
{
   auto b = psio3::encode(psio3::bin{}, std::uint64_t{0xDEADBEEF01020304});
   REQUIRE(b.size() == 8);
   auto v =
      psio3::decode<std::uint64_t>(psio3::bin{}, std::span<const char>{b});
   REQUIRE(v == 0xDEADBEEF01020304);
}

TEST_CASE("bin round-trips std::string with u32 length prefix", "[bin][string]")
{
   std::string s = "hi";
   auto        b = psio3::encode(psio3::bin{}, s);
   REQUIRE(b.size() == 4 + 2);
   auto v =
      psio3::decode<std::string>(psio3::bin{}, std::span<const char>{b});
   REQUIRE(v == "hi");
}

TEST_CASE("bin round-trips std::vector", "[bin][vector]")
{
   std::vector<std::uint32_t> v{1, 2, 3};
   auto                       b = psio3::encode(psio3::bin{}, v);
   REQUIRE(b.size() == 4 + 12);
   auto back = psio3::decode<std::vector<std::uint32_t>>(
      psio3::bin{}, std::span<const char>{b});
   REQUIRE(back == v);
}

TEST_CASE("bin round-trips std::optional", "[bin][optional]")
{
   std::optional<std::int32_t> some = 7;
   auto                        b    = psio3::encode(psio3::bin{}, some);
   REQUIRE(b.size() == 1 + 4);
   auto back = psio3::decode<std::optional<std::int32_t>>(
      psio3::bin{}, std::span<const char>{b});
   REQUIRE(back.has_value());
   REQUIRE(*back == 7);

   std::optional<std::int32_t> none;
   auto                        b2 = psio3::encode(psio3::bin{}, none);
   REQUIRE(b2.size() == 1);
   auto back2 = psio3::decode<std::optional<std::int32_t>>(
      psio3::bin{}, std::span<const char>{b2});
   REQUIRE(!back2.has_value());
}

TEST_CASE("bin round-trips reflected records", "[bin][record]")
{
   BinPerson pp{"alice", 30};
   auto      b = psio3::encode(psio3::bin{}, pp);
   auto back = psio3::decode<BinPerson>(psio3::bin{}, std::span<const char>{b});
   REQUIRE(back.name == "alice");
   REQUIRE(back.age == 30);
}

TEST_CASE("bin round-trips nested records", "[bin][record][nested]")
{
   BinPoint p{3, -4};
   auto     b = psio3::encode(psio3::bin{}, p);
   REQUIRE(b.size() == 8);
   auto back = psio3::decode<BinPoint>(psio3::bin{}, std::span<const char>{b});
   REQUIRE(back.x == 3);
   REQUIRE(back.y == -4);
}

TEST_CASE("bin scoped sugar works", "[bin][format_tag_base]")
{
   BinPoint p{1, 2};
   auto     a = psio3::encode(psio3::bin{}, p);
   auto     b = psio3::bin::encode(p);
   REQUIRE(a == b);
}
