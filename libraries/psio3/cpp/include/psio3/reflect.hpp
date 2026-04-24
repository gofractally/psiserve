#pragma once
//
// psio3/reflect.hpp — Layer 1 reflection trait.
//
// Exposes per-type metadata through psio3::reflect<T>:
//
//   psio3::reflect<T>::name                           struct name (string_view)
//   psio3::reflect<T>::member_count                   number of fields
//   psio3::reflect<T>::member_pointer<I>              &T::field_I
//   psio3::reflect<T>::member_name<I>                 "field_I"
//   psio3::reflect<T>::field_number<I>                1-based ordinal (source order)
//   psio3::reflect<T>::member_type<I>                 type of field I
//   psio3::reflect<T>::index_of(name)                 runtime name → index
//   psio3::reflect<T>::index_of_field_number(n)       field-number → index
//   psio3::reflect<T>::visit_field_by_name(obj, name, fn)
//   psio3::reflect<T>::visit_field_by_number(obj, n, fn)
//   psio3::reflect<T>::for_each_field(obj, fn)        iterate all
//   psio3::reflect<T>::is_reflected                   SFINAE-style check
//
// Population: `PSIO3_REFLECT(Type, field1, field2, ...)` macro, callable
// at namespace scope right after the struct definition (no namespace
// gymnastics needed — macro uses ADL to plug the metadata into the
// user's namespace).
//
// Dispatch: `psio3::reflect<T>` is a class template whose base is the
// type returned by an ADL-found helper `psio3_reflect_helper(T*)`.
// Unreflected types fall through to an empty default with
// is_reflected = false.
//
// Reflection covers public non-static data members only (design § 5.2.5.4).

#include <boost/preprocessor.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psio3 {

   namespace detail {
      struct reflect_default_impl {
         static constexpr bool is_reflected = false;
      };

      // Reasonable linear scan — sufficient for structs up to BeaconState
      // scale (~20 fields) at no measurable runtime cost. A true
      // perfect-hash backend lands in phase 13 (design § 5.2.5.1); the
      // public surface does not change.
      template <std::size_t N>
      constexpr std::optional<std::size_t>
      linear_index_of(const std::array<std::string_view, N>& names,
                      std::string_view target) noexcept
      {
         for (std::size_t i = 0; i < N; ++i)
            if (names[i] == target) return i;
         return std::nullopt;
      }
   }

   // Fallback ADL hook: returns the default empty impl for unreflected
   // types. Users' PSIO3_REFLECT macro emits a more-specific overload
   // in their namespace; ADL picks that one over this fallback.
   template <typename T>
   constexpr detail::reflect_default_impl psio3_reflect_helper(T*) noexcept
   {
      return {};
   }

   // Public facade. Inherits from whatever the ADL dispatch returns —
   // a detail::reflect_default_impl for unreflected types, or the
   // user's generated struct for reflected types.
   template <typename T>
   struct reflect : decltype(psio3_reflect_helper(static_cast<T*>(nullptr)))
   {
   };

   // Concept gate.
   template <typename T>
   concept Reflected = reflect<T>::is_reflected;

}  // namespace psio3

// ── Macro implementation ─────────────────────────────────────────────────
//
// PSIO3_REFLECT(Type, f1, f2, ...) emits an ADL-found helper function
// in the enclosing namespace:
//
//     inline auto psio3_reflect_helper(Type*) noexcept { return … impl … ; }
//
// The returned unnamed struct carries all the metadata psio3::reflect<T>
// exposes. Because the helper lives in the same namespace as the struct,
// users can call PSIO3_REFLECT right after the struct definition without
// leaving the namespace.
//
// Each per-field helper takes (r, data, I, FIELD).

#define PSIO3_REFLECT_MEMBER_POINTER_CASE_(r, TYPE, I, FIELD) \
   if constexpr (Idx == (I)) return &TYPE::FIELD;

#define PSIO3_REFLECT_MEMBER_NAME_CASE_(r, _, I, FIELD) \
   if constexpr (Idx == (I)) return ::std::string_view(BOOST_PP_STRINGIZE(FIELD));

#define PSIO3_REFLECT_MEMBER_TYPE_CASE_(r, TYPE, I, FIELD)                                    \
   if constexpr (Idx == (I))                                                                  \
      return ::std::type_identity<                                                            \
         ::std::remove_reference_t<decltype(::std::declval<TYPE&>().FIELD)>>{};

#define PSIO3_REFLECT_NAME_ARRAY_ELEM_(r, _, I, FIELD) \
   BOOST_PP_COMMA_IF(I) ::std::string_view(BOOST_PP_STRINGIZE(FIELD))

#define PSIO3_REFLECT_VISIT_NAME_CASE_(r, _, I, FIELD)                \
   if (lookup_name == ::std::string_view(BOOST_PP_STRINGIZE(FIELD)))  \
   {                                                                  \
      ::std::invoke(::std::forward<F>(fn), obj.FIELD);                \
      return true;                                                    \
   }

#define PSIO3_REFLECT_VISIT_NUMBER_CASE_(r, _, I, FIELD)  \
   if (lookup_num == static_cast<::std::uint32_t>((I) + 1)) \
   {                                                      \
      ::std::invoke(::std::forward<F>(fn), obj.FIELD);    \
      return true;                                        \
   }

#define PSIO3_REFLECT_FOR_EACH_CASE_(r, _, I, FIELD)              \
   ::std::invoke(::std::forward<F>(fn),                           \
                 ::std::integral_constant<::std::size_t, (I)>{},  \
                 ::std::string_view(BOOST_PP_STRINGIZE(FIELD)),   \
                 obj.FIELD);

// NOTE: this macro takes an *unqualified* type identifier. Invoke
// PSIO3_REFLECT inside the namespace containing the type:
//
//     namespace eth {
//        struct Validator { ... };
//        PSIO3_REFLECT(Validator, pubkey, withdrawal_credentials, ...)
//     }
//
// The macro concatenates the type identifier into a helper struct
// name, so TYPE must be a simple identifier (no `::`, no templates).
// Templated types and qualified names will be supported via a larger
// macro expansion in a later phase; today's phase-1 constraint covers
// all of v1 psio's existing callers.

#define PSIO3_REFLECT(TYPE, ...) \
   PSIO3_REFLECT_IMPL_(TYPE, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

#define PSIO3_REFLECT_IMPL_(TYPE, SEQ)                                                   \
   struct BOOST_PP_CAT(psio3_reflect_impl_, TYPE)                                        \
   {                                                                                     \
      using type = TYPE;                                                                 \
      static constexpr bool               is_reflected = true;                           \
      static constexpr ::std::string_view name         = BOOST_PP_STRINGIZE(TYPE);       \
      static constexpr ::std::size_t      member_count = BOOST_PP_SEQ_SIZE(SEQ);         \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr auto _member_pointer_impl_() noexcept                             \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO3_REFLECT_MEMBER_POINTER_CASE_, TYPE, SEQ)          \
      }                                                                                  \
      template <::std::size_t Idx>                                                       \
      static constexpr auto member_pointer = _member_pointer_impl_<Idx>();               \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr ::std::string_view _member_name_impl_() noexcept                  \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO3_REFLECT_MEMBER_NAME_CASE_, _, SEQ)                \
      }                                                                                  \
      template <::std::size_t Idx>                                                       \
      static constexpr ::std::string_view member_name = _member_name_impl_<Idx>();       \
                                                                                         \
      template <::std::size_t Idx>                                                       \
      static constexpr auto _member_type_impl_() noexcept                                \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO3_REFLECT_MEMBER_TYPE_CASE_, TYPE, SEQ)             \
      }                                                                                  \
      template <::std::size_t Idx>                                                       \
      using member_type = typename decltype(_member_type_impl_<Idx>())::type;            \
                                                                                         \
      /* Source-order 1-based field numbers. Annotations (phase 2) */                    \
      /* override per-field via psio3::annotate<&T::m>.             */                   \
      template <::std::size_t Idx>                                                       \
      static constexpr ::std::uint32_t field_number                                      \
         = static_cast<::std::uint32_t>(Idx + 1);                                        \
                                                                                         \
      static constexpr ::std::array<::std::string_view, member_count> _names_array_      \
         = {BOOST_PP_SEQ_FOR_EACH_I(PSIO3_REFLECT_NAME_ARRAY_ELEM_, _, SEQ)};             \
                                                                                         \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of(::std::string_view name_) noexcept                                        \
      {                                                                                  \
         return ::psio3::detail::linear_index_of(_names_array_, name_);                  \
      }                                                                                  \
                                                                                         \
      static constexpr ::std::optional<::std::size_t>                                    \
      index_of_field_number(::std::uint32_t n) noexcept                                  \
      {                                                                                  \
         if (n < 1 || n > member_count) return ::std::nullopt;                           \
         return static_cast<::std::size_t>(n - 1);                                       \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr bool visit_field_by_name(                                         \
         Obj& obj, ::std::string_view lookup_name, F&& fn)                               \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO3_REFLECT_VISIT_NAME_CASE_, _, SEQ)                 \
         return false;                                                                   \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr bool visit_field_by_number(                                       \
         Obj& obj, ::std::uint32_t lookup_num, F&& fn)                                   \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO3_REFLECT_VISIT_NUMBER_CASE_, _, SEQ)               \
         return false;                                                                   \
      }                                                                                  \
                                                                                         \
      template <typename Obj, typename F>                                                \
      static constexpr void for_each_field(Obj& obj, F&& fn)                             \
      {                                                                                  \
         BOOST_PP_SEQ_FOR_EACH_I(PSIO3_REFLECT_FOR_EACH_CASE_, _, SEQ)                   \
      }                                                                                  \
   };                                                                                    \
   inline auto psio3_reflect_helper(TYPE*) noexcept                                      \
   {                                                                                     \
      return BOOST_PP_CAT(psio3_reflect_impl_, TYPE){};                                  \
   }
