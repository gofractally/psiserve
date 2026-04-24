#include <catch2/catch.hpp>
#include <psio/bounded.hpp>
#include <psio/emit_wit.hpp>
#include <psio/ext_int.hpp>
#include <psio/fracpack.hpp>
#include <psio/schema.hpp>

// ── Reflected types exercising ext_int and bounded containers ─────────────────

struct SchEthAccount
{
   psio::uint256 balance;
   psio::uint128 nonce;
};
PSIO_REFLECT(SchEthAccount, definitionWillNotChange(), balance, nonce)

struct SchBeaconish
{
   std::uint64_t                              slot;
   psio::bounded_string<64>                   graffiti;
   psio::bounded_bytes<256>                   message_data;
   psio::bounded_list<psio::uint256, 1024>    balances;
};
PSIO_REFLECT(SchBeaconish, slot, graffiti, message_data, balances)

// ── Schema generation for ext_int types ──────────────────────────────────────

TEST_CASE("schema: uint128/int128/uint256 become Int types with correct bit width",
          "[schema][ext_int]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}.insert<SchEthAccount>("EthAccount").build();

   auto type_of = [&](const S::AnyType& t) -> const S::AnyType* {
      return t.resolve(schema);
   };

   const auto* root_any = schema.get("EthAccount");
   REQUIRE(root_any != nullptr);
   const auto* resolved = root_any->resolve(schema);
   REQUIRE(resolved != nullptr);

   auto* obj = std::get_if<S::Struct>(&resolved->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 2);

   auto check_int = [&](const S::AnyType& t, std::uint32_t bits, bool signed_) {
      const auto* r = t.resolve(schema);
      REQUIRE(r != nullptr);
      auto* i = std::get_if<S::Int>(&r->value);
      REQUIRE(i != nullptr);
      REQUIRE(i->bits == bits);
      REQUIRE(i->isSigned == signed_);
   };

   check_int(obj->members[0].type, 256, false);  // balance = uint256
   check_int(obj->members[1].type, 128, false);  // nonce   = uint128
   (void)type_of;
}

// ── Schema generation for bounded types ──────────────────────────────────────

TEST_CASE("schema: bounded_list emits BoundedList with maxCount", "[schema][bounded]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}.insert<SchBeaconish>("Beaconish").build();

   const auto* resolved = schema.get("Beaconish")->resolve(schema);
   auto*       obj      = std::get_if<S::Object>(&resolved->value);
   REQUIRE(obj != nullptr);
   REQUIRE(obj->members.size() == 4);

   // balances: bounded_list<uint256, 1024> → BoundedList with maxCount=1024
   const auto* balances_r = obj->members[3].type.resolve(schema);
   auto*       blist      = std::get_if<S::BoundedList>(&balances_r->value);
   REQUIRE(blist != nullptr);
   REQUIRE(blist->maxCount == 1024);

   // Inner type should resolve to uint256 → Int{256, unsigned}
   const auto* inner = blist->type->resolve(schema);
   auto*       i     = std::get_if<S::Int>(&inner->value);
   REQUIRE(i != nullptr);
   REQUIRE(i->bits == 256);
}

TEST_CASE("schema: bounded_string emits Custom{BoundedList<u8>, \"string\"}",
          "[schema][bounded]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}.insert<SchBeaconish>("Beaconish").build();

   const auto* resolved = schema.get("Beaconish")->resolve(schema);
   auto*       obj      = std::get_if<S::Object>(&resolved->value);
   REQUIRE(obj != nullptr);

   // graffiti: bounded_string<64> → Custom{BoundedList<u8>, "string"} with maxCount=64
   const auto* graffiti_r = obj->members[1].type.resolve(schema);
   auto*       custom     = std::get_if<S::Custom>(&graffiti_r->value);
   REQUIRE(custom != nullptr);
   REQUIRE(custom->id == "string");

   auto* blist = std::get_if<S::BoundedList>(&custom->type->value);
   REQUIRE(blist != nullptr);
   REQUIRE(blist->maxCount == 64);
}

TEST_CASE("schema: bounded_bytes emits BoundedList<u8>", "[schema][bounded]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}.insert<SchBeaconish>("Beaconish").build();

   const auto* resolved = schema.get("Beaconish")->resolve(schema);
   auto*       obj      = std::get_if<S::Object>(&resolved->value);
   REQUIRE(obj != nullptr);

   // message_data: bounded_bytes<256> → BoundedList<u8> with maxCount=256
   const auto* data_r = obj->members[2].type.resolve(schema);
   auto*       blist  = std::get_if<S::BoundedList>(&data_r->value);
   REQUIRE(blist != nullptr);
   REQUIRE(blist->maxCount == 256);

   const auto* inner = blist->type->resolve(schema);
   auto*       i     = std::get_if<S::Int>(&inner->value);
   REQUIRE(i != nullptr);
   REQUIRE(i->bits == 8);
   REQUIRE(i->isSigned == false);
}

// ── WIT emission ──────────────────────────────────────────────────────────────

TEST_CASE("schema: WIT emit renders bounded types with @psio:max comment",
          "[schema][bounded][wit]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}.insert<SchBeaconish>("Beaconish").build();
   auto wit    = psio::schema_types::emit_wit(schema);

   // Should see the bound comment for at least one bounded field.
   INFO("WIT output:\n" << wit);
   REQUIRE(wit.find("@psio:max=") != std::string::npos);
   REQUIRE(wit.find("@psio:max=1024") != std::string::npos);  // balances bound
   REQUIRE(wit.find("@psio:max=256") != std::string::npos);   // message_data bound
}

TEST_CASE("schema: WIT emit renders uint256 as u256", "[schema][ext_int][wit]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}.insert<SchEthAccount>("EthAccount").build();
   auto wit    = psio::schema_types::emit_wit(schema);

   INFO("WIT output:\n" << wit);
   REQUIRE(wit.find("u256") != std::string::npos);
   REQUIRE(wit.find("u128") != std::string::npos);
}

// ── Schema round-trip through fracpack preserves bounds ───────────────────────

TEST_CASE("schema: bounded info survives fracpack round-trip", "[schema][bounded][fracpack]")
{
   namespace S = psio::schema_types;
   auto schema = S::SchemaBuilder{}.insert<SchBeaconish>("Beaconish").build();

   auto bytes = psio::to_frac(schema);
   auto rt    = psio::from_frac<S::Schema>(bytes);

   const auto* resolved = rt.get("Beaconish")->resolve(rt);
   auto*       obj      = std::get_if<S::Object>(&resolved->value);
   REQUIRE(obj != nullptr);

   const auto* balances_r = obj->members[3].type.resolve(rt);
   auto*       blist      = std::get_if<S::BoundedList>(&balances_r->value);
   REQUIRE(blist != nullptr);
   REQUIRE(blist->maxCount == 1024);
}
