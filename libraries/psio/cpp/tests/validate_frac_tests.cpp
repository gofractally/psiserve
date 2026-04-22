// Dynamic FracPack validation tests.
//
// Exercises the compile(schema, root) / validate_frac(buffer, validator)
// flow: schema reflection → compile → validate arbitrary byte buffers without
// the validator being compiled against the user types.
//
// This is the "ship a schema blob + data blob; validator has never seen the
// C++ types" path, complementing the compile-time validate_frac<T>.

#include <catch2/catch.hpp>
#include <psio/fracpack.hpp>
#include <psio/schema.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ── Test types ──────────────────────────────────────────────────────────────

struct FvPoint
{
   int32_t x;
   int32_t y;
};
PSIO_REFLECT(FvPoint, x, y)

struct FvPerson
{
   std::string             name;
   uint32_t                age;
   std::optional<uint32_t> score;
};
PSIO_REFLECT(FvPerson, name, age, score)

struct FvContainer
{
   std::string          label;
   std::vector<FvPoint> points;
};
PSIO_REFLECT(FvContainer, label, points)

// ────────────────────────────────────────────────────────────────────────
//  CORE ROUND-TRIP: serialize T, validate dynamically
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("dyn validate: valid FracPack of a struct", "[schema][validate_frac]")
{
   auto schema = psio::SchemaBuilder{}.insert<FvPoint>("Point").build();
   auto v      = psio::compile(schema, "Point");
   REQUIRE(v.valid());

   FvPoint p{7, 42};
   auto    bytes = psio::to_frac(p);

   auto result = psio::validate_frac(bytes, v);
   REQUIRE(result == psio::validation_t::valid);
}

TEST_CASE("dyn validate: variable-size struct with optional", "[schema][validate_frac]")
{
   auto schema = psio::SchemaBuilder{}.insert<FvPerson>("Person").build();
   auto v      = psio::compile(schema, "Person");
   REQUIRE(v.valid());

   FvPerson p{"alice", 30, 99};
   auto     bytes = psio::to_frac(p);

   REQUIRE(psio::validate_frac(bytes, v) == psio::validation_t::valid);
}

TEST_CASE("dyn validate: nested list of struct", "[schema][validate_frac]")
{
   auto schema = psio::SchemaBuilder{}.insert<FvContainer>("Container").build();
   auto v      = psio::compile(schema, "Container");
   REQUIRE(v.valid());

   FvContainer c{"test", {{1, 2}, {3, 4}, {5, 6}}};
   auto        bytes = psio::to_frac(c);

   REQUIRE(psio::validate_frac(bytes, v) == psio::validation_t::valid);
}

// ────────────────────────────────────────────────────────────────────────
//  INVALID INPUTS
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("dyn validate: unknown root type returns invalid", "[schema][validate_frac]")
{
   auto schema = psio::SchemaBuilder{}.insert<FvPoint>("Point").build();
   auto v      = psio::compile(schema, "NotATypeInTheSchema");
   REQUIRE_FALSE(v.valid());

   FvPoint p{1, 2};
   auto    bytes = psio::to_frac(p);
   REQUIRE(psio::validate_frac(bytes, v) == psio::validation_t::invalid);
}

TEST_CASE("dyn validate: truncated buffer is invalid", "[schema][validate_frac]")
{
   auto schema = psio::SchemaBuilder{}.insert<FvPerson>("Person").build();
   auto v      = psio::compile(schema, "Person");
   REQUIRE(v.valid());

   FvPerson p{"bob", 40, std::nullopt};
   auto     bytes = psio::to_frac(p);

   // Chop off the tail — validator must reject truncation of variable-size data.
   std::vector<char> truncated(bytes.begin(), bytes.begin() + bytes.size() / 2);
   REQUIRE(psio::validate_frac(truncated, v) == psio::validation_t::invalid);
}

TEST_CASE("dyn validate: empty buffer against fixed-size struct", "[schema][validate_frac]")
{
   auto schema = psio::SchemaBuilder{}.insert<FvPoint>("Point").build();
   auto v      = psio::compile(schema, "Point");
   REQUIRE(v.valid());

   std::vector<char> empty;
   REQUIRE(psio::validate_frac(empty, v) == psio::validation_t::invalid);
}

// ────────────────────────────────────────────────────────────────────────
//  COMPILE ONCE, VALIDATE MANY
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("dyn validate: reuse validator across many buffers", "[schema][validate_frac]")
{
   auto schema = psio::SchemaBuilder{}.insert<FvPoint>("Point").build();
   auto v      = psio::compile(schema, "Point");
   REQUIRE(v.valid());

   for (int32_t i = 0; i < 100; ++i)
   {
      FvPoint p{i, i * 2};
      auto    bytes = psio::to_frac(p);
      REQUIRE(psio::validate_frac(bytes, v) == psio::validation_t::valid);
   }
}

// ────────────────────────────────────────────────────────────────────────
//  SELF-HOSTING: ship the schema as FracPack, rehydrate, validate
// ────────────────────────────────────────────────────────────────────────
//
// This is the original design goal: a remote validator that was never compiled
// against the user's types receives (a) the schema as FracPack bytes and
// (b) a data blob, and answers yes/no.

TEST_CASE("dyn validate: schema ships as FracPack and rehydrates", "[schema][validate_frac][self-hosting]")
{
   // Sender side: build the schema, serialize it.
   auto              schema      = psio::SchemaBuilder{}.insert<FvContainer>("Container").build();
   std::vector<char> schema_wire = psio::to_frac(schema);

   FvContainer       payload{"shipped", {{10, 20}, {30, 40}}};
   std::vector<char> payload_wire = psio::to_frac(payload);

   // Receiver side: rehydrate schema, compile, validate.
   // The receiver has no knowledge of FvContainer as a C++ type.
   psio::Schema rehydrated;
   psio::from_frac(rehydrated, schema_wire);

   auto v = psio::compile(rehydrated, "Container");
   REQUIRE(v.valid());

   auto result = psio::validate_frac(payload_wire, v);
   REQUIRE(result == psio::validation_t::valid);
}
