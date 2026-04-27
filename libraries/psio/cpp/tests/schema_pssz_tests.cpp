// pSSZ round-trip on Schema IR.
//
// pSSZ is the canonical schema-schema wire format: every IR type is
// PSIO_REFLECT'd so a populated Schema is just another reflected
// struct as far as the codec is concerned.
//
// Currently working: flat IR records (Package, Int, Float,
// Attribute, etc.) round-trip cleanly, AND any IR type that recurses
// through AnyType — Member, Func, Object containing variable-shape
// members — round-trip through Box<T> as a transparent wrapper.
//
// Still blocked on a full Schema round-trip: pssz has no case for
// std::map<K, V> (size_of_v / encode_value / decode_value all hit
// `unsupported type` static_asserts).  Schema::types is a std::map;
// either pssz adds a case mirroring vector<pair<K, V>>, or Schema
// swaps the map for a vector<pair> with the map invariants
// (sorted-by-key, unique-keys) enforced at the IR layer.

#include <psio/schema_ir.hpp>
#include <psio/pssz.hpp>

#include <catch.hpp>

#include <cstdint>
#include <string>

using psio::Package;
using psio::schema_types::Float;
using psio::schema_types::Int;

// ─── Working: flat IR records round-trip ─────────────────────────────

TEST_CASE("pssz: Int round-trips", "[schema_pssz]")
{
   Int original{32, true};
   auto bytes   = psio::encode(psio::pssz{}, original);
   REQUIRE_FALSE(bytes.empty());
   auto decoded = psio::decode<Int>(psio::pssz{}, bytes);
   CHECK(decoded == original);
}

TEST_CASE("pssz: Float round-trips", "[schema_pssz]")
{
   Float original{8, 23};
   auto bytes   = psio::encode(psio::pssz{}, original);
   auto decoded = psio::decode<Float>(psio::pssz{}, bytes);
   CHECK(decoded == original);
}

TEST_CASE("pssz: Package round-trips with version", "[schema_pssz]")
{
   Package original{
      .name       = "wasi:clocks",
      .version    = "0.2.3",
      .attributes = {}};
   auto bytes   = psio::encode(psio::pssz{}, original);
   auto decoded = psio::decode<Package>(psio::pssz{}, bytes);
   CHECK(decoded.name == original.name);
   CHECK(decoded.version == original.version);
}

TEST_CASE("pssz: Package with multiple attributes round-trips",
          "[schema_pssz]")
{
   using psio::schema_types::Attribute;
   Package original{
      .name       = "test:pkg",
      .version    = "1.0.0",
      .attributes = {Attribute{.name = "since",
                               .value = std::string{"0.1.0"}},
                     Attribute{.name = "stable", .value = std::nullopt}}};
   auto bytes   = psio::encode(psio::pssz{}, original);
   auto decoded = psio::decode<Package>(psio::pssz{}, bytes);
   CHECK(decoded.attributes.size() == 2);
   CHECK(decoded.attributes[0].name == "since");
   CHECK(decoded.attributes[0].value.has_value());
   CHECK(*decoded.attributes[0].value == "0.1.0");
   CHECK(decoded.attributes[1].name == "stable");
   CHECK_FALSE(decoded.attributes[1].value.has_value());
}

// ─── Box<T> as transparent wrapper ───────────────────────────────────

TEST_CASE("pssz: Box<int> encodes identically to inner int",
          "[schema_pssz][box]")
{
   using psio::schema_types::Box;
   std::int32_t plain = 42;
   Box<std::int32_t> boxed{42};

   auto plain_bytes = psio::encode(psio::pssz{}, plain);
   auto boxed_bytes = psio::encode(psio::pssz{}, boxed);
   CHECK(plain_bytes == boxed_bytes);

   auto decoded = psio::decode<Box<std::int32_t>>(psio::pssz{}, boxed_bytes);
   CHECK(*decoded == 42);
}

TEST_CASE("pssz: Box<Int> round-trips inside an IR record",
          "[schema_pssz][box]")
{
   using psio::Member;
   using psio::schema_types::AnyType;
   using psio::schema_types::Box;
   using psio::schema_types::Int;

   Member original{
      .name       = "seconds",
      .type       = Box<AnyType>{AnyType{Int{64, false}}},
      .attributes = {}};

   auto bytes   = psio::encode(psio::pssz{}, original);
   auto decoded = psio::decode<Member>(psio::pssz{}, bytes);

   CHECK(decoded.name == "seconds");
   REQUIRE(decoded.type.value);
   CHECK(decoded == original);
}

TEST_CASE("pssz: nested Box<AnyType> recursive shape round-trips",
          "[schema_pssz][box]")
{
   using psio::schema_types::AnyType;
   using psio::schema_types::Box;
   using psio::schema_types::Int;
   using psio::schema_types::List;

   // list<list<u8>> — one Box layer for List, one for the inner list.
   AnyType original{List{Box<AnyType>{
      List{Box<AnyType>{Int{8, false}}}}}};

   auto bytes   = psio::encode(psio::pssz{}, original);
   auto decoded = psio::decode<AnyType>(psio::pssz{}, bytes);
   CHECK(decoded == original);
}
