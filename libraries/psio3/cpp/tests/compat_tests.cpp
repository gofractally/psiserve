// Phase 17 — compat layer.
//
// Validates that including psio3/compat/psio_aliases.hpp gives existing
// consumers the `psio::` spellings they used against psio v1, now
// resolving to psio3. Once Phase 18 renames psio3 → psio, this header
// disappears and callsites keep working verbatim.

#include <psio3/compat/psio_aliases.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>
#include <vector>

// Use the old PSIO_REFLECT spelling — the macro alias re-routes.
struct CompatPoint
{
   std::int32_t x;
   std::int32_t y;
};
PSIO_REFLECT(CompatPoint, x, y)

struct CompatPerson
{
   std::string                 name;
   std::int32_t                age;
   std::vector<std::uint32_t>  scores;
};
PSIO_REFLECT(CompatPerson, name, age, scores)

TEST_CASE("psio:: namespace alias routes encode/decode through psio3",
          "[compat]")
{
   CompatPoint p{4, 7};
   auto        bytes = psio::encode(psio::ssz{}, p);
   auto        back =
      psio::decode<CompatPoint>(psio::ssz{}, std::span<const char>{bytes});
   REQUIRE(back.x == 4);
   REQUIRE(back.y == 7);
}

TEST_CASE("psio::reflect<T> resolves through the namespace alias",
          "[compat][reflect]")
{
   STATIC_REQUIRE(psio::reflect<CompatPoint>::member_count == 2);
   STATIC_REQUIRE(psio::reflect<CompatPoint>::member_name<0> == "x");
   STATIC_REQUIRE(psio::reflect<CompatPoint>::member_name<1> == "y");
}

TEST_CASE("psio:: identity with psio3:: — they name the same entities",
          "[compat]")
{
   // Tag types are the same struct regardless of namespace spelling.
   STATIC_REQUIRE(std::is_same_v<psio::ssz, psio3::ssz>);
   STATIC_REQUIRE(std::is_same_v<psio::json, psio3::json>);
   STATIC_REQUIRE(std::is_same_v<psio::frac32, psio3::frac32>);
}

TEST_CASE("psio::schema_of routes to psio3 schema emitter",
          "[compat][schema]")
{
   auto sc = psio::schema_of<CompatPerson>();
   REQUIRE(sc.is_record());
   REQUIRE(sc.as_record().fields.size() == 3);
   REQUIRE(sc.as_record().fields[0].name == "name");
}

TEST_CASE("psio::transcode is callable under the aliased namespace",
          "[compat][transcode]")
{
   CompatPerson pp{"jack", 19, {1, 2, 3}};
   auto         sc = psio::schema_of<CompatPerson>();
   auto         ssz_bytes = psio::encode(psio::ssz{}, pp);
   auto         json_str  = psio::transcode(
      psio::ssz{}, sc, std::span<const char>{ssz_bytes}, psio::json{});

   REQUIRE(json_str.find("\"name\":\"jack\"") != std::string::npos);
   REQUIRE(json_str.find("\"age\":19") != std::string::npos);
}

TEST_CASE("Mixed psio:: and psio3:: call sites share state",
          "[compat]")
{
   CompatPoint p{11, 22};
   auto        ssz_bytes_new = psio3::encode(psio3::ssz{}, p);
   auto        ssz_bytes_old = psio::encode(psio::ssz{}, p);
   REQUIRE(ssz_bytes_new == ssz_bytes_old);
}
