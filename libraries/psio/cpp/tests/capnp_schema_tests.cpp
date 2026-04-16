// capnp_schema_tests.cpp — Catch2 tests for capnp_schema<T>()
//
// Verifies that generated .capnp schema text contains correct field names,
// types, ordinals, union syntax, and enum definitions.

#include <catch2/catch.hpp>
#include <psio/capnp_schema.hpp>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <variant>
#include <vector>

// ── Test types ──────────────────────────────────────────────────────────────

struct SchemaPoint
{
   double x = 0;
   double y = 0;
};
PSIO_REFLECT(SchemaPoint, definitionWillNotChange(), x, y)

struct SchemaToken
{
   uint16_t    kind   = 0;
   uint32_t    offset = 0;
   uint32_t    length = 0;
   std::string text;
};
PSIO_REFLECT(SchemaToken, kind, offset, length, text)

struct SchemaUser
{
   uint64_t                 id       = 0;
   std::string              name;
   std::string              email;
   uint32_t                 age      = 0;
   double                   score    = 0;
   std::vector<std::string> tags;
   bool                     verified = false;
};
PSIO_REFLECT(SchemaUser, id, name, email, age, score, tags, verified)

struct SchemaLineItem
{
   std::string product;
   uint32_t    qty        = 0;
   double      unit_price = 0;
};
PSIO_REFLECT(SchemaLineItem, product, qty, unit_price)

struct SchemaOrder
{
   uint64_t                      id = 0;
   SchemaUser                    customer;
   std::vector<SchemaLineItem>   items;
   double                        total = 0;
   std::string                   note;
};
PSIO_REFLECT(SchemaOrder, id, customer, items, total, note)

// Union test type
struct SchemaShape
{
   double area = 0;
   std::variant<double, std::string, std::monostate> shape;
};
PSIO_REFLECT(SchemaShape, area, shape)

// Enum test type
enum class Color : uint16_t
{
   Red   = 0,
   Green = 1,
   Blue  = 2,
};
PSIO_REFLECT_ENUM(Color, Red, Green, Blue)

struct SchemaWithEnum
{
   Color       color = Color::Red;
   std::string label;
};
PSIO_REFLECT(SchemaWithEnum, color, label)

// Struct with default values
struct SchemaDefaults
{
   int32_t  count    = 42;
   double   ratio    = 3.14;
   bool     enabled  = true;
   uint64_t id       = 0;
};
PSIO_REFLECT(SchemaDefaults, count, ratio, enabled, id)

// Struct with vector of structs
struct SchemaContainer
{
   std::vector<SchemaPoint>  points;
   std::vector<uint32_t>     ids;
   std::vector<uint8_t>      raw_data;
};
PSIO_REFLECT(SchemaContainer, points, ids, raw_data)

// ── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("capnp schema: simple struct with scalars", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaPoint>();
   INFO("Generated schema:\n" << schema);

   // Must start with a file ID
   REQUIRE(schema.find("@0x") == 0);
   REQUIRE(schema.find(";\n") != std::string::npos);

   // Must contain the struct declaration with a schema ID
   REQUIRE(schema.find("struct SchemaPoint @0x") != std::string::npos);

   // Must have fields with correct ordinals and types
   REQUIRE(schema.find("x @0 :Float64") != std::string::npos);
   REQUIRE(schema.find("y @1 :Float64") != std::string::npos);
}

TEST_CASE("capnp schema: mixed scalar and pointer fields", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaToken>();
   INFO("Generated schema:\n" << schema);

   REQUIRE(schema.find("struct SchemaToken @0x") != std::string::npos);
   REQUIRE(schema.find("kind @0 :UInt16") != std::string::npos);
   REQUIRE(schema.find("offset @1 :UInt32") != std::string::npos);
   REQUIRE(schema.find("length @2 :UInt32") != std::string::npos);
   REQUIRE(schema.find("text @3 :Text") != std::string::npos);
}

TEST_CASE("capnp schema: nested struct and lists", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaOrder>();
   INFO("Generated schema:\n" << schema);

   // Main struct
   REQUIRE(schema.find("struct SchemaOrder @0x") != std::string::npos);
   REQUIRE(schema.find("id @0 :UInt64") != std::string::npos);
   REQUIRE(schema.find("customer @1 :SchemaUser") != std::string::npos);
   REQUIRE(schema.find("items @2 :List(SchemaLineItem)") != std::string::npos);
   REQUIRE(schema.find("total @3 :Float64") != std::string::npos);
   REQUIRE(schema.find("note @4 :Text") != std::string::npos);

   // Dependency structs must appear before the main struct
   auto user_pos  = schema.find("struct SchemaUser @0x");
   auto item_pos  = schema.find("struct SchemaLineItem @0x");
   auto order_pos = schema.find("struct SchemaOrder @0x");
   REQUIRE(user_pos != std::string::npos);
   REQUIRE(item_pos != std::string::npos);
   REQUIRE(user_pos < order_pos);
   REQUIRE(item_pos < order_pos);
}

TEST_CASE("capnp schema: union via std::variant", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaShape>();
   INFO("Generated schema:\n" << schema);

   REQUIRE(schema.find("struct SchemaShape @0x") != std::string::npos);
   REQUIRE(schema.find("area @0 :Float64") != std::string::npos);

   // Union fields: the variant expands to ordinals @1, @2, @3.
   // The discriminant is implicit (capnp allocates it automatically).
   REQUIRE(schema.find("union {") != std::string::npos);
   REQUIRE(schema.find("shape0 @1 :Float64") != std::string::npos);
   REQUIRE(schema.find("shape1 @2 :Text") != std::string::npos);
   REQUIRE(schema.find("shape2 @3 :Void") != std::string::npos);

   // No explicit discriminant field — capnp handles it implicitly
   REQUIRE(schema.find("shapeDisc") == std::string::npos);
}

TEST_CASE("capnp schema: enum definition", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaWithEnum>();
   INFO("Generated schema:\n" << schema);

   // Enum definition must appear as a dependency
   REQUIRE(schema.find("enum Color @0x") != std::string::npos);
   REQUIRE(schema.find("red @0") != std::string::npos);
   REQUIRE(schema.find("green @1") != std::string::npos);
   REQUIRE(schema.find("blue @2") != std::string::npos);

   // Struct using the enum
   REQUIRE(schema.find("struct SchemaWithEnum @0x") != std::string::npos);
   REQUIRE(schema.find("color @0 :Color") != std::string::npos);
   REQUIRE(schema.find("label @1 :Text") != std::string::npos);
}

TEST_CASE("capnp schema: default values", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaDefaults>();
   INFO("Generated schema:\n" << schema);

   REQUIRE(schema.find("count @0 :Int32 = 42") != std::string::npos);
   REQUIRE(schema.find("ratio @1 :Float64 = 3.14") != std::string::npos);
   REQUIRE(schema.find("enabled @2 :Bool = true") != std::string::npos);
   // id has default 0, so no " = 0" suffix
   auto id_pos = schema.find("id @3 :UInt64");
   REQUIRE(id_pos != std::string::npos);
   // Make sure there's no " = 0" immediately after
   auto after_id = schema.substr(id_pos, 30);
   REQUIRE(after_id.find("= 0") == std::string::npos);
}

TEST_CASE("capnp schema: container types (List, Data)", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaContainer>();
   INFO("Generated schema:\n" << schema);

   REQUIRE(schema.find("points @0 :List(SchemaPoint)") != std::string::npos);
   REQUIRE(schema.find("ids @1 :List(UInt32)") != std::string::npos);
   REQUIRE(schema.find("rawData @2 :Data") != std::string::npos);

   // SchemaPoint dependency must be emitted
   REQUIRE(schema.find("struct SchemaPoint @0x") != std::string::npos);
}

TEST_CASE("capnp schema: user struct with lists and bools", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaUser>();
   INFO("Generated schema:\n" << schema);

   REQUIRE(schema.find("id @0 :UInt64") != std::string::npos);
   REQUIRE(schema.find("name @1 :Text") != std::string::npos);
   REQUIRE(schema.find("email @2 :Text") != std::string::npos);
   REQUIRE(schema.find("age @3 :UInt32") != std::string::npos);
   REQUIRE(schema.find("score @4 :Float64") != std::string::npos);
   REQUIRE(schema.find("tags @5 :List(Text)") != std::string::npos);
   REQUIRE(schema.find("verified @6 :Bool") != std::string::npos);
}

TEST_CASE("capnp schema: deterministic IDs", "[schema][cp]")
{
   // Same type should always produce the same schema
   auto schema1 = psio::capnp_schema<SchemaPoint>();
   auto schema2 = psio::capnp_schema<SchemaPoint>();
   REQUIRE(schema1 == schema2);
}

TEST_CASE("capnp schema: file ID is distinct from struct ID", "[schema][cp]")
{
   auto schema = psio::capnp_schema<SchemaPoint>();

   // Extract file ID (first line)
   auto semicolon = schema.find(';');
   auto file_id   = schema.substr(1, semicolon - 1);

   // Extract struct ID
   auto struct_at  = schema.find("struct SchemaPoint @");
   auto struct_id_start = schema.find("@0x", struct_at);
   auto struct_id_end   = schema.find(' ', struct_id_start);
   if (struct_id_end == std::string::npos)
      struct_id_end = schema.find('{', struct_id_start);
   auto struct_id = schema.substr(struct_id_start + 1, struct_id_end - struct_id_start - 1);

   // File ID and struct ID should be different
   INFO("file_id=" << file_id << " struct_id=" << struct_id);
   REQUIRE(file_id != struct_id);
}

// ── Validation with official capnp tool (if available) ──────────────────

namespace
{
   bool capnp_tool_available()
   {
      return std::system("capnp --version > /dev/null 2>&1") == 0;
   }

   bool validate_schema_with_capnp(const std::string& schema, const char* name)
   {
      std::string path = std::string("/tmp/psio_test_") + name + ".capnp";
      {
         std::ofstream f(path);
         f << schema;
      }
      std::string cmd = "capnp compile -o- " + path + " > /dev/null 2>&1";
      return std::system(cmd.c_str()) == 0;
   }
}  // namespace

TEST_CASE("capnp schema: validates with official capnp tool", "[schema][cp][capnp-tool]")
{
   if (!capnp_tool_available())
   {
      WARN("capnp tool not found, skipping validation");
      return;
   }

   SECTION("simple struct")
   {
      auto schema = psio::capnp_schema<SchemaPoint>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "point"));
   }

   SECTION("mixed types")
   {
      auto schema = psio::capnp_schema<SchemaToken>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "token"));
   }

   SECTION("nested struct with dependencies")
   {
      auto schema = psio::capnp_schema<SchemaOrder>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "order"));
   }

   SECTION("union via variant")
   {
      auto schema = psio::capnp_schema<SchemaShape>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "shape"));
   }

   SECTION("enum")
   {
      auto schema = psio::capnp_schema<SchemaWithEnum>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "with_enum"));
   }

   SECTION("default values")
   {
      auto schema = psio::capnp_schema<SchemaDefaults>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "defaults"));
   }

   SECTION("containers (list, data)")
   {
      auto schema = psio::capnp_schema<SchemaContainer>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "container"));
   }

   SECTION("user struct")
   {
      auto schema = psio::capnp_schema<SchemaUser>();
      INFO("Schema:\n" << schema);
      REQUIRE(validate_schema_with_capnp(schema, "user"));
   }
}
