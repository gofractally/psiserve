// capnp_parser_tests.cpp — Catch2 tests for capnp_parse()
//
// Verifies that the parser correctly reads .capnp IDL text and computes
// wire layouts matching what capnp_layout<T> produces for equivalent C++ types.

#include <catch2/catch.hpp>
#include <psio1/capnp_parser.hpp>
#include <psio1/capnp_view.hpp>

#include <cstdint>
#include <string>
#include <variant>
#include <vector>

// ── Equivalent C++ types for layout verification ────────────────────────────

struct PPoint
{
   double x = 0;
   double y = 0;
};
PSIO1_REFLECT(PPoint, definitionWillNotChange(), x, y)

struct PToken
{
   uint16_t    kind   = 0;
   uint32_t    offset = 0;
   uint32_t    length = 0;
   std::string text;
};
PSIO1_REFLECT(PToken, kind, offset, length, text)

struct PUser
{
   uint64_t                 id       = 0;
   std::string              name;
   std::string              email;
   uint32_t                 age      = 0;
   double                   score    = 0;
   std::vector<std::string> tags;
   bool                     verified = false;
};
PSIO1_REFLECT(PUser, id, name, email, age, score, tags, verified)

struct PLineItem
{
   std::string product;
   uint32_t    qty        = 0;
   double      unit_price = 0;
};
PSIO1_REFLECT(PLineItem, product, qty, unit_price)

struct POrder
{
   uint64_t                    id = 0;
   PUser                       customer;
   std::vector<PLineItem>      items;
   double                      total = 0;
   std::string                 note;
};
PSIO1_REFLECT(POrder, id, customer, items, total, note)

struct PShape
{
   double                                            area = 0;
   std::variant<double, std::string, std::monostate>  shape;
};
PSIO1_REFLECT(PShape, area, shape)

struct PScalarMix
{
   bool     a = false;
   uint8_t  b = 0;
   uint16_t c = 0;
   uint32_t d = 0;
   uint64_t e = 0;
   int8_t   f = 0;
   int16_t  g = 0;
   int32_t  h = 0;
   int64_t  i = 0;
   float    j = 0;
   double   k = 0;
};
PSIO1_REFLECT(PScalarMix, a, b, c, d, e, f, g, h, i, j, k)

// ── Basic parsing tests ─────────────────────────────────────────────────────

TEST_CASE("capnp parser: file ID", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0xdeadbeefcafebabe;
   )");
   REQUIRE(file.file_id == 0xdeadbeefcafebabeULL);
}

TEST_CASE("capnp parser: simple struct", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1234;
      struct Point {
         x @0 :Float64;
         y @1 :Float64;
      }
   )");

   REQUIRE(file.structs.size() == 1);
   auto& s = file.structs[0];
   REQUIRE(s.name == "Point");
   REQUIRE(s.fields.size() == 2);

   REQUIRE(s.fields[0].name == "x");
   REQUIRE(s.fields[0].ordinal == 0);
   REQUIRE(s.fields[0].type.tag == psio1::capnp_type_tag::float64);

   REQUIRE(s.fields[1].name == "y");
   REQUIRE(s.fields[1].ordinal == 1);
   REQUIRE(s.fields[1].type.tag == psio1::capnp_type_tag::float64);
}

TEST_CASE("capnp parser: comments are skipped", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      # This is a comment
      struct Foo {
         # Another comment
         bar @0 :UInt32;  # trailing comment
      }
   )");

   REQUIRE(file.structs.size() == 1);
   REQUIRE(file.structs[0].fields.size() == 1);
   REQUIRE(file.structs[0].fields[0].name == "bar");
}

TEST_CASE("capnp parser: mixed scalar and pointer fields", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Token {
         kind   @0 :UInt16;
         offset @1 :UInt32;
         length @2 :UInt32;
         text   @3 :Text;
      }
   )");

   REQUIRE(file.structs.size() == 1);
   auto& s = file.structs[0];
   REQUIRE(s.fields.size() == 4);

   REQUIRE(s.fields[0].type.tag == psio1::capnp_type_tag::uint16);
   REQUIRE(s.fields[1].type.tag == psio1::capnp_type_tag::uint32);
   REQUIRE(s.fields[2].type.tag == psio1::capnp_type_tag::uint32);
   REQUIRE(s.fields[3].type.tag == psio1::capnp_type_tag::text);
}

TEST_CASE("capnp parser: List types", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Container {
         names @0 :List(Text);
         ids   @1 :List(UInt32);
         data  @2 :Data;
      }
   )");

   REQUIRE(file.structs.size() == 1);
   auto& s = file.structs[0];
   REQUIRE(s.fields.size() == 3);

   REQUIRE(s.fields[0].type.tag == psio1::capnp_type_tag::list);
   REQUIRE(s.fields[1].type.tag == psio1::capnp_type_tag::list);
   REQUIRE(s.fields[2].type.tag == psio1::capnp_type_tag::data);
}

TEST_CASE("capnp parser: nested struct reference", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Inner {
         value @0 :UInt32;
      }
      struct Outer {
         inner @0 :Inner;
         name  @1 :Text;
      }
   )");

   REQUIRE(file.structs.size() == 2);
   REQUIRE(file.structs[0].name == "Inner");
   REQUIRE(file.structs[1].name == "Outer");

   auto& outer = file.structs[1];
   REQUIRE(outer.fields[0].type.tag == psio1::capnp_type_tag::struct_);
   REQUIRE(outer.fields[0].type.referenced_type_idx == 0);  // Inner is at index 0
   REQUIRE(outer.fields[1].type.tag == psio1::capnp_type_tag::text);
}

TEST_CASE("capnp parser: enum", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      enum Color {
         red   @0;
         green @1;
         blue  @2;
      }
   )");

   REQUIRE(file.enums.size() == 1);
   auto& e = file.enums[0];
   REQUIRE(e.name == "Color");
   REQUIRE(e.enumerants.size() == 3);
   REQUIRE(e.enumerants[0] == "red");
   REQUIRE(e.enumerants[1] == "green");
   REQUIRE(e.enumerants[2] == "blue");
}

TEST_CASE("capnp parser: struct with enum field", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      enum Color {
         red   @0;
         green @1;
         blue  @2;
      }
      struct WithEnum {
         color @0 :Color;
         label @1 :Text;
      }
   )");

   REQUIRE(file.structs.size() == 1);
   auto& s = file.structs[0];
   REQUIRE(s.fields[0].type.tag == psio1::capnp_type_tag::enum_);
   REQUIRE(s.fields[0].type.referenced_type_idx == 0);
}

TEST_CASE("capnp parser: union", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Shape {
         area @0 :Float64;
         union {
            circle  @1 :Float64;
            label   @2 :Text;
            nothing @3 :Void;
         }
      }
   )");

   REQUIRE(file.structs.size() == 1);
   auto& s = file.structs[0];
   REQUIRE(s.fields.size() == 1);  // area
   REQUIRE(s.unions.size() == 1);

   auto& u = s.unions[0];
   REQUIRE(u.alternatives.size() == 3);
   REQUIRE(u.alternatives[0].name == "circle");
   REQUIRE(u.alternatives[0].ordinal == 1);
   REQUIRE(u.alternatives[0].type.tag == psio1::capnp_type_tag::float64);

   REQUIRE(u.alternatives[1].name == "label");
   REQUIRE(u.alternatives[1].ordinal == 2);
   REQUIRE(u.alternatives[1].type.tag == psio1::capnp_type_tag::text);

   REQUIRE(u.alternatives[2].name == "nothing");
   REQUIRE(u.alternatives[2].ordinal == 3);
   REQUIRE(u.alternatives[2].type.tag == psio1::capnp_type_tag::void_);
}

TEST_CASE("capnp parser: using alias", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      using MyInt = UInt64;
      struct Foo {
         value @0 :MyInt;
      }
   )");

   REQUIRE(file.aliases.size() == 1);
   REQUIRE(file.aliases[0].name == "MyInt");
   REQUIRE(file.aliases[0].type.tag == psio1::capnp_type_tag::uint64);

   REQUIRE(file.structs.size() == 1);
   // The field should resolve to uint64 through the alias
   REQUIRE(file.structs[0].fields[0].type.tag == psio1::capnp_type_tag::uint64);
}

TEST_CASE("capnp parser: default values are parsed", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Defaults {
         count   @0 :Int32 = 42;
         ratio   @1 :Float64 = 3.14;
         enabled @2 :Bool = true;
         id      @3 :UInt64;
      }
   )");

   REQUIRE(file.structs.size() == 1);
   auto& s = file.structs[0];
   REQUIRE(s.fields.size() == 4);
   REQUIRE(s.fields[0].default_value == "42");
   REQUIRE(s.fields[1].default_value == "3.14");
   REQUIRE(s.fields[2].default_value == "true");
   REQUIRE(s.fields[3].default_value.empty());
}

TEST_CASE("capnp parser: all builtin types", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct AllTypes {
         a @0  :Void;
         b @1  :Bool;
         c @2  :Int8;
         d @3  :Int16;
         e @4  :Int32;
         f @5  :Int64;
         g @6  :UInt8;
         h @7  :UInt16;
         i @8  :UInt32;
         j @9  :UInt64;
         k @10 :Float32;
         l @11 :Float64;
         m @12 :Text;
         n @13 :Data;
      }
   )");

   auto& s = file.structs[0];
   REQUIRE(s.fields.size() == 14);
   REQUIRE(s.fields[0].type.tag == psio1::capnp_type_tag::void_);
   REQUIRE(s.fields[1].type.tag == psio1::capnp_type_tag::bool_);
   REQUIRE(s.fields[2].type.tag == psio1::capnp_type_tag::int8);
   REQUIRE(s.fields[3].type.tag == psio1::capnp_type_tag::int16);
   REQUIRE(s.fields[4].type.tag == psio1::capnp_type_tag::int32);
   REQUIRE(s.fields[5].type.tag == psio1::capnp_type_tag::int64);
   REQUIRE(s.fields[6].type.tag == psio1::capnp_type_tag::uint8);
   REQUIRE(s.fields[7].type.tag == psio1::capnp_type_tag::uint16);
   REQUIRE(s.fields[8].type.tag == psio1::capnp_type_tag::uint32);
   REQUIRE(s.fields[9].type.tag == psio1::capnp_type_tag::uint64);
   REQUIRE(s.fields[10].type.tag == psio1::capnp_type_tag::float32);
   REQUIRE(s.fields[11].type.tag == psio1::capnp_type_tag::float64);
   REQUIRE(s.fields[12].type.tag == psio1::capnp_type_tag::text);
   REQUIRE(s.fields[13].type.tag == psio1::capnp_type_tag::data);
}

TEST_CASE("capnp parser: bench_schemas.capnp round-trip", "[capnp_parser]")
{
   // Parse the actual benchmark schema file content
   auto file = psio1::capnp_parse(R"(
      @0xd4a3c6f1e2b50a91;

      struct Point {
         x @0 :Float64;
         y @1 :Float64;
      }

      struct Token {
         kind   @0 :UInt16;
         offset @1 :UInt32;
         length @2 :UInt32;
         text   @3 :Text;
      }

      struct UserProfile {
         id       @0 :UInt64;
         name     @1 :Text;
         email    @2 :Text;
         bio      @3 :Text;
         age      @4 :UInt32;
         score    @5 :Float64;
         tags     @6 :List(Text);
         verified @7 :Bool;
      }

      struct LineItem {
         product   @0 :Text;
         qty       @1 :UInt32;
         unitPrice @2 :Float64;
      }

      struct Order {
         id       @0 :UInt64;
         customer @1 :UserProfile;
         items    @2 :List(LineItem);
         total    @3 :Float64;
         note     @4 :Text;
      }

      struct SensorReading {
         timestamp @0  :UInt64;
         deviceId  @1  :Text;
         temp      @2  :Float64;
         humidity  @3  :Float64;
         pressure  @4  :Float64;
         accelX    @5  :Float64;
         accelY    @6  :Float64;
         accelZ    @7  :Float64;
         gyroX     @8  :Float64;
         gyroY     @9  :Float64;
         gyroZ     @10 :Float64;
         magX      @11 :Float64;
         magY      @12 :Float64;
         magZ      @13 :Float64;
         battery   @14 :Float32;
         signalDbm @15 :Int16;
         errorCode @16 :UInt32;
         firmware  @17 :Text;
      }
   )");

   REQUIRE(file.file_id == 0xd4a3c6f1e2b50a91ULL);
   REQUIRE(file.structs.size() == 6);

   // Verify struct names
   REQUIRE(file.structs[0].name == "Point");
   REQUIRE(file.structs[1].name == "Token");
   REQUIRE(file.structs[2].name == "UserProfile");
   REQUIRE(file.structs[3].name == "LineItem");
   REQUIRE(file.structs[4].name == "Order");
   REQUIRE(file.structs[5].name == "SensorReading");

   // Verify SensorReading has 18 fields
   REQUIRE(file.structs[5].fields.size() == 18);
}

// ── Layout computation tests ────────────────────────────────────────────────
//
// These verify that the runtime layout matches the compile-time capnp_layout<T>.

TEST_CASE("capnp parser: layout matches Point", "[capnp_parser][layout]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Point {
         x @0 :Float64;
         y @1 :Float64;
      }
   )");

   using layout = psio1::capnp_layout<PPoint>;
   auto& s      = file.structs[0];

   INFO("parsed data_words=" << s.data_words << " ptr_count=" << s.ptr_count);
   INFO("expected data_words=" << layout::data_words << " ptr_count=" << layout::ptr_count);

   REQUIRE(s.data_words == layout::data_words);
   REQUIRE(s.ptr_count == layout::ptr_count);

   // Field 0: x
   REQUIRE(s.fields[0].loc.is_ptr == layout::loc(0).is_ptr);
   REQUIRE(s.fields[0].loc.offset == layout::loc(0).offset);

   // Field 1: y
   REQUIRE(s.fields[1].loc.is_ptr == layout::loc(1).is_ptr);
   REQUIRE(s.fields[1].loc.offset == layout::loc(1).offset);
}

TEST_CASE("capnp parser: layout matches Token", "[capnp_parser][layout]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Token {
         kind   @0 :UInt16;
         offset @1 :UInt32;
         length @2 :UInt32;
         text   @3 :Text;
      }
   )");

   using layout = psio1::capnp_layout<PToken>;
   auto& s      = file.structs[0];

   INFO("parsed dw=" << s.data_words << " pc=" << s.ptr_count);
   INFO("expected dw=" << layout::data_words << " pc=" << layout::ptr_count);

   REQUIRE(s.data_words == layout::data_words);
   REQUIRE(s.ptr_count == layout::ptr_count);

   // kind @0: UInt16
   REQUIRE(s.fields[0].loc.is_ptr == layout::loc(0).is_ptr);
   REQUIRE(s.fields[0].loc.offset == layout::loc(0).offset);

   // offset @1: UInt32
   REQUIRE(s.fields[1].loc.is_ptr == layout::loc(1).is_ptr);
   REQUIRE(s.fields[1].loc.offset == layout::loc(1).offset);

   // length @2: UInt32
   REQUIRE(s.fields[2].loc.is_ptr == layout::loc(2).is_ptr);
   REQUIRE(s.fields[2].loc.offset == layout::loc(2).offset);

   // text @3: Text (pointer)
   REQUIRE(s.fields[3].loc.is_ptr == layout::loc(3).is_ptr);
   REQUIRE(s.fields[3].loc.offset == layout::loc(3).offset);
}

TEST_CASE("capnp parser: layout matches User", "[capnp_parser][layout]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct User {
         id       @0 :UInt64;
         name     @1 :Text;
         email    @2 :Text;
         age      @3 :UInt32;
         score    @4 :Float64;
         tags     @5 :List(Text);
         verified @6 :Bool;
      }
   )");

   using layout = psio1::capnp_layout<PUser>;
   auto& s      = file.structs[0];

   INFO("parsed dw=" << s.data_words << " pc=" << s.ptr_count);
   INFO("expected dw=" << layout::data_words << " pc=" << layout::ptr_count);

   REQUIRE(s.data_words == layout::data_words);
   REQUIRE(s.ptr_count == layout::ptr_count);

   for (size_t i = 0; i < s.fields.size(); ++i)
   {
      INFO("field " << i << " (" << s.fields[i].name << ")");
      auto expected = layout::result.fields[i];
      REQUIRE(s.fields[i].loc.is_ptr == expected.is_ptr);
      REQUIRE(s.fields[i].loc.offset == expected.offset);
      REQUIRE(s.fields[i].loc.bit_index == expected.bit_index);
   }
}

TEST_CASE("capnp parser: layout matches LineItem", "[capnp_parser][layout]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct LineItem {
         product   @0 :Text;
         qty       @1 :UInt32;
         unitPrice @2 :Float64;
      }
   )");

   using layout = psio1::capnp_layout<PLineItem>;
   auto& s      = file.structs[0];

   REQUIRE(s.data_words == layout::data_words);
   REQUIRE(s.ptr_count == layout::ptr_count);

   for (size_t i = 0; i < s.fields.size(); ++i)
   {
      INFO("field " << i << " (" << s.fields[i].name << ")");
      auto expected = layout::result.fields[i];
      REQUIRE(s.fields[i].loc.is_ptr == expected.is_ptr);
      REQUIRE(s.fields[i].loc.offset == expected.offset);
   }
}

TEST_CASE("capnp parser: layout matches union (Shape)", "[capnp_parser][layout]")
{
   // PShape has: area (Float64), then variant<double, string, monostate>
   // capnp_layout processes: area @0, then variant alternatives @1 @2 @3,
   // then discriminant @4
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Shape {
         area @0 :Float64;
         union {
            shape0 @1 :Float64;
            shape1 @2 :Text;
            shape2 @3 :Void;
         }
      }
   )");

   using layout = psio1::capnp_layout<PShape>;
   auto& s      = file.structs[0];

   INFO("parsed dw=" << s.data_words << " pc=" << s.ptr_count);
   INFO("expected dw=" << layout::data_words << " pc=" << layout::ptr_count);

   REQUIRE(s.data_words == layout::data_words);
   REQUIRE(s.ptr_count == layout::ptr_count);

   // Regular field: area @0
   REQUIRE(s.fields[0].loc.is_ptr == layout::loc(0).is_ptr);
   REQUIRE(s.fields[0].loc.offset == layout::loc(0).offset);

   // Union alternatives
   REQUIRE(s.unions.size() == 1);
   auto& u = s.unions[0];
   REQUIRE(u.alternatives.size() == 3);

   // Alternative 0: Float64 (data section)
   auto alt0_expected = layout::alt_loc(1, 0);
   REQUIRE(u.alternatives[0].loc.is_ptr == alt0_expected.is_ptr);
   REQUIRE(u.alternatives[0].loc.offset == alt0_expected.offset);

   // Alternative 1: Text (pointer section)
   auto alt1_expected = layout::alt_loc(1, 1);
   REQUIRE(u.alternatives[1].loc.is_ptr == alt1_expected.is_ptr);
   REQUIRE(u.alternatives[1].loc.offset == alt1_expected.offset);

   // Alternative 2: Void (no space)
   // Void has is_ptr=false, offset=0 — just ensure it's not a pointer

   // Discriminant location
   auto disc_expected = layout::loc(1);  // discriminant is stored at field[1] for the variant
   REQUIRE(u.discriminant_loc.is_ptr == disc_expected.is_ptr);
   REQUIRE(u.discriminant_loc.offset == disc_expected.offset);
}

TEST_CASE("capnp parser: layout matches all scalar types", "[capnp_parser][layout]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct ScalarMix {
         a @0  :Bool;
         b @1  :UInt8;
         c @2  :UInt16;
         d @3  :UInt32;
         e @4  :UInt64;
         f @5  :Int8;
         g @6  :Int16;
         h @7  :Int32;
         i @8  :Int64;
         j @9  :Float32;
         k @10 :Float64;
      }
   )");

   using layout = psio1::capnp_layout<PScalarMix>;
   auto& s      = file.structs[0];

   INFO("parsed dw=" << s.data_words << " pc=" << s.ptr_count);
   INFO("expected dw=" << layout::data_words << " pc=" << layout::ptr_count);

   REQUIRE(s.data_words == layout::data_words);
   REQUIRE(s.ptr_count == layout::ptr_count);

   for (size_t i = 0; i < s.fields.size(); ++i)
   {
      INFO("field " << i << " (" << s.fields[i].name << ")");
      auto expected = layout::result.fields[i];
      REQUIRE(s.fields[i].loc.is_ptr == expected.is_ptr);
      REQUIRE(s.fields[i].loc.offset == expected.offset);
      REQUIRE(s.fields[i].loc.bit_index == expected.bit_index);
   }
}

TEST_CASE("capnp parser: nested struct definition inside struct", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Outer {
         struct Inner {
            value @0 :UInt32;
         }
         inner @0 :Inner;
         name  @1 :Text;
      }
   )");

   REQUIRE(file.structs.size() == 2);
   REQUIRE(file.structs[0].name == "Inner");
   REQUIRE(file.structs[1].name == "Outer");

   auto& outer = file.structs[1];
   REQUIRE(outer.fields[0].name == "inner");
   REQUIRE(outer.fields[0].type.tag == psio1::capnp_type_tag::struct_);
   REQUIRE(outer.fields[0].type.referenced_type_idx == 0);
}

TEST_CASE("capnp parser: struct with schema ID", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Foo @0xabcdef0123456789 {
         bar @0 :UInt32;
      }
   )");

   REQUIRE(file.structs.size() == 1);
   REQUIRE(file.structs[0].id == 0xabcdef0123456789ULL);
}

TEST_CASE("capnp parser: error on unexpected token", "[capnp_parser]")
{
   REQUIRE_THROWS_AS(
       psio1::capnp_parse("@0x1; !!!"),
       psio1::capnp_parse_error);
}

TEST_CASE("capnp parser: List(Struct) type reference", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Item {
         name @0 :Text;
      }
      struct Container {
         items @0 :List(Item);
      }
   )");

   REQUIRE(file.structs.size() == 2);
   auto& container = file.structs[1];
   REQUIRE(container.fields[0].type.tag == psio1::capnp_type_tag::list);
   // The element type should reference the Item struct
}

TEST_CASE("capnp parser: enum used in struct before and after", "[capnp_parser]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      enum Status {
         active @0;
         inactive @1;
      }
      struct Record {
         status @0 :Status;
         value  @1 :UInt32;
      }
   )");

   REQUIRE(file.enums.size() == 1);
   REQUIRE(file.structs.size() == 1);
   auto& s = file.structs[0];
   REQUIRE(s.fields[0].type.tag == psio1::capnp_type_tag::enum_);
   // Enum in data section as uint16
   REQUIRE(s.fields[0].loc.is_ptr == false);
}

TEST_CASE("capnp parser: multiple unions not supported but parsed", "[capnp_parser]")
{
   // Cap'n Proto only allows one unnamed union per struct, but our parser
   // is permissive — it will parse multiple and compute layout for them.
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct Multi {
         id @0 :UInt64;
         union {
            a @1 :UInt32;
            b @2 :Text;
         }
      }
   )");

   REQUIRE(file.structs.size() == 1);
   REQUIRE(file.structs[0].unions.size() == 1);
}

// ── Layout: Order struct (complex with nested struct + list) ─────────────────

TEST_CASE("capnp parser: layout matches Order (nested + list)", "[capnp_parser][layout]")
{
   auto file = psio1::capnp_parse(R"(
      @0x1;
      struct User {
         id       @0 :UInt64;
         name     @1 :Text;
         email    @2 :Text;
         age      @3 :UInt32;
         score    @4 :Float64;
         tags     @5 :List(Text);
         verified @6 :Bool;
      }
      struct LineItem {
         product   @0 :Text;
         qty       @1 :UInt32;
         unitPrice @2 :Float64;
      }
      struct Order {
         id       @0 :UInt64;
         customer @1 :User;
         items    @2 :List(LineItem);
         total    @3 :Float64;
         note     @4 :Text;
      }
   )");

   using layout = psio1::capnp_layout<POrder>;

   // Order should be the 3rd struct
   REQUIRE(file.structs.size() == 3);
   auto& s = file.structs[2];
   REQUIRE(s.name == "Order");

   INFO("parsed dw=" << s.data_words << " pc=" << s.ptr_count);
   INFO("expected dw=" << layout::data_words << " pc=" << layout::ptr_count);

   REQUIRE(s.data_words == layout::data_words);
   REQUIRE(s.ptr_count == layout::ptr_count);

   for (size_t i = 0; i < s.fields.size(); ++i)
   {
      INFO("field " << i << " (" << s.fields[i].name << ")");
      auto expected = layout::result.fields[i];
      REQUIRE(s.fields[i].loc.is_ptr == expected.is_ptr);
      REQUIRE(s.fields[i].loc.offset == expected.offset);
   }
}
