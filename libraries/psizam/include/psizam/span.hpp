#pragma once

#include <span>
#include <type_traits>

namespace psizam {
   using std::span;

   inline constexpr std::size_t dynamic_extent = std::dynamic_extent;

   namespace detail {
      template <typename T>
      constexpr std::true_type is_span_type(span<T>) { return {}; }
      template <typename T>
      constexpr std::false_type is_span_type(T) { return {}; }
   } // ns psizam::detail

   template <typename T>
   constexpr inline static bool is_span_type_v = std::is_same_v<decltype(detail::is_span_type(std::declval<T>())), std::true_type>;

} // namespace psizam
