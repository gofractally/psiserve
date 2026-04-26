#pragma once
// Zero-copy pSSZ views. See `.issues/pssz-format-design.md` for format spec.
//
// Given a pSSZ buffer, `pssz_view<T, F>` exposes the logical structure
// without materializing std:: containers. Every accessor is span arithmetic
// over the underlying bytes.
//
// Format F parameterizes offset/header widths (pssz8 / pssz16 / pssz32).
// Call pssz_validate<T, F>(buf) before viewing untrusted input.
//
// Differences from ssz_view:
//   - F-parameterized offset width (u8 / u16 / u32)
//   - Reflected containers: skip the extensibility header (sizeof F::header_type)
//     before the fixed region when T is not DWNC
//   - std::optional<T> with fixed T has no selector byte — presence derived
//     from span == 0 (None) vs span == sizeof(T) (Some)

#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/check.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/reflect.hpp>
#include <psio1/to_pssz.hpp>
#include <psio1/view.hpp>  // view_proxy, for the unified view<T, Fmt> plumbing

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <type_traits>

namespace psio1
{
   template <typename T, typename F = frac_format_pssz32>
   class pssz_view;

   // ── Primitives ────────────────────────────────────────────────────────────

#define PSIO1_PSSZ_VIEW_PRIMITIVE(T)                                         \
   template <typename F>                                                    \
   class pssz_view<T, F>                                                    \
   {                                                                        \
      const char* data_;                                                    \
                                                                            \
     public:                                                                \
      pssz_view() = default;                                                \
      pssz_view(const char* d, std::uint32_t = sizeof(T)) : data_(d) {}    \
      T get() const noexcept                                                \
      {                                                                     \
         T v;                                                               \
         std::memcpy(&v, data_, sizeof(T));                                 \
         return v;                                                          \
      }                                                                     \
      operator T() const noexcept { return get(); }                         \
   };

   PSIO1_PSSZ_VIEW_PRIMITIVE(bool)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::int8_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::uint8_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::int16_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::uint16_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::int32_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::uint32_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::int64_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(std::uint64_t)
   PSIO1_PSSZ_VIEW_PRIMITIVE(float)
   PSIO1_PSSZ_VIEW_PRIMITIVE(double)
   PSIO1_PSSZ_VIEW_PRIMITIVE(unsigned __int128)
   PSIO1_PSSZ_VIEW_PRIMITIVE(__int128)
#undef PSIO1_PSSZ_VIEW_PRIMITIVE

   template <typename F>
   class pssz_view<uint256, F>
   {
      const char* data_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t = 32) : data_(d) {}
      uint256 get() const noexcept
      {
         uint256 v;
         std::memcpy(&v, data_, 32);
         return v;
      }
      operator uint256() const noexcept { return get(); }
   };

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N, typename F>
   class pssz_view<bitvector<N>, F>
   {
      const char* data_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t = (N + 7) / 8) : data_(d) {}
      bool test(std::size_t i) const noexcept
      {
         return (static_cast<std::uint8_t>(data_[i >> 3]) >> (i & 7u)) & 1u;
      }
      static constexpr std::size_t size() noexcept { return N; }
   };

   // ── std::string ───────────────────────────────────────────────────────────

   template <typename F>
   class pssz_view<std::string, F>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}
      std::string_view view() const noexcept { return {data_, span_}; }
      operator std::string_view() const noexcept { return view(); }
      std::size_t size() const noexcept { return span_; }
   };

   template <std::size_t N, typename F>
   class pssz_view<bounded_string<N>, F>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}
      std::string_view view() const noexcept { return {data_, span_}; }
      operator std::string_view() const noexcept { return view(); }
      std::size_t size() const noexcept { return span_; }
   };

   // ── std::array<T, N> ──────────────────────────────────────────────────────

   template <typename T, std::size_t N, typename F>
   class pssz_view<std::array<T, N>, F>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}

      static constexpr std::size_t size() noexcept { return N; }

      pssz_view<T, F> operator[](std::size_t i) const
      {
         if constexpr (pssz_is_fixed_size_v<T>)
         {
            constexpr std::uint32_t elem = pssz_fixed_size<T>::value;
            return pssz_view<T, F>(data_ + i * elem, elem);
         }
         else
         {
            using off_t                   = typename F::offset_type;
            constexpr std::size_t ob       = F::offset_bytes;
            off_t off_i = 0, off_next = 0;
            std::memcpy(&off_i, data_ + i * ob, ob);
            if (i + 1 < N)
               std::memcpy(&off_next, data_ + (i + 1) * ob, ob);
            else
               off_next = static_cast<off_t>(span_);
            return pssz_view<T, F>(data_ + off_i, off_next - off_i);
         }
      }
   };

   // ── std::vector<T> ────────────────────────────────────────────────────────

   template <typename T, typename F>
   class pssz_view<std::vector<T>, F>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}

      std::uint32_t size() const noexcept
      {
         if (span_ == 0) return 0;
         if constexpr (pssz_is_fixed_size_v<T>)
            return span_ / pssz_fixed_size<T>::value;
         else
         {
            using off_t                   = typename F::offset_type;
            constexpr std::size_t ob       = F::offset_bytes;
            off_t first = 0;
            std::memcpy(&first, data_, ob);
            return first / static_cast<std::uint32_t>(ob);
         }
      }

      bool empty() const noexcept { return size() == 0; }

      pssz_view<T, F> operator[](std::size_t i) const
      {
         if constexpr (pssz_is_fixed_size_v<T>)
         {
            constexpr std::uint32_t elem = pssz_fixed_size<T>::value;
            return pssz_view<T, F>(data_ + i * elem, elem);
         }
         else
         {
            using off_t                   = typename F::offset_type;
            constexpr std::size_t ob       = F::offset_bytes;
            std::uint32_t n    = size();
            off_t off_i = 0, off_next = 0;
            std::memcpy(&off_i, data_ + i * ob, ob);
            if (i + 1 < n)
               std::memcpy(&off_next, data_ + (i + 1) * ob, ob);
            else
               off_next = static_cast<off_t>(span_);
            return pssz_view<T, F>(data_ + off_i, off_next - off_i);
         }
      }
   };

   template <typename T, std::size_t N, typename F>
   class pssz_view<bounded_list<T, N>, F>
   {
      pssz_view<std::vector<T>, F> inner_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t span) : inner_(d, span) {}
      std::uint32_t   size() const noexcept { return inner_.size(); }
      bool            empty() const noexcept { return inner_.empty(); }
      pssz_view<T, F> operator[](std::size_t i) const { return inner_[i]; }
   };

   // ── std::optional<T> ──────────────────────────────────────────────────────
   // If min_encoded_size<T> == 0: 1-byte selector + optional payload.
   // Else: None == 0 bytes, Some == encoded T.

   template <typename T, typename F>
   class pssz_view<std::optional<T>, F>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      pssz_view() = default;
      pssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}

      bool has_value() const noexcept
      {
         if constexpr (pssz_optional_needs_selector<T>)
         {
            if (span_ == 0) return false;
            return static_cast<std::uint8_t>(data_[0]) == 1;
         }
         else
         {
            return span_ > 0;
         }
      }

      pssz_view<T, F> operator*() const
      {
         if constexpr (pssz_optional_needs_selector<T>)
            return pssz_view<T, F>(data_ + 1, span_ - 1);
         else
            return pssz_view<T, F>(data_, span_);
      }
   };

   // ── Reflected container ───────────────────────────────────────────────────

   namespace pssz_view_detail
   {
      // Byte offset of the I-th field's slot within the FIXED REGION (not
      // counting the extensibility header).
      template <typename F, typename T, std::size_t I>
      consteval std::size_t field_slot_offset()
      {
         using tuple_t = struct_tuple_t<T>;
         return []<std::size_t... Js>(std::index_sequence<Js...>)
         {
            auto sz = []<std::size_t J>(std::integral_constant<std::size_t, J>)
            {
               using FT = std::tuple_element_t<J, tuple_t>;
               if constexpr (pssz_is_fixed_size_v<FT>)
                  return pssz_fixed_size<FT>::value;
               else
                  return F::offset_bytes;
            };
            return (sz(std::integral_constant<std::size_t, Js>{}) + ... + std::size_t{0});
         }
         (std::make_index_sequence<I>{});
      }

      template <typename T, std::size_t I>
      consteval std::size_t next_var_index_after()
      {
         using tuple_t        = struct_tuple_t<T>;
         constexpr std::size_t N = std::tuple_size_v<tuple_t>;
         std::size_t result = N;
         [&]<std::size_t... Js>(std::index_sequence<Js...>)
         {
            (
                [&]<std::size_t J>(std::integral_constant<std::size_t, J>)
                {
                   if constexpr (J > I)
                   {
                      using FT = std::tuple_element_t<J, tuple_t>;
                      if constexpr (!pssz_is_fixed_size_v<FT>)
                      {
                         if (result == N) result = J;
                      }
                   }
                }(std::integral_constant<std::size_t, Js>{}),
                ...);
         }(std::make_index_sequence<N>{});
         return result;
      }
   }  // namespace pssz_view_detail

   // ── pssz_fmt<F>: adapter plugging pSSZ into the unified view<T, Fmt> ─
   //
   // Parallel to ssz_fmt: provides `ptr_t`, `root<T>()`, `field<T, N>()`
   // so `view<T, pssz_fmt<F>>` inherits the PSIO1_REFLECT-generated
   // named-accessor proxy. `pssz_view<T, F>` is the thin wrapper below.

   template <typename F>
   struct pssz_fmt
   {
      struct ptr_t
      {
         const char*   data;  // start of whole T (including extensibility hdr)
         std::uint32_t span;  // whole T span
         bool operator==(const ptr_t&) const = default;
         operator bool() const { return data != nullptr; }
      };

      template <typename T>
      static ptr_t root(const void* buf)
      {
         return ptr_t{static_cast<const char*>(buf), 0};
      }

      template <typename T>
      static ptr_t root_with_span(const char* buf, std::uint32_t span)
      {
         return ptr_t{buf, span};
      }

      template <typename T, std::size_t N>
      static auto field(ptr_t p)
      {
         using tuple_t = struct_tuple_t<T>;
         static_assert(N < std::tuple_size_v<tuple_t>, "field index out of range");
         using FT = std::tuple_element_t<N, tuple_t>;
         constexpr std::size_t slot =
             pssz_view_detail::field_slot_offset<F, T, N>();

         // Skip the extensibility header for non-DWNC structs.
         const char* fs =
             pssz_detail::is_dwnc<T>() ? p.data : p.data + F::header_bytes;
         std::uint32_t fs_span =
             pssz_detail::is_dwnc<T>()
                 ? p.span
                 : p.span - static_cast<std::uint32_t>(F::header_bytes);

         if constexpr (pssz_is_fixed_size_v<FT>)
         {
            return pssz_view<FT, F>(fs + slot, pssz_fixed_size<FT>::value);
         }
         else
         {
            using off_t                   = typename F::offset_type;
            constexpr std::size_t ob       = F::offset_bytes;
            off_t off = 0;
            std::memcpy(&off, fs + slot, ob);
            constexpr std::size_t next =
                pssz_view_detail::next_var_index_after<T, N>();
            std::uint32_t stop = 0;
            if constexpr (next < std::tuple_size_v<tuple_t>)
            {
               constexpr std::size_t next_slot =
                   pssz_view_detail::field_slot_offset<F, T, next>();
               off_t nxt = 0;
               std::memcpy(&nxt, fs + next_slot, ob);
               stop = static_cast<std::uint32_t>(nxt);
            }
            else
            {
               stop = fs_span;
            }
            return pssz_view<FT, F>(fs + off,
                                     static_cast<std::uint32_t>(stop - off));
         }
      }
   };

   template <typename T, typename F>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   class pssz_view<T, F>
       : public reflect<T>::template proxy<view_proxy<T, pssz_fmt<F>>>
   {
      using base = typename reflect<T>::template proxy<view_proxy<T, pssz_fmt<F>>>;

     public:
      pssz_view() : base(typename pssz_fmt<F>::ptr_t{}) {}
      pssz_view(const char* d, std::uint32_t span)
          : base(typename pssz_fmt<F>::ptr_t{d, span})
      {
      }

      const char* raw_data() const noexcept { return this->psio_get_proxy().ptr().data; }
      std::uint32_t raw_span() const noexcept { return this->psio_get_proxy().ptr().span; }
   };

   // ── Entry point ───────────────────────────────────────────────────────────

   template <typename T, typename F = frac_format_pssz32>
   pssz_view<T, F> pssz_view_of(std::span<const char> buf)
   {
      return pssz_view<T, F>(buf.data(), static_cast<std::uint32_t>(buf.size()));
   }

   template <typename T, typename F = frac_format_pssz32>
   pssz_view<T, F> pssz_view_of(const std::vector<char>& buf)
   {
      return pssz_view_of<T, F>(std::span<const char>(buf.data(), buf.size()));
   }

}  // namespace psio1
