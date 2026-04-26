#pragma once
// SSZ (Simple Serialize) decoding. See to_ssz.hpp for wire rules.
//
// Decoder interface: every decode entry point takes a byte span and populates
// the target value. For variable-size types at the top level the caller hands
// us the full buffer; for variable-size fields inside a container, the span
// is derived from the enclosing container's offset table.

#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/check.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/reflect.hpp>
#include <psio1/stream.hpp>
#include <psio1/to_ssz.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace psio1
{
   // ── Core decoder interface ────────────────────────────────────────────────
   //
   // Every from_ssz overload takes:
   //   value     — out-param receiving the decoded value
   //   src       — buffer start
   //   span_beg  — byte offset of *this* value's start within src
   //   span_end  — byte offset of the end of *this* value's region
   //
   // Fixed-size types ignore span_end (advance by their intrinsic size).
   // Variable-size types use the full [span_beg, span_end) range, deriving
   // counts / bounds from that span.

   // Forward decl of the reflected-struct decoder.
   template <typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void from_ssz(T& value, const char* src, std::uint32_t span_beg,
                 std::uint32_t span_end);

   // Forward decls so templated containers (std::vector<T>, reflected structs)
   // can resolve calls to decoders for element types that are themselves
   // declared later in the file. Strict two-phase lookup inside templates
   // requires the overloads to be visible at template *definition* time.
   inline void from_ssz(std::string& s, const char* src, std::uint32_t pos,
                         std::uint32_t end);
   template <std::size_t N>
   void from_ssz(bounded_string<N>& s, const char* src, std::uint32_t pos,
                 std::uint32_t end);
   template <typename T>
   void from_ssz(std::optional<T>& opt, const char* src, std::uint32_t pos,
                 std::uint32_t end);


   // ── Primitives ────────────────────────────────────────────────────────────

   // bool is validated (must be 0x00 or 0x01 on the wire).
   inline void from_ssz(bool& value, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      check(end - pos >= 1, "ssz underrun reading bool");
      std::uint8_t b = static_cast<std::uint8_t>(src[pos]);
      check(b <= 1, "ssz: invalid bool encoding");
      value = (b != 0);
   }

   // Every other numeric primitive: raw little-endian memcpy.
   template <SszNumeric T>
   void from_ssz(T& value, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      check(end - pos >= sizeof(T), "ssz underrun reading primitive");
      std::memcpy(&value, src + pos, sizeof(T));
   }

   // __int128 and unsigned __int128 are covered by the SszNumeric template
   // above. uint256 is a struct, not a primitive, so gets its own overload.
   inline void from_ssz(uint256& v, const char* src, std::uint32_t pos,
                         std::uint32_t end)
   {
      check(end - pos >= 32, "ssz underrun reading uint256");
      std::memcpy(&v, src + pos, 32);
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N>
   void from_ssz(bitvector<N>& v, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      constexpr std::size_t NB = (N + 7) / 8;
      check(end - pos >= NB, "ssz underrun reading bitvector");
      std::memcpy(v.data(), src + pos, NB);
   }

   template <std::size_t N>
   void from_ssz(std::bitset<N>& bs, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      constexpr std::size_t NB = (N + 7) / 8;
      check(end - pos >= NB, "ssz underrun reading bitset");
      unpack_bitset_bytes(reinterpret_cast<const std::uint8_t*>(src + pos), bs);
   }

   // Bitlist: scan [pos, end) for the highest set bit — that's the delimiter.
   // Everything below is data.
   template <std::size_t MaxN>
   void from_ssz(bitlist<MaxN>& v, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      check(end > pos, "ssz bitlist: empty byte span invalid");
      std::uint32_t span = end - pos;
      // Find the last non-zero byte.
      std::int32_t last = static_cast<std::int32_t>(span) - 1;
      while (last >= 0 && static_cast<std::uint8_t>(src[pos + last]) == 0)
         --last;
      check(last >= 0, "ssz bitlist: missing delimiter bit");
      std::uint8_t last_byte = static_cast<std::uint8_t>(src[pos + last]);
      // Highest set bit position within last_byte.
      int hi = 31 - __builtin_clz(static_cast<unsigned int>(last_byte));
      std::size_t bit_count = static_cast<std::size_t>(last) * 8 + static_cast<std::size_t>(hi);
      check(bit_count <= MaxN, "ssz bitlist: bit_count exceeds bound");

      // Clear the delimiter bit from the copied payload.
      std::vector<std::uint8_t> tmp(span);
      std::memcpy(tmp.data(), src + pos, span);
      tmp[last] &= static_cast<std::uint8_t>(~(1u << hi));
      v.assign_raw(bit_count, tmp.data());
   }

   // ── std::array<T, N> = Vector[T, N] ───────────────────────────────────────

   template <typename T, std::size_t N>
   void from_ssz(std::array<T, N>& arr, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      if constexpr (N == 0)
      {
         return;
      }
      else if constexpr (ssz_memcpy_ok_v<T>)
      {
         constexpr std::size_t byte_count = N * sizeof(T);
         check(end - pos >= byte_count, "ssz underrun reading fixed array");
         std::memcpy(arr.data(), src + pos, byte_count);
      }
      else if constexpr (ssz_is_fixed_size_v<T>)
      {
         constexpr std::size_t elem_size = ssz_fixed_size<T>::value;
         constexpr std::size_t total     = N * elem_size;
         check(end - pos >= total, "ssz underrun reading fixed array");
         for (std::size_t i = 0; i < N; ++i)
            from_ssz(arr[i], src, pos + i * elem_size, pos + (i + 1) * elem_size);
      }
      else
      {
         // Variable elements: offset table of size N, then payloads.
         check(end - pos >= N * 4, "ssz underrun reading variable array header");
         std::uint32_t offs[N];
         for (std::size_t i = 0; i < N; ++i)
            std::memcpy(&offs[i], src + pos + i * 4, 4);
         std::uint32_t container_start = pos;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t beg = container_start + offs[i];
            std::uint32_t stop =
                (i + 1 < N) ? (container_start + offs[i + 1]) : end;
            check(beg >= pos && stop <= end && beg <= stop,
                  "ssz: variable array element offset out of range");
            from_ssz(arr[i], src, beg, stop);
         }
      }
   }

   // ── std::vector<T> = List[T, *] ───────────────────────────────────────────

   template <typename T>
   void from_ssz(std::vector<T>& v, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      std::uint32_t span = end - pos;
      if constexpr (ssz_memcpy_ok_v<T>)
      {
         check(span % sizeof(T) == 0, "ssz: list span not divisible by element size");
         std::size_t n = span / sizeof(T);
         // resize(n) value-initializes (zero-fills) every element before
         // memcpy overwrites it — doubling the write-bandwidth cost on
         // large vectors. assign(first, last) with pointer iterators of
         // a trivially-copyable T goes through __uninitialized_copy_a
         // which lowers to a single memcpy with no zero-init pass. On a
         // 260 MiB validator list this is a 2.2× speedup.
         const T* first = reinterpret_cast<const T*>(src + pos);
         v.assign(first, first + n);
      }
      else if constexpr (ssz_is_fixed_size_v<T>)
      {
         constexpr std::size_t elem_size = ssz_fixed_size<T>::value;
         check(span % elem_size == 0, "ssz: list span not divisible by element size");
         std::size_t n = span / elem_size;
         v.resize(n);
         for (std::size_t i = 0; i < n; ++i)
            from_ssz(v[i], src, pos + i * elem_size, pos + (i + 1) * elem_size);
      }
      else
      {
         // Variable elements: derive count from first offset.
         if (span == 0)
         {
            v.clear();
            return;
         }
         check(span >= 4, "ssz: variable list too short for header");
         std::uint32_t first_offset = 0;
         std::memcpy(&first_offset, src + pos, 4);
         check(first_offset % 4 == 0 && first_offset <= span,
               "ssz: invalid first offset in variable list");
         std::size_t n = first_offset / 4;
         std::vector<std::uint32_t> offs(n);
         check(span >= n * 4, "ssz: truncated variable list offset table");
         for (std::size_t i = 0; i < n; ++i)
            std::memcpy(&offs[i], src + pos + i * 4, 4);
         v.resize(n);
         for (std::size_t i = 0; i < n; ++i)
         {
            std::uint32_t beg  = pos + offs[i];
            std::uint32_t stop = (i + 1 < n) ? (pos + offs[i + 1]) : end;
            check(beg >= pos && stop <= end && beg <= stop,
                  "ssz: variable list element offset out of range");
            from_ssz(v[i], src, beg, stop);
         }
      }
   }

   // bounded_list forwards with bound enforcement
   template <typename T, std::size_t N>
   void from_ssz(bounded_list<T, N>& v, const char* src, std::uint32_t pos,
                 std::uint32_t end)
   {
      from_ssz(v.storage(), src, pos, end);
      check(v.size() <= N, "ssz: bounded_list overflow on decode");
   }

   // ── Strings / bytes: span-derived ─────────────────────────────────────────

   inline void from_ssz(std::string& s, const char* src, std::uint32_t pos,
                         std::uint32_t end)
   {
      s.assign(src + pos, src + end);
   }
   template <std::size_t N>
   void from_ssz(bounded_string<N>& s, const char* src, std::uint32_t pos,
                 std::uint32_t end)
   {
      std::uint32_t span = end - pos;
      check(span <= N, "ssz: bounded_string overflow on decode");
      s.storage().assign(src + pos, src + end);
   }

   // ── std::optional = SSZ Union[null, T] ────────────────────────────────────
   // See to_ssz.hpp for encoding. 0x00 → None; 0x01 + payload → Some.

   template <typename T>
   void from_ssz(std::optional<T>& opt, const char* src, std::uint32_t pos,
                 std::uint32_t end)
   {
      check(end > pos, "ssz: optional selector byte missing");
      std::uint8_t selector = static_cast<std::uint8_t>(src[pos]);
      if (selector == 0)
      {
         check(end == pos + 1, "ssz: optional selector 0 must have empty payload");
         opt.reset();
         return;
      }
      check(selector == 1, "ssz: invalid optional selector byte");
      opt.emplace();
      from_ssz(*opt, src, pos + 1, end);
   }

   // ── Reflected Container ──────────────────────────────────────────────────

   template <typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void from_ssz(T& value, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      {
         using tuple_t                 = struct_tuple_t<T>;
         constexpr std::size_t NFIELDS  = std::tuple_size_v<tuple_t>;
         constexpr std::size_t header   = ssz_detail::fixed_header_size<T>();
         check(end - pos >= header, "ssz container: not enough bytes for fixed region");

         if constexpr (ssz_is_fixed_size_v<T> &&
                        layout_detail::is_memcpy_layout_struct<T>())
         {
            // Fast path: mem layout matches wire. Single memcpy.
            std::memcpy(&value, src + pos, sizeof(T));
         }
         else if constexpr (ssz_is_fixed_size_v<T>)
         {
            // All-fixed: decode each field at its known offset.
            std::uint32_t field_pos = pos;
            psio1::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member)
                {
                   auto read_fixed = [&](auto m)
                   {
                      using FT              = std::remove_cvref_t<decltype(value.*m)>;
                      constexpr std::size_t fs = ssz_fixed_size<FT>::value;
                      from_ssz(value.*m, src, field_pos, field_pos + fs);
                      field_pos += fs;
                   };
                   (read_fixed(member), ...);
                });
         }
         else
         {
            // Mixed: first pass — collect variable-field offsets in order.
            std::array<std::uint32_t, NFIELDS> offsets{};
            std::array<bool, NFIELDS>          is_variable{};
            std::uint32_t                      field_pos = pos;
            std::size_t                        idx       = 0;
            psio1::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member)
                {
                   auto scan = [&](auto m)
                   {
                      using FT = std::remove_cvref_t<decltype(value.*m)>;
                      if constexpr (ssz_is_fixed_size_v<FT>)
                      {
                         constexpr std::size_t fs = ssz_fixed_size<FT>::value;
                         field_pos += fs;
                      }
                      else
                      {
                         is_variable[idx] = true;
                         std::uint32_t off = 0;
                         std::memcpy(&off, src + field_pos, 4);
                         offsets[idx] = off;
                         field_pos += 4;
                      }
                      ++idx;
                   };
                   (scan(member), ...);
                });

            // Validate offsets: must be non-decreasing within [header, end-pos],
            // and within the container's own byte range.
            std::uint32_t container_end = end - pos;
            std::uint32_t prev          = static_cast<std::uint32_t>(header);
            for (std::size_t i = 0; i < NFIELDS; ++i)
            {
               if (is_variable[i])
               {
                  check(offsets[i] >= prev && offsets[i] <= container_end,
                        "ssz container: variable offset out of range");
                  prev = offsets[i];
               }
            }

            // Second pass — decode fixed fields from their inline positions, and
            // variable fields using [offset_i, next_var_offset_or_end).
            field_pos = pos;
            idx       = 0;

            // Compute next-variable-offset-or-end per field index.
            std::array<std::uint32_t, NFIELDS> var_end{};
            std::uint32_t                      last_var_end = end;
            for (std::size_t i = NFIELDS; i-- > 0;)
            {
               if (is_variable[i])
               {
                  var_end[i]   = last_var_end;
                  last_var_end = pos + offsets[i];
               }
            }

            psio1::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member)
                {
                   auto decode = [&](auto m)
                   {
                      using FT = std::remove_cvref_t<decltype(value.*m)>;
                      if constexpr (ssz_is_fixed_size_v<FT>)
                      {
                         constexpr std::size_t fs = ssz_fixed_size<FT>::value;
                         from_ssz(value.*m, src, field_pos, field_pos + fs);
                         field_pos += fs;
                      }
                      else
                      {
                         std::uint32_t beg = pos + offsets[idx];
                         std::uint32_t stop = var_end[idx];
                         from_ssz(value.*m, src, beg, stop);
                         field_pos += 4;
                      }
                      ++idx;
                   };
                   (decode(member), ...);
                });
         }
      }
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_from_ssz(T& value, std::span<const char> bytes)
   {
      check(bytes.size() <= std::numeric_limits<std::uint32_t>::max(),
            "ssz buffer too large");
      from_ssz(value, bytes.data(), 0, static_cast<std::uint32_t>(bytes.size()));
   }

   template <typename T>
   T convert_from_ssz(std::span<const char> bytes)
   {
      T value{};
      convert_from_ssz(value, bytes);
      return value;
   }

   template <typename T>
   T convert_from_ssz(const std::vector<char>& bytes)
   {
      return convert_from_ssz<T>(std::span<const char>(bytes.data(), bytes.size()));
   }

   // ── Validate-only walker: check structural integrity without materializing.
   //
   // Walks the same recursive shape as from_ssz but skips value-population
   // work — useful for "is this buffer a well-formed T?" pre-checks (e.g. in
   // front of an ssz_view) without paying the full decode cost.

   template <typename T>
   void ssz_validate_impl(const char* src, std::uint32_t pos, std::uint32_t end);

   template <typename T>
      requires(ssz_is_fixed_size_v<T> && !Reflected<T>)
   void ssz_validate_impl(const char* /*src*/, std::uint32_t pos, std::uint32_t end)
   {
      check(end - pos >= ssz_fixed_size<T>::value, "ssz validate: underrun on fixed scalar");
   }

   // bitvector / bitset are handled by the fixed-size impl above (they match
   // the Reflected<T>=false + fixed-size constraint).

   template <std::size_t MaxN>
   void ssz_validate_impl_bitlist(const char* src, std::uint32_t pos, std::uint32_t end)
   {
      check(end > pos, "ssz validate bitlist: empty span");
      std::uint32_t span = end - pos;
      std::int32_t  last = static_cast<std::int32_t>(span) - 1;
      while (last >= 0 && static_cast<std::uint8_t>(src[pos + last]) == 0)
         --last;
      check(last >= 0, "ssz validate bitlist: no delimiter");
      std::uint8_t last_byte = static_cast<std::uint8_t>(src[pos + last]);
      int          hi        = 31 - __builtin_clz(static_cast<unsigned int>(last_byte));
      std::size_t  bits      = static_cast<std::size_t>(last) * 8 + static_cast<std::size_t>(hi);
      check(bits <= MaxN, "ssz validate bitlist: bit_count exceeds bound");
   }

   template <std::size_t MaxN>
   void ssz_validate_impl(bitlist<MaxN>*, const char* src, std::uint32_t pos,
                          std::uint32_t end)
   {
      ssz_validate_impl_bitlist<MaxN>(src, pos, end);
   }

   template <typename T>
   void ssz_validate_impl_vector(const char* src, std::uint32_t pos, std::uint32_t end)
   {
      std::uint32_t span = end - pos;
      if constexpr (ssz_is_fixed_size_v<T>)
      {
         constexpr std::size_t esz = ssz_fixed_size<T>::value;
         check(span % esz == 0, "ssz validate: list span not divisible");
         if constexpr (!has_bitwise_serialization<T>() && !ssz_is_fixed_size_v<T>)
         {
            // never taken — guarded
         }
         else if constexpr (!has_bitwise_serialization<T>())
         {
            std::size_t n = span / esz;
            for (std::size_t i = 0; i < n; ++i)
               ssz_validate_impl<T>(src, pos + i * esz, pos + (i + 1) * esz);
         }
         // has_bitwise_serialization: bytes already LE, no recursion needed.
      }
      else
      {
         if (span == 0)
            return;
         check(span >= 4, "ssz validate: variable list too short");
         std::uint32_t first = 0;
         std::memcpy(&first, src + pos, 4);
         check(first % 4 == 0 && first <= span,
               "ssz validate: invalid first offset");
         std::size_t n = first / 4;
         check(span >= n * 4, "ssz validate: truncated offset table");
         std::uint32_t prev = first;
         for (std::size_t i = 0; i < n; ++i)
         {
            std::uint32_t off_i;
            std::memcpy(&off_i, src + pos + i * 4, 4);
            check(off_i >= ((i == 0) ? first : prev) && off_i <= span,
                  "ssz validate: list offset out of range");
            prev = off_i;
         }
         // Recurse per element.
         for (std::size_t i = 0; i < n; ++i)
         {
            std::uint32_t off_i, stop;
            std::memcpy(&off_i, src + pos + i * 4, 4);
            if (i + 1 < n)
               std::memcpy(&stop, src + pos + (i + 1) * 4, 4);
            else
               stop = span;
            ssz_validate_impl<T>(src, pos + off_i, pos + stop);
         }
      }
   }

   template <typename T>
   void ssz_validate_impl(std::vector<T>*, const char* src, std::uint32_t pos,
                          std::uint32_t end)
   {
      ssz_validate_impl_vector<T>(src, pos, end);
   }

   template <typename T, std::size_t N>
   void ssz_validate_impl(bounded_list<T, N>*, const char* src, std::uint32_t pos,
                          std::uint32_t end)
   {
      ssz_validate_impl_vector<T>(src, pos, end);
      // Count bound check via span arithmetic
      std::uint32_t span = end - pos;
      if constexpr (ssz_is_fixed_size_v<T>)
      {
         constexpr std::size_t esz = ssz_fixed_size<T>::value;
         check(span / esz <= N, "ssz validate bounded_list: count exceeds bound");
      }
      else
      {
         if (span > 0)
         {
            std::uint32_t first = 0;
            std::memcpy(&first, src + pos, 4);
            check(first / 4 <= N, "ssz validate bounded_list: count exceeds bound");
         }
      }
   }

   // std::string / bounded_string: span is valid UTF-8 is NOT verified (spec
   // doesn't require UTF-8 validation at the SSZ layer).
   inline void ssz_validate_impl(std::string*, const char*, std::uint32_t, std::uint32_t) {}

   template <std::size_t N>
   void ssz_validate_impl(bounded_string<N>*, const char*, std::uint32_t pos,
                          std::uint32_t end)
   {
      check(end - pos <= N, "ssz validate bounded_string: span exceeds bound");
   }

   template <typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void ssz_validate_impl(T*, const char* src, std::uint32_t pos, std::uint32_t end)
   {
      using tuple_t                    = struct_tuple_t<T>;
      constexpr std::size_t NFIELDS     = std::tuple_size_v<tuple_t>;
      constexpr std::size_t header      = ssz_detail::fixed_header_size<T>();
      check(end - pos >= header, "ssz validate container: header underrun");

      if constexpr (ssz_is_fixed_size_v<T>)
      {
         std::uint32_t field_pos = pos;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (
                [&]<std::size_t I>(std::integral_constant<std::size_t, I>)
                {
                   using FT = std::tuple_element_t<I, tuple_t>;
                   constexpr std::size_t fs = ssz_fixed_size<FT>::value;
                   ssz_validate_impl<FT>(src, field_pos, field_pos + fs);
                   field_pos += fs;
                }(std::integral_constant<std::size_t, Is>{}),
                ...);
         }(std::make_index_sequence<NFIELDS>{});
      }
      else
      {
         std::array<std::uint32_t, NFIELDS> offsets{};
         std::array<bool, NFIELDS>          is_var{};
         std::uint32_t                      fp = pos;
         std::size_t                        idx = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (
                [&]<std::size_t I>(std::integral_constant<std::size_t, I>)
                {
                   using FT = std::tuple_element_t<I, tuple_t>;
                   if constexpr (ssz_is_fixed_size_v<FT>)
                   {
                      constexpr std::size_t fs = ssz_fixed_size<FT>::value;
                      ssz_validate_impl<FT>(src, fp, fp + fs);
                      fp += fs;
                   }
                   else
                   {
                      is_var[idx] = true;
                      std::uint32_t o = 0;
                      std::memcpy(&o, src + fp, 4);
                      offsets[idx] = o;
                      fp += 4;
                   }
                   ++idx;
                }(std::integral_constant<std::size_t, Is>{}),
                ...);
         }(std::make_index_sequence<NFIELDS>{});

         // Validate offsets are non-decreasing and within container bounds.
         std::uint32_t container_end = end - pos;
         std::uint32_t prev          = static_cast<std::uint32_t>(header);
         for (std::size_t i = 0; i < NFIELDS; ++i)
         {
            if (is_var[i])
            {
               check(offsets[i] >= prev && offsets[i] <= container_end,
                     "ssz validate container: variable offset out of range");
               prev = offsets[i];
            }
         }

         // Recurse into each variable field.
         std::array<std::uint32_t, NFIELDS> var_end{};
         std::uint32_t                      last_var_end = end;
         for (std::size_t i = NFIELDS; i-- > 0;)
         {
            if (is_var[i])
            {
               var_end[i]   = last_var_end;
               last_var_end = pos + offsets[i];
            }
         }

         idx = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (
                [&]<std::size_t I>(std::integral_constant<std::size_t, I>)
                {
                   using FT = std::tuple_element_t<I, tuple_t>;
                   if constexpr (!ssz_is_fixed_size_v<FT>)
                   {
                      std::uint32_t beg  = pos + offsets[idx];
                      std::uint32_t stop = var_end[idx];
                      ssz_validate_impl<FT>(src, beg, stop);
                   }
                   ++idx;
                }(std::integral_constant<std::size_t, Is>{}),
                ...);
         }(std::make_index_sequence<NFIELDS>{});
      }
   }

   // Generic template dispatcher that falls through to the pointer-based
   // overloads above. Call sites use this.
   template <typename T>
   void ssz_validate_impl(const char* src, std::uint32_t pos, std::uint32_t end)
   {
      if constexpr (ssz_is_fixed_size_v<T> && !Reflected<T> && !is_bitvector_v<T> &&
                    !is_std_bitset_v<T>)
      {
         check(end - pos >= ssz_fixed_size<T>::value, "ssz validate: scalar underrun");
      }
      else
      {
         ssz_validate_impl(static_cast<T*>(nullptr), src, pos, end);
      }
   }

   template <typename T>
   void ssz_validate(std::span<const char> bytes)
   {
      check(bytes.size() <= std::numeric_limits<std::uint32_t>::max(),
            "ssz buffer too large");
      ssz_validate_impl<T>(bytes.data(), 0, static_cast<std::uint32_t>(bytes.size()));
   }

   template <typename T>
   void ssz_validate(const std::vector<char>& bytes)
   {
      ssz_validate<T>(std::span<const char>(bytes.data(), bytes.size()));
   }

}  // namespace psio1
