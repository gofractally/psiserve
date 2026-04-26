// Phase 2 — annotation system tests.

#include <psio3/annotate.hpp>
#include <psio3/reflect.hpp>
#include <psio3/wrappers.hpp>

#include <catch.hpp>

#include <cstdint>
#include <tuple>

namespace {

   struct Foo
   {
      int         a;
      std::string b;
   };

}  // namespace

// Desugared form: specialize psio3::annotate<&Foo::a> and <&Foo::b>
// at namespace scope. This is exactly what the (future) PSIO3_ATTRS
// macro will expand to; writing it by hand works today.

template <>
inline constexpr auto psio3::annotate<&Foo::a> = std::tuple{
   psio3::length_bound{.exact = 48},
   psio3::field_num_spec{.value = 1},
};

template <>
inline constexpr auto psio3::annotate<&Foo::b> = std::tuple{
   psio3::utf8_spec{.max = 256, .field_num = 2},
};

template <>
inline constexpr auto psio3::annotate<psio3::type<Foo>{}> = std::tuple{
   psio3::definition_will_not_change{},
};

TEST_CASE("annotate<X> defaults to empty tuple", "[annotate]")
{
   struct Uncharted
   {
      int z;
   };
   STATIC_REQUIRE(std::tuple_size_v<decltype(psio3::annotate<&Uncharted::z>)> == 0);
}

TEST_CASE("annotate<X> round-trips the spec tuple", "[annotate]")
{
   constexpr auto a_anns = psio3::annotate<&Foo::a>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(a_anns)> == 2);

   constexpr auto b_anns = psio3::annotate<&Foo::b>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(b_anns)> == 1);

   constexpr auto foo_type_anns = psio3::annotate<psio3::type<Foo>{}>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(foo_type_anns)> == 1);
}

TEST_CASE("find_spec returns the spec by type, or nullopt", "[annotate]")
{
   constexpr auto anns = psio3::annotate<&Foo::a>;

   auto lb = psio3::find_spec<psio3::length_bound>(anns);
   REQUIRE(lb.has_value());
   REQUIRE(lb->exact.has_value());
   REQUIRE(*lb->exact == 48);

   auto fn = psio3::find_spec<psio3::field_num_spec>(anns);
   REQUIRE(fn.has_value());
   REQUIRE(fn->value == 1);

   auto us = psio3::find_spec<psio3::utf8_spec>(anns);
   REQUIRE(!us.has_value());
}

TEST_CASE("has_spec_v is a compile-time predicate", "[annotate]")
{
   using A = std::remove_cvref_t<decltype(psio3::annotate<&Foo::a>)>;
   using B = std::remove_cvref_t<decltype(psio3::annotate<&Foo::b>)>;

   STATIC_REQUIRE((psio3::has_spec_v<psio3::length_bound, A>));
   STATIC_REQUIRE((!psio3::has_spec_v<psio3::utf8_spec, A>));

   STATIC_REQUIRE((psio3::has_spec_v<psio3::utf8_spec, B>));
   STATIC_REQUIRE((!psio3::has_spec_v<psio3::length_bound, B>));
}

TEST_CASE("Spec category tags separate static from runtime specs",
          "[annotate][category]")
{
   STATIC_REQUIRE(psio3::is_runtime_spec_v<psio3::length_bound>);
   STATIC_REQUIRE(psio3::is_runtime_spec_v<psio3::utf8_spec>);
   STATIC_REQUIRE(psio3::is_runtime_spec_v<psio3::sorted_spec>);

   STATIC_REQUIRE(psio3::is_static_spec_v<psio3::field_num_spec>);
   STATIC_REQUIRE(psio3::is_static_spec_v<psio3::skip_spec>);
   STATIC_REQUIRE(psio3::is_static_spec_v<psio3::definition_will_not_change>);
}

TEST_CASE("Shape applicability trait answers both ways",
          "[annotate][applies_to]")
{
   // length_bound applies to VariableSequence / ByteString / Bitfield.
   STATIC_REQUIRE(
      (psio3::spec_applies_to_shape_v<psio3::length_bound,
                                      psio3::VariableSequenceShape>));
   STATIC_REQUIRE(
      (psio3::spec_applies_to_shape_v<psio3::length_bound,
                                      psio3::ByteStringShape>));
   STATIC_REQUIRE(
      (!psio3::spec_applies_to_shape_v<psio3::length_bound,
                                       psio3::RecordShape>));

   // field_num_spec has no applies_to → permissive.
   STATIC_REQUIRE(
      (psio3::spec_applies_to_shape_v<psio3::field_num_spec,
                                      psio3::RecordShape>));
   STATIC_REQUIRE(
      (psio3::spec_applies_to_shape_v<psio3::field_num_spec,
                                      psio3::PrimitiveShape>));
}

TEST_CASE("Convenience factories build the right spec values", "[annotate]")
{
   STATIC_REQUIRE((psio3::spec::bytes<48>.exact.value() == 48));
   STATIC_REQUIRE((psio3::spec::max_size<1024>.max.value() == 1024));
   STATIC_REQUIRE((psio3::spec::min_size<4>.min.value() == 4));
   STATIC_REQUIRE((psio3::spec::utf8<256>.max == 256));
   STATIC_REQUIRE((psio3::spec::hex<32>.bytes == 32));
   STATIC_REQUIRE((psio3::spec::field<3>.value == 3));
}

TEST_CASE("operator| composes spec values into a tuple", "[annotate]")
{
   constexpr auto combined = psio3::spec::bytes<48> | psio3::spec::field<1>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(combined)> == 2);
   STATIC_REQUIRE(std::get<0>(combined).exact.value() == 48);
   STATIC_REQUIRE(std::get<1>(combined).value == 1);

   // Chain of 3.
   constexpr auto triple =
       psio3::spec::bytes<32> | psio3::spec::field<2> | psio3::spec::sorted;
   STATIC_REQUIRE(std::tuple_size_v<decltype(triple)> == 3);
   STATIC_REQUIRE(std::get<2>(triple).unique == false);
   STATIC_REQUIRE(std::get<2>(triple).ascending == true);
}

TEST_CASE("Type-level annotations route through psio3::type<T>{}",
          "[annotate]")
{
   constexpr auto type_anns = psio3::annotate<psio3::type<Foo>{}>;
   auto dwnc =
       psio3::find_spec<psio3::definition_will_not_change>(type_anns);
   REQUIRE(dwnc.has_value());
}

// ── PSIO3_ATTRS macro coverage ─────────────────────────────────────────

namespace macro_attrs {
   struct Widget {
      std::int32_t id;
      std::string  label;
      std::int64_t amount;
   };
}

// Single-field form.
PSIO3_FIELD_ATTRS(macro_attrs::Widget, id,
   psio3::field_num_spec{.value = 7})

// Multi-field form via parenthesized entries. Spec expressions that
// contain commas (designated-init lists, operator| chains) must be
// wrapped in their own parens so the preprocessor's argument splitter
// sees one spec per entry.
PSIO3_ATTRS(macro_attrs::Widget,
   (label, (psio3::utf8_spec{.max = 128, .field_num = 2}
          | psio3::length_bound{.max = 128})),
   (amount, (psio3::field_num_spec{.value = 3})))

// Type-level form.
PSIO3_TYPE_ATTRS(macro_attrs::Widget, psio3::definition_will_not_change{})

TEST_CASE("PSIO3_FIELD_ATTRS emits a member annotation", "[annotate][macro]")
{
   constexpr auto anns = psio3::annotate<&macro_attrs::Widget::id>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(anns)> == 1);
   auto f = psio3::find_spec<psio3::field_num_spec>(anns);
   REQUIRE(f.has_value());
   REQUIRE(f->value == 7);
}

TEST_CASE("PSIO3_ATTRS routes (field, specs) entries", "[annotate][macro]")
{
   constexpr auto label_anns = psio3::annotate<&macro_attrs::Widget::label>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(label_anns)> == 2);
   auto u = psio3::find_spec<psio3::utf8_spec>(label_anns);
   REQUIRE(u.has_value());
   REQUIRE(u->max == 128);

   constexpr auto amount_anns = psio3::annotate<&macro_attrs::Widget::amount>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(amount_anns)> == 1);
   auto f = psio3::find_spec<psio3::field_num_spec>(amount_anns);
   REQUIRE(f->value == 3);
}

TEST_CASE("PSIO3_TYPE_ATTRS emits a type-level annotation",
          "[annotate][macro]")
{
   constexpr auto type_anns =
      psio3::annotate<psio3::type<macro_attrs::Widget>{}>;
   auto d =
      psio3::find_spec<psio3::definition_will_not_change>(type_anns);
   REQUIRE(d.has_value());
}

// ── length_bound on optional<T> propagates to inner T ────────────────
//
// The applicability check unwraps transparent wrappers — a spec attached
// to an `optional<U>` field is also accepted if it would apply to U.
// This allows annotation-form bounds to reach into common wrapper types
// without separate codec plumbing per wrapper.

struct OptBoundForm {
   std::optional<std::string>                title;
   std::optional<std::vector<std::uint32_t>> tags;
};
PSIO3_REFLECT(OptBoundForm, title, tags)
PSIO3_FIELD_ATTRS(OptBoundForm, title,
   psio3::length_bound{.max = 64})
PSIO3_FIELD_ATTRS(OptBoundForm, tags,
   psio3::length_bound{.max = 16})

// ── PSIO3_REFLECT keyword dispatch (attr / definitionWillNotChange) ──

struct AttrSyntaxRecord
{
   std::int32_t                 id     = 0;
   std::string                  label;
   std::vector<std::uint32_t>   tags;
   std::optional<std::string>   note;
};
PSIO3_REFLECT(AttrSyntaxRecord,
   id,
   attr(label, max<63>),
   attr(tags,  max<255> | field<7>),
   attr(note,  max<255>))

struct DwncRecord
{
   std::uint64_t a = 0;
   std::uint64_t b = 0;
};
PSIO3_REFLECT(DwncRecord, a, b, definitionWillNotChange())

TEST_CASE("PSIO3_REFLECT attr(...) registers field + annotation",
          "[annotate][reflect][attr]")
{
   using R = psio3::reflect<AttrSyntaxRecord>;
   STATIC_REQUIRE(R::member_count == 4);

   // Bare field — no annotation.
   constexpr auto id_anns = psio3::annotate<&AttrSyntaxRecord::id>;
   STATIC_REQUIRE(std::tuple_size_v<decltype(id_anns)> == 0);

   // attr(label, max<63>) — single spec.
   constexpr auto label_anns = psio3::annotate<&AttrSyntaxRecord::label>;
   auto label_lb = psio3::find_spec<psio3::length_bound>(label_anns);
   REQUIRE(label_lb.has_value());
   REQUIRE(label_lb->max == 63);

   // attr(tags, max<255> | field<7>) — composed specs.
   constexpr auto tags_anns = psio3::annotate<&AttrSyntaxRecord::tags>;
   auto tags_lb = psio3::find_spec<psio3::length_bound>(tags_anns);
   auto tags_fn = psio3::find_spec<psio3::field_num_spec>(tags_anns);
   REQUIRE(tags_lb.has_value());
   REQUIRE(tags_lb->max == 255);
   REQUIRE(tags_fn.has_value());
   REQUIRE(tags_fn->value == 7);

   // attr(note, max<255>) — annotation reaches the inner string via
   // the optional unwrap rule (landed earlier).
   constexpr auto note_anns = psio3::annotate<&AttrSyntaxRecord::note>;
   auto note_lb = psio3::find_spec<psio3::length_bound>(note_anns);
   REQUIRE(note_lb.has_value());
   REQUIRE(note_lb->max == 255);
}

TEST_CASE("PSIO3_REFLECT definitionWillNotChange() emits type-level dwnc",
          "[annotate][reflect][dwnc]")
{
   using R = psio3::reflect<DwncRecord>;
   // Field count is 2 — definitionWillNotChange() is not a field.
   STATIC_REQUIRE(R::member_count == 2);

   constexpr auto type_anns =
      psio3::annotate<psio3::type<DwncRecord>{}>;
   auto dwnc =
      psio3::find_spec<psio3::definition_will_not_change>(type_anns);
   REQUIRE(dwnc.has_value());
}

TEST_CASE("length_bound on optional<T> reaches the inner T",
          "[annotate][optional]")
{
   // Compiling these effective_annotations_for instantiations exercises
   // the static_assert in all_specs_apply.  Before the fix this failed
   // with "spec is attached to a field whose shape is not in the spec's
   // applies_to set" because OptionalShape isn't in length_bound's
   // applies_to.  After the fix, applicability sees through optional<>
   // and finds the inner ByteString / VariableSequence shape.
   using TitleEff = psio3::effective_annotations_for<
      OptBoundForm, std::optional<std::string>, &OptBoundForm::title>;
   constexpr auto title_lb =
      psio3::find_spec<psio3::length_bound>(TitleEff::value);
   REQUIRE(title_lb.has_value());
   REQUIRE(title_lb->max == 64);

   using TagsEff = psio3::effective_annotations_for<
      OptBoundForm, std::optional<std::vector<std::uint32_t>>,
      &OptBoundForm::tags>;
   constexpr auto tags_lb =
      psio3::find_spec<psio3::length_bound>(TagsEff::value);
   REQUIRE(tags_lb.has_value());
   REQUIRE(tags_lb->max == 16);
}

// ── WIT attribute specs (final, canonical, unique_keys, flags, ...) ──

#include <map>
#include <set>
#include <string>
#include <unordered_map>

struct WitAttrRecord
{
   std::int32_t v = 0;
};
PSIO3_REFLECT(WitAttrRecord, v)
PSIO3_TYPE_ATTRS(WitAttrRecord,
   psio3::canonical | psio3::final_v
   | psio3::since_spec{.version = "0.2.0"})

TEST_CASE("WIT attribute specs available + composable",
          "[annotate][wit_attrs]")
{
   STATIC_REQUIRE(std::is_same_v<
      decltype(psio3::canonical), const psio3::canonical_spec>);
   STATIC_REQUIRE(std::is_same_v<
      decltype(psio3::final_v), const psio3::final_spec>);

   constexpr auto type_anns =
      psio3::annotate<psio3::type<WitAttrRecord>{}>;
   REQUIRE(psio3::find_spec<psio3::canonical_spec>(type_anns)
              .has_value());
   REQUIRE(psio3::find_spec<psio3::final_spec>(type_anns)
              .has_value());
   auto since = psio3::find_spec<psio3::since_spec>(type_anns);
   REQUIRE(since.has_value());
   REQUIRE(std::string_view{since->version} == "0.2.0");
}

TEST_CASE("std::map carries sorted + unique_keys via inherent_annotations",
          "[annotate][inherent]")
{
   using M = std::map<std::int32_t, std::string>;
   constexpr auto anns = psio3::inherent_annotations<M>::value;
   auto srt = psio3::find_spec<psio3::sorted_spec>(anns);
   auto uk  = psio3::find_spec<psio3::unique_keys_spec>(anns);
   REQUIRE(srt.has_value());
   REQUIRE(srt->unique == true);
   REQUIRE(uk.has_value());
}

TEST_CASE("std::unordered_map carries unique_keys only", "[annotate][inherent]")
{
   using M = std::unordered_map<std::int32_t, std::string>;
   constexpr auto anns = psio3::inherent_annotations<M>::value;
   REQUIRE(psio3::find_spec<psio3::unique_keys_spec>(anns).has_value());
   REQUIRE_FALSE(psio3::find_spec<psio3::sorted_spec>(anns).has_value());
}

TEST_CASE("std::set carries sorted + unique_keys", "[annotate][inherent]")
{
   using S = std::set<std::int64_t>;
   constexpr auto anns = psio3::inherent_annotations<S>::value;
   REQUIRE(psio3::find_spec<psio3::sorted_spec>(anns).has_value());
   REQUIRE(psio3::find_spec<psio3::unique_keys_spec>(anns).has_value());
}

TEST_CASE("std::u8string carries utf8", "[annotate][inherent]")
{
   constexpr auto anns =
      psio3::inherent_annotations<std::u8string>::value;
   REQUIRE(psio3::find_spec<psio3::utf8_spec>(anns).has_value());
}
