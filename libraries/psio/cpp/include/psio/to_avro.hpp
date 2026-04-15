#pragma once
// Avro binary encoding — driven by PSIO_REFLECT, same types, different wire format.
//
// Avro spec: https://avro.apache.org/docs/current/specification/
//
// Key encoding rules:
//   - Integers:   zig-zag + unsigned varint (same as protobuf sint32/sint64)
//   - Floats:     raw 4-byte little-endian IEEE 754
//   - Doubles:    raw 8-byte little-endian IEEE 754
//   - Booleans:   single byte (0x00 or 0x01)
//   - Strings:    long(length) + UTF-8 bytes   (long = zig-zag varint)
//   - Bytes:      long(length) + raw bytes
//   - Arrays:     one or more blocks of [long(count), items...], terminated by long(0)
//   - Records:    fields concatenated in schema order
//   - Unions:     long(branch index) + value
//   - Enums:      long(ordinal)
//   - Fixed:      raw N bytes (no length prefix)
//   - Optionals:  mapped to union{null, T} — null=index 0, present=index 1
//
// The "long" type in Avro is a signed 64-bit zig-zag varint.

#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace psio
{
   // ── Avro zig-zag varint (signed long) ─────────────────────────────────────

   template <typename S>
   void avro_long_to_bin(int64_t val, S& stream)
   {
      // Zig-zag encode: (val << 1) ^ (val >> 63)
      uint64_t zz = (static_cast<uint64_t>(val) << 1) ^ static_cast<uint64_t>(val >> 63);
      // Unsigned varint
      do
      {
         uint8_t b = zz & 0x7f;
         zz >>= 7;
         b |= ((zz > 0) << 7);
         stream.write(b);
      } while (zz);
   }

   // ── Forward declarations ──────────────────────────────────────────────────

   template <typename S>
   void to_avro(std::string_view sv, S& stream);

   template <typename S>
   void to_avro(const std::string& s, S& stream);

   template <typename T, typename S>
   void to_avro(const std::vector<T>& obj, S& stream);

   template <typename T, typename S>
   void to_avro(const std::optional<T>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_avro(const std::variant<Ts...>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_avro(const std::tuple<Ts...>& obj, S& stream);

   template <typename T, typename S>
   void to_avro(const T& obj, S& stream);

   // ── Scalars ───────────────────────────────────────────────────────────────

   // Boolean: single byte
   template <typename S>
   void to_avro(bool val, S& stream)
   {
      uint8_t b = val ? 1 : 0;
      stream.write(b);
   }

   // Signed integers → zig-zag varint
   template <typename S>
   void to_avro(int8_t val, S& stream)
   {
      avro_long_to_bin(val, stream);
   }

   template <typename S>
   void to_avro(int16_t val, S& stream)
   {
      avro_long_to_bin(val, stream);
   }

   template <typename S>
   void to_avro(int32_t val, S& stream)
   {
      avro_long_to_bin(val, stream);
   }

   template <typename S>
   void to_avro(int64_t val, S& stream)
   {
      avro_long_to_bin(val, stream);
   }

   // Unsigned integers → zig-zag varint (cast to signed, Avro has no unsigned)
   template <typename S>
   void to_avro(uint8_t val, S& stream)
   {
      avro_long_to_bin(static_cast<int64_t>(val), stream);
   }

   template <typename S>
   void to_avro(uint16_t val, S& stream)
   {
      avro_long_to_bin(static_cast<int64_t>(val), stream);
   }

   template <typename S>
   void to_avro(uint32_t val, S& stream)
   {
      avro_long_to_bin(static_cast<int64_t>(val), stream);
   }

   template <typename S>
   void to_avro(uint64_t val, S& stream)
   {
      // Avro has no unsigned 64-bit. Encode as signed (values > INT64_MAX
      // will round-trip correctly through the zig-zag encoding but may
      // confuse Avro consumers that expect signed longs).
      avro_long_to_bin(static_cast<int64_t>(val), stream);
   }

   // Float: raw 4-byte little-endian
   template <typename S>
   void to_avro(float val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // Double: raw 8-byte little-endian
   template <typename S>
   void to_avro(double val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // Scoped enums: encode as zig-zag varint ordinal
   template <typename T, typename S>
      requires std::is_enum_v<T>
   void to_avro(T val, S& stream)
   {
      avro_long_to_bin(static_cast<int64_t>(static_cast<std::underlying_type_t<T>>(val)), stream);
   }

   // ── Strings ───────────────────────────────────────────────────────────────

   template <typename S>
   void to_avro(std::string_view sv, S& stream)
   {
      avro_long_to_bin(static_cast<int64_t>(sv.size()), stream);
      stream.write(sv.data(), sv.size());
   }

   template <typename S>
   void to_avro(const std::string& s, S& stream)
   {
      to_avro(std::string_view{s}, stream);
   }

   // ── Arrays (block encoding) ───────────────────────────────────────────────
   //
   // Avro arrays are encoded as one or more blocks. Each block is:
   //   long(count) followed by count items.
   // A block with count=0 terminates the array.
   // For simplicity, we emit a single block with all items, then a 0-block.

   template <typename T, typename S>
   void to_avro(const std::vector<T>& obj, S& stream)
   {
      if (!obj.empty())
      {
         avro_long_to_bin(static_cast<int64_t>(obj.size()), stream);
         for (auto& x : obj)
         {
            to_avro(x, stream);
         }
      }
      avro_long_to_bin(0, stream);  // terminating 0-count block
   }

   // ── Fixed-length arrays ───────────────────────────────────────────────────
   // Avro "fixed": raw N bytes if element is byte-sized, otherwise treat as
   // array encoding. std::array<T, N> where T is byte-like maps to Avro fixed.

   template <typename T, std::size_t N, typename S>
   void to_avro(const std::array<T, N>& obj, S& stream)
   {
      if constexpr (sizeof(T) == 1 && has_bitwise_serialization<T>())
      {
         // Avro "fixed": raw bytes, no length prefix
         stream.write(reinterpret_cast<const char*>(obj.data()), N);
      }
      else
      {
         // Encode as Avro array
         if (N > 0)
         {
            avro_long_to_bin(static_cast<int64_t>(N), stream);
            for (auto& x : obj)
            {
               to_avro(x, stream);
            }
         }
         avro_long_to_bin(0, stream);
      }
   }

   // ── Optionals → union{null, T} ───────────────────────────────────────────
   // null = branch index 0 (no data follows)
   // Some(T) = branch index 1 + encoded T

   template <typename T, typename S>
   void to_avro(const std::optional<T>& obj, S& stream)
   {
      if (!obj)
      {
         avro_long_to_bin(0, stream);  // null branch
      }
      else
      {
         avro_long_to_bin(1, stream);  // value branch
         to_avro(*obj, stream);
      }
   }

   // ── Variants → union ─────────────────────────────────────────────────────

   template <typename... Ts, typename S>
   void to_avro(const std::variant<Ts...>& obj, S& stream)
   {
      avro_long_to_bin(static_cast<int64_t>(obj.index()), stream);
      std::visit([&](auto& x) { to_avro(x, stream); }, obj);
   }

   // ── Tuples → record (positional) ─────────────────────────────────────────

   template <int i, typename T, typename S>
   void to_avro_tuple(const T& obj, S& stream)
   {
      if constexpr (i < std::tuple_size_v<T>)
      {
         to_avro(std::get<i>(obj), stream);
         to_avro_tuple<i + 1>(obj, stream);
      }
   }

   template <typename... Ts, typename S>
   void to_avro(const std::tuple<Ts...>& obj, S& stream)
   {
      to_avro_tuple<0>(obj, stream);
   }

   // ── Pairs ─────────────────────────────────────────────────────────────────

   template <typename First, typename Second, typename S>
   void to_avro(const std::pair<First, Second>& obj, S& stream)
   {
      to_avro(obj.first, stream);
      to_avro(obj.second, stream);
   }

   // ── Structs (via reflection) ──────────────────────────────────────────────

   template <typename T, typename S>
   void to_avro(const T& obj, S& stream)
   {
      // Records: fields concatenated in schema order, no delimiters
      psio::apply_members((typename reflect<T>::data_members*)nullptr,
                          [&](auto... member) { (to_avro(obj.*member, stream), ...); });
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_to_avro(const T& t, std::vector<char>& bin)
   {
      size_stream ss;
      to_avro(t, ss);

      auto orig_size = bin.size();
      bin.resize(orig_size + ss.size);
      fixed_buf_stream fbs(bin.data() + orig_size, ss.size);
      to_avro(t, fbs);
      check(fbs.pos == fbs.end, stream_error::underrun);
   }

   template <typename T>
   std::vector<char> convert_to_avro(const T& t)
   {
      std::vector<char> result;
      convert_to_avro(t, result);
      return result;
   }

}  // namespace psio
