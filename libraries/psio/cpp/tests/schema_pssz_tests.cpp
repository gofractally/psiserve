// pSSZ round-trip on Schema IR.
//
// pSSZ is the canonical schema-schema wire format: every IR type is
// PSIO_REFLECT'd so a populated Schema is just another reflected
// struct as far as the codec is concerned.  This file pins the
// progress so far + flags the codec gaps that block a full Schema
// round-trip.
//
// Currently working: every flat IR record (Package, Int, Float,
// Member-without-AnyType, etc.) round-trips through pssz cleanly.
//
// Currently blocked: the full Schema round-trip needs two pssz codec
// extensions:
//
//   1. Box<T> support — pssz must recognise psio::schema_types::Box<T>
//      and treat it as transparent (encode forwards to T; decode
//      reads T into a fresh Box).  Box<T> is the cycle-breaker on
//      Member::type / Func::result, so any IR type that recurses
//      through AnyType hits this gap.
//
//   2. std::map<K, V> support — pssz currently has no case for
//      associative containers (size_of_v / encode_value /
//      decode_value all hit `unsupported type` static_asserts).
//      Schema::types is a std::map; either pssz adds a case
//      mirroring std::vector<std::pair<K, V>>, or Schema swaps the
//      map for a vector<pair> with the map's invariant
//      (sorted-by-key, unique-keys) enforced at the IR layer.
//
// Both are real codec extensions touching the offset-table /
// fixed-vs-variable lane machinery.  Tracked separately; this file
// pins the boundary and is updated as those land.

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

// Full-Schema round-trip lands here once Box<T> + std::map cases are
// added to pssz.  A failing/skipped case isn't useful; this comment
// is the note.
