#pragma once
//
// psio3/format_tag_base.hpp — CRTP base for format tags.
//
// Every format tag (`psio3::ssz`, `psio3::frac`, ...) inherits from
// `psio3::format_tag_base<Derived>` to get the scoped-sugar form:
//
//     psio3::encode(psio3::frac{}, v, sink)   // generic CPO form
//     psio3::frac::encode(v, sink)            // scoped sugar — same code
//
// The scoped statics are one-line delegates to the matching CPO.
// They exist purely so users with a single known format at a call
// site can avoid the tag-passed-as-value ceremony.

#include <psio3/cpo.hpp>
#include <psio3/error.hpp>

#include <memory>
#include <span>
#include <utility>

namespace psio3 {

   template <typename Derived>
   struct format_tag_base
   {
      // ── encode ─────────────────────────────────────────────────────────
      template <typename T, typename Sink>
      static constexpr void encode(const T& v, Sink& s)
         noexcept(noexcept(::psio3::encode(Derived{}, v, s)))
      {
         ::psio3::encode(Derived{}, v, s);
      }

      template <typename T>
      static constexpr auto encode(const T& v)
         noexcept(noexcept(::psio3::encode(Derived{}, v)))
      {
         return ::psio3::encode(Derived{}, v);
      }

      // ── decode ─────────────────────────────────────────────────────────
      template <typename T>
      static constexpr T decode(std::span<const char> b) noexcept
      {
         return ::psio3::decode<T>(Derived{}, b);
      }

      // ── size_of ────────────────────────────────────────────────────────
      template <typename T>
      static constexpr auto size_of(const T& v) noexcept
      {
         return ::psio3::size_of(Derived{}, v);
      }

      // ── validate ───────────────────────────────────────────────────────
      template <typename T>
      [[nodiscard]] static constexpr codec_status
      validate(std::span<const char> b) noexcept
      {
         return ::psio3::validate<T>(Derived{}, b);
      }

      template <typename T>
      [[nodiscard]] static constexpr codec_status
      validate_strict(std::span<const char> b) noexcept
      {
         return ::psio3::validate_strict<T>(Derived{}, b);
      }

      // ── make_boxed ─────────────────────────────────────────────────────
      template <typename T>
      [[nodiscard]] static constexpr std::unique_ptr<T>
      make_boxed(std::span<const char> b) noexcept
      {
         return ::psio3::make_boxed<T>(Derived{}, b);
      }
   };

}  // namespace psio3
