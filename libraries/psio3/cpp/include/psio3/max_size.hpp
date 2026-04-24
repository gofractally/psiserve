#pragma once
//
// psio3/max_size.hpp — compile-time upper bound on a type's encoded
// byte count, given its shape and its fields' length_bound annotations.
//
// `psio3::max_encoded_size<T>()` returns `std::optional<std::size_t>`:
//   - some(N) when every variable-length sub-field of T carries a
//     `length_bound{.max=M}` (or exact=M) annotation, producing a
//     finite upper bound over all possible encoded values
//   - nullopt when any sub-field is unbounded — the encoder cannot
//     guarantee the output will fit in any predetermined width
//
// Field-level bounds participate through psio3's standard annotation
// merge: effective_annotations_for<Rec, F, MemberPtr> gathers
// member > wrapper > type specs, and the walk below reads length_bound
// out of that merged tuple. Member beats wrapper beats type, so a field
// can tighten a type-level default.
//
// Assumed wire overhead is pssz-family with W=4 (the widest offset
// size). That is the conservative choice — if max fits in 0xff with
// W=4 offsets, it certainly fits with W=1 offsets; the narrower the
// offset, the fewer overhead bytes spent, so the max only shrinks.
//
// Consumers today: `auto_pssz_width_v<T>` in pssz.hpp picks the
// narrowest pssz width whose offsets can index every addressable byte
// in the worst case. Future consumers: buffer pre-sizing in any
// format, schema generation for fixed-width wire dialects.

#include <psio3/annotate.hpp>
#include <psio3/ext_int.hpp>
#include <psio3/reflect.hpp>
#include <psio3/shapes.hpp>
#include <psio3/wrappers.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psio3 {

   namespace detail::max_size_impl {

      // ── Shape classifiers local to this walk ──────────────────────────────

      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename E, typename A>
      struct is_std_vector<std::vector<E, A>> : std::true_type {};

      template <typename T>
      struct is_std_array : std::false_type {};
      template <typename E, std::size_t N>
      struct is_std_array<std::array<E, N>> : std::true_type
      {
         static constexpr std::size_t n = N;
         using element_type             = E;
      };

      // Extract the effective count cap from a length_bound: exact wins,
      // then max. min alone does not bound the upper side.
      constexpr std::optional<std::uint32_t>
      bound_max(const length_bound& lb) noexcept
      {
         if (lb.exact)
            return lb.exact;
         if (lb.max)
            return lb.max;
         return std::nullopt;
      }

      // Header bookkeeping each pssz-style format imposes on a variable
      // field. We bill every variable field 4 bytes (offset or length
      // prefix under W=4). Pure fixed records pay zero.
      inline constexpr std::size_t kVariableFieldOverhead = 4;

      // Max encoded bytes for a bare type, with no field-annotation
      // context. Returns nullopt for unbounded variable-length types
      // (std::vector, std::string) — those must be used via a
      // length-bounded annotation or a bounded<> wrapper for the parent
      // to have a finite max.
      template <typename T>
      consteval std::optional<std::size_t> max_bare() noexcept;

      template <typename Annots>
      struct find_length_bound
      {
         static constexpr auto value =
            ::psio3::find_spec<::psio3::length_bound>(Annots{});
      };

      // Max encoded bytes for a *field* of FieldType, given its merged
      // effective-annotations tuple. Honors length_bound when the field
      // shape is variable-length (string, vector).
      template <typename FieldType, typename AnnotsTuple>
      consteval std::optional<std::size_t>
      max_field_with_annots(AnnotsTuple annots) noexcept
      {
         using F = std::remove_cvref_t<FieldType>;

         // String → length_bound is a byte count.
         if constexpr (std::is_same_v<F, std::string>)
         {
            auto lb = ::psio3::find_spec<::psio3::length_bound>(annots);
            if (lb)
               if (auto n = bound_max(*lb))
                  return static_cast<std::size_t>(*n) +
                         kVariableFieldOverhead;
            return std::nullopt;
         }
         // Vector → length_bound is an element count; multiply by
         // per-element max. Fails if the element type is itself
         // unbounded (e.g. vector<string> without nested bounds).
         else if constexpr (is_std_vector<F>::value)
         {
            using E = typename F::value_type;
            auto lb = ::psio3::find_spec<::psio3::length_bound>(annots);
            if (!lb)
               return std::nullopt;
            auto n = bound_max(*lb);
            if (!n)
               return std::nullopt;
            auto e = max_bare<E>();
            if (!e)
               return std::nullopt;
            return static_cast<std::size_t>(*n) * *e +
                   kVariableFieldOverhead;
         }
         else
         {
            // Fixed shapes, records, primitives — bounds don't apply.
            return max_bare<F>();
         }
      }

      template <typename T>
      consteval std::optional<std::size_t> max_bare() noexcept
      {
         using U = std::remove_cvref_t<T>;

         if constexpr (std::is_same_v<U, bool>)
            return 1;
         else if constexpr (std::is_same_v<U, ::psio3::uint256>)
            return 32;
         else if constexpr (std::is_same_v<U, ::psio3::uint128> ||
                            std::is_same_v<U, ::psio3::int128>)
            return 16;
         else if constexpr (std::is_enum_v<U>)
            return sizeof(std::underlying_type_t<U>);
         else if constexpr (std::is_arithmetic_v<U>)
            return sizeof(U);
         else if constexpr (is_std_array<U>::value)
         {
            using E = typename is_std_array<U>::element_type;
            auto e  = max_bare<E>();
            if (!e)
               return std::nullopt;
            return is_std_array<U>::n * *e;
         }
         else if constexpr (::psio3::Record<U>)
         {
            using R                 = ::psio3::reflect<U>;
            constexpr std::size_t N = R::member_count;
            std::size_t total = 0;
            bool        bounded = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto add_field = [&]<std::size_t I>() {
                  using FT = typename R::template member_type<I>;
                  using eff =
                     ::psio3::effective_annotations_for<
                        U, FT, R::template member_pointer<I>>;
                  auto m = max_field_with_annots<FT>(eff::value);
                  if (!m)
                  {
                     bounded = false;
                     return;
                  }
                  total += *m;
               };
               (add_field.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            if (!bounded)
               return std::nullopt;
            return total;
         }
         else
         {
            // std::string, std::vector<_> — require annotation context.
            return std::nullopt;
         }
      }

   }  // namespace detail::max_size_impl

   // Public entry point — returns the upper bound in bytes over all
   // possible encoded values of T, or nullopt when any sub-field is
   // unbounded. The bound assumes pssz W=4 offset overhead and is a
   // correct upper bound for every narrower pssz width.
   template <typename T>
   consteval std::optional<std::size_t> max_encoded_size() noexcept
   {
      return detail::max_size_impl::max_bare<T>();
   }

}  // namespace psio3
