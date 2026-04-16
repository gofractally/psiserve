// fbs_parser_tests.cpp — Catch2 tests for fbs_parse()
//
// Verifies that the FlatBuffers schema parser correctly tokenizes,
// parses, resolves types, and computes layouts for .fbs IDL text.

#include <catch2/catch.hpp>
#include <psio/fbs_parser.hpp>

#include <string_view>

using namespace psio;

// =========================================================================
// Basic parsing
// =========================================================================

TEST_CASE("fbs_parser: empty schema", "[fbs]")
{
   auto schema = fbs_parse("");
   CHECK(schema.types.empty());
   CHECK(schema.root_type.empty());
   CHECK(schema.ns.empty());
}

TEST_CASE("fbs_parser: namespace", "[fbs]")
{
   auto schema = fbs_parse("namespace game.entities;");
   CHECK(schema.ns == "game.entities");
}

TEST_CASE("fbs_parser: simple table", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Monster {
         name:string;
         hp:short = 100;
         mana:short = 150;
      }
   )");

   REQUIRE(schema.types.size() == 1);
   auto& t = schema.types[0];
   CHECK(t.name == "Monster");
   CHECK(t.kind == fbs_type_kind::table_);
   REQUIRE(t.fields.size() == 3);

   CHECK(t.fields[0].name == "name");
   CHECK(t.fields[0].type == fbs_base_type::string_);

   CHECK(t.fields[1].name == "hp");
   CHECK(t.fields[1].type == fbs_base_type::int16_);
   CHECK(t.fields[1].has_default);
   CHECK(t.fields[1].default_int == 100);

   CHECK(t.fields[2].name == "mana");
   CHECK(t.fields[2].type == fbs_base_type::int16_);
   CHECK(t.fields[2].has_default);
   CHECK(t.fields[2].default_int == 150);
}

TEST_CASE("fbs_parser: struct definition", "[fbs]")
{
   auto schema = fbs_parse(R"(
      struct Vec3 {
         x:float;
         y:float;
         z:float;
      }
   )");

   REQUIRE(schema.types.size() == 1);
   auto& t = schema.types[0];
   CHECK(t.name == "Vec3");
   CHECK(t.kind == fbs_type_kind::struct_);
   REQUIRE(t.fields.size() == 3);

   CHECK(t.fields[0].name == "x");
   CHECK(t.fields[0].type == fbs_base_type::float32_);
   CHECK(t.fields[1].name == "y");
   CHECK(t.fields[2].name == "z");

   // Check struct layout
   CHECK(t.struct_size == 12);  // 3 * 4 bytes
   CHECK(t.struct_align == 4);

   CHECK(t.fields[0].struct_offset == 0);
   CHECK(t.fields[1].struct_offset == 4);
   CHECK(t.fields[2].struct_offset == 8);
}

TEST_CASE("fbs_parser: struct alignment", "[fbs]")
{
   auto schema = fbs_parse(R"(
      struct Mixed {
         a:byte;
         b:int;
         c:byte;
         d:double;
      }
   )");

   REQUIRE(schema.types.size() == 1);
   auto& t = schema.types[0];

   // byte(1) + pad(3) + int(4) + byte(1) + pad(7) + double(8) = 24
   CHECK(t.fields[0].struct_offset == 0);   // byte at 0
   CHECK(t.fields[1].struct_offset == 4);   // int at 4 (aligned to 4)
   CHECK(t.fields[2].struct_offset == 8);   // byte at 8
   CHECK(t.fields[3].struct_offset == 16);  // double at 16 (aligned to 8)
   CHECK(t.struct_size == 24);              // padded to align 8
   CHECK(t.struct_align == 8);
}

TEST_CASE("fbs_parser: enum definition", "[fbs]")
{
   auto schema = fbs_parse(R"(
      enum Color : ubyte {
         Red = 0,
         Green,
         Blue = 5,
         Alpha,
      }
   )");

   REQUIRE(schema.types.size() == 1);
   auto& t = schema.types[0];
   CHECK(t.name == "Color");
   CHECK(t.kind == fbs_type_kind::enum_);
   CHECK(t.underlying_type == fbs_base_type::uint8_);
   REQUIRE(t.enum_values.size() == 4);

   CHECK(t.enum_values[0].name == "Red");
   CHECK(t.enum_values[0].value == 0);
   CHECK(t.enum_values[1].name == "Green");
   CHECK(t.enum_values[1].value == 1);
   CHECK(t.enum_values[2].name == "Blue");
   CHECK(t.enum_values[2].value == 5);
   CHECK(t.enum_values[3].name == "Alpha");
   CHECK(t.enum_values[3].value == 6);
}

TEST_CASE("fbs_parser: union definition", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Sword { damage:int; }
      table Shield { armor:int; }
      union Equipment { Sword, Shield }
   )");

   REQUIRE(schema.types.size() == 3);

   auto* u = schema.find_type("Equipment");
   REQUIRE(u != nullptr);
   CHECK(u->kind == fbs_type_kind::union_);
   REQUIRE(u->union_members.size() == 2);
   CHECK(u->union_members[0].name == "Sword");
   CHECK(u->union_members[0].type_idx >= 0);
   CHECK(u->union_members[1].name == "Shield");
   CHECK(u->union_members[1].type_idx >= 0);
}

// =========================================================================
// Type references and resolution
// =========================================================================

TEST_CASE("fbs_parser: table with struct field", "[fbs]")
{
   auto schema = fbs_parse(R"(
      struct Vec3 { x:float; y:float; z:float; }
      table Monster {
         pos:Vec3;
         name:string;
      }
   )");

   REQUIRE(schema.types.size() == 2);
   auto* monster = schema.find_type("Monster");
   REQUIRE(monster != nullptr);
   REQUIRE(monster->fields.size() == 2);

   CHECK(monster->fields[0].name == "pos");
   CHECK(monster->fields[0].type == fbs_base_type::struct_);
   CHECK(monster->fields[0].type_idx >= 0);
   CHECK(monster->fields[0].is_offset_type == false);  // structs inline in tables

   CHECK(monster->fields[1].name == "name");
   CHECK(monster->fields[1].type == fbs_base_type::string_);
   CHECK(monster->fields[1].is_offset_type == true);
}

TEST_CASE("fbs_parser: table with nested table", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Weapon { damage:int; }
      table Monster {
         weapon:Weapon;
         name:string;
      }
   )");

   auto* monster = schema.find_type("Monster");
   REQUIRE(monster != nullptr);
   CHECK(monster->fields[0].name == "weapon");
   CHECK(monster->fields[0].type == fbs_base_type::table_);
   CHECK(monster->fields[0].is_offset_type == true);
}

TEST_CASE("fbs_parser: vector types", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Inventory {
         items:[string];
         counts:[int];
         data:[ubyte];
      }
   )");

   auto* inv = schema.find_type("Inventory");
   REQUIRE(inv != nullptr);
   REQUIRE(inv->fields.size() == 3);

   CHECK(inv->fields[0].type == fbs_base_type::vector_);
   CHECK(inv->fields[0].elem_type == fbs_base_type::string_);

   CHECK(inv->fields[1].type == fbs_base_type::vector_);
   CHECK(inv->fields[1].elem_type == fbs_base_type::int32_);

   CHECK(inv->fields[2].type == fbs_base_type::vector_);
   CHECK(inv->fields[2].elem_type == fbs_base_type::uint8_);
}

TEST_CASE("fbs_parser: vector of tables", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Item { name:string; value:int; }
      table Inventory { items:[Item]; }
   )");

   auto* inv = schema.find_type("Inventory");
   REQUIRE(inv != nullptr);
   CHECK(inv->fields[0].type == fbs_base_type::vector_);
   CHECK(inv->fields[0].elem_type == fbs_base_type::table_);
   CHECK(inv->fields[0].elem_type_idx >= 0);
}

TEST_CASE("fbs_parser: enum field reference", "[fbs]")
{
   auto schema = fbs_parse(R"(
      enum Color : byte { Red, Green, Blue }
      table Pixel {
         x:int;
         y:int;
         color:Color;
      }
   )");

   auto* pixel = schema.find_type("Pixel");
   REQUIRE(pixel != nullptr);
   CHECK(pixel->fields[2].name == "color");
   CHECK(pixel->fields[2].type == fbs_base_type::enum_);
   CHECK(pixel->fields[2].type_idx >= 0);
}

// =========================================================================
// Vtable slot assignment
// =========================================================================

TEST_CASE("fbs_parser: vtable slot assignment (sequential)", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T {
         a:int;
         b:string;
         c:float;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].vtable_slot == 0);
   CHECK(t->fields[1].vtable_slot == 1);
   CHECK(t->fields[2].vtable_slot == 2);
   CHECK(t->vtable_slot_count == 3);
}

TEST_CASE("fbs_parser: vtable slot with union (2 slots)", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table A { x:int; }
      table B { y:int; }
      union U { A, B }
      table T {
         name:string;
         equipped:U;
         hp:int;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].vtable_slot == 0);  // name
   CHECK(t->fields[1].vtable_slot == 1);  // equipped (union: slots 1,2)
   CHECK(t->fields[2].vtable_slot == 3);  // hp (after union's 2 slots)
   CHECK(t->vtable_slot_count == 4);
}

TEST_CASE("fbs_parser: explicit field IDs", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T {
         c:float (id: 2);
         a:int   (id: 0);
         b:string (id: 1);
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].name == "c");
   CHECK(t->fields[0].vtable_slot == 2);
   CHECK(t->fields[1].name == "a");
   CHECK(t->fields[1].vtable_slot == 0);
   CHECK(t->fields[2].name == "b");
   CHECK(t->fields[2].vtable_slot == 1);
}

// =========================================================================
// Field metadata
// =========================================================================

TEST_CASE("fbs_parser: deprecated field", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T {
         old_field:int (deprecated);
         new_field:int;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].metadata.deprecated == true);
   CHECK(t->fields[1].metadata.deprecated == false);
}

TEST_CASE("fbs_parser: required field", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T {
         name:string (required);
         age:int;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].metadata.required == true);
   CHECK(t->fields[1].metadata.required == false);
}

// =========================================================================
// Comments
// =========================================================================

TEST_CASE("fbs_parser: line comments", "[fbs]")
{
   auto schema = fbs_parse(R"(
      // This is a comment
      table T {
         // field comment
         x:int; // trailing comment
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields.size() == 1);
   CHECK(t->fields[0].name == "x");
}

TEST_CASE("fbs_parser: block comments", "[fbs]")
{
   auto schema = fbs_parse(R"(
      /* block comment */
      table T {
         /* multi
            line
            comment */
         x:int;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields.size() == 1);
}

// =========================================================================
// All scalar types
// =========================================================================

TEST_CASE("fbs_parser: all scalar types", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table AllTypes {
         f_bool:bool;
         f_byte:byte;
         f_ubyte:ubyte;
         f_short:short;
         f_ushort:ushort;
         f_int:int;
         f_uint:uint;
         f_long:long;
         f_ulong:ulong;
         f_float:float;
         f_double:double;
         f_int8:int8;
         f_uint8:uint8;
         f_int16:int16;
         f_uint16:uint16;
         f_int32:int32;
         f_uint32:uint32;
         f_int64:int64;
         f_uint64:uint64;
         f_float32:float32;
         f_float64:float64;
         f_string:string;
      }
   )");

   auto* t = schema.find_type("AllTypes");
   REQUIRE(t != nullptr);
   REQUIRE(t->fields.size() == 22);

   CHECK(t->fields[0].type == fbs_base_type::bool_);
   CHECK(t->fields[1].type == fbs_base_type::int8_);
   CHECK(t->fields[2].type == fbs_base_type::uint8_);
   CHECK(t->fields[3].type == fbs_base_type::int16_);
   CHECK(t->fields[4].type == fbs_base_type::uint16_);
   CHECK(t->fields[5].type == fbs_base_type::int32_);
   CHECK(t->fields[6].type == fbs_base_type::uint32_);
   CHECK(t->fields[7].type == fbs_base_type::int64_);
   CHECK(t->fields[8].type == fbs_base_type::uint64_);
   CHECK(t->fields[9].type == fbs_base_type::float32_);
   CHECK(t->fields[10].type == fbs_base_type::float64_);
   // int8..float64 aliases
   CHECK(t->fields[11].type == fbs_base_type::int8_);
   CHECK(t->fields[12].type == fbs_base_type::uint8_);
   CHECK(t->fields[13].type == fbs_base_type::int16_);
   CHECK(t->fields[14].type == fbs_base_type::uint16_);
   CHECK(t->fields[15].type == fbs_base_type::int32_);
   CHECK(t->fields[16].type == fbs_base_type::uint32_);
   CHECK(t->fields[17].type == fbs_base_type::int64_);
   CHECK(t->fields[18].type == fbs_base_type::uint64_);
   CHECK(t->fields[19].type == fbs_base_type::float32_);
   CHECK(t->fields[20].type == fbs_base_type::float64_);
   CHECK(t->fields[21].type == fbs_base_type::string_);
}

// =========================================================================
// Top-level directives
// =========================================================================

TEST_CASE("fbs_parser: root_type", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Monster { hp:int; }
      root_type Monster;
   )");

   CHECK(schema.root_type == "Monster");
}

TEST_CASE("fbs_parser: file_identifier and file_extension", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T { x:int; }
      file_identifier "TEST";
      file_extension "bin";
   )");

   CHECK(schema.file_identifier == "TEST");
   CHECK(schema.file_extension == "bin");
}

TEST_CASE("fbs_parser: include", "[fbs]")
{
   auto schema = fbs_parse(R"(
      include "common.fbs";
      include "types.fbs";
      table T { x:int; }
   )");

   REQUIRE(schema.includes.size() == 2);
   CHECK(schema.includes[0] == "common.fbs");
   CHECK(schema.includes[1] == "types.fbs");
}

TEST_CASE("fbs_parser: attribute declaration", "[fbs]")
{
   auto schema = fbs_parse(R"(
      attribute "priority";
      table T { x:int; }
   )");

   REQUIRE(schema.attributes.size() == 1);
   CHECK(schema.attributes[0] == "priority");
}

// =========================================================================
// Namespace-qualified type lookup
// =========================================================================

TEST_CASE("fbs_parser: namespace-qualified lookup", "[fbs]")
{
   auto schema = fbs_parse(R"(
      namespace game;
      table Monster { hp:int; }
   )");

   // Should be findable by both names
   CHECK(schema.find_type("Monster") != nullptr);
   CHECK(schema.find_type("game.Monster") != nullptr);
   CHECK(schema.find_type("Monster")->full_name == "game.Monster");
}

// =========================================================================
// Dynamic schema
// =========================================================================

TEST_CASE("fbs_parser: dynamic_schema built for tables", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Monster {
         name:string;
         hp:short;
         mana:short;
      }
   )");

   auto* t = schema.find_type("Monster");
   REQUIRE(t != nullptr);
   REQUIRE(t->dyn_schema != nullptr);
   CHECK(t->dyn_schema->field_count == 3);

   // Should be able to find fields by name
   auto* f = t->dyn_schema->find("name");
   CHECK(f != nullptr);
   CHECK(f->type == dynamic_type::t_text);

   f = t->dyn_schema->find("hp");
   CHECK(f != nullptr);
   CHECK(f->type == dynamic_type::t_i16);

   f = t->dyn_schema->find("mana");
   CHECK(f != nullptr);
   CHECK(f->type == dynamic_type::t_i16);
}

TEST_CASE("fbs_parser: dynamic_schema nested table reference", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table Weapon { damage:int; }
      table Monster {
         name:string;
         weapon:Weapon;
      }
   )");

   auto* monster = schema.find_type("Monster");
   REQUIRE(monster != nullptr);
   REQUIRE(monster->dyn_schema != nullptr);

   auto* weapon_field = monster->dyn_schema->find("weapon");
   REQUIRE(weapon_field != nullptr);
   CHECK(weapon_field->type == dynamic_type::t_struct);
   CHECK(weapon_field->nested != nullptr);  // points to Weapon's dynamic_schema

   // The nested schema should have the "damage" field
   auto* damage = weapon_field->nested->find("damage");
   CHECK(damage != nullptr);
   CHECK(damage->type == dynamic_type::t_i32);
}

// =========================================================================
// Complex schema (Monster example from FlatBuffers docs)
// =========================================================================

TEST_CASE("fbs_parser: FlatBuffers Monster example", "[fbs]")
{
   auto schema = fbs_parse(R"(
      namespace MyGame.Sample;

      enum Color : byte { Red = 0, Green, Blue = 2 }

      union Equipment { Weapon }

      struct Vec3 {
         x:float;
         y:float;
         z:float;
      }

      table Monster {
         pos:Vec3;
         mana:short = 150;
         hp:short = 100;
         name:string;
         friendly:bool = false;
         inventory:[ubyte];
         color:Color = Blue;
         weapons:[Weapon];
         equipped:Equipment;
         path:[Vec3];
      }

      table Weapon {
         name:string;
         damage:short;
      }

      root_type Monster;
   )");

   CHECK(schema.ns == "MyGame.Sample");
   CHECK(schema.root_type == "Monster");

   // Check all types were parsed
   CHECK(schema.find_type("Color") != nullptr);
   CHECK(schema.find_type("Equipment") != nullptr);
   CHECK(schema.find_type("Vec3") != nullptr);
   CHECK(schema.find_type("Monster") != nullptr);
   CHECK(schema.find_type("Weapon") != nullptr);

   // Color enum
   auto* color = schema.find_type("Color");
   REQUIRE(color != nullptr);
   CHECK(color->kind == fbs_type_kind::enum_);
   CHECK(color->underlying_type == fbs_base_type::int8_);
   REQUIRE(color->enum_values.size() == 3);
   CHECK(color->enum_values[0].value == 0);
   CHECK(color->enum_values[1].value == 1);
   CHECK(color->enum_values[2].value == 2);

   // Vec3 struct
   auto* vec3 = schema.find_type("Vec3");
   REQUIRE(vec3 != nullptr);
   CHECK(vec3->kind == fbs_type_kind::struct_);
   CHECK(vec3->struct_size == 12);

   // Monster table
   auto* monster = schema.find_type("Monster");
   REQUIRE(monster != nullptr);
   CHECK(monster->kind == fbs_type_kind::table_);
   REQUIRE(monster->fields.size() == 10);

   // pos is a struct
   CHECK(monster->fields[0].type == fbs_base_type::struct_);
   // mana is short with default 150
   CHECK(monster->fields[1].type == fbs_base_type::int16_);
   CHECK(monster->fields[1].default_int == 150);
   // hp is short with default 100
   CHECK(monster->fields[2].default_int == 100);
   // inventory is [ubyte]
   CHECK(monster->fields[5].type == fbs_base_type::vector_);
   CHECK(monster->fields[5].elem_type == fbs_base_type::uint8_);
   // color is enum
   CHECK(monster->fields[6].type == fbs_base_type::enum_);
   // weapons is [Weapon]
   CHECK(monster->fields[7].type == fbs_base_type::vector_);
   CHECK(monster->fields[7].elem_type == fbs_base_type::table_);
   // equipped is union (takes 2 vtable slots)
   CHECK(monster->fields[8].type == fbs_base_type::union_);
   // path is [Vec3]
   CHECK(monster->fields[9].type == fbs_base_type::vector_);
   CHECK(monster->fields[9].elem_type == fbs_base_type::struct_);

   // Check vtable slots — union takes 2 slots
   // fields: pos(0), mana(1), hp(2), name(3), friendly(4),
   //         inventory(5), color(6), weapons(7), equipped(8,9), path(10)
   CHECK(monster->fields[8].vtable_slot == 8);   // union type byte
   CHECK(monster->fields[9].vtable_slot == 10);  // path, after union's 2 slots
   CHECK(monster->vtable_slot_count == 11);
}

// =========================================================================
// Error handling
// =========================================================================

TEST_CASE("fbs_parser: error on unexpected token", "[fbs]")
{
   CHECK_THROWS_AS(fbs_parse("invalid garbage"), fbs_parse_error);
}

TEST_CASE("fbs_parser: error on missing semicolon", "[fbs]")
{
   CHECK_THROWS_AS(fbs_parse("namespace foo"), fbs_parse_error);
}

TEST_CASE("fbs_parser: error on missing field type", "[fbs]")
{
   CHECK_THROWS_AS(fbs_parse("table T { x:; }"), fbs_parse_error);
}

// =========================================================================
// Default values
// =========================================================================

TEST_CASE("fbs_parser: boolean defaults", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T {
         a:bool = true;
         b:bool = false;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].has_default);
   CHECK(t->fields[0].default_int == 1);
   CHECK(t->fields[1].has_default);
   CHECK(t->fields[1].default_int == 0);
}

TEST_CASE("fbs_parser: hex integer defaults", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T {
         flags:uint = 0xFF;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].default_int == 255);
}

TEST_CASE("fbs_parser: negative integer default", "[fbs]")
{
   auto schema = fbs_parse(R"(
      table T {
         x:int = -42;
      }
   )");

   auto* t = schema.find_type("T");
   REQUIRE(t != nullptr);
   CHECK(t->fields[0].default_int == -42);
}
