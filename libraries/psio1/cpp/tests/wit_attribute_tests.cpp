// WIT attribute parsing tests.
//
// Covers both the W3C-spec feature-gating attributes (@since, @unstable,
// @deprecated) and PSIO's semantic-invariant extensions (@final, @sorted,
// @unique-keys, @canonical, @utf8) across every AST position that can
// carry attributes: types, fields, variant cases, enum labels, flags,
// functions, interfaces, and worlds.

#include <catch2/catch.hpp>
#include <psio1/wit_parser.hpp>

#include <algorithm>
#include <string>

namespace {

const psio1::wit_attribute* find_attr(const std::vector<psio1::wit_attribute>& attrs,
                                     std::string_view name)
{
   auto it = std::find_if(attrs.begin(), attrs.end(),
                          [&](const psio1::wit_attribute& a) { return a.name == name; });
   return it == attrs.end() ? nullptr : &*it;
}

const psio1::wit_type_def* find_type(const psio1::wit_world& w, std::string_view name)
{
   for (const auto& t : w.types)
      if (t.name == name)
         return &t;
   return nullptr;
}

const psio1::wit_named_type* find_field(const psio1::wit_type_def& td, std::string_view name)
{
   for (const auto& f : td.fields)
      if (f.name == name)
         return &f;
   return nullptr;
}

const psio1::wit_func* find_func(const psio1::wit_world& w, std::string_view name)
{
   for (const auto& f : w.funcs)
      if (f.name == name)
         return &f;
   return nullptr;
}

} // namespace

// ────────────────────────────────────────────────────────────────────────
//  BARE ATTRIBUTE (no args)
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("wit attr: @final on record", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         @final
         record point { x: s32, y: s32 }
      }
   )");
   auto* td = find_type(w, "point");
   REQUIRE(td != nullptr);
   REQUIRE(td->attributes.size() == 1);
   REQUIRE(td->attributes[0].name == "final");
   REQUIRE(td->attributes[0].arg_key.empty());
   REQUIRE(td->attributes[0].arg_value.empty());
}

TEST_CASE("wit attr: @sorted on list field", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         record index {
            @sorted
            keys: list<s32>
         }
      }
   )");
   auto* td = find_type(w, "index");
   REQUIRE(td != nullptr);
   auto* f = find_field(*td, "keys");
   REQUIRE(f != nullptr);
   REQUIRE(f->attributes.size() == 1);
   REQUIRE(f->attributes[0].name == "sorted");
}

TEST_CASE("wit attr: kebab-case name @unique-keys", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         record dict {
            @unique-keys
            entries: list<tuple<string, s32>>
         }
      }
   )");
   auto* td = find_type(w, "dict");
   REQUIRE(td != nullptr);
   auto* f = find_field(*td, "entries");
   REQUIRE(f != nullptr);
   REQUIRE(find_attr(f->attributes, "unique-keys") != nullptr);
}

// ────────────────────────────────────────────────────────────────────────
//  ATTRIBUTE WITH KEY=VALUE ARG
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("wit attr: @since(version = 0.2.0) on record", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         @since(version = 0.2.0)
         record new-shape { r: f64 }
      }
   )");
   auto* td = find_type(w, "new-shape");
   REQUIRE(td != nullptr);
   auto* a = find_attr(td->attributes, "since");
   REQUIRE(a != nullptr);
   REQUIRE(a->arg_key == "version");
   REQUIRE(a->arg_value == "0.2.0");
}

TEST_CASE("wit attr: @unstable(feature = foo) on function", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         @unstable(feature = experimental-api)
         ping: func() -> string;
      }
   )");
   auto* f = find_func(w, "ping");
   REQUIRE(f != nullptr);
   auto* a = find_attr(f->attributes, "unstable");
   REQUIRE(a != nullptr);
   REQUIRE(a->arg_key == "feature");
   REQUIRE(a->arg_value == "experimental-api");
}

TEST_CASE("wit attr: @since on field", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         record user {
            name: string,
            @since(version = 1.2.3)
            created-at: u64
         }
      }
   )");
   auto* td = find_type(w, "user");
   REQUIRE(td != nullptr);
   auto* f = find_field(*td, "created-at");
   REQUIRE(f != nullptr);
   auto* a = find_attr(f->attributes, "since");
   REQUIRE(a != nullptr);
   REQUIRE(a->arg_value == "1.2.3");
}

// ────────────────────────────────────────────────────────────────────────
//  VARIANT CASES / ENUM LABELS / FLAGS LABELS
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("wit attr: @since on variant case", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         variant msg {
            hello,
            @since(version = 0.2.0)
            goodbye(string)
         }
      }
   )");
   auto* td = find_type(w, "msg");
   REQUIRE(td != nullptr);
   auto* c = find_field(*td, "goodbye");
   REQUIRE(c != nullptr);
   REQUIRE(find_attr(c->attributes, "since") != nullptr);
}

TEST_CASE("wit attr: @deprecated on enum label", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         enum color {
            red,
            @deprecated(version = 2.0.0)
            green,
            blue
         }
      }
   )");
   auto* td = find_type(w, "color");
   REQUIRE(td != nullptr);
   auto* g = find_field(*td, "green");
   REQUIRE(g != nullptr);
   REQUIRE(find_attr(g->attributes, "deprecated") != nullptr);
}

TEST_CASE("wit attr: bare attribute on flags label", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         flags permissions {
            read,
            @unstable(feature = admin-controls)
            admin,
            write
         }
      }
   )");
   auto* td = find_type(w, "permissions");
   REQUIRE(td != nullptr);
   auto* a = find_field(*td, "admin");
   REQUIRE(a != nullptr);
   REQUIRE(find_attr(a->attributes, "unstable") != nullptr);
}

// ────────────────────────────────────────────────────────────────────────
//  MULTIPLE ATTRIBUTES ON THE SAME ITEM
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("wit attr: stack @final @canonical on record", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         @final
         @canonical
         record frozen { a: u8, b: u8 }
      }
   )");
   auto* td = find_type(w, "frozen");
   REQUIRE(td != nullptr);
   REQUIRE(td->attributes.size() == 2);
   REQUIRE(find_attr(td->attributes, "final") != nullptr);
   REQUIRE(find_attr(td->attributes, "canonical") != nullptr);
}

TEST_CASE("wit attr: @since and @final together", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         @since(version = 0.3.0)
         @final
         record v3-point { x: f64, y: f64, z: f64 }
      }
   )");
   auto* td = find_type(w, "v3-point");
   REQUIRE(td != nullptr);
   REQUIRE(td->attributes.size() == 2);
   auto* since = find_attr(td->attributes, "since");
   REQUIRE(since != nullptr);
   REQUIRE(since->arg_value == "0.3.0");
   REQUIRE(find_attr(td->attributes, "final") != nullptr);
}

// ────────────────────────────────────────────────────────────────────────
//  INTERFACE / WORLD LEVEL
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("wit attr: @since on interface", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      @since(version = 0.2.0)
      interface new-api {
         ping: func() -> string;
      }
   )");
   REQUIRE(w.exports.size() == 1);
   REQUIRE(w.exports[0].name == "new-api");
   auto* a = find_attr(w.exports[0].attributes, "since");
   REQUIRE(a != nullptr);
   REQUIRE(a->arg_value == "0.2.0");
}

TEST_CASE("wit attr: @unstable on world", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      @unstable(feature = preview)
      world preview-app {
         export greet: func(name: string) -> string;
      }
   )");
   REQUIRE(w.name == "preview-app");
   auto* a = find_attr(w.attributes, "unstable");
   REQUIRE(a != nullptr);
   REQUIRE(a->arg_value == "preview");
}

// ────────────────────────────────────────────────────────────────────────
//  UNKNOWN ATTRIBUTE PASS-THROUGH
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("wit attr: unknown attribute preserved, not rejected", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         @custom-semantic(hint = thing)
         record blob { data: list<u8> }
      }
   )");
   auto* td = find_type(w, "blob");
   REQUIRE(td != nullptr);
   auto* a = find_attr(td->attributes, "custom-semantic");
   REQUIRE(a != nullptr);
   REQUIRE(a->arg_key == "hint");
   REQUIRE(a->arg_value == "thing");
}

// ────────────────────────────────────────────────────────────────────────
//  NO ATTRIBUTES — REGRESSION (ensure existing parse still works)
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("wit attr: plain record without attributes parses unchanged", "[wit][attributes]")
{
   auto w = psio1::wit_parse(R"(
      interface test {
         record plain { x: s32, y: s32 }
      }
   )");
   auto* td = find_type(w, "plain");
   REQUIRE(td != nullptr);
   REQUIRE(td->attributes.empty());
   REQUIRE(td->fields.size() == 2);
   REQUIRE(td->fields[0].attributes.empty());
   REQUIRE(td->fields[1].attributes.empty());
}
