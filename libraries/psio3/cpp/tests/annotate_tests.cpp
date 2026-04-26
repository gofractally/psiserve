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
