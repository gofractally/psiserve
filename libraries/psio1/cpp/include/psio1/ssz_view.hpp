#pragma once
// Zero-copy SSZ views.
//
// Given a validated SSZ buffer (`ssz_validate<T>` passes), `ssz_view<T>`
// exposes the logical structure without materializing any std:: containers.
// Every accessor is a span-arithmetic operation over the underlying bytes.
//
// Design:
//   - ssz_view<T> stores (const char* data, std::uint32_t span) where span is
//     the byte count belonging to *this* value (derived from the parent's
//     offset table or the outer buffer size).
//   - Primitive views return by value via a `get()` method and implicit
//     conversion operator.
//   - Array/List views offer size() + operator[] returning sub-views.
//   - Container views offer field<I>() returning typed sub-views.
//   - Bitlist views offer size() (decoded from delimiter) + test(i).
//
// Call ssz_validate<T>(buf) before constructing views if the source is
// untrusted. Views themselves do minimal bounds checking — they trust that
// offsets passed down from the parent are internally consistent.

#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/check.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/reflect.hpp>
#include <psio1/to_ssz.hpp>   // ssz_is_fixed_size_v, ssz_fixed_size
#include <psio1/view.hpp>     // view_proxy, for unified view<T, Fmt> plumbing

#include <array>
#include <bit>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <type_traits>

namespace psio1
{
   template <typename T>
   class ssz_view;

   // ── Primitives: get-by-value + implicit conversion ────────────────────────

#define PSIO1_SSZ_VIEW_PRIMITIVE(T)                                         \
   template <>                                                             \
   class ssz_view<T>                                                       \
   {                                                                       \
      const char* data_;                                                   \
                                                                           \
     public:                                                               \
      ssz_view() = default;                                                \
      ssz_view(const char* d, std::uint32_t = sizeof(T)) : data_(d) {}    \
      T get() const noexcept                                               \
      {                                                                    \
         T v;                                                              \
         std::memcpy(&v, data_, sizeof(T));                                \
         return v;                                                         \
      }                                                                    \
      operator T() const noexcept { return get(); }                        \
   };

   PSIO1_SSZ_VIEW_PRIMITIVE(bool)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::int8_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::uint8_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::int16_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::uint16_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::int32_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::uint32_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::int64_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(std::uint64_t)
   PSIO1_SSZ_VIEW_PRIMITIVE(unsigned __int128)
   PSIO1_SSZ_VIEW_PRIMITIVE(__int128)
#undef PSIO1_SSZ_VIEW_PRIMITIVE

   template <>
   class ssz_view<uint256>
   {
      const char* data_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t = 32) : data_(d) {}
      uint256 get() const noexcept
      {
         uint256 v;
         std::memcpy(&v, data_, 32);
         return v;
      }
      operator uint256() const noexcept { return get(); }
      const char* raw() const noexcept { return data_; }  // for hashing
   };

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N>
   class ssz_view<bitvector<N>>
   {
      const char* data_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t = (N + 7) / 8) : data_(d) {}

      bool test(std::size_t i) const noexcept
      {
         return (static_cast<std::uint8_t>(data_[i >> 3]) >> (i & 7u)) & 1u;
      }
      static constexpr std::size_t size() noexcept { return N; }

      bitvector<N> get() const noexcept
      {
         bitvector<N> v;
         std::memcpy(v.data(), data_, (N + 7) / 8);
         return v;
      }
      const char* raw() const noexcept { return data_; }
   };

   template <std::size_t N>
   class ssz_view<std::bitset<N>>
   {
      const char* data_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t = (N + 7) / 8) : data_(d) {}

      bool test(std::size_t i) const noexcept
      {
         return (static_cast<std::uint8_t>(data_[i >> 3]) >> (i & 7u)) & 1u;
      }
      static constexpr std::size_t size() noexcept { return N; }

      std::bitset<N> get() const noexcept
      {
         std::bitset<N> bs;
         unpack_bitset_bytes(reinterpret_cast<const std::uint8_t*>(data_), bs);
         return bs;
      }
   };

   template <std::size_t MaxN>
   class ssz_view<bitlist<MaxN>>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}

      // Length: scan for highest set bit (delimiter) in the last non-zero byte.
      std::size_t size() const noexcept
      {
         std::int32_t last = static_cast<std::int32_t>(span_) - 1;
         while (last >= 0 && static_cast<std::uint8_t>(data_[last]) == 0)
            --last;
         if (last < 0)
            return 0;  // shouldn't happen for valid bitlist
         std::uint8_t last_byte = static_cast<std::uint8_t>(data_[last]);
         int hi = 31 - __builtin_clz(static_cast<unsigned int>(last_byte));
         return static_cast<std::size_t>(last) * 8 + static_cast<std::size_t>(hi);
      }

      bool test(std::size_t i) const noexcept
      {
         return (static_cast<std::uint8_t>(data_[i >> 3]) >> (i & 7u)) & 1u;
      }
   };

   // ── std::array = Vector[T, N] ─────────────────────────────────────────────

   template <typename T, std::size_t N>
   class ssz_view<std::array<T, N>>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}

      static constexpr std::size_t size() noexcept { return N; }

      ssz_view<T> operator[](std::size_t i) const
      {
         if constexpr (ssz_is_fixed_size_v<T>)
         {
            constexpr std::uint32_t elem = ssz_fixed_size<T>::value;
            return ssz_view<T>(data_ + i * elem, elem);
         }
         else
         {
            // Variable elements: offset table of N u32s + payloads.
            std::uint32_t off_i, off_next;
            std::memcpy(&off_i, data_ + i * 4, 4);
            if (i + 1 < N)
               std::memcpy(&off_next, data_ + (i + 1) * 4, 4);
            else
               off_next = span_;
            return ssz_view<T>(data_ + off_i, off_next - off_i);
         }
      }
   };

   // ── std::vector<T> = List[T, *] ───────────────────────────────────────────

   template <typename T>
   class ssz_view<std::vector<T>>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}

      std::uint32_t size() const noexcept
      {
         if (span_ == 0)
            return 0;
         if constexpr (ssz_is_fixed_size_v<T>)
         {
            return span_ / ssz_fixed_size<T>::value;
         }
         else
         {
            std::uint32_t first = 0;
            std::memcpy(&first, data_, 4);
            return first / 4;
         }
      }

      bool empty() const noexcept { return size() == 0; }

      ssz_view<T> operator[](std::size_t i) const
      {
         if constexpr (ssz_is_fixed_size_v<T>)
         {
            constexpr std::uint32_t elem = ssz_fixed_size<T>::value;
            return ssz_view<T>(data_ + i * elem, elem);
         }
         else
         {
            std::uint32_t n = size();
            std::uint32_t off_i, off_next;
            std::memcpy(&off_i, data_ + i * 4, 4);
            if (i + 1 < n)
               std::memcpy(&off_next, data_ + (i + 1) * 4, 4);
            else
               off_next = span_;
            return ssz_view<T>(data_ + off_i, off_next - off_i);
         }
      }
   };

   // bounded_list forwards
   template <typename T, std::size_t N>
   class ssz_view<bounded_list<T, N>>
   {
      ssz_view<std::vector<T>> inner_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t span) : inner_(d, span) {}
      std::uint32_t size() const noexcept { return inner_.size(); }
      bool          empty() const noexcept { return inner_.empty(); }
      ssz_view<T>   operator[](std::size_t i) const { return inner_[i]; }
   };

   // ── Strings / bytes: span-derived string_view ─────────────────────────────

   template <>
   class ssz_view<std::string>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}
      std::string_view view() const noexcept { return {data_, span_}; }
      operator std::string_view() const noexcept { return view(); }
      std::size_t size() const noexcept { return span_; }
   };

   template <std::size_t N>
   class ssz_view<bounded_string<N>>
   {
      const char*   data_;
      std::uint32_t span_;

     public:
      ssz_view() = default;
      ssz_view(const char* d, std::uint32_t span) : data_(d), span_(span) {}
      std::string_view view() const noexcept { return {data_, span_}; }
      operator std::string_view() const noexcept { return view(); }
      std::size_t size() const noexcept { return span_; }
   };

   // ── Reflected Container: field<I>() accessor ──────────────────────────────

   namespace ssz_view_detail
   {
      // Offset (within fixed region) of the I-th field's slot.
      template <typename T, std::size_t I>
      consteval std::size_t field_slot_offset()
      {
         using tuple_t = struct_tuple_t<T>;
         return []<std::size_t... Js>(std::index_sequence<Js...>)
         {
            auto sz = []<std::size_t J>(std::integral_constant<std::size_t, J>)
            {
               using FT = std::tuple_element_t<J, tuple_t>;
               if constexpr (ssz_is_fixed_size_v<FT>)
                  return ssz_fixed_size<FT>::value;
               else
                  return std::size_t{4};
            };
            return (sz(std::integral_constant<std::size_t, Js>{}) + ... + 0);
         }
         (std::make_index_sequence<I>{});
      }

      // Index of the *next* variable field after I (or sizeof tuple if none).
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
                      if constexpr (!ssz_is_fixed_size_v<FT>)
                      {
                         if (result == N)
                            result = J;
                      }
                   }
                }(std::integral_constant<std::size_t, Js>{}),
                ...);
         }(std::make_index_sequence<N>{});
         return result;
      }
   }

   // ── ssz_fmt: adapter plugging SSZ into the unified view<T, Fmt> proxy ─
   //
   // `reflect<T>::proxy<view_proxy<T, Fmt>>` generates one method per
   // PSIO1_REFLECT field name (e.g., `v.pubkey()`, `v.effective_balance()`)
   // that calls `Fmt::field<T, N>()`. Providing ssz_fmt here means
   // `view<T, ssz_fmt>` gets the same named-accessor API as frac_view —
   // which was the whole point of PSIO1_REFLECT's proxy machinery.

   struct ssz_fmt
   {
      // Pointer type needs to carry (data, span) for variable-type views.
      struct ptr_t
      {
         const char*   data;
         std::uint32_t span;
         bool operator==(const ptr_t&) const = default;
         operator bool() const { return data != nullptr; }
      };

      template <typename T>
      static ptr_t root(const void* buf)
      {
         // When called via view<T>::from_buffer(data), the caller knows
         // the total buffer size via external means (span may have been
         // encoded as the vector length). Default to "unknown span":
         // accessors that need span recover it from the buffer context.
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

         constexpr std::size_t slot = ssz_view_detail::field_slot_offset<T, N>();

         if constexpr (ssz_is_fixed_size_v<FT>)
         {
            return ssz_view<FT>(p.data + slot, ssz_fixed_size<FT>::value);
         }
         else
         {
            std::uint32_t off = 0;
            std::memcpy(&off, p.data + slot, 4);
            constexpr std::size_t next = ssz_view_detail::next_var_index_after<T, N>();
            std::uint32_t stop;
            if constexpr (next < std::tuple_size_v<tuple_t>)
            {
               constexpr std::size_t next_slot = ssz_view_detail::field_slot_offset<T, next>();
               std::memcpy(&stop, p.data + next_slot, 4);
            }
            else
            {
               stop = p.span;
            }
            return ssz_view<FT>(p.data + off, stop - off);
         }
      }
   };

   // Reflected-struct view: inherit from proxy so PSIO1_REFLECT field names
   // become methods (v.name(), v.id(), etc.). Replaces the old
   // `.field<I>()` pattern.
   template <typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   class ssz_view<T>
       : public reflect<T>::template proxy<view_proxy<T, ssz_fmt>>
   {
      using base = typename reflect<T>::template proxy<view_proxy<T, ssz_fmt>>;

     public:
      ssz_view() : base(ssz_fmt::ptr_t{}) {}
      ssz_view(const char* d, std::uint32_t span)
          : base(ssz_fmt::ptr_t{d, span})
      {
      }

      const char*   raw_data() const noexcept { return this->psio_get_proxy().ptr().data; }
      std::uint32_t raw_span() const noexcept { return this->psio_get_proxy().ptr().span; }
   };

   // ── Public entry point ────────────────────────────────────────────────────

   template <typename T>
   ssz_view<T> ssz_view_of(std::span<const char> buf)
   {
      check(buf.size() <= std::numeric_limits<std::uint32_t>::max(),
            "ssz buffer too large");
      return ssz_view<T>(buf.data(), static_cast<std::uint32_t>(buf.size()));
   }

   template <typename T>
   ssz_view<T> ssz_view_of(const std::vector<char>& buf)
   {
      return ssz_view_of<T>(std::span<const char>(buf.data(), buf.size()));
   }

}  // namespace psio1
