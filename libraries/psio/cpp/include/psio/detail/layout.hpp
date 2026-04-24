#pragma once
// Compile-time struct-layout predicates shared across format encoders.
//
// Useful when a format's wire encoding is "concatenate fields in declaration
// order with no framing" (pack_bin DWNC, SSZ fixed containers, bincode fixed
// structs, borsh fixed structs). If a reflected C++ struct has:
//
//   1. All members bitwise-serializable (arithmetic, enum, or opted-in via
//      is_bitwise_copy), and
//   2. sizeof(T) == sum of member sizes (i.e. no struct alignment padding),
//
// then the in-memory bytes exactly match the wire bytes, and the encoder can
// collapse the whole struct to one stream.write of sizeof(T) bytes.
//
// Users whose natural struct layout has alignment padding (e.g. bool followed
// by uint64) can eliminate it with the compiler attribute:
//
//   struct __attribute__((packed)) Validator { ... };
//
// On ARM64 / x86_64 the unaligned field access that results is native and has
// no measurable per-field cost. On both GCC and Clang the attribute is
// supported; psio is Unix-only per CLAUDE.md so we don't handle MSVC's
// `#pragma pack` alternative.

#include <psio/reflect.hpp>
#include <psio/stream.hpp>

#include <cstddef>
#include <type_traits>

namespace psio::layout_detail
{
   // Sum of sizeof(member_type) across all reflected members of T.
   template <typename T>
   consteval std::size_t sum_of_member_sizes()
   {
      return psio::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [](auto... member)
          {
             return (std::size_t{0} + ... +
                     sizeof(std::remove_cvref_t<decltype(std::declval<T>().*member)>));
          });
   }

   // Every reflected member of T satisfies has_bitwise_serialization.
   template <typename T>
   consteval bool all_members_bitwise()
   {
      return psio::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [](auto... member)
          {
             return (true && ... &&
                     has_bitwise_serialization<std::remove_cvref_t<
                         decltype(std::declval<T>().*member)>>());
          });
   }

   // T is a reflected struct whose in-memory bytes exactly match its wire
   // bytes when serialized as "concatenated fields" (no framing, no padding).
   // Format-agnostic: callers gate on additional format-specific conditions.
   template <typename T>
   consteval bool is_memcpy_layout_struct()
   {
      if constexpr (!Reflected<T>)
         return false;
      else
         return all_members_bitwise<T>() && (sum_of_member_sizes<T>() == sizeof(T));
   }
}  // namespace psio::layout_detail
