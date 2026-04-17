#pragma once

// WIT attribute support for PSIO-reflected types.
//
// Two registries, opt-in via ADL overloads:
//
//   type_attrs_of<T>()         — attributes attached to a C++ type
//   member_attrs_of<MP>()      — attributes attached to &T::member
//
// Both return a std::tuple of tag values. Empty tuple by default.
// Users extend by defining overloads of psio_type_attrs_lookup /
// psio_member_attrs_lookup in the namespace of their type — ADL finds
// them at instantiation time.
//
// The user surface is three macros:
//   PSIO_TYPE_ATTRS(T, tags...)
//   PSIO_FIELD_ATTRS(T, FIELD, tags...)
// and — when used inside PSIO_REFLECT — the inline forms
//   ATTRS(field, tags...), SORTED(field), UTF8(field), PADDING(field),
//   UNIQUE_KEYS(field), canonical(), final().
//
// See doc/wit-attributes.md for the full attribute vocabulary.

#include <map>
#include <set>
#include <string>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>

#include <psio/reflect.hpp>  // for psio::FixedString

namespace psio
{

// ── Tag types ─────────────────────────────────────────────────────────────

struct sorted_tag      {};
struct unique_keys_tag {};
struct utf8_tag        {};
struct canonical_tag   {};
struct final_tag       {};
struct padding_tag     {};
struct flags_tag       {};

template <unsigned N>
struct number_tag
{
   unsigned value = N;
};

template <FixedString V>
struct since_tag
{
   static constexpr auto version = V;
};

template <FixedString N>
struct unstable_tag
{
   static constexpr auto feature = N;
};

template <FixedString V>
struct deprecated_tag
{
   static constexpr auto version = V;
};

// ── Registry keys ─────────────────────────────────────────────────────────

template <typename T>
struct type_attrs_key
{
};

// The class type T is a template PARAMETER of the key so that the user's
// namespace (T's namespace) is part of member_attrs_key's associated
// namespace set for ADL. A bare non-type template parameter (the member
// pointer alone) would not carry T's namespace.
template <typename T, auto MemberPtr>
struct member_attrs_key
{
};

// Extract the class type of a pointer-to-member so callers can pass just
// the pointer-to-member (member_attrs_of<&T::f>()) without also naming T.
namespace detail
{
   template <typename MP>
   struct member_ptr_class;

   template <typename C, typename M>
   struct member_ptr_class<M C::*>
   {
      using type = C;
   };
}  // namespace detail

// ── Default overloads (empty tuple) ───────────────────────────────────────
//
// ADL picks these up when no user or stdlib overload is more specific.

template <typename T>
constexpr auto psio_type_attrs_lookup(type_attrs_key<T>)
{
   return std::tuple<>{};
}

template <typename T, auto MP>
constexpr auto psio_member_attrs_lookup(member_attrs_key<T, MP>)
{
   return std::tuple<>{};
}

// ── Public accessors ──────────────────────────────────────────────────────

template <typename T>
constexpr auto type_attrs_of()
{
   if constexpr (std::is_final_v<T>)
      return std::tuple_cat(std::tuple{final_tag{}},
                            psio_type_attrs_lookup(type_attrs_key<T>{}));
   else
      return psio_type_attrs_lookup(type_attrs_key<T>{});
}

template <auto MP>
constexpr auto member_attrs_of()
{
   using class_t = typename detail::member_ptr_class<decltype(MP)>::type;
   return psio_member_attrs_lookup(member_attrs_key<class_t, MP>{});
}

// Convenience: test whether a tag of type Tag appears in a tuple of attrs.
namespace detail
{
   template <typename Tag, typename Tuple, std::size_t... Is>
   constexpr bool has_tag_impl(const Tuple&, std::index_sequence<Is...>)
   {
      return ((std::is_same_v<Tag, std::tuple_element_t<Is, Tuple>>) || ...);
   }
}  // namespace detail

template <typename Tag, typename Tuple>
constexpr bool has_tag(const Tuple& t)
{
   return detail::has_tag_impl<Tag>(
       t, std::make_index_sequence<std::tuple_size_v<Tuple>>{});
}

template <typename Tag, typename T>
constexpr bool type_has_tag()
{
   return has_tag<Tag>(type_attrs_of<T>());
}

template <typename Tag, auto MP>
constexpr bool member_has_tag()
{
   return has_tag<Tag>(member_attrs_of<MP>());
}

// ── Built-in specializations for common stdlib types ──────────────────────
//
// These live in psio:: so ADL always finds them (type_attrs_key<T> has
// psio as an associated namespace regardless of T).

template <typename K, typename V, typename C, typename A>
constexpr auto psio_type_attrs_lookup(type_attrs_key<std::map<K, V, C, A>>)
{
   return std::tuple{sorted_tag{}, unique_keys_tag{}};
}

template <typename K, typename C, typename A>
constexpr auto psio_type_attrs_lookup(type_attrs_key<std::set<K, C, A>>)
{
   return std::tuple{sorted_tag{}, unique_keys_tag{}};
}

template <typename K, typename V, typename H, typename E, typename A>
constexpr auto psio_type_attrs_lookup(
    type_attrs_key<std::unordered_map<K, V, H, E, A>>)
{
   // Unordered map is keyed — uniqueness holds, order does not.
   return std::tuple{unique_keys_tag{}};
}

template <typename K, typename H, typename E, typename A>
constexpr auto psio_type_attrs_lookup(
    type_attrs_key<std::unordered_set<K, H, E, A>>)
{
   return std::tuple{unique_keys_tag{}};
}

template <typename A>
constexpr auto psio_type_attrs_lookup(
    type_attrs_key<std::basic_string<char8_t, std::char_traits<char8_t>, A>>)
{
   return std::tuple{utf8_tag{}};
}

}  // namespace psio

// ── User-facing macros ────────────────────────────────────────────────────

// Declare type-level attributes for T at namespace scope:
//
//   PSIO_TYPE_ATTRS(MyRecord, psio::canonical_tag{}, psio::final_tag{})
//
// Expands to an ADL-visible overload in the current namespace.
#define PSIO_TYPE_ATTRS(T, ...)                                      \
   constexpr auto psio_type_attrs_lookup(::psio::type_attrs_key<T>)  \
   {                                                                 \
      return ::std::tuple{__VA_ARGS__};                              \
   }

// Declare field-level attributes for T::FIELD at namespace scope:
//
//   PSIO_FIELD_ATTRS(Container, points, psio::sorted_tag{})
//
// Expands to an ADL-visible overload in the current namespace. The key
// includes T as a template parameter so that T's namespace is part of
// the overload's associated-namespace set (see member_attrs_key above).
#define PSIO_FIELD_ATTRS(T, FIELD, ...)                                   \
   constexpr auto psio_member_attrs_lookup(                               \
       ::psio::member_attrs_key<T, &T::FIELD>)                            \
   {                                                                      \
      return ::std::tuple{__VA_ARGS__};                                   \
   }
