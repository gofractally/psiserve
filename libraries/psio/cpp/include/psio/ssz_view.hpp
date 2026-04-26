#pragma once
//
// psio3/ssz_view.hpp — SSZ's `view_layout::traits` specialization.
//
// The unified view templates in `psio3/view.hpp` are format-agnostic.
// This header wires SSZ's wire layout into them — no view templates
// are duplicated. Users get `view<std::vector<T>, ssz>`,
// `view<std::optional<T>, ssz>`, `view<std::variant<Ts...>, ssz>`
// simply by including this header alongside `psio3/ssz.hpp`.
//
// Adding another format follows the same recipe: write a traits
// specialization in `<format>_view.hpp`, include it, and the same
// `view<T, Fmt>` template machinery lights up.

#include <psio/ssz.hpp>
#include <psio/view.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace psio1::view_layout {

   // SSZ wire layout:
   //   vector<fixed T>:    span = n × fixed_size<T> bytes.
   //   vector<variable T>: span = [u32 offset_0][offset_1][...][payload]
   //                       where n = offset_0 / 4 and offset_i is the
   //                       byte offset of element i relative to the
   //                       start of the span.
   //   optional<T>:        span = [u8 selector][payload if selector != 0]
   //   variant<Ts...>:     span = [u8 selector][payload]
   template <>
   struct traits<::psio::ssz>
   {
      // ── vector ─────────────────────────────────────────────────────────
      template <typename T>
      static std::size_t vector_count(std::span<const char> s) noexcept
      {
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz =
               ::psio::detail::ssz_impl::fixed_size_of<T>();
            if constexpr (esz == 0)
               return 0;
            else
               return s.size() / esz;
         }
         else
         {
            if (s.size() < 4)
               return 0;
            std::uint32_t first = 0;
            std::memcpy(&first, s.data(), 4);
            return first / 4u;
         }
      }

      template <typename T>
      static std::span<const char>
      vector_element_span(std::span<const char> s, std::size_t i) noexcept
      {
         if constexpr (::psio::detail::ssz_impl::is_fixed_v<T>)
         {
            constexpr std::size_t esz =
               ::psio::detail::ssz_impl::fixed_size_of<T>();
            return s.subspan(i * esz, esz);
         }
         else
         {
            const std::size_t n = vector_count<T>(s);
            if (i >= n)
               return {};
            std::uint32_t off_i = 0;
            std::memcpy(&off_i, s.data() + i * 4u, 4);
            std::uint32_t off_next;
            if (i + 1 < n)
               std::memcpy(&off_next, s.data() + (i + 1) * 4u, 4);
            else
               off_next = static_cast<std::uint32_t>(s.size());
            return s.subspan(off_i, off_next - off_i);
         }
      }

      // ── optional ───────────────────────────────────────────────────────
      template <typename T>
      static bool optional_has_value(std::span<const char> s) noexcept
      {
         if (s.empty())
            return false;
         return static_cast<unsigned char>(s[0]) != 0;
      }

      template <typename T>
      static std::span<const char>
      optional_payload_span(std::span<const char> s) noexcept
      {
         if (s.empty())
            return {};
         return s.subspan(1);
      }

      // ── variant ────────────────────────────────────────────────────────
      template <typename... Ts>
      static std::size_t variant_index(std::span<const char> s) noexcept
      {
         if (s.empty())
            return 0;
         return static_cast<std::size_t>(
            static_cast<unsigned char>(s[0]));
      }

      template <typename... Ts>
      static std::span<const char>
      variant_payload_span(std::span<const char> s) noexcept
      {
         if (s.empty())
            return {};
         return s.subspan(1);
      }
   };

}  // namespace psio1::view_layout
