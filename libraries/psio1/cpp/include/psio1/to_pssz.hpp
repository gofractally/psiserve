#pragma once
// pSSZ (PsiSSZ) encoder — hybrid of SSZ's implicit sizing and fracpack's
// extensibility. See `.issues/pssz-format-design.md` for the full
// design rationale.
//
// Wire format (per container):
//   [header: fixed_size_bytes]  ← sized by Format::header_bytes; omitted
//                                  when the type is DWNC and skip_header
//                                  applies.
//   [fixed region: inline fixed fields + offset slots for variable fields]
//   [heap region: variable payloads in order]
//
// Key differences from fracpack:
//   - Offsets are **container-relative** (from start of the fixed region),
//     not pointer-relative.
//   - Variable payloads have NO length prefix. Size of field i is derived
//     from the adjacency of offsets[i] and offsets[i+1] (implicit sizing,
//     SSZ-style).
//   - std::optional<T> only carries a 1-byte selector when
//     min_encoded_size<T> == 0 (string/vector/etc). Fixed-size T uses
//     0 or sizeof(T) bytes; the size itself disambiguates None/Some.
//
// Offset/header widths are parameterized by the Format tag
// (frac_format_pssz8 / _pssz16 / _pssz32). Use `auto_pssz_format_t<T>` to
// pick the narrowest format that can encode every value of T.

#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/detail/layout.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/reflect.hpp>
#include <psio1/stream.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio1
{
   // ── Format tags ───────────────────────────────────────────────────────────

   struct frac_format_pssz8
   {
      using offset_type            = std::uint8_t;
      using header_type            = std::uint8_t;
      static constexpr std::size_t offset_bytes = 1;
      static constexpr std::size_t header_bytes = 1;
      static constexpr std::size_t max_total    = 0xff;
   };
   struct frac_format_pssz16
   {
      using offset_type            = std::uint16_t;
      using header_type            = std::uint16_t;
      static constexpr std::size_t offset_bytes = 2;
      static constexpr std::size_t header_bytes = 2;
      static constexpr std::size_t max_total    = 0xffff;
   };
   struct frac_format_pssz32
   {
      using offset_type            = std::uint32_t;
      using header_type            = std::uint32_t;
      static constexpr std::size_t offset_bytes = 4;
      static constexpr std::size_t header_bytes = 4;
      static constexpr std::size_t max_total    = 0xffffffffull;
   };

   template <typename F>
   inline constexpr bool is_pssz_format_v =
       std::is_same_v<F, frac_format_pssz8> ||
       std::is_same_v<F, frac_format_pssz16> ||
       std::is_same_v<F, frac_format_pssz32>;

   // ── Fixed/variable classification ─────────────────────────────────────────

   template <typename T>
   struct pssz_is_fixed_size : std::false_type
   {
   };

   template <>
   struct pssz_is_fixed_size<bool> : std::true_type
   {
   };

   // All numeric primitives are fixed-size. See SszNumeric concept in to_ssz.hpp.
   template <typename T>
   concept PsszNumeric =
       (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) ||
       std::is_same_v<T, __int128> ||
       std::is_same_v<T, unsigned __int128>;

   template <PsszNumeric T>
   struct pssz_is_fixed_size<T> : std::true_type
   {
   };
   template <>
   struct pssz_is_fixed_size<uint256> : std::true_type
   {
   };

   template <typename T, std::size_t N>
   struct pssz_is_fixed_size<std::array<T, N>>
       : std::bool_constant<pssz_is_fixed_size<T>::value>
   {
   };

   template <std::size_t N>
   struct pssz_is_fixed_size<bitvector<N>> : std::true_type
   {
   };
   template <std::size_t N>
   struct pssz_is_fixed_size<std::bitset<N>> : std::true_type
   {
   };

   namespace pssz_detail
   {
      template <typename T>
      consteval bool all_fields_fixed()
      {
         if constexpr (!Reflected<T>)
            return false;
         else
         {
            using tuple_t        = struct_tuple_t<T>;
            constexpr std::size_t N = std::tuple_size_v<tuple_t>;
            return []<std::size_t... Is>(std::index_sequence<Is...>)
            {
               return (pssz_is_fixed_size<std::tuple_element_t<Is, tuple_t>>::value && ...);
            }
            (std::make_index_sequence<N>{});
         }
      }

      // A reflected struct is treated as "DWNC for pSSZ" iff the user
      // marked definitionWillNotChange() in PSIO1_REFLECT. DWNC pSSZ structs
      // skip the extensibility header entirely.
      template <typename T>
      consteval bool is_dwnc()
      {
         if constexpr (!Reflected<T>)
            return false;
         else
            return reflect<T>::definitionWillNotChange;
      }
   }  // namespace pssz_detail

   template <typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   struct pssz_is_fixed_size<T>
       : std::bool_constant<pssz_detail::all_fields_fixed<T>() &&
                             pssz_detail::is_dwnc<T>()>
   {
      // Only DWNC all-fixed structs are "fixed" in pSSZ terms, since a
      // non-DWNC struct carries an extensibility header whose length
      // depends on Format — and pssz_is_fixed_size is format-agnostic
      // today. For non-DWNC structs we conservatively treat them as
      // variable even when all fields are fixed; the encoder still emits
      // compact bytes for them.
   };

   template <typename T>
   inline constexpr bool pssz_is_fixed_size_v = pssz_is_fixed_size<T>::value;

   // ── pssz_fixed_size ───────────────────────────────────────────────────────

   template <typename T>
   struct pssz_fixed_size;  // undefined for variable

   template <>
   struct pssz_fixed_size<bool> : std::integral_constant<std::size_t, 1>
   {
   };
   template <PsszNumeric T>
   struct pssz_fixed_size<T> : std::integral_constant<std::size_t, sizeof(T)>
   {
   };
   template <>
   struct pssz_fixed_size<uint256> : std::integral_constant<std::size_t, 32>
   {
   };
   template <typename T, std::size_t N>
      requires(pssz_is_fixed_size_v<T>)
   struct pssz_fixed_size<std::array<T, N>>
       : std::integral_constant<std::size_t, N * pssz_fixed_size<T>::value>
   {
   };
   template <std::size_t N>
   struct pssz_fixed_size<bitvector<N>>
       : std::integral_constant<std::size_t, (N + 7) / 8>
   {
   };
   template <std::size_t N>
   struct pssz_fixed_size<std::bitset<N>>
       : std::integral_constant<std::size_t, (N + 7) / 8>
   {
   };
   template <typename T>
      requires(pssz_is_fixed_size_v<T> && Reflected<T>)
   struct pssz_fixed_size<T>
       : std::integral_constant<std::size_t, sizeof(T)>
   {
   };

   // ── min_encoded_size<T>: drives optional-selector decision ────────────────
   //
   // Value is the tightest lower bound on the wire size of T under pSSZ.
   // If min > 0, an Optional<T> needs no selector byte: None == 0 bytes,
   // Some == >=min bytes, and the span adjacency disambiguates.
   // If min == 0, we need a 1-byte Union selector (0x00 = None,
   // 0x01+payload = Some).

   template <typename T>
   consteval std::size_t min_encoded_size();

   namespace pssz_detail
   {
      template <typename T>
      consteval std::size_t min_encoded_size_impl()
      {
         if constexpr (pssz_is_fixed_size_v<T>)
            return pssz_fixed_size<T>::value;
         else if constexpr (is_bitlist_v<T>)
            return 1;  // SSZ bitlist always has delimiter byte
         else if constexpr (std::is_same_v<T, std::string> ||
                             std::is_same_v<T, std::string_view> ||
                             is_bounded_string_v<T>)
            return 0;
         else if constexpr (requires(const T& obj) { obj.size(); obj.data(); })
            return 0;  // std::vector<T>, bounded_list<T,N>
         else if constexpr (requires(const T& obj) { obj.has_value(); *obj; })
            return 0;  // std::optional<T>
         else if constexpr (Reflected<T> && !is_bitvector_v<T> &&
                             !is_bitlist_v<T> && !is_std_bitset_v<T>)
         {
            // Reflected extensible struct: header + fixed region.
            // Conservative: use the smallest header width (pssz8 = 1 byte).
            // This is a lower bound — actual encoding may use wider.
            using tuple_t = struct_tuple_t<T>;
            constexpr std::size_t N = std::tuple_size_v<tuple_t>;
            if constexpr (pssz_detail::is_dwnc<T>())
               return sizeof(T);  // DWNC: no header, memcpy sizeof(T)
            else
            {
               constexpr std::size_t header = 1;  // pssz8 header is 1 byte
               // Each field contributes either its fixed size (if fixed)
               // or 1 byte for the offset slot (pssz8 minimum).
               constexpr std::size_t fixed_region = []<std::size_t... Is>(
                   std::index_sequence<Is...>) {
                  return ((pssz_is_fixed_size_v<std::tuple_element_t<Is, tuple_t>>
                              ? pssz_fixed_size<std::tuple_element_t<Is, tuple_t>>::value
                              : 1 /* pssz8 offset slot */) +
                          ... + 0);
               }(std::make_index_sequence<N>{});
               return header + fixed_region;
            }
         }
         else
            return 0;
      }
   }

   template <typename T>
   consteval std::size_t min_encoded_size()
   {
      return pssz_detail::min_encoded_size_impl<T>();
   }

   template <typename T>
   inline constexpr bool pssz_optional_needs_selector =
       min_encoded_size<T>() == 0;

   // ── max_encoded_size<T>: returns std::optional (nullopt = unbounded) ──────

   template <typename T>
   consteval std::optional<std::size_t> max_encoded_size();

   namespace pssz_detail
   {
      template <typename T>
      consteval std::optional<std::size_t> max_encoded_size_impl();

      template <typename FT>
      consteval std::optional<std::size_t> field_max_contribution()
      {
         if constexpr (pssz_is_fixed_size_v<FT>)
            return std::optional<std::size_t>{pssz_fixed_size<FT>::value};
         else
         {
            auto fm = max_encoded_size_impl<FT>();
            if (!fm) return std::nullopt;
            return std::optional<std::size_t>{*fm + 4};  // +offset slot
         }
      }

      template <typename Tuple, std::size_t... Is>
      consteval std::optional<std::size_t> max_encoded_size_fields(
          std::index_sequence<Is...>, std::size_t header)
      {
         std::size_t total = header;
         bool        any_unbounded = false;
         (([&] {
             constexpr auto fm = field_max_contribution<
                 std::tuple_element_t<Is, Tuple>>();
             if constexpr (!fm.has_value())
                any_unbounded = true;
             else
                total += *fm;
          }()), ...);
         if (any_unbounded) return std::nullopt;
         return std::optional<std::size_t>{total};
      }

      template <typename T>
      consteval std::optional<std::size_t> max_encoded_size_impl()
      {
         if constexpr (pssz_is_fixed_size_v<T>)
            return std::optional<std::size_t>{pssz_fixed_size<T>::value};
         else if constexpr (std::is_same_v<T, std::string> ||
                             std::is_same_v<T, std::string_view>)
            return std::nullopt;  // unbounded
         else if constexpr (is_bounded_string_v<T>)
            return std::optional<std::size_t>{T::max_size_v};
         else if constexpr (is_bounded_list_v<T>)
         {
            using elem_t = typename T::value_type;
            constexpr std::size_t max_n = T::max_size_v;
            constexpr auto elem_max = max_encoded_size_impl<elem_t>();
            if (!elem_max) return std::nullopt;
            // Each element may itself be variable → prepend 4-byte offset.
            // For fixed elements: max_n * elem_max. For variable elements:
            // max_n * (offset + elem_max). Conservative: use pssz32 offset.
            if constexpr (pssz_is_fixed_size_v<elem_t>)
               return std::optional<std::size_t>{max_n * *elem_max};
            else
               return std::optional<std::size_t>{max_n * (*elem_max + 4)};
         }
         else if constexpr (requires(const T& obj) { obj.has_value(); *obj; })
         {
            using inner_t = std::remove_cvref_t<decltype(*std::declval<T&>())>;
            constexpr auto inner_max = max_encoded_size_impl<inner_t>();
            if (!inner_max) return std::nullopt;
            constexpr std::size_t selector = min_encoded_size<inner_t>() == 0 ? 1 : 0;
            return std::optional<std::size_t>{selector + *inner_max};
         }
         else if constexpr (requires { typename T::value_type; }
                             && requires(const T& obj) { obj.size(); obj.data(); })
            return std::nullopt;  // std::vector — unbounded
         else if constexpr (Reflected<T> && !is_bitvector_v<T> &&
                             !is_bitlist_v<T> && !is_std_bitset_v<T>)
         {
            using tuple_t = struct_tuple_t<T>;
            constexpr std::size_t N = std::tuple_size_v<tuple_t>;
            constexpr std::size_t header = pssz_detail::is_dwnc<T>() ? 0 : 4;
            return max_encoded_size_fields<tuple_t>(std::make_index_sequence<N>{},
                                                      header);
         }
         else
            return std::nullopt;
      }
   }

   template <typename T>
   consteval std::optional<std::size_t> max_encoded_size()
   {
      return pssz_detail::max_encoded_size_impl<T>();
   }

   // ── auto_pssz_format_t<T>: narrowest format that fits ─────────────────────

   template <typename T>
   using auto_pssz_format_t = std::conditional_t<
       max_encoded_size<T>().has_value() && *max_encoded_size<T>() <= 0xff,
       frac_format_pssz8,
       std::conditional_t<max_encoded_size<T>().has_value() &&
                              *max_encoded_size<T>() <= 0xffff,
                          frac_format_pssz16,
                          frac_format_pssz32>>;

   // ── Forward decls ─────────────────────────────────────────────────────────

   template <typename F, typename T, typename S>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void to_pssz(const T& obj, S& stream);

   template <typename F, typename T>
   std::uint32_t pssz_size(const T& obj);

   // ── Primitives ────────────────────────────────────────────────────────────

   template <typename F, typename S>
   void to_pssz(bool val, S& stream)
   {
      std::uint8_t b = val ? 1 : 0;
      stream.write(reinterpret_cast<const char*>(&b), 1);
   }

   template <typename F, PsszNumeric T, typename S>
   void to_pssz(T val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename F, typename S>
   void to_pssz(const uint256& val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <typename F, std::size_t N, typename S>
   void to_pssz(const bitvector<N>& v, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(v.data()),
                   bitvector<N>::byte_count);
   }

   template <typename F, std::size_t N, typename S>
   void to_pssz(const std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8] = {};
      for (std::size_t i = 0; i < N; ++i)
         if (bs.test(i))
            buf[i >> 3] |= std::uint8_t{1} << (i & 7);
      stream.write(reinterpret_cast<const char*>(buf), (N + 7) / 8);
   }

   template <typename F, std::size_t MaxN, typename S>
   void to_pssz(const bitlist<MaxN>& v, S& stream)
   {
      std::size_t  bit_count   = v.size();
      std::size_t  total_bytes = (bit_count >> 3) + 1;
      std::vector<std::uint8_t> buf(total_bytes, 0);
      auto src = v.bytes();
      if (!src.empty())
         std::memcpy(buf.data(), src.data(), src.size());
      buf[bit_count >> 3] |= static_cast<std::uint8_t>(1u << (bit_count & 7u));
      stream.write(reinterpret_cast<const char*>(buf.data()), total_bytes);
   }

   // ── std::array<T, N> = Vector[T, N] ───────────────────────────────────────

   template <typename F, typename T, std::size_t N, typename S>
   void to_pssz(const std::array<T, N>& arr, S& stream)
   {
      if constexpr (pssz_is_fixed_size_v<T> && has_bitwise_serialization<T>())
      {
         if constexpr (N > 0)
            stream.write(reinterpret_cast<const char*>(arr.data()),
                         N * sizeof(T));
      }
      else if constexpr (pssz_is_fixed_size_v<T>)
      {
         for (auto& x : arr)
            to_pssz<F>(x, stream);
      }
      else
      {
         // Variable elements: single-pass with backpatching.
         // Reserve the offset table (N × ob bytes), then emit each payload
         // and memcpy its container-relative offset into the table position
         // we already advanced past.
         using off_t = typename F::offset_type;
         constexpr std::size_t ob = F::offset_bytes;
         char* slot_start = stream.pos;
         stream.pos += N * ob;
         for (std::size_t i = 0; i < N; ++i)
         {
            off_t rel = static_cast<off_t>(stream.pos - slot_start);
            std::memcpy(slot_start + i * ob, &rel, ob);
            to_pssz<F>(arr[i], stream);
         }
      }
   }

   // ── std::vector<T> / bounded_list<T, N> — no length prefix ────────────────

   template <typename F, typename T, typename S>
   void to_pssz(const std::vector<T>& v, S& stream)
   {
      using off_t = typename F::offset_type;
      constexpr std::size_t ob = F::offset_bytes;
      if constexpr (pssz_is_fixed_size_v<T> && has_bitwise_serialization<T>())
      {
         if (!v.empty())
            stream.write(reinterpret_cast<const char*>(v.data()),
                         v.size() * sizeof(T));
      }
      else if constexpr (pssz_is_fixed_size_v<T>)
      {
         for (auto& x : v)
            to_pssz<F>(x, stream);
      }
      else
      {
         // Variable-element vector: single-pass backpatching.
         std::size_t n          = v.size();
         char*       slot_start = stream.pos;
         stream.pos += n * ob;
         for (std::size_t i = 0; i < n; ++i)
         {
            off_t rel = static_cast<off_t>(stream.pos - slot_start);
            std::memcpy(slot_start + i * ob, &rel, ob);
            to_pssz<F>(v[i], stream);
         }
      }
   }

   template <typename F, typename T, std::size_t N, typename S>
   void to_pssz(const bounded_list<T, N>& v, S& stream)
   {
      to_pssz<F>(v.storage(), stream);
   }

   // ── std::string / bounded_string — raw bytes, no length prefix ────────────

   template <typename F, typename S>
   void to_pssz(std::string_view sv, S& stream)
   {
      if (!sv.empty())
         stream.write(sv.data(), sv.size());
   }
   template <typename F, typename S>
   void to_pssz(const std::string& s, S& stream)
   {
      to_pssz<F>(std::string_view{s}, stream);
   }
   template <typename F, std::size_t N, typename S>
   void to_pssz(const bounded_string<N>& s, S& stream)
   {
      to_pssz<F>(s.view(), stream);
   }

   // ── std::optional<T> — selector iff min_encoded_size<T> == 0 ──────────────

   template <typename F, typename T, typename S>
   void to_pssz(const std::optional<T>& opt, S& stream)
   {
      if constexpr (pssz_optional_needs_selector<T>)
      {
         std::uint8_t selector = opt.has_value() ? 1 : 0;
         stream.write(reinterpret_cast<const char*>(&selector), 1);
         if (opt.has_value())
            to_pssz<F>(*opt, stream);
      }
      else
      {
         // No selector: None = 0 bytes, Some = encoded T bytes. Size
         // comes from the enclosing container's offset adjacency.
         if (opt.has_value())
            to_pssz<F>(*opt, stream);
      }
   }

   // ── Reflected container ───────────────────────────────────────────────────

   namespace pssz_detail
   {
      template <typename F, typename T>
      consteval std::size_t fixed_header_size()
      {
         using tuple_t = struct_tuple_t<T>;
         constexpr std::size_t N = std::tuple_size_v<tuple_t>;
         return []<std::size_t... Is>(std::index_sequence<Is...>)
         {
            auto field_bytes = [](auto marker) {
               using FT = typename decltype(marker)::type;
               if constexpr (pssz_is_fixed_size_v<FT>)
                  return pssz_fixed_size<FT>::value;
               else
                  return F::offset_bytes;
            };
            return (field_bytes(std::type_identity<
                                std::tuple_element_t<Is, tuple_t>>{}) +
                    ... + std::size_t{0});
         }
         (std::make_index_sequence<N>{});
      }
   }  // namespace pssz_detail

   template <typename F, typename T, typename S>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void to_pssz(const T& obj, S& stream)
   {
      using tuple_t                      = struct_tuple_t<T>;
      constexpr std::size_t N             = std::tuple_size_v<tuple_t>;
      constexpr std::size_t header_bytes  = pssz_detail::fixed_header_size<F, T>();
      using off_t                         = typename F::offset_type;
      using hdr_t                         = typename F::header_type;
      constexpr bool is_dwnc              = pssz_detail::is_dwnc<T>();

      if constexpr (is_dwnc && layout_detail::is_memcpy_layout_struct<T>() &&
                     pssz_detail::all_fields_fixed<T>())
      {
         // DWNC all-fixed with memcpy layout: single write, no header.
         stream.write(reinterpret_cast<const char*>(&obj), sizeof(T));
         return;
      }

      // Emit extensibility header (fixed_size count), unless DWNC.
      if constexpr (!is_dwnc)
      {
         hdr_t hdr = static_cast<hdr_t>(header_bytes);
         stream.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));
      }

      if constexpr (pssz_detail::all_fields_fixed<T>())
      {
         // All-fixed container (with or without header): walk fields inline.
         psio1::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member) { (to_pssz<F>(obj.*member, stream), ...); });
         return;
      }

      // Mixed: single-pass with backpatching. Walk fields once — write
      // fixed values inline, record offset-slot pointers for variable
      // fields and advance past the slot; then walk variable fields again
      // (no size recursion) to emit payloads and patch each slot with the
      // container-relative offset.
      char* const  fixed_start = stream.pos;
      char*        slots[N > 0 ? N : 1] = {};
      std::size_t  slot_count = 0;

      psio1::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [&](auto... member)
          {
             auto emit_fixed = [&](auto m)
             {
                using FT = std::remove_cvref_t<decltype(obj.*m)>;
                if constexpr (pssz_is_fixed_size_v<FT>)
                   to_pssz<F>(obj.*m, stream);
                else
                {
                   slots[slot_count++] = stream.pos;
                   stream.pos += F::offset_bytes;
                }
             };
             (emit_fixed(member), ...);
          });

      // Now stream.pos == fixed_start + header_bytes. Walk variable fields,
      // backpatch each slot with (current pos − fixed_start), then emit.
      slot_count = 0;
      psio1::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [&](auto... member)
          {
             auto emit_var = [&](auto m)
             {
                using FT = std::remove_cvref_t<decltype(obj.*m)>;
                if constexpr (!pssz_is_fixed_size_v<FT>)
                {
                   off_t rel = static_cast<off_t>(stream.pos - fixed_start);
                   std::memcpy(slots[slot_count++], &rel, F::offset_bytes);
                   to_pssz<F>(obj.*m, stream);
                }
             };
             (emit_var(member), ...);
          });
   }

   // ── pssz_size: reflection-accelerated total-size probe ────────────────────

   namespace pssz_detail
   {
      template <typename F, typename T>
      std::uint32_t pssz_size_container(const T& obj)
      {
         using tuple_t = struct_tuple_t<T>;
         constexpr std::size_t N = std::tuple_size_v<tuple_t>;
         constexpr std::size_t header_bytes = fixed_header_size<F, T>();
         constexpr bool        dwnc = is_dwnc<T>();
         std::uint32_t total = static_cast<std::uint32_t>(header_bytes) +
                                (dwnc ? 0u : static_cast<std::uint32_t>(
                                                 F::header_bytes));
         psio1::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member)
             {
                auto visit = [&](auto m)
                {
                   using FT = std::remove_cvref_t<decltype(obj.*m)>;
                   if constexpr (!pssz_is_fixed_size_v<FT>)
                      total += pssz_size<F>(obj.*m);
                };
                (visit(member), ...);
             });
         return total;
      }
   }  // namespace pssz_detail

   template <typename F, typename T>
   std::uint32_t pssz_size(const T& obj)
   {
      if constexpr (pssz_is_fixed_size_v<T>)
      {
         return pssz_fixed_size<T>::value;
      }
      else if constexpr (is_bitlist_v<T>)
      {
         return static_cast<std::uint32_t>((obj.size() + 8) / 8);
      }
      else if constexpr (std::is_same_v<T, std::string> ||
                          std::is_same_v<T, std::string_view>)
      {
         return static_cast<std::uint32_t>(obj.size());
      }
      else if constexpr (is_bounded_string_v<T>)
      {
         return static_cast<std::uint32_t>(obj.size());
      }
      else if constexpr (requires(const T& v) { v.storage(); }
                          && is_bounded_list_v<T>)
      {
         return pssz_size<F>(obj.storage());
      }
      else if constexpr (requires(const T& v) { v.has_value(); *v; })
      {
         // std::optional
         using inner_t = std::remove_cvref_t<decltype(*obj)>;
         if constexpr (pssz_optional_needs_selector<inner_t>)
            return 1u + (obj.has_value() ? pssz_size<F>(*obj) : 0u);
         else
            return obj.has_value() ? pssz_size<F>(*obj) : 0u;
      }
      else
      {
         if constexpr (requires(const T& v) { v.size(); v.data(); typename T::value_type; })
         {
            using E = typename T::value_type;
            std::uint32_t n = static_cast<std::uint32_t>(obj.size());
            if constexpr (pssz_is_fixed_size_v<E>)
               return n * pssz_fixed_size<E>::value;
            else
            {
               std::uint32_t sum = n * static_cast<std::uint32_t>(F::offset_bytes);
               for (auto const& e : obj)
                  sum += pssz_size<F>(e);
               return sum;
            }
         }
         else if constexpr (Reflected<T> && !is_bitvector_v<T> &&
                             !is_bitlist_v<T> && !is_std_bitset_v<T>)
         {
            return pssz_detail::pssz_size_container<F, T>(obj);
         }
         else
         {
            static_assert(sizeof(T) == 0, "pssz_size: no handler for T");
            return 0;
         }
      }
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename F = frac_format_pssz32, typename T>
   void convert_to_pssz(const T& t, std::vector<char>& bin)
   {
      std::uint32_t total = pssz_size<F>(t);
      auto          orig  = bin.size();
      bin.resize(orig + total);
      fixed_buf_stream fbs(bin.data() + orig, total);
      to_pssz<F>(t, fbs);
      check(fbs.pos == fbs.end, stream_error::underrun);
   }

   template <typename F = frac_format_pssz32, typename T>
   std::vector<char> convert_to_pssz(const T& t)
   {
      std::vector<char> out;
      convert_to_pssz<F>(t, out);
      return out;
   }

}  // namespace psio1

