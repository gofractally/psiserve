#pragma once
// SSZ (Simple Serialize) encoding — Ethereum consensus-layer canonical format.
//
// This is the `to_ssz` side. See also `from_ssz.hpp`. Read-only semantics:
// SSZ buffers are serialize-once, not mutated in place, so there is no
// MutView equivalent.
//
// Wire rules (per ethereum/consensus-specs):
//   - Integers: fixed-width little-endian (u8..u256, i8..i64)
//   - bool: 1 byte (0x00/0x01)
//   - Vector[T, N]: N concatenated elements, no length prefix (length is type)
//   - List[T, N]: variable-length sequence; bound N is schema-level validation
//   - Bitvector[N]: packed bits LSB-first, ceil(N/8) bytes
//   - Bitlist[N]: packed bits + delimiter bit at position `len`; length derived
//                 from scanning backward for highest set bit within the parent-
//                 given byte span
//   - Container: fields in declaration order. Fixed-size fields serialize
//                inline; variable-size fields write a 4-byte container-relative
//                offset in their declared position, with the payload appended
//                to a tail region after the fixed region.
//   - Union (std::variant): 1-byte selector + value
//
// Key distinguishing feature from fracpack: offsets are **container-relative**
// (from the start of the enclosing container), so variable-field spans are
// *implicit* via offset adjacency. No length prefix is embedded in variable
// payloads.

#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/detail/layout.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio
{
   // ── Fixed/variable classification ─────────────────────────────────────────
   //
   // `ssz_is_fixed_size_v<T>` is true iff T has a compile-time constant wire
   // size that doesn't depend on the value. Determines whether a field embeds
   // inline or via an offset slot in an SSZ container.

   // Trait an SSZ "basic type" satisfies: any numeric primitive that encodes
   // as its raw little-endian bytes. Covers standard arithmetic types (int
   // 8/16/32/64 both signed and unsigned, float, double) plus the GCC/clang
   // __int128 extensions. bool is a basic type too but handled separately
   // because its wire form is a normalized 0/1 byte rather than raw bytes.
   template <typename T>
   concept SszNumeric =
       (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) ||
       std::is_same_v<T, __int128> ||
       std::is_same_v<T, unsigned __int128>;

   template <typename T>
   struct ssz_is_fixed_size : std::false_type
   {
   };

   template <>
   struct ssz_is_fixed_size<bool> : std::true_type
   {
   };
   template <SszNumeric T>
   struct ssz_is_fixed_size<T> : std::true_type
   {
   };
   template <>
   struct ssz_is_fixed_size<uint256> : std::true_type
   {
   };

   // std::array<T, N>: fixed iff T is fixed.
   template <typename T, std::size_t N>
   struct ssz_is_fixed_size<std::array<T, N>>
       : std::bool_constant<ssz_is_fixed_size<T>::value>
   {
   };

   // Fixed bit types.
   template <std::size_t N>
   struct ssz_is_fixed_size<bitvector<N>> : std::true_type
   {
   };
   template <std::size_t N>
   struct ssz_is_fixed_size<std::bitset<N>> : std::true_type
   {
   };

   // Reflected containers: fixed iff every field is fixed.
   namespace ssz_detail
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
               return (ssz_is_fixed_size<std::tuple_element_t<Is, tuple_t>>::value && ...);
            }
            (std::make_index_sequence<N>{});
         }
      }
   }

   template <typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   struct ssz_is_fixed_size<T> : std::bool_constant<ssz_detail::all_fields_fixed<T>()>
   {
   };

   template <typename T>
   inline constexpr bool ssz_is_fixed_size_v = ssz_is_fixed_size<T>::value;

   // ── ssz_fixed_size: compile-time byte count for fixed types ───────────────

   template <typename T>
   struct ssz_fixed_size;  // intentionally undefined for non-fixed

   template <>
   struct ssz_fixed_size<bool> : std::integral_constant<std::size_t, 1>
   {
   };
   template <SszNumeric T>
   struct ssz_fixed_size<T> : std::integral_constant<std::size_t, sizeof(T)>
   {
   };
   template <>
   struct ssz_fixed_size<uint256> : std::integral_constant<std::size_t, 32>
   {
   };

   template <typename T, std::size_t N>
      requires(ssz_is_fixed_size_v<T>)
   struct ssz_fixed_size<std::array<T, N>>
       : std::integral_constant<std::size_t, N * ssz_fixed_size<T>::value>
   {
   };

   template <std::size_t N>
   struct ssz_fixed_size<bitvector<N>> : std::integral_constant<std::size_t, (N + 7) / 8>
   {
   };
   template <std::size_t N>
   struct ssz_fixed_size<std::bitset<N>> : std::integral_constant<std::size_t, (N + 7) / 8>
   {
   };

   // Reflected fixed container: sum of field fixed sizes.
   namespace ssz_detail
   {
      template <typename T>
      consteval std::size_t sum_fixed_sizes()
      {
         using tuple_t        = struct_tuple_t<T>;
         constexpr std::size_t N = std::tuple_size_v<tuple_t>;
         return []<std::size_t... Is>(std::index_sequence<Is...>)
         {
            return (ssz_fixed_size<std::tuple_element_t<Is, tuple_t>>::value + ... + 0);
         }
         (std::make_index_sequence<N>{});
      }
   }

   template <typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T> && ssz_is_fixed_size_v<T>)
   struct ssz_fixed_size<T>
       : std::integral_constant<std::size_t, ssz_detail::sum_fixed_sizes<T>()>
   {
   };

   // ── Forward declarations ──────────────────────────────────────────────────

   // Reflected-struct encoder, defined below. Constrained to avoid shadowing
   // the per-type overloads (strings, vectors, arrays, bitlists, etc.) and
   // the numeric primitive template.
   template <typename T, typename S>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void to_ssz(const T& obj, S& stream);


   template <typename T>
   std::uint32_t ssz_size(const T& obj);

   // ── Primitives ────────────────────────────────────────────────────────────

   // bool is normalized to a single 0x00 / 0x01 byte (SSZ spec).
   template <typename S>
   void to_ssz(bool val, S& stream)
   {
      std::uint8_t b = val ? 1 : 0;
      stream.write(reinterpret_cast<const char*>(&b), 1);
   }

   // Every other numeric primitive (int8..int64, uint8..uint64, float,
   // double, and the __int128 extensions) is its raw little-endian bytes.
   template <SszNumeric T, typename S>
   void to_ssz(T val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_ssz(const uint256& val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N, typename S>
   void to_ssz(const bitvector<N>& v, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(v.data()), bitvector<N>::byte_count);
   }

   template <std::size_t N, typename S>
   void to_ssz(const std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8];
      pack_bitset_bytes(bs, buf);
      stream.write(reinterpret_cast<const char*>(buf), (N + 7) / 8);
   }

   // Bitlist[N]: delimiter-bit encoding.
   //   - `bit_count` data bits, packed LSB-first.
   //   - A single '1' bit at position `bit_count` marks the end.
   //   - Total bytes = ceil((bit_count + 1) / 8) = (bit_count + 8) / 8.
   //   - Empty bitlist encodes as a single 0x01 byte.
   // psio::bitlist maintains zero trailing bits beyond bit_count, so we can
   // copy its bytes straight through and just set the delimiter.
   template <std::size_t MaxN, typename S>
   void to_ssz(const bitlist<MaxN>& v, S& stream)
   {
      std::size_t bit_count   = v.size();
      std::size_t total_bytes = (bit_count + 8) / 8;

      std::vector<std::uint8_t> buf(total_bytes, 0);
      auto src = v.bytes();
      if (!src.empty())
         std::memcpy(buf.data(), src.data(), src.size());
      buf[bit_count >> 3] |= static_cast<std::uint8_t>(1u << (bit_count & 7u));
      stream.write(reinterpret_cast<const char*>(buf.data()), total_bytes);
   }

   // Can we treat an element T as raw bytes on both sides of the wire?
   // True for primitives (arithmetic, enums, opt-in via is_bitwise_copy) AND
   // for reflected structs with no alignment padding (the wire layout
   // matches the C++ memory layout byte-for-byte — typical when the user
   // applies __attribute__((packed))).
   template <typename T>
   inline constexpr bool ssz_memcpy_ok_v =
       ssz_is_fixed_size_v<T> &&
       (has_bitwise_serialization<T>() ||
        layout_detail::is_memcpy_layout_struct<T>());

   // ── std::array<T, N> = SSZ Vector[T, N] ───────────────────────────────────

   template <typename T, std::size_t N, typename S>
   void to_ssz(const std::array<T, N>& arr, S& stream)
   {
      if constexpr (ssz_memcpy_ok_v<T>)
      {
         // Memcpy path: contiguous LE bytes already match wire.
         if constexpr (N > 0)
            stream.write(reinterpret_cast<const char*>(arr.data()), N * sizeof(T));
      }
      else if constexpr (ssz_is_fixed_size_v<T>)
      {
         for (auto& x : arr)
            to_ssz(x, stream);
      }
      else
      {
         // Variable-element Vector[T, N]: single-pass backpatching.
         char* slot_start = stream.pos;
         stream.pos += N * 4;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t rel =
                static_cast<std::uint32_t>(stream.pos - slot_start);
            std::memcpy(slot_start + i * 4, &rel, 4);
            to_ssz(arr[i], stream);
         }
      }
   }

   // ── std::vector<T> = SSZ List[T, *] (bound enforced at schema level) ──────

   template <typename T, typename S>
   void to_ssz(const std::vector<T>& v, S& stream)
   {
      if constexpr (ssz_memcpy_ok_v<T>)
      {
         if (!v.empty())
            stream.write(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
      }
      else if constexpr (ssz_is_fixed_size_v<T>)
      {
         for (auto& x : v)
            to_ssz(x, stream);
      }
      else
      {
         // List of variable-size elements: offset table + tail.
         // Variable-element List[T]: single-pass backpatching.
         std::size_t n          = v.size();
         char*       slot_start = stream.pos;
         stream.pos += n * 4;
         for (std::size_t i = 0; i < n; ++i)
         {
            std::uint32_t rel =
                static_cast<std::uint32_t>(stream.pos - slot_start);
            std::memcpy(slot_start + i * 4, &rel, 4);
            to_ssz(v[i], stream);
         }
      }
   }

   // bounded_list forwards to std::vector path
   template <typename T, std::size_t N, typename S>
   void to_ssz(const bounded_list<T, N>& v, S& stream)
   {
      to_ssz(v.storage(), stream);
   }

   // ── String / byte containers = SSZ ByteList ──────────────────────────────

   template <typename S>
   void to_ssz(std::string_view sv, S& stream)
   {
      if (!sv.empty())
         stream.write(sv.data(), sv.size());
   }
   template <typename S>
   void to_ssz(const std::string& s, S& stream)
   {
      to_ssz(std::string_view{s}, stream);
   }
   template <std::size_t N, typename S>
   void to_ssz(const bounded_string<N>& s, S& stream)
   {
      to_ssz(s.view(), stream);
   }

   // ── std::optional = SSZ Union[null, T] ────────────────────────────────────
   //
   // Encoding: 1-byte selector followed by the payload when present.
   //   None    → 0x00
   //   Some(x) → 0x01 || serialized(x)
   //
   // This matches the Union / Optional encoding proposed in the upstream SSZ
   // progressive types / union specifications. Always variable-size because
   // the presence bit plus optional tail can't be known from the type alone.

   template <typename T, typename S>
   void to_ssz(const std::optional<T>& opt, S& stream)
   {
      std::uint8_t selector = opt.has_value() ? 1 : 0;
      stream.write(reinterpret_cast<const char*>(&selector), 1);
      if (opt.has_value())
         to_ssz(*opt, stream);
   }

   // ── Reflected Container ──────────────────────────────────────────────────

   namespace ssz_detail
   {
      template <typename T>
      consteval std::size_t fixed_header_size()
      {
         using tuple_t        = struct_tuple_t<T>;
         constexpr std::size_t N = std::tuple_size_v<tuple_t>;
         return []<std::size_t... Is>(std::index_sequence<Is...>)
         {
            auto per_field = []<std::size_t I>(std::integral_constant<std::size_t, I>)
            {
               using FT = std::tuple_element_t<I, tuple_t>;
               if constexpr (ssz_is_fixed_size_v<FT>)
                  return ssz_fixed_size<FT>::value;
               else
                  return std::size_t{4};  // offset slot
            };
            return (per_field(std::integral_constant<std::size_t, Is>{}) + ... + 0);
         }
         (std::make_index_sequence<N>{});
      }
   }

   template <typename T, typename S>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void to_ssz(const T& obj, S& stream)
   {
      {
         using tuple_t                     = struct_tuple_t<T>;
         constexpr std::size_t N            = std::tuple_size_v<tuple_t>;
         constexpr std::size_t header_bytes = ssz_detail::fixed_header_size<T>();

         if constexpr (ssz_is_fixed_size_v<T> &&
                        layout_detail::is_memcpy_layout_struct<T>())
         {
            // Fast path: in-memory bytes match wire bytes exactly (user has
            // applied __attribute__((packed)) or layout happens to align).
            // Single stream.write covers the whole struct.
            stream.write(reinterpret_cast<const char*>(&obj), sizeof(T));
         }
         else if constexpr (ssz_is_fixed_size_v<T>)
         {
            // All-fixed container with alignment padding: walk fields.
            psio::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member) { (to_ssz(obj.*member, stream), ...); });
         }
         else
         {
            // Mixed container: single-pass with backpatching. Walk the
            // fields once, writing inline fixed values and reserving
            // 4-byte offset-slot placeholders for variable fields (we
            // remember each slot's address). Then walk variable fields
            // once more emitting payloads, back-filling each placeholder
            // with (current stream pos − fixed-region start) as we go.
            // Eliminates the prior 3-pass scheme and its recursive
            // ssz_size walks — which compounded quadratically on nested
            // types like a BeaconState full of containers.
            char* const fixed_start = stream.pos;
            char*       slots[N > 0 ? N : 1] = {};
            std::size_t slot_count = 0;

            psio::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member)
                {
                   auto emit_fixed = [&](auto m)
                   {
                      using FT = std::remove_cvref_t<decltype(obj.*m)>;
                      if constexpr (ssz_is_fixed_size_v<FT>)
                         to_ssz(obj.*m, stream);
                      else
                      {
                         slots[slot_count++] = stream.pos;
                         stream.pos += 4;
                      }
                   };
                   (emit_fixed(member), ...);
                });

            slot_count = 0;
            psio::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member)
                {
                   auto emit_var = [&](auto m)
                   {
                      using FT = std::remove_cvref_t<decltype(obj.*m)>;
                      if constexpr (!ssz_is_fixed_size_v<FT>)
                      {
                         std::uint32_t rel =
                             static_cast<std::uint32_t>(stream.pos - fixed_start);
                         std::memcpy(slots[slot_count++], &rel, 4);
                         to_ssz(obj.*m, stream);
                      }
                   };
                   (emit_var(member), ...);
                });
         }
      }
   }

   // ── ssz_size: reflection-accelerated size walker ─────────────────────────
   //
   // Avoids the size_stream dry-pack by computing size directly from metadata:
   //   - Fixed types: compile-time constant
   //   - std::vector<T> with fixed T: O(1) — n * fixed_size(T)
   //   - std::vector<T> with variable T: O(n) — n * 4 + Σ ssz_size(elem)
   //   - std::string / bounded_string: O(1) — s.size()
   //   - bitlist: O(1) — (bit_count + 8) / 8
   //   - Container: O(variable fields) — fixed_header_size + Σ ssz_size(var)
   //
   // This is what convert_to_ssz uses to preallocate its output buffer.

   template <typename T>
   std::uint32_t ssz_size(const T& obj);

   namespace ssz_detail
   {
      template <typename T>
      std::uint32_t ssz_size_container(const T& obj)
      {
         using tuple_t                     = struct_tuple_t<T>;
         constexpr std::size_t NFIELDS      = std::tuple_size_v<tuple_t>;
         constexpr std::size_t header_bytes = fixed_header_size<T>();
         std::uint32_t tail = 0;
         psio::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member)
             {
                auto visit = [&](auto m)
                {
                   using FT = std::remove_cvref_t<decltype(obj.*m)>;
                   if constexpr (!ssz_is_fixed_size_v<FT>)
                      tail += ssz_size(obj.*m);
                };
                (visit(member), ...);
             });
         return static_cast<std::uint32_t>(header_bytes) + tail;
      }
   }

   template <typename T>
   std::uint32_t ssz_size(const T& obj)
   {
      if constexpr (ssz_is_fixed_size_v<T>)
      {
         return ssz_fixed_size<T>::value;
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
      else if constexpr (requires { obj.storage(); } && is_bounded_list_v<T>)
      {
         return ssz_size(obj.storage());  // delegate to std::vector path
      }
      else if constexpr (requires { obj.has_value(); *obj; })  // std::optional
      {
         return 1u + (obj.has_value() ? ssz_size(*obj) : 0u);
      }
      else
      {
         // std::vector<T> (both fixed and variable element)
         if constexpr (requires { obj.size(); obj.data(); })
         {
            using E              = typename T::value_type;
            std::uint32_t n      = static_cast<std::uint32_t>(obj.size());
            if constexpr (ssz_is_fixed_size_v<E>)
            {
               return n * ssz_fixed_size<E>::value;
            }
            else
            {
               std::uint32_t sum = n * 4;
               for (auto const& e : obj)
                  sum += ssz_size(e);
               return sum;
            }
         }
         else if constexpr (Reflected<T> && !is_bitvector_v<T> &&
                             !is_bitlist_v<T> && !is_std_bitset_v<T>)
         {
            return ssz_detail::ssz_size_container(obj);
         }
         else
         {
            // Fallback: size_stream walk (shouldn't fire for supported types).
            size_stream ss;
            to_ssz(obj, ss);
            return static_cast<std::uint32_t>(ss.size);
         }
      }
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_to_ssz(const T& t, std::vector<char>& bin)
   {
      // Reflection-accelerated size probe — typically O(variable-field count)
      // for containers, O(1) for fixed-size types. Avoids the dry-pack walk
      // that size_stream would do.
      std::uint32_t total = ssz_size(t);
      auto          orig_size = bin.size();
      bin.resize(orig_size + total);
      fixed_buf_stream fbs(bin.data() + orig_size, total);
      to_ssz(t, fbs);
      check(fbs.pos == fbs.end, stream_error::underrun);
   }

   template <typename T>
   std::vector<char> convert_to_ssz(const T& t)
   {
      std::vector<char> result;
      convert_to_ssz(t, result);
      return result;
   }

}  // namespace psio
