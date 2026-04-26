#pragma once

// ──────────────────────────────────────────────────────────────────────
// to_json_fast.hpp — Compile-time JSON template serialization
//
// For reflected structs, the JSON output is mostly static text (braces,
// field names, colons, commas). Only the values change at runtime.
// This file precomputes all static fragments at compile time, then at
// runtime does a single reserve() + sequential memcpy/format calls.
//
// Key optimizations vs to_json.hpp:
//   - No double-pass (size_stream + fixed_buf_stream)
//   - No per-codepoint UTF-8 validation for field names
//   - Fast-path memcpy for strings that need no escaping
//   - Direct integer formatting (no snprintf)
//   - Single reserve(), write directly into std::string buffer
// ──────────────────────────────────────────────────────────────────────

#include <psio1/fpconv.h>
#include <psio1/reflect.hpp>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio1
{
namespace json_fast
{

   // ── Fast integer formatting ──────────────────────────────────────────

   // Lookup table for two-digit pairs "00".."99"
   struct digit_pair_table
   {
      char pairs[200];
      constexpr digit_pair_table()
          : pairs{}
      {
         for (int i = 0; i < 100; ++i)
         {
            pairs[i * 2]     = static_cast<char>('0' + i / 10);
            pairs[i * 2 + 1] = static_cast<char>('0' + i % 10);
         }
      }
   };

   inline constexpr digit_pair_table g_digit_pairs{};

   // Write an unsigned integer directly into buf, return number of chars written.
   inline int write_u64(char* buf, uint64_t v)
   {
      if (v == 0)
      {
         buf[0] = '0';
         return 1;
      }

      char  tmp[20];
      char* p = tmp + 20;

      while (v >= 100)
      {
         auto const r = static_cast<unsigned>(v % 100);
         v /= 100;
         p -= 2;
         std::memcpy(p, g_digit_pairs.pairs + r * 2, 2);
      }

      if (v >= 10)
      {
         p -= 2;
         std::memcpy(p, g_digit_pairs.pairs + static_cast<unsigned>(v) * 2, 2);
      }
      else
      {
         *--p = static_cast<char>('0' + v);
      }

      int len = static_cast<int>(tmp + 20 - p);
      std::memcpy(buf, p, static_cast<size_t>(len));
      return len;
   }

   // Write a signed int64 into buf, return number of chars written.
   inline int write_i64(char* buf, int64_t v)
   {
      if (v >= 0)
         return write_u64(buf, static_cast<uint64_t>(v));
      buf[0] = '-';
      return 1 + write_u64(buf + 1, static_cast<uint64_t>(-(v + 1)) + 1);
   }

   // ── String escaping fast path ────────────────────────────────────────

   inline bool needs_escape(unsigned char c)
   {
      return c < 0x20 || c == '"' || c == '\\' || c == 0x7F;
   }

   // Scan a string for any bytes needing escape.
   inline bool string_is_safe(const char* data, size_t len)
   {
      size_t i = 0;

      // Process 8 bytes at a time
      for (; i + 8 <= len; i += 8)
      {
         uint64_t chunk;
         std::memcpy(&chunk, data + i, 8);

         // Check for bytes < 0x20 (control chars)
         uint64_t sub  = chunk - 0x2020202020202020ULL;
         uint64_t mask = ~chunk & sub & 0x8080808080808080ULL;
         if (mask)
            return false;

         // Check for '"' (0x22), '\\' (0x5C), DEL (0x7F)
         uint64_t xor_quote  = chunk ^ 0x2222222222222222ULL;
         uint64_t xor_bslash = chunk ^ 0x5C5C5C5C5C5C5C5CULL;
         uint64_t xor_del    = chunk ^ 0x7F7F7F7F7F7F7F7FULL;

         constexpr uint64_t lo = 0x0101010101010101ULL;
         constexpr uint64_t hi = 0x8080808080808080ULL;

         if (((xor_quote - lo) & ~xor_quote & hi) |
             ((xor_bslash - lo) & ~xor_bslash & hi) |
             ((xor_del - lo) & ~xor_del & hi))
            return false;
      }

      for (; i < len; ++i)
      {
         if (needs_escape(static_cast<unsigned char>(data[i])))
            return false;
      }
      return true;
   }

   // Write an escaped JSON string value (with quotes) into buf.
   inline char* write_escaped_string(char* buf, const char* data, size_t len)
   {
      static constexpr char hex_chars[] = "0123456789ABCDEF";

      *buf++ = '"';
      for (size_t i = 0; i < len; ++i)
      {
         unsigned char c = static_cast<unsigned char>(data[i]);
         if (__builtin_expect(!needs_escape(c), 1))
         {
            *buf++ = static_cast<char>(c);
         }
         else
         {
            *buf++ = '\\';
            switch (c)
            {
               case '"':
                  *buf++ = '"';
                  break;
               case '\\':
                  *buf++ = '\\';
                  break;
               case '\b':
                  *buf++ = 'b';
                  break;
               case '\f':
                  *buf++ = 'f';
                  break;
               case '\n':
                  *buf++ = 'n';
                  break;
               case '\r':
                  *buf++ = 'r';
                  break;
               case '\t':
                  *buf++ = 't';
                  break;
               default:
                  *buf++ = 'u';
                  *buf++ = '0';
                  *buf++ = '0';
                  *buf++ = hex_chars[c >> 4];
                  *buf++ = hex_chars[c & 0xF];
                  break;
            }
         }
      }
      *buf++ = '"';
      return buf;
   }

   // ── Buffer writer ────────────────────────────────────────────────────

   struct json_writer
   {
      std::string result;
      char*       cursor;

      explicit json_writer(size_t reserve_size)
      {
         result.resize(reserve_size);
         cursor = result.data();
      }

      void write_raw(const char* data, size_t len)
      {
         std::memcpy(cursor, data, len);
         cursor += len;
      }

      void write_char(char c) { *cursor++ = c; }

      std::string finish() &&
      {
         result.resize(static_cast<size_t>(cursor - result.data()));
         return std::move(result);
      }

      void write_string(const char* data, size_t len)
      {
         if (string_is_safe(data, len))
         {
            *cursor++ = '"';
            std::memcpy(cursor, data, len);
            cursor += len;
            *cursor++ = '"';
         }
         else
         {
            cursor = write_escaped_string(cursor, data, len);
         }
      }

      void write_uint64(uint64_t v)
      {
         *cursor++ = '"';
         cursor += write_u64(cursor, v);
         *cursor++ = '"';
      }

      void write_int64(int64_t v)
      {
         *cursor++ = '"';
         cursor += write_i64(cursor, v);
         *cursor++ = '"';
      }

      void write_uint32(uint32_t v) { cursor += write_u64(cursor, v); }
      void write_int32(int32_t v) { cursor += write_i64(cursor, v); }
      void write_uint16(uint16_t v) { cursor += write_u64(cursor, v); }
      void write_int16(int16_t v) { cursor += write_i64(cursor, v); }
      void write_uint8(unsigned char v) { cursor += write_u64(cursor, v); }
      void write_int8(signed char v) { cursor += write_i64(cursor, v); }

      void write_bool(bool v)
      {
         if (v)
         {
            std::memcpy(cursor, "true", 4);
            cursor += 4;
         }
         else
         {
            std::memcpy(cursor, "false", 5);
            cursor += 5;
         }
      }

      void write_double(double v)
      {
         if (__builtin_expect(std::isfinite(v), 1))
         {
            int n = fpconv_dtoa(v, cursor);
            cursor += n;
         }
         else if (v == std::numeric_limits<double>::infinity())
         {
            std::memcpy(cursor, "\"Infinity\"", 10);
            cursor += 10;
         }
         else if (v == -std::numeric_limits<double>::infinity())
         {
            std::memcpy(cursor, "\"-Infinity\"", 11);
            cursor += 11;
         }
         else
         {
            std::memcpy(cursor, "\"NaN\"", 5);
            cursor += 5;
         }
      }

      void write_float(float v) { write_double(static_cast<double>(v)); }
   };

   // ── Helper to get member count from MemberList ───────────────────────

   template <typename ML>
   struct member_list_size;

   template <auto... M>
   struct member_list_size<MemberList<M...>>
   {
      static constexpr size_t value = sizeof...(M);
   };

   // ── Unified type dispatch ────────────────────────────────────────────
   // Using a struct with static methods + if constexpr to handle all types
   // in a single template, avoiding the forward-declaration ordering issues.

   struct json_ops
   {
      // ── estimate_size: compute upper bound on JSON output size ────────

      template <typename T>
      static size_t estimate_size(const T& v)
      {
         using V = std::remove_cvref_t<T>;

         if constexpr (std::is_same_v<V, std::string>)
         {
            return v.size() * 6 + 2;  // worst case: all need \u00XX escaping
         }
         else if constexpr (std::is_same_v<V, std::string_view>)
         {
            return v.size() * 6 + 2;
         }
         else if constexpr (std::is_same_v<V, bool>)
         {
            return 5;  // "false"
         }
         else if constexpr (std::is_same_v<V, double> || std::is_same_v<V, float>)
         {
            return 24;  // fpconv max
         }
         else if constexpr (std::is_same_v<V, uint64_t> || std::is_same_v<V, int64_t>)
         {
            return 22;  // quotes + 20 digits
         }
         else if constexpr (std::is_same_v<V, uint32_t> || std::is_same_v<V, int32_t>)
         {
            return 11;
         }
         else if constexpr (std::is_same_v<V, uint16_t> || std::is_same_v<V, int16_t>)
         {
            return 6;
         }
         else if constexpr (std::is_same_v<V, unsigned char> ||
                            std::is_same_v<V, signed char> || std::is_same_v<V, char>)
         {
            return 4;
         }
         else if constexpr (is_std_optional_v<V>)
         {
            if (v)
               return estimate_size(*v);
            return 4;  // "null"
         }
         else if constexpr (is_std_vector_v<V>)
         {
            size_t s = 2;  // [ ]
            for (auto& item : v)
               s += estimate_size(item) + 1;  // + comma
            return s;
         }
         else if constexpr (reflect<V>::is_defined)
         {
            // Reflected struct
            constexpr size_t static_overhead = compute_struct_overhead<V>();
            size_t           s               = static_overhead;
            for_each_member(
                &v, (typename reflect<V>::data_members*)nullptr,
                [&](const auto& member) { s += estimate_size(member); });
            return s;
         }
         else
         {
            // Fallback: generous estimate
            return 64;
         }
      }

      // ── write_value: write JSON for any supported type ────────────────

      template <typename T>
      static void write(json_writer& w, const T& v)
      {
         using V = std::remove_cvref_t<T>;

         if constexpr (std::is_same_v<V, std::string>)
         {
            w.write_string(v.data(), v.size());
         }
         else if constexpr (std::is_same_v<V, std::string_view>)
         {
            w.write_string(v.data(), v.size());
         }
         else if constexpr (std::is_same_v<V, bool>)
         {
            w.write_bool(v);
         }
         else if constexpr (std::is_same_v<V, double>)
         {
            w.write_double(v);
         }
         else if constexpr (std::is_same_v<V, float>)
         {
            w.write_float(v);
         }
         else if constexpr (std::is_same_v<V, uint64_t>)
         {
            w.write_uint64(v);
         }
         else if constexpr (std::is_same_v<V, int64_t>)
         {
            w.write_int64(v);
         }
         else if constexpr (std::is_same_v<V, uint32_t>)
         {
            w.write_uint32(v);
         }
         else if constexpr (std::is_same_v<V, int32_t>)
         {
            w.write_int32(v);
         }
         else if constexpr (std::is_same_v<V, uint16_t>)
         {
            w.write_uint16(v);
         }
         else if constexpr (std::is_same_v<V, int16_t>)
         {
            w.write_int16(v);
         }
         else if constexpr (std::is_same_v<V, unsigned char>)
         {
            w.write_uint8(v);
         }
         else if constexpr (std::is_same_v<V, signed char>)
         {
            w.write_int8(v);
         }
         else if constexpr (is_std_optional_v<V>)
         {
            if (v)
               write(w, *v);
            else
               w.write_raw("null", 4);
         }
         else if constexpr (is_std_vector_v<V>)
         {
            w.write_char('[');
            bool first = true;
            for (auto& item : v)
            {
               if (!first)
                  w.write_char(',');
               first = false;
               write(w, item);
            }
            w.write_char(']');
         }
         else if constexpr (reflect<V>::is_defined)
         {
            write_struct(w, v);
         }
      }

      // ── write_struct: compile-time fragment approach ──────────────────

      template <typename T>
      static void write_struct(json_writer& w, const T& v)
      {
         constexpr auto names = reflect<T>::data_member_names;

         apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... members)
             {
                size_t idx = 0;
                (
                    [&]
                    {
                       const char* name = names[idx];
                       size_t      nlen = __builtin_strlen(name);

                       if (idx == 0)
                          w.write_char('{');
                       else
                          w.write_char(',');

                       w.write_char('"');
                       w.write_raw(name, nlen);
                       w.write_char('"');
                       w.write_char(':');

                       write(w, v.*members);
                       ++idx;
                    }(),
                    ...);

                if (idx == 0)
                   w.write_char('{');
             });

         w.write_char('}');
      }

      // Compile-time static overhead for struct
      template <typename T>
      static constexpr size_t compute_struct_overhead()
      {
         size_t           total    = 2;  // { }
         constexpr auto   names    = reflect<T>::data_member_names;
         constexpr size_t n_fields = member_list_size<typename reflect<T>::data_members>::value;

         for (size_t i = 0; i < n_fields; ++i)
         {
            size_t      name_len = 0;
            const char* nm       = names[i];
            while (nm[name_len])
               ++name_len;
            total += name_len + 3;  // "name":
            if (i > 0)
               total += 1;  // comma
         }
         return total;
      }
   };

   // ── Public API ───────────────────────────────────────────────────────

   template <typename T>
   std::string to_json_fast(const T& v)
   {
      size_t estimated = json_ops::estimate_size(v);

      json_writer w(estimated);
      json_ops::write(w, v);

      return std::move(w).finish();
   }

}  // namespace json_fast

   using json_fast::to_json_fast;

}  // namespace psio1
