#pragma once
//
// psio3/annotate.hpp — Layer 1 annotation system.
//
// Annotations are pure user data attached to types or members and
// consumed by format authors. Design spec: `.issues/psio-v2-design.md`
// § 5.3.
//
// Producer / consumer model:
//   - Producers (users) specialize `psio3::annotate<X>` for
//     `X = &T::field` (member-level) or `X = psio3::type<T>{}` (type-level),
//     attaching a tuple of spec structs — pure data, no behavior.
//   - Consumers (formats) query specs via `find_spec<Spec>(anns)` /
//     `has_spec_v<Spec, Tuple>`; unknown specs are ignored.
//
// Resolution rule (applies across the whole library):
//
//   member annotation
//     > wrapper-inherent annotation (field type's `inherent_annotations`)
//       > type annotation (`annotate<type<EnclosingT>{}>`)
//         > format fallback
//
// This same rule governs plain annotations, scalar presentations
// (§5.3.7), and every format-level dispatch. Callers go through
// `effective_annotations<EnclosingT, FieldType, MemberPtr>` (see
// wrappers.hpp), never `annotate` directly, so the three sources are
// merged and conflict-checked in one place.
//
// Key API:
//
//   psio3::annotate<&T::field>          → std::tuple<Specs...>
//   psio3::annotate<psio3::type<T>{}>   → std::tuple<Specs...>
//   psio3::find_spec<Spec>(anns)        → std::optional<Spec>
//   psio3::has_spec_v<Spec, Tuple>      → bool

#include <psio3/reflect.hpp>

#include <boost/preprocessor.hpp>

#include <cstdint>
#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psio3 {

   // ── type<T> wrapper ────────────────────────────────────────────────────
   //
   // C++ NTTPs can't accept bare types, so type<T>{} serves as the
   // value-level stand-in for "annotate the type T itself" (rather than
   // one of its fields). Structural (default ==) so it's usable as an
   // NTTP under C++20's generalized rules.
   template <typename T>
   struct type
   {
      constexpr bool operator==(const type&) const noexcept = default;
   };

   // ── Spec categories ────────────────────────────────────────────────────
   //
   // Static specs influence code generation and emit no runtime work
   // once dispatch is resolved. Runtime specs are checked at validate
   // time. Formats can split the effective-annotation tuple at compile
   // time (`if constexpr` for static, loop for runtime) so `validate()`
   // doesn't pay for specs that are already resolved.
   //
   // Each spec type declares its category as `using spec_category = …`.
   struct static_spec_tag
   {
   };
   struct runtime_spec_tag
   {
   };

   // Fallback trait — specs without an explicit `spec_category` alias
   // default to runtime (safe default: formats treat them as dynamic).
   namespace detail {
      template <typename S, typename = void>
      struct spec_category_of
      {
         using type = runtime_spec_tag;
      };
      template <typename S>
      struct spec_category_of<S, std::void_t<typename S::spec_category>>
      {
         using type = typename S::spec_category;
      };
   }
   template <typename S>
   using spec_category_t = typename detail::spec_category_of<S>::type;

   template <typename S>
   inline constexpr bool is_static_spec_v =
      std::is_same_v<spec_category_t<S>, static_spec_tag>;
   template <typename S>
   inline constexpr bool is_runtime_spec_v =
      std::is_same_v<spec_category_t<S>, runtime_spec_tag>;

   // ── Shape applicability ────────────────────────────────────────────────
   //
   // A spec may declare `using applies_to = shape_set<Shapes...>` — the
   // shapes for which it is meaningful. `effective_annotations` emits a
   // static_assert when a spec is attached to a member whose shape is
   // not in the set. A spec without `applies_to` is treated as
   // applicable to every shape (permissive).
   //
   // Shape tags are the concept names themselves — Primitive, Enum,
   // FixedSequence, VariableSequence, ByteString, Optional, Variant,
   // Bitfield, Record — used only as nominal type tags here, mirrored
   // in shapes.hpp.
   template <typename... ShapeTags>
   struct shape_set
   {
   };

   // Nominal shape-family tags. These mirror the concepts in shapes.hpp
   // and exist at the type level so `applies_to` can name them without
   // dragging in the concept dependency.
   struct PrimitiveShape{};
   struct EnumShape{};
   struct FixedSequenceShape{};
   struct VariableSequenceShape{};
   struct ByteStringShape{};
   struct OptionalShape{};
   struct VariantShape{};
   struct BitfieldShape{};
   struct RecordShape{};

   namespace detail {
      // Shape membership on a (possibly void / non-shape_set) type.
      // The generic form handles `void` — used when a spec has no
      // `applies_to` declaration — by answering `true` (permissive).
      template <typename Shape, typename Set>
      struct shape_in : std::true_type
      {
      };
      template <typename Shape, typename... Shapes>
      struct shape_in<Shape, shape_set<Shapes...>>
         : std::bool_constant<(std::is_same_v<Shape, Shapes> || ...)>
      {
      };

      template <typename S, typename = void>
      struct applies_to_of
      {
         using type = void;  // signals "permissive" — handled above
      };
      template <typename S>
      struct applies_to_of<S, std::void_t<typename S::applies_to>>
      {
         using type = typename S::applies_to;
      };
   }

   template <typename Spec, typename Shape>
   inline constexpr bool spec_applies_to_shape_v =
      detail::shape_in<Shape,
                       typename detail::applies_to_of<Spec>::type>::value;

   // ── Built-in spec structs ──────────────────────────────────────────────
   //
   // Each spec is an aggregate with designated-init-friendly members.
   // Format authors query via `find_spec<SpecType>(annotations)`.

   // Unified length-constraint spec. The SHAPE (not the spec) decides
   // which dimension is being bounded: byte count for ByteStringShape,
   // element count for VariableSequenceShape, bit count for Bitfield.
   //
   // Replaces the pre-consolidation `bytes_spec{.size}` and
   // `max_size_spec{.max}`:
   //   bytes_spec{.size = 48}   ⇒ length_bound{.exact = 48}
   //   max_size_spec{.max = N}  ⇒ length_bound{.max   = N}
   struct length_bound
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<VariableSequenceShape,
                                      ByteStringShape,
                                      BitfieldShape,
                                      FixedSequenceShape>;

      std::optional<std::uint32_t> exact;
      std::optional<std::uint32_t> max;
      std::optional<std::uint32_t> min;

      // Used by the conflict-checker in effective_annotations.
      constexpr bool operator==(const length_bound&) const = default;
   };

   struct utf8_spec
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<ByteStringShape>;

      std::uint32_t max       = 0;   // runtime upper bound; 0 = unbounded
      std::uint32_t field_num = 0;

      constexpr bool operator==(const utf8_spec&) const = default;
   };

   struct hex_spec
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<ByteStringShape>;

      std::uint32_t bytes     = 0;   // source byte count; wire length = 2 * bytes
      std::uint32_t field_num = 0;

      constexpr bool operator==(const hex_spec&) const = default;
   };

   struct sorted_spec
   {
      using spec_category = runtime_spec_tag;
      using applies_to    = shape_set<VariableSequenceShape, FixedSequenceShape>;

      bool unique    = false;
      bool ascending = true;

      constexpr bool operator==(const sorted_spec&) const = default;
   };

   template <typename T>
   struct default_value_spec
   {
      using spec_category = static_spec_tag;
      T value;

      constexpr bool operator==(const default_value_spec&) const = default;
   };
   template <typename T>
   default_value_spec(T) -> default_value_spec<T>;

   template <auto Factory>
   struct default_factory_spec
   {
      using spec_category = static_spec_tag;

      constexpr bool operator==(const default_factory_spec&) const = default;
   };

   struct field_num_spec
   {
      using spec_category = static_spec_tag;
      std::uint32_t value = 0;

      constexpr bool operator==(const field_num_spec&) const = default;
   };

   struct skip_spec
   {
      using spec_category = static_spec_tag;
      bool value = true;

      constexpr bool operator==(const skip_spec&) const = default;
   };

   // Type-level only; spelled out, never abbreviated, so the reader
   // sees the wire-stability commitment they're making.
   struct definition_will_not_change
   {
      using spec_category = static_spec_tag;

      constexpr bool operator==(const definition_will_not_change&) const = default;
   };

   // ── Presentation tag types (§5.3.7) ────────────────────────────────────
   //
   // Nominal tag structs used by `as_spec<Tag>` annotations and as the
   // second template parameter of `psio3::adapter<T, Tag>`. Any
   // namespace can add more tags; the library pre-ships the common
   // blockchain/protocol ones. These are separate from the broad
   // categories (binary_category / text_category) in adapter.hpp —
   // specific tags let a member-level annotation pick among a type's
   // named adapters, not just its default for a category.
   struct hex_tag
   {
   };
   struct base58_tag
   {
   };
   struct decimal_tag
   {
   };
   struct rfc3339_tag
   {
   };
   struct unix_us_tag
   {
   };
   struct unix_ms_tag
   {
   };

   // Convenience instances for succinct `as<Tag>` callsites.
   inline constexpr hex_tag     hex{};
   inline constexpr base58_tag  base58{};
   inline constexpr decimal_tag decimal{};
   inline constexpr rfc3339_tag rfc3339{};
   inline constexpr unix_us_tag unix_us{};
   inline constexpr unix_ms_tag unix_ms{};

   // ── as_spec — member-level adapter override ───────────────────────────
   //
   // Attached via `psio3::annotate<&T::field> = std::tuple{as<hex_tag>}`,
   // this tells format walkers to use `adapter<FieldType, hex_tag>` for
   // THIS field, regardless of the format's category-level default
   // (§5.3.7 member-beats-type precedence).
   template <typename Tag>
   struct as_spec
   {
      using spec_category = static_spec_tag;
      using adapter_tag   = Tag;

      constexpr bool operator==(const as_spec&) const = default;
   };

   // `psio3::as<psio3::hex_tag>` spelling for annotate tuples.
   // The short alias `psio3::as<psio3::hex>` doesn't quite work
   // because the template parameter is a TYPE, not a value — users
   // write either `as<hex_tag>` or desugar to `as_spec<hex_tag>{}`
   // directly.
   template <typename Tag>
   inline constexpr as_spec<Tag> as{};

   // ── Convenience factory templates ──────────────────────────────────────
   //
   // Nested `spec` namespace so short names (`bytes`, `utf8`, …) stay
   // free for the storage free functions in the surrounding psio3
   // namespace (`psio3::bytes(buf)` returns the byte span of a buffer).

   namespace spec {

      // Length-constraint factories — all three desugar to length_bound.
      template <std::uint32_t N>
      inline constexpr length_bound bytes{.exact = N};   // exact wire length

      template <std::uint32_t N>
      inline constexpr length_bound max_size{.max = N};  // upper bound

      template <std::uint32_t N>
      inline constexpr length_bound min_size{.min = N};  // lower bound

      template <std::uint32_t N>
      inline constexpr utf8_spec utf8{.max = N};

      template <std::uint32_t N>
      inline constexpr hex_spec hex{.bytes = N};

      template <std::uint32_t N>
      inline constexpr field_num_spec field{.value = N};

      inline constexpr sorted_spec sorted{};
      inline constexpr sorted_spec sorted_set{.unique = true};

      inline constexpr skip_spec skip{};

      template <auto V>
      inline constexpr default_value_spec<decltype(V)> default_val{V};

      inline constexpr definition_will_not_change definition_will_not_change_v{};

   }  // namespace spec

   // ── Annotation trait ───────────────────────────────────────────────────
   //
   // The primary template is an empty tuple. Users specialize for each
   // field/type they want to annotate; specializations hold a tuple of
   // spec values.

   template <auto X>
   inline constexpr auto annotate = std::tuple<>{};

   // ── find_spec helper ───────────────────────────────────────────────────
   //
   // Format-author-facing query. Returns the spec value if present in
   // the tuple, else nullopt. Spec types are expected to be unique
   // within a given annotation tuple.

   template <typename Wanted, typename... Ts>
   constexpr std::optional<Wanted>
   find_spec(const std::tuple<Ts...>& anns) noexcept
   {
      if constexpr ((std::is_same_v<std::remove_cvref_t<Ts>, Wanted> || ...))
         return std::get<Wanted>(anns);
      else
         return std::nullopt;
   }

   // ── has_spec — compile-time "is this spec present?" ───────────────────
   template <typename Wanted, typename Tuple>
   inline constexpr bool has_spec_v = false;

   template <typename Wanted, typename... Ts>
   inline constexpr bool has_spec_v<Wanted, std::tuple<Ts...>> =
       (std::is_same_v<std::remove_cvref_t<Ts>, Wanted> || ...);

}  // namespace psio3

// ── Composition helpers ─────────────────────────────────────────────────

namespace psio3 {

   // Single-spec → 1-tuple adapter, for uniform composition.
   template <typename A>
   constexpr auto to_spec_tuple(const A& a) noexcept
   {
      return std::tuple<A>{a};
   }
   template <typename... Ts>
   constexpr auto to_spec_tuple(const std::tuple<Ts...>& t) noexcept
   {
      return t;
   }

   // `spec | spec` composes into a tuple. Only enabled for non-arithmetic
   // types so it does not compete with the bitwise `|` on integers.
   template <typename A, typename B>
      requires(!std::is_arithmetic_v<A> && !std::is_arithmetic_v<B>)
   constexpr auto operator|(const A& a, const B& b) noexcept
   {
      return std::tuple_cat(to_spec_tuple(a), to_spec_tuple(b));
   }

}  // namespace psio3

// ── PSIO3_ATTRS macro (§5.3.5 Form 2) ─────────────────────────────────────
//
// Adds annotations to an already-reflected type without touching the
// reflection macro. Supports both field-level and type-level annotations
// via parenthesized entries. Each entry is `(field_name, spec_expr)` or
// `(type, spec_expr)` — the literal identifier `type` selects the
// type-level slot.
//
//   PSIO3_ATTRS(Validator,
//      (pubkey,            psio3::length_bound{.exact = 48}),
//      (effective_balance, psio3::field_num_spec{.value = 3}),
//      (type,              psio3::definition_will_not_change{}))
//
// Each (name, specs) pair expands to one namespace-scope specialization
// of `psio3::annotate<X>`. `specs` may be a single spec value or a
// `spec | spec | …` composition; it is wrapped via `to_spec_tuple` so
// both shapes work uniformly.
//
// Also provided for callers that prefer one-entry-per-invocation:
//
//   PSIO3_FIELD_ATTRS(Validator, pubkey, psio3::length_bound{.exact = 48})
//   PSIO3_TYPE_ATTRS(Validator, psio3::definition_will_not_change{})

#define PSIO3_FIELD_ATTRS(TYPE, FIELD, ...)                         \
   template <>                                                      \
   inline constexpr auto ::psio3::annotate<&TYPE::FIELD> =          \
      ::psio3::to_spec_tuple(__VA_ARGS__);

#define PSIO3_TYPE_ATTRS(TYPE, ...)                                 \
   template <>                                                      \
   inline constexpr auto ::psio3::annotate<::psio3::type<TYPE>{}> = \
      ::psio3::to_spec_tuple(__VA_ARGS__);

// Multi-field form — each entry is `(field_name, (spec_expr))`. The
// inner parens isolate any commas in the spec expression (such as
// those inside designated initializers like `{.max = 128, .min = 4}`)
// from the preprocessor's argument splitter. For type-level
// annotations, call `PSIO3_TYPE_ATTRS(TYPE, …)` directly.
#define PSIO3_ATTRS_ENTRY_(r, TYPE, ENTRY)                          \
   PSIO3_FIELD_ATTRS(TYPE, BOOST_PP_TUPLE_ELEM(0, ENTRY),           \
                     BOOST_PP_REMOVE_PARENS(                        \
                        BOOST_PP_TUPLE_ELEM(1, ENTRY)))

#define PSIO3_ATTRS(TYPE, ...)                                      \
   BOOST_PP_SEQ_FOR_EACH(PSIO3_ATTRS_ENTRY_, TYPE,                  \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))
