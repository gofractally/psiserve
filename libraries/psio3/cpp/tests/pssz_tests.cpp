// Phase 7 — pSSZ format tag family (pssz8 / pssz16 / pssz32).
//
// Same shape coverage as ssz_tests.cpp but verifies:
//   1. All three widths round-trip primitives, arrays, vectors, strings,
//      optionals, and reflected records.
//   2. Narrower offsets produce smaller wire output.
//   3. Each width's codec agrees with itself on its own output.

#include <psio3/pssz.hpp>
#include <psio3/reflect.hpp>

#include <catch.hpp>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct PssPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO3_REFLECT(PssPoint, x, y)

struct PssLabelled
{
   std::int32_t id;
   std::string  name;
};
PSIO3_REFLECT(PssLabelled, id, name)

struct PssPacket
{
   std::uint16_t             version;
   std::vector<std::int32_t> payload;
};
PSIO3_REFLECT(PssPacket, version, payload)

TEMPLATE_TEST_CASE("pssz round-trips primitives across widths",
                   "[pssz][primitive]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   auto b  = psio3::encode(F{}, std::uint32_t{0xDEADBEEF});
   REQUIRE(b.size() == 4);
   auto v =
      psio3::decode<std::uint32_t>(F{}, std::span<const char>{b});
   REQUIRE(v == 0xDEADBEEF);
}

TEMPLATE_TEST_CASE("pssz round-trips std::string across widths",
                   "[pssz][string]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   auto b = psio3::encode(F{}, std::string{"hi"});
   auto v = psio3::decode<std::string>(F{}, std::span<const char>{b});
   REQUIRE(v == "hi");
}

TEMPLATE_TEST_CASE("pssz round-trips fixed records across widths",
                   "[pssz][record]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   PssPoint p{2, 5};
   auto     b = psio3::encode(F{}, p);
   REQUIRE(b.size() == 8);
   auto v = psio3::decode<PssPoint>(F{}, std::span<const char>{b});
   REQUIRE(v.x == 2);
   REQUIRE(v.y == 5);
}

TEST_CASE("pssz8 record-with-variable-field uses 1-byte offsets",
          "[pssz][record][variable]")
{
   PssLabelled l{7, "abc"};
   auto        b8  = psio3::encode(psio3::pssz8{}, l);
   auto        b16 = psio3::encode(psio3::pssz16{}, l);
   auto        b32 = psio3::encode(psio3::pssz32{}, l);

   // Fixed id(4) + offset (1/2/4) + tail "abc" (3).
   REQUIRE(b8.size() == 4 + 1 + 3);
   REQUIRE(b16.size() == 4 + 2 + 3);
   REQUIRE(b32.size() == 4 + 4 + 3);
}

TEMPLATE_TEST_CASE("pssz round-trips records with variable fields",
                   "[pssz][record][variable]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   PssLabelled l{42, "alice"};
   auto        b = psio3::encode(F{}, l);
   auto        v = psio3::decode<PssLabelled>(F{}, std::span<const char>{b});
   REQUIRE(v.id == 42);
   REQUIRE(v.name == "alice");
}

TEMPLATE_TEST_CASE("pssz round-trips vectors of fixed elements",
                   "[pssz][vector]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   std::vector<std::uint32_t> v{1, 2, 3};
   auto                       b = psio3::encode(F{}, v);
   REQUIRE(b.size() == 12);
   auto back = psio3::decode<std::vector<std::uint32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back == v);
}

TEMPLATE_TEST_CASE("pssz round-trips records with a vector field",
                   "[pssz][record][vector]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   PssPacket p{9, {100, 200, 300}};
   auto      b    = psio3::encode(F{}, p);
   auto      back = psio3::decode<PssPacket>(F{}, std::span<const char>{b});
   REQUIRE(back.version == 9);
   REQUIRE(back.payload == std::vector<std::int32_t>{100, 200, 300});
}

TEMPLATE_TEST_CASE("pssz round-trips std::optional across widths",
                   "[pssz][optional]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   std::optional<std::uint32_t> some = 123;
   auto                         b    = psio3::encode(F{}, some);
   REQUIRE(b.size() == 4);
   auto back = psio3::decode<std::optional<std::uint32_t>>(
      F{}, std::span<const char>{b});
   REQUIRE(back.has_value());
   REQUIRE(*back == 123);

   std::optional<std::uint32_t> none;
   auto                         b2 = psio3::encode(F{}, none);
   REQUIRE(b2.size() == 0);
}

TEMPLATE_TEST_CASE("pssz size_of agrees with encode output size",
                   "[pssz][size_of]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   PssLabelled l{1, "hello"};
   auto        b = psio3::encode(F{}, l);
   REQUIRE(psio3::size_of(F{}, l) == b.size());
}

TEMPLATE_TEST_CASE("pssz validate accepts well-formed buffers",
                   "[pssz][validate]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   auto b  = psio3::encode(F{}, std::uint32_t{0xCAFEBABE});
   auto s = psio3::validate<std::uint32_t>(F{}, std::span<const char>{b});
   REQUIRE(s.ok());
}

TEMPLATE_TEST_CASE("pssz validate rejects truncated primitives",
                   "[pssz][validate]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   char tiny[2]{};
   auto s =
      psio3::validate<std::uint32_t>(F{}, std::span<const char>{tiny, 2});
   REQUIRE(!s.ok());
   REQUIRE(s.error().format_name == "pssz");
}

TEMPLATE_TEST_CASE("pssz scoped sugar matches generic CPO",
                   "[pssz][format_tag_base]", psio3::pssz8, psio3::pssz16,
                   psio3::pssz32)
{
   using F = TestType;
   PssPoint p{8, -4};
   auto     a = psio3::encode(F{}, p);
   auto     b = F::encode(p);
   REQUIRE(a == b);

   auto v  = psio3::decode<PssPoint>(F{}, std::span<const char>{a});
   auto v2 = F::template decode<PssPoint>(std::span<const char>{a});
   REQUIRE(v.x == v2.x);
   REQUIRE(v.y == v2.y);
}
