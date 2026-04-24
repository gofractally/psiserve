#pragma once
// pSSZ decoder. See to_pssz.hpp for the wire format.
//
// Every from_pssz overload takes:
//   value  — out-param
//   src    — buffer start
//   pos    — byte offset of this value's start within src
//   end    — byte offset of this value's end within src

#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/check.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/to_pssz.hpp>

#include <array>
#include <bitset>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

namespace psio
{
   // Forward decls so container/vector decoders can find later overloads at
   // template definition time.
   template <typename F, typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void from_pssz(T& value, const char* src, std::uint32_t pos,
                  std::uint32_t end);

   template <typename F>
   inline void from_pssz(std::string& s, const char* src, std::uint32_t pos,
                          std::uint32_t end);
   template <typename F, std::size_t N>
   void from_pssz(bounded_string<N>& s, const char* src, std::uint32_t pos,
                  std::uint32_t end);
   template <typename F, typename T>
   void from_pssz(std::optional<T>& opt, const char* src, std::uint32_t pos,
                  std::uint32_t end);
   template <typename F, typename T>
   void from_pssz(std::vector<T>& v, const char* src, std::uint32_t pos,
                  std::uint32_t end);

   // ── Primitives ────────────────────────────────────────────────────────────

   template <typename F>
   inline void from_pssz(bool& value, const char* src, std::uint32_t pos,
                          std::uint32_t end)
   {
      check(end - pos >= 1, "pssz underrun reading bool");
      std::uint8_t b = static_cast<std::uint8_t>(src[pos]);
      check(b <= 1, "pssz: invalid bool encoding");
      value = (b != 0);
   }

   template <typename F, PsszNumeric T>
   void from_pssz(T& value, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      check(end - pos >= sizeof(T), "pssz underrun reading primitive");
      std::memcpy(&value, src + pos, sizeof(T));
   }

   template <typename F>
   inline void from_pssz(uint256& v, const char* src, std::uint32_t pos,
                          std::uint32_t end)
   {
      check(end - pos >= 32, "pssz underrun reading uint256");
      std::memcpy(&v, src + pos, 32);
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <typename F, std::size_t N>
   void from_pssz(bitvector<N>& v, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      constexpr std::size_t NB = (N + 7) / 8;
      check(end - pos >= NB, "pssz underrun reading bitvector");
      std::memcpy(v.data(), src + pos, NB);
   }

   template <typename F, std::size_t N>
   void from_pssz(std::bitset<N>& bs, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      constexpr std::size_t NB = (N + 7) / 8;
      check(end - pos >= NB, "pssz underrun reading bitset");
      bs.reset();
      for (std::size_t i = 0; i < N; ++i)
      {
         std::uint8_t byte = static_cast<std::uint8_t>(src[pos + (i >> 3)]);
         if (byte & (std::uint8_t{1} << (i & 7)))
            bs.set(i);
      }
   }

   template <typename F, std::size_t MaxN>
   void from_pssz(bitlist<MaxN>& v, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      check(end > pos, "pssz bitlist: empty byte span invalid");
      std::uint32_t span = end - pos;
      std::int32_t last = static_cast<std::int32_t>(span) - 1;
      while (last >= 0 && static_cast<std::uint8_t>(src[pos + last]) == 0)
         --last;
      check(last >= 0, "pssz bitlist: missing delimiter bit");
      std::uint8_t last_byte = static_cast<std::uint8_t>(src[pos + last]);
      std::uint8_t high = 7;
      while (!(last_byte & (std::uint8_t{1} << high)))
         --high;
      std::size_t bit_count = static_cast<std::size_t>(last) * 8 + high;
      check(bit_count <= MaxN, "pssz bitlist overflow on decode");
      std::vector<std::uint8_t> tmp(static_cast<std::size_t>(span), 0);
      std::memcpy(tmp.data(), src + pos, span);
      tmp[last] &= static_cast<std::uint8_t>((std::uint8_t{1} << high) - 1);
      v.assign_raw(bit_count, tmp.data());
   }

   // ── std::array<T, N> ──────────────────────────────────────────────────────

   template <typename F, typename T, std::size_t N>
   void from_pssz(std::array<T, N>& arr, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      if constexpr (pssz_is_fixed_size_v<T> && has_bitwise_serialization<T>())
      {
         constexpr std::size_t byte_count = N * sizeof(T);
         check(end - pos >= byte_count, "pssz array: underrun");
         if constexpr (N > 0)
            std::memcpy(arr.data(), src + pos, byte_count);
      }
      else if constexpr (pssz_is_fixed_size_v<T>)
      {
         constexpr std::size_t fs = pssz_fixed_size<T>::value;
         check(end - pos >= N * fs, "pssz array: underrun");
         for (std::size_t i = 0; i < N; ++i)
            from_pssz<F>(arr[i], src, pos + i * fs, pos + (i + 1) * fs);
      }
      else
      {
         using off_t                   = typename F::offset_type;
         constexpr std::size_t ob       = F::offset_bytes;
         std::uint32_t span = end - pos;
         check(span >= N * ob, "pssz array: offset table underrun");
         std::array<std::uint32_t, N> offs{};
         for (std::size_t i = 0; i < N; ++i)
         {
            off_t o = 0;
            std::memcpy(&o, src + pos + i * ob, ob);
            offs[i] = static_cast<std::uint32_t>(o);
         }
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t beg  = pos + offs[i];
            std::uint32_t stop = (i + 1 < N) ? (pos + offs[i + 1]) : end;
            from_pssz<F>(arr[i], src, beg, stop);
         }
      }
   }

   // ── std::vector<T> ────────────────────────────────────────────────────────

   template <typename F, typename T>
   void from_pssz(std::vector<T>& v, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      std::uint32_t span = end - pos;
      using off_t                   = typename F::offset_type;
      constexpr std::size_t ob       = F::offset_bytes;

      if constexpr (pssz_is_fixed_size_v<T> && has_bitwise_serialization<T>())
      {
         check(span % sizeof(T) == 0,
               "pssz: vector span not divisible by element size");
         std::size_t n = span / sizeof(T);
         const T* first = reinterpret_cast<const T*>(src + pos);
         v.assign(first, first + n);  // skip zero-init; see project_vector_resize_zero_init
      }
      else if constexpr (pssz_is_fixed_size_v<T>)
      {
         constexpr std::size_t fs = pssz_fixed_size<T>::value;
         check(span % fs == 0, "pssz: vector span not divisible by element size");
         std::size_t n = span / fs;
         v.resize(n);
         for (std::size_t i = 0; i < n; ++i)
            from_pssz<F>(v[i], src, pos + i * fs, pos + (i + 1) * fs);
      }
      else
      {
         if (span == 0) { v.clear(); return; }
         check(span >= ob, "pssz: vector too short for first offset");
         off_t first_off = 0;
         std::memcpy(&first_off, src + pos, ob);
         check(first_off % ob == 0 && first_off <= span,
               "pssz: invalid first offset in vector");
         std::size_t n = first_off / ob;
         std::vector<std::uint32_t> offs(n);
         check(span >= n * ob, "pssz: truncated offset table");
         for (std::size_t i = 0; i < n; ++i)
         {
            off_t o = 0;
            std::memcpy(&o, src + pos + i * ob, ob);
            offs[i] = static_cast<std::uint32_t>(o);
         }
         v.resize(n);
         for (std::size_t i = 0; i < n; ++i)
         {
            std::uint32_t beg  = pos + offs[i];
            std::uint32_t stop = (i + 1 < n) ? (pos + offs[i + 1]) : end;
            check(beg >= pos && stop <= end && beg <= stop,
                  "pssz: vector element offset out of range");
            from_pssz<F>(v[i], src, beg, stop);
         }
      }
   }

   template <typename F, typename T, std::size_t N>
   void from_pssz(bounded_list<T, N>& v, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      from_pssz<F>(v.storage(), src, pos, end);
      check(v.size() <= N, "pssz: bounded_list overflow");
   }

   // ── Strings ───────────────────────────────────────────────────────────────

   template <typename F>
   inline void from_pssz(std::string& s, const char* src, std::uint32_t pos,
                          std::uint32_t end)
   {
      s.assign(src + pos, src + end);
   }
   template <typename F, std::size_t N>
   void from_pssz(bounded_string<N>& s, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      std::uint32_t span = end - pos;
      check(span <= N, "pssz: bounded_string overflow");
      s.storage().assign(src + pos, src + end);
   }

   // ── std::optional<T> ──────────────────────────────────────────────────────

   template <typename F, typename T>
   void from_pssz(std::optional<T>& opt, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      std::uint32_t span = end - pos;
      if constexpr (pssz_optional_needs_selector<T>)
      {
         check(span >= 1, "pssz: optional selector byte missing");
         std::uint8_t selector = static_cast<std::uint8_t>(src[pos]);
         if (selector == 0)
         {
            check(span == 1,
                  "pssz: optional selector 0 must have empty payload");
            opt.reset();
            return;
         }
         check(selector == 1, "pssz: invalid optional selector byte");
         opt.emplace();
         from_pssz<F>(*opt, src, pos + 1, end);
      }
      else
      {
         // No selector: span == 0 → None, span > 0 → Some.
         if (span == 0)
         {
            opt.reset();
            return;
         }
         opt.emplace();
         from_pssz<F>(*opt, src, pos, end);
      }
   }

   // ── Reflected container ───────────────────────────────────────────────────

   template <typename F, typename T>
      requires(Reflected<T> && !is_bitvector_v<T> && !is_bitlist_v<T> &&
               !is_std_bitset_v<T>)
   void from_pssz(T& value, const char* src, std::uint32_t pos,
                  std::uint32_t end)
   {
      using tuple_t                      = struct_tuple_t<T>;
      constexpr std::size_t NFIELDS       = std::tuple_size_v<tuple_t>;
      constexpr std::size_t hdr_size      = pssz_detail::fixed_header_size<F, T>();
      using off_t                         = typename F::offset_type;
      using hdr_t                         = typename F::header_type;
      constexpr std::size_t hdr_bytes     = F::header_bytes;
      constexpr std::size_t ob            = F::offset_bytes;
      constexpr bool        dwnc          = pssz_detail::is_dwnc<T>();

      if constexpr (dwnc && layout_detail::is_memcpy_layout_struct<T>() &&
                     pssz_detail::all_fields_fixed<T>())
      {
         check(end - pos >= sizeof(T), "pssz DWNC: underrun");
         std::memcpy(&value, src + pos, sizeof(T));
         return;
      }

      std::uint32_t field_pos = pos;

      if constexpr (!dwnc)
      {
         check(end - pos >= hdr_bytes, "pssz: not enough bytes for header");
         hdr_t wire_hdr = 0;
         std::memcpy(&wire_hdr, src + pos, hdr_bytes);
         // wire_hdr is the writer's fixed_size. We trust it as the extent
         // of the fixed region; trailing fields past it default-construct.
         field_pos += hdr_bytes;
         (void)wire_hdr;  // may be used later for extensibility handling
      }

      const std::uint32_t fixed_start = field_pos;
      const std::uint32_t fixed_end   = fixed_start + static_cast<std::uint32_t>(hdr_size);
      check(end >= fixed_end, "pssz: not enough bytes for fixed region");

      if constexpr (pssz_detail::all_fields_fixed<T>())
      {
         psio::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member)
             {
                auto read = [&](auto m)
                {
                   using FT                  = std::remove_cvref_t<decltype(value.*m)>;
                   constexpr std::size_t fs  = pssz_fixed_size<FT>::value;
                   from_pssz<F>(value.*m, src, field_pos, field_pos + fs);
                   field_pos += fs;
                };
                (read(member), ...);
             });
         return;
      }

      // Mixed: first pass — gather variable-field offsets in field order.
      std::array<std::uint32_t, NFIELDS> offsets{};
      std::array<bool, NFIELDS>          is_variable{};
      std::uint32_t                      scan_pos = fixed_start;
      std::size_t                        idx = 0;
      psio::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [&](auto... member)
          {
             auto scan = [&](auto m)
             {
                using FT = std::remove_cvref_t<decltype(value.*m)>;
                if constexpr (pssz_is_fixed_size_v<FT>)
                {
                   constexpr std::size_t fs = pssz_fixed_size<FT>::value;
                   scan_pos += fs;
                }
                else
                {
                   is_variable[idx] = true;
                   off_t o = 0;
                   std::memcpy(&o, src + scan_pos, ob);
                   offsets[idx] = static_cast<std::uint32_t>(o);
                   scan_pos += ob;
                }
                ++idx;
             };
             (scan(member), ...);
          });

      // Container-relative offsets: actual position = fixed_start + offset.
      // Derive each variable field's end from the NEXT variable offset (or
      // the container end).
      std::uint32_t container_end_rel = end - fixed_start;
      std::array<std::uint32_t, NFIELDS> var_end_rel{};
      std::uint32_t next_start = container_end_rel;
      for (std::size_t i = NFIELDS; i-- > 0;)
      {
         if (is_variable[i])
         {
            var_end_rel[i] = next_start;
            next_start     = offsets[i];
         }
      }

      // Second pass: decode each field.
      field_pos = fixed_start;
      idx       = 0;
      psio::apply_members(
          (typename reflect<T>::data_members*)nullptr,
          [&](auto... member)
          {
             auto decode = [&](auto m)
             {
                using FT = std::remove_cvref_t<decltype(value.*m)>;
                if constexpr (pssz_is_fixed_size_v<FT>)
                {
                   constexpr std::size_t fs = pssz_fixed_size<FT>::value;
                   from_pssz<F>(value.*m, src, field_pos, field_pos + fs);
                   field_pos += fs;
                }
                else
                {
                   std::uint32_t beg  = fixed_start + offsets[idx];
                   std::uint32_t stop = fixed_start + var_end_rel[idx];
                   from_pssz<F>(value.*m, src, beg, stop);
                   field_pos += ob;
                }
                ++idx;
             };
             (decode(member), ...);
          });
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename F = frac_format_pssz32, typename T>
   void convert_from_pssz(T& value, std::span<const char> bytes)
   {
      check(bytes.size() <= std::numeric_limits<std::uint32_t>::max(),
            "pssz buffer too large");
      from_pssz<F>(value, bytes.data(), 0,
                    static_cast<std::uint32_t>(bytes.size()));
   }

   template <typename F = frac_format_pssz32, typename T>
   T convert_from_pssz(std::span<const char> bytes)
   {
      T value{};
      convert_from_pssz<F>(value, bytes);
      return value;
   }

   template <typename F = frac_format_pssz32, typename T>
   T convert_from_pssz(const std::vector<char>& bytes)
   {
      return convert_from_pssz<F, T>(
          std::span<const char>(bytes.data(), bytes.size()));
   }

}  // namespace psio
