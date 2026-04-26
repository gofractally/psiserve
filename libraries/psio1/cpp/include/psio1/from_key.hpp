#pragma once

// Template-driven key format deserialization.
//
// Reverses the encoding produced by to_key.hpp, reconstructing
// native C++ types from memcmp-sortable byte sequences.
// All from_key overloads take input_stream& for direct buffer access,
// enabling memchr-based bulk scanning of null-escaped data.

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <psio1/reflect.hpp>
#include <psio1/stream.hpp>

namespace psio1
{
   // Forward declarations — all use input_stream for direct buffer access
   void from_key(std::string& obj, input_stream& stream);
   template <typename T>
   void from_key(std::optional<T>& obj, input_stream& stream);
   template <typename T>
   void from_key(std::vector<T>& obj, input_stream& stream);
   template <typename... Ts>
   void from_key(std::variant<Ts...>& obj, input_stream& stream);
   template <typename First, typename Second>
   void from_key(std::pair<First, Second>& obj, input_stream& stream);
   template <typename... Ts>
   void from_key(std::tuple<Ts...>& obj, input_stream& stream);
   template <typename T>
   void from_key(T& obj, input_stream& stream);

   // ── Helpers ──────────────────────────────────────────────────────────

   namespace key_detail
   {
      // Read N-byte big-endian unsigned integer (single read + bswap)
      template <typename T>
      T read_big_endian(input_stream& stream)
      {
         T v;
         stream.read(reinterpret_cast<char*>(&v), sizeof(T));
#if __BYTE_ORDER__ != __ORDER_BIG_ENDIAN__
         if constexpr (sizeof(T) == 1)
            return v;
         else if constexpr (sizeof(T) == 2)
            return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v)));
         else if constexpr (sizeof(T) == 4)
            return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v)));
         else if constexpr (sizeof(T) == 8)
            return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v)));
#else
         return v;
#endif
      }

      // Read null-escaped bytes until \0\0 terminator.
      // Uses memchr to scan for nulls in bulk rather than byte-by-byte.
      inline void read_escaped(std::string& out, input_stream& stream)
      {
         out.clear();
         while (stream.pos < stream.end)
         {
            auto* null_pos =
                static_cast<const char*>(std::memchr(stream.pos, '\0', stream.end - stream.pos));
            if (!null_pos)
               abort_error(stream_error::overrun);  // unterminated key
            // Bulk-append non-null run
            if (null_pos > stream.pos)
               out.append(stream.pos, null_pos - stream.pos);
            stream.pos = null_pos + 1;
            // Check byte after null
            if (stream.pos >= stream.end)
               abort_error(stream_error::overrun);
            char next = *stream.pos++;
            if (next == '\0')
               return;  // \0\0 = terminator
            // \0\1 = escaped null byte
            out.push_back('\0');
         }
         abort_error(stream_error::overrun);
      }
   }  // namespace key_detail

   // ── Variant dispatch helper ──────────────────────────────────────────

   namespace key_detail
   {
      template <uint32_t I, typename... Ts>
      void variant_from_key(std::variant<Ts...>& v, uint32_t i, input_stream& stream)
      {
         if constexpr (I < sizeof...(Ts))
         {
            if (i == I)
            {
               auto& x = v.template emplace<I>();
               from_key(x, stream);
            }
            else
            {
               variant_from_key<I + 1>(v, i, stream);
            }
         }
         else
         {
            abort_error(stream_error::bad_variant_index);
         }
      }
   }  // namespace key_detail

   // ── Catch-all: scalars + reflected structs ───────────────────────────

   template <typename T>
   void from_key(T& obj, input_stream& stream)
   {
      if constexpr (std::is_same_v<T, bool>)
      {
         char ch;
         stream.read(&ch, 1);
         obj = (ch != 0);
      }
      else if constexpr (std::is_same_v<T, float>)
      {
         uint32_t bits = key_detail::read_big_endian<uint32_t>(stream);
         if (bits & 0x80000000u)
            bits ^= 0x80000000u;  // was positive: un-flip sign bit
         else
            bits = ~bits;  // was negative: un-flip all bits
         std::memcpy(&obj, &bits, 4);
      }
      else if constexpr (std::is_same_v<T, double>)
      {
         uint64_t bits = key_detail::read_big_endian<uint64_t>(stream);
         if (bits & 0x8000000000000000ull)
            bits ^= 0x8000000000000000ull;
         else
            bits = ~bits;
         std::memcpy(&obj, &bits, 8);
      }
      else if constexpr (std::is_enum_v<T>)
      {
         std::underlying_type_t<T> v;
         from_key(v, stream);
         obj = static_cast<T>(v);
      }
      else if constexpr (std::is_unsigned_v<T> && std::is_integral_v<T>)
      {
         obj = key_detail::read_big_endian<T>(stream);
      }
      else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>)
      {
         using U = std::make_unsigned_t<T>;
         U uv   = key_detail::read_big_endian<U>(stream);
         uv ^= (U(1) << (sizeof(T) * 8 - 1));  // un-flip sign bit
         obj = static_cast<T>(uv);
      }
      else
      {
         // Reflected struct
         apply_members((typename reflect<T>::data_members*)nullptr,
                       [&](auto... member) { (from_key(obj.*member, stream), ...); });
      }
   }

   // ── Strings ──────────────────────────────────────────────────────────

   inline void from_key(std::string& obj, input_stream& stream)
   {
      key_detail::read_escaped(obj, stream);
   }

   // ── Optionals ────────────────────────────────────────────────────────

   template <typename T>
   void from_key(std::optional<T>& obj, input_stream& stream)
   {
      char marker;
      stream.read(&marker, 1);
      if (marker == '\0')
      {
         obj.reset();
      }
      else
      {
         obj.emplace();
         from_key(*obj, stream);
      }
   }

   // ── Vectors ──────────────────────────────────────────────────────────

   template <typename T>
   void from_key(std::vector<T>& obj, input_stream& stream)
   {
      obj.clear();
      if constexpr (key_detail::is_octet_v<T>)
      {
         // Octet containers: use memchr to find nulls, bulk-copy runs between them
         while (stream.pos < stream.end)
         {
            auto* null_pos = static_cast<const char*>(
                std::memchr(stream.pos, '\0', stream.end - stream.pos));
            if (!null_pos)
               abort_error(stream_error::overrun);
            // Bulk-append non-null run
            size_t run = null_pos - stream.pos;
            if (run > 0)
            {
               if constexpr (std::is_same_v<T, int8_t>)
               {
                  // Need to un-flip sign bit on each byte
                  size_t base = obj.size();
                  obj.resize(base + run);
                  for (size_t i = 0; i < run; ++i)
                     obj[base + i] =
                         static_cast<int8_t>(static_cast<uint8_t>(stream.pos[i]) ^ 0x80);
               }
               else
               {
                  // uint8_t/char: identity — bulk append
                  obj.insert(obj.end(), reinterpret_cast<const T*>(stream.pos),
                             reinterpret_cast<const T*>(null_pos));
               }
            }
            stream.pos = null_pos + 1;
            if (stream.pos >= stream.end)
               abort_error(stream_error::overrun);
            char next = *stream.pos++;
            if (next == '\0')
               return;  // \0\0 = terminator
            // \0\1 = escaped null: the key-encoded byte was \0
            if constexpr (std::is_same_v<T, int8_t>)
               obj.push_back(static_cast<int8_t>(0x80));  // 0x00 ^ 0x80
            else
               obj.push_back(static_cast<T>(0));
         }
         abort_error(stream_error::overrun);
      }
      else
      {
         // Non-octet: \1 prefix per element, \0 terminator
         while (true)
         {
            char marker;
            stream.read(&marker, 1);
            if (marker == '\0')
               return;
            // marker == \1: element present
            obj.emplace_back();
            from_key(obj.back(), stream);
         }
      }
   }

   // ── Variants ─────────────────────────────────────────────────────────

   template <typename... Ts>
   void from_key(std::variant<Ts...>& obj, input_stream& stream)
   {
      char idx;
      stream.read(&idx, 1);
      key_detail::variant_from_key<0>(obj, static_cast<uint32_t>(static_cast<uint8_t>(idx)),
                                      stream);
   }

   // ── Tuples / Pairs ───────────────────────────────────────────────────

   template <typename First, typename Second>
   void from_key(std::pair<First, Second>& obj, input_stream& stream)
   {
      from_key(obj.first, stream);
      from_key(obj.second, stream);
   }

   namespace key_detail
   {
      template <size_t I = 0, typename... Ts>
      void from_key_tuple(std::tuple<Ts...>& obj, input_stream& stream)
      {
         if constexpr (I < sizeof...(Ts))
         {
            from_key(std::get<I>(obj), stream);
            from_key_tuple<I + 1>(obj, stream);
         }
      }
   }  // namespace key_detail

   template <typename... Ts>
   void from_key(std::tuple<Ts...>& obj, input_stream& stream)
   {
      key_detail::from_key_tuple(obj, stream);
   }

   // ── Convenience ──────────────────────────────────────────────────────

   template <typename T>
   T from_key(input_stream& stream)
   {
      T obj;
      from_key(obj, stream);
      return obj;
   }

   template <typename T>
   T convert_from_key(const std::vector<char>& key)
   {
      input_stream stream(key.data(), key.size());
      return from_key<T>(stream);
   }

   template <typename T>
   void convert_from_key(T& obj, const std::vector<char>& key)
   {
      input_stream stream(key.data(), key.size());
      from_key(obj, stream);
   }

}  // namespace psio1
