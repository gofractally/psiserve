// Phase A reflection-side attribute tests.
//
// Covers the C++ → schema-IR direction: C++ tags declared via
// PSIO_TYPE_ATTRS / PSIO_FIELD_ATTRS (plus stdlib auto-attrs and
// std::is_final_v auto-detect and PSIO_REFLECT canonical()/final() flags)
// flow into the Attribute entries attached to Struct / Object / Variant /
// Member in the built Schema.
//
// The sibling file wit_attribute_tests.cpp covers the WIT-parsing direction.

#include <catch2/catch.hpp>

#include <psio/attributes.hpp>
#include <psio/schema.hpp>

#include <map>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

namespace attr_test
{
   // ── Plain record whose fields carry per-member attributes ──────────────

   struct Container
   {
      std::vector<int> points;  // declared sorted via PSIO_FIELD_ATTRS
      int              id;      // declared @number(5) via PSIO_FIELD_ATTRS
   };
   PSIO_REFLECT(Container, points, id)

   // Declare field-level attributes via ADL overloads.
   // PSIO_FIELD_ATTRS must appear in a namespace the argument's type (the
   // member pointer) is associated with — here we put it alongside Container.
   PSIO_FIELD_ATTRS(Container, points, psio::sorted_tag{})
   PSIO_FIELD_ATTRS(Container, id, psio::number_tag<5>{})

   // ── Type-level attribute via PSIO_TYPE_ATTRS ──────────────────────────

   struct TaggedRecord
   {
      int value;
   };
   PSIO_REFLECT(TaggedRecord, value)
   PSIO_TYPE_ATTRS(TaggedRecord, psio::canonical_tag{})

   // ── C++ `final` → auto-detected final_tag via std::is_final_v ─────────

   struct ClosedRecord final
   {
      int value;
   };
   PSIO_REFLECT(ClosedRecord, value)

   // ── Struct with canonical()/final() on PSIO_REFLECT flag matcher ──────
   //
   // Also exercises definitionWillNotChange() independently — final() must
   // NOT imply definitionWillNotChange().

   struct FlagRecord
   {
      int value;
   };
   PSIO_REFLECT(FlagRecord, value, canonical(), final())

   struct WillNotChange
   {
      int value;
   };
   PSIO_REFLECT(WillNotChange, value, definitionWillNotChange())

   // ── Struct whose field carries a stdlib-auto-tagged type ──────────────

   struct KeyedStore
   {
      std::map<std::string, int> entries;
   };
   PSIO_REFLECT(KeyedStore, entries)

   // ── Variant type ──────────────────────────────────────────────────────

   using MyVariant = std::variant<int, std::string>;

}  // namespace attr_test

// ── Helpers ───────────────────────────────────────────────────────────────

namespace
{
   const psio::schema_types::Attribute* find_attr(
       const std::vector<psio::schema_types::Attribute>& attrs,
       std::string_view                             name)
   {
      for (const auto& a : attrs)
         if (a.name == name)
            return &a;
      return nullptr;
   }

   // Resolve a named type in the schema, skipping through Type aliases.
   const psio::schema_types::AnyType* resolve(const psio::schema_types::Schema& s,
                                        const std::string&          name)
   {
      const auto* cur = s.get(name);
      while (cur)
      {
         if (const auto* alias = std::get_if<psio::schema_types::Type>(&cur->value))
            cur = s.get(alias->type);
         else
            break;
      }
      return cur;
   }
}  // namespace

// ────────────────────────────────────────────────────────────────────────
//  L1 / L2 accessors: type_attrs_of<T>() and member_attrs_of<&T::f>()
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("reflection attrs: empty by default", "[psio][attributes]")
{
   auto t = psio::type_attrs_of<attr_test::Container>();
   STATIC_REQUIRE(std::tuple_size_v<decltype(t)> == 0);
}

TEST_CASE("reflection attrs: std::is_final_v auto-adds final_tag",
          "[psio][attributes]")
{
   auto t = psio::type_attrs_of<attr_test::ClosedRecord>();
   STATIC_REQUIRE(psio::has_tag<psio::final_tag>(t));
}

TEST_CASE("reflection attrs: PSIO_TYPE_ATTRS registers tags",
          "[psio][attributes]")
{
   auto t = psio::type_attrs_of<attr_test::TaggedRecord>();
   STATIC_REQUIRE(psio::has_tag<psio::canonical_tag>(t));
}

TEST_CASE("reflection attrs: std::map auto-attrs (sorted, unique-keys)",
          "[psio][attributes]")
{
   using M = std::map<std::string, int>;
   auto t  = psio::type_attrs_of<M>();
   STATIC_REQUIRE(psio::has_tag<psio::sorted_tag>(t));
   STATIC_REQUIRE(psio::has_tag<psio::unique_keys_tag>(t));
}

TEST_CASE("reflection attrs: std::unordered_map drops sorted, keeps unique",
          "[psio][attributes]")
{
   using M = std::unordered_map<std::string, int>;
   auto t  = psio::type_attrs_of<M>();
   STATIC_REQUIRE(!psio::has_tag<psio::sorted_tag>(t));
   STATIC_REQUIRE(psio::has_tag<psio::unique_keys_tag>(t));
}

TEST_CASE("reflection attrs: std::u8string → utf8", "[psio][attributes]")
{
   auto t = psio::type_attrs_of<std::u8string>();
   STATIC_REQUIRE(psio::has_tag<psio::utf8_tag>(t));
}

TEST_CASE("reflection attrs: PSIO_FIELD_ATTRS registers per-member tags",
          "[psio][attributes]")
{
   auto pts = psio::member_attrs_of<&attr_test::Container::points>();
   STATIC_REQUIRE(psio::has_tag<psio::sorted_tag>(pts));
   STATIC_REQUIRE(std::tuple_size_v<decltype(pts)> == 1);
}

// ────────────────────────────────────────────────────────────────────────
//  SchemaBuilder wires attributes into the IR
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("schema: field-level attribute flows to Member.attributes",
          "[psio][attributes]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::Container>("container");
   auto schema = std::move(b).build();

   const auto* any = resolve(schema, "container");
   REQUIRE(any);
   const auto* obj = std::get_if<S::Object>(&any->value);
   REQUIRE(obj);
   REQUIRE(obj->members.size() == 2);

   // `points` field carries sorted attribute.
   const auto& points = obj->members[0];
   REQUIRE(points.name == "points");
   REQUIRE(find_attr(points.attributes, "sorted") != nullptr);

   // `id` field carries number(5).
   const auto& id = obj->members[1];
   REQUIRE(id.name == "id");
   const auto* num = find_attr(id.attributes, "number");
   REQUIRE(num);
   REQUIRE(num->value == "5");
}

TEST_CASE("schema: type-level PSIO_TYPE_ATTRS flows to Object.attributes",
          "[psio][attributes]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::TaggedRecord>("tagged");
   auto schema = std::move(b).build();

   const auto* any = resolve(schema, "tagged");
   REQUIRE(any);
   const auto* obj = std::get_if<S::Object>(&any->value);
   REQUIRE(obj);
   REQUIRE(find_attr(obj->attributes, "canonical") != nullptr);
}

TEST_CASE("schema: std::is_final_v auto-attaches final attribute",
          "[psio][attributes]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::ClosedRecord>("closed");
   auto schema = std::move(b).build();

   const auto* any = resolve(schema, "closed");
   REQUIRE(any);
   const auto* obj = std::get_if<S::Object>(&any->value);
   REQUIRE(obj);
   REQUIRE(find_attr(obj->attributes, "final") != nullptr);
}

TEST_CASE("schema: canonical()/final() PSIO_REFLECT flags flow to IR",
          "[psio][attributes]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::FlagRecord>("flagged");
   auto schema = std::move(b).build();

   const auto* any = resolve(schema, "flagged");
   REQUIRE(any);
   const auto* obj = std::get_if<S::Object>(&any->value);
   REQUIRE(obj);
   REQUIRE(find_attr(obj->attributes, "canonical") != nullptr);
   REQUIRE(find_attr(obj->attributes, "final") != nullptr);
}

TEST_CASE("schema: definitionWillNotChange is independent of final()",
          "[psio][attributes]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::WillNotChange>("wnc");
   auto schema = std::move(b).build();

   // definitionWillNotChange() picks the Struct wire-format branch; final
   // attribute is absent because the C++ struct isn't final and didn't
   // declare final() in PSIO_REFLECT.
   const auto* any = resolve(schema, "wnc");
   REQUIRE(any);
   const auto* s = std::get_if<S::Struct>(&any->value);
   REQUIRE(s);
   REQUIRE(find_attr(s->attributes, "final") == nullptr);
}

TEST_CASE("schema: variant type carries attributes slot",
          "[psio][attributes]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::MyVariant>("myvar");
   auto schema = std::move(b).build();

   const auto* any = resolve(schema, "myvar");
   REQUIRE(any);
   const auto* v = std::get_if<S::Variant>(&any->value);
   REQUIRE(v);
   // Nothing declared — attributes empty but the field exists.
   REQUIRE(v->attributes.empty());
}

// ────────────────────────────────────────────────────────────────────────
//  FracPack round-trip: aggregate-type attributes survive pack/unpack
//
//  Before dropping clio_unwrap_packable on Object/Struct/Variant, these
//  types serialized as bare vector<Member> — their attributes field was
//  in-memory only. Now they pack as proper extensible structs and the
//  attributes tail rides the wire.
// ────────────────────────────────────────────────────────────────────────

TEST_CASE("schema fracpack: Object attributes survive round-trip",
          "[psio][attributes][fracpack]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::TaggedRecord>("tagged");   // PSIO_TYPE_ATTRS canonical
   b.insert<attr_test::Container>("container");   // field-level sorted/number
   auto schema = std::move(b).build();

   auto bytes = psio::to_frac(schema);
   auto rt    = psio::from_frac<S::Schema>(bytes);

   // Type-level attribute on an Object survives.
   const auto* tagged = resolve(rt, "tagged");
   REQUIRE(tagged);
   const auto* tobj = std::get_if<S::Object>(&tagged->value);
   REQUIRE(tobj);
   REQUIRE(find_attr(tobj->attributes, "canonical") != nullptr);

   // Field-level attributes on Members survive (pre-existing behavior).
   const auto* cont = resolve(rt, "container");
   REQUIRE(cont);
   const auto* cobj = std::get_if<S::Object>(&cont->value);
   REQUIRE(cobj);
   REQUIRE(cobj->members.size() == 2);
   REQUIRE(find_attr(cobj->members[0].attributes, "sorted") != nullptr);
   const auto* num = find_attr(cobj->members[1].attributes, "number");
   REQUIRE(num);
   REQUIRE(num->value == "5");
}

TEST_CASE("schema fracpack: Struct attributes survive round-trip",
          "[psio][attributes][fracpack]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::WillNotChange>("wnc");
   auto schema = std::move(b).build();

   // Add a synthetic type attribute post-build to prove wire-serialization
   // of aggregate attrs, not just the SchemaBuilder population path.
   auto* any = const_cast<S::AnyType*>(schema.get("wnc"));
   REQUIRE(any);
   auto* s = std::get_if<S::Struct>(&any->value);
   REQUIRE(s);
   s->attributes.push_back({"canonical", std::nullopt});

   auto bytes = psio::to_frac(schema);
   auto rt    = psio::from_frac<S::Schema>(bytes);

   const auto* rt_any = resolve(rt, "wnc");
   REQUIRE(rt_any);
   const auto* rt_s = std::get_if<S::Struct>(&rt_any->value);
   REQUIRE(rt_s);
   REQUIRE(find_attr(rt_s->attributes, "canonical") != nullptr);
}

TEST_CASE("schema fracpack: Variant attributes survive round-trip",
          "[psio][attributes][fracpack]")
{
   namespace S = psio::schema_types;
   S::SchemaBuilder b;
   b.insert<attr_test::MyVariant>("myvar");
   auto schema = std::move(b).build();

   auto* any = const_cast<S::AnyType*>(schema.get("myvar"));
   REQUIRE(any);
   auto* v = std::get_if<S::Variant>(&any->value);
   REQUIRE(v);
   v->attributes.push_back({"since", std::string{"0.3.0"}});

   auto bytes = psio::to_frac(schema);
   auto rt    = psio::from_frac<S::Schema>(bytes);

   const auto* rt_any = resolve(rt, "myvar");
   REQUIRE(rt_any);
   const auto* rt_v = std::get_if<S::Variant>(&rt_any->value);
   REQUIRE(rt_v);
   const auto* since = find_attr(rt_v->attributes, "since");
   REQUIRE(since);
   REQUIRE(since->value == "0.3.0");
}
