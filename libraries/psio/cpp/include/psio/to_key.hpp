#pragma once

// Template-driven key format serialization.
//
// Produces memcmp-sortable byte sequences from native C++ types.
// memcmp(to_key(a), to_key(b)) == compare(a, b) for all supported types.
//
// Encoding rules:
//   Unsigned integers:  big-endian
//   Signed integers:    flip sign bit + big-endian
//   Floats/doubles:     IEEE-754 sign transform + big-endian
//   bool:               0x00 (false) < 0x01 (true)
//   Strings:            content with \0→\0\1 escaping + \0\0 terminator
//   Vectors (octet):    same as strings (escape + \0\0 terminator)
//   Vectors (other):    \1 + element per entry, \0 terminator
//   Optionals:          \0 absent, \1 + value present
//   Variants:           1-byte index + value
//   Structs:            fields concatenated in reflection order

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>
#include <vector>

#include <psio/reflect.hpp>
#include <psio/stream.hpp>

namespace psio
{
   // Forward declarations
   template <typename S>
   void to_key(std::string_view sv, S& stream);
   template <typename S>
   void to_key(const std::string& s, S& stream);
   template <typename T, typename S>
   void to_key(const std::optional<T>& obj, S& stream);
   template <typename T, typename S>
   void to_key(const std::vector<T>& obj, S& stream);
   template <typename... Ts, typename S>
   void to_key(const std::variant<Ts...>& obj, S& stream);
   template <typename First, typename Second, typename S>
   void to_key(const std::pair<First, Second>& obj, S& stream);
   template <typename... Ts, typename S>
   void to_key(const std::tuple<Ts...>& obj, S& stream);
   template <typename T, typename S>
   void to_key(const T& obj, S& stream);

   // ── Scalar encoding ──────────────────────────────────────────────────

   namespace key_detail
   {
      // Byte-swap to/from big-endian using compiler intrinsics
      template <typename T>
      T bswap(T v)
      {
         if constexpr (sizeof(T) == 1)
            return v;
         else if constexpr (sizeof(T) == 2)
            return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v)));
         else if constexpr (sizeof(T) == 4)
            return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v)));
         else if constexpr (sizeof(T) == 8)
            return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v)));
      }

      template <typename T>
      T to_big_endian(T v)
      {
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
         return v;
#else
         return bswap(v);
#endif
      }

      // Write N-byte unsigned integer in big-endian order (single write)
      template <typename T, typename S>
      void write_big_endian(T v, S& stream)
      {
         v = to_big_endian(v);
         stream.write(reinterpret_cast<const char*>(&v), sizeof(T));
      }

      template <typename T>
      constexpr bool is_octet_v =
          std::is_same_v<T, uint8_t> || std::is_same_v<T, int8_t> || std::is_same_v<T, char>;

      // Write bytes with null-escaping for octet containers/strings.
      // Each \0 byte in the encoded output is followed by \1.
      // Terminated with \0\0.
      // Uses memchr to find nulls in bulk rather than scanning byte-by-byte.
      template <typename S>
      void write_escaped(const char* data, size_t len, S& stream)
      {
         const char* p   = data;
         const char* end = data + len;
         while (p < end)
         {
            auto* null_pos = static_cast<const char*>(std::memchr(p, '\0', end - p));
            if (!null_pos)
            {
               stream.write(p, end - p);
               break;
            }
            // Write non-null run + escaped null (\0\1) in as few writes as possible
            if (null_pos > p)
               stream.write(p, null_pos - p);
            static constexpr char esc[2] = {'\0', '\1'};
            stream.write(esc, 2);
            p = null_pos + 1;
         }
         static constexpr char term[2] = {'\0', '\0'};
         stream.write(term, 2);
      }
   }  // namespace key_detail

   // ── Catch-all: scalars + reflected structs ───────────────────────────

   template <typename T, typename S>
   void to_key(const T& obj, S& stream)
   {
      if constexpr (std::is_same_v<T, bool>)
      {
         stream.write(static_cast<char>(obj ? 1 : 0));
      }
      else if constexpr (std::is_same_v<T, float>)
      {
         uint32_t bits;
         std::memcpy(&bits, &obj, 4);
         if (bits == 0x80000000u)
            bits = 0;  // -0 → +0
         if (bits & 0x80000000u)
            bits = ~bits;         // negative: flip all bits
         else
            bits ^= 0x80000000u;  // positive: flip sign bit
         key_detail::write_big_endian(bits, stream);
      }
      else if constexpr (std::is_same_v<T, double>)
      {
         uint64_t bits;
         std::memcpy(&bits, &obj, 8);
         if (bits == 0x8000000000000000ull)
            bits = 0;
         if (bits & 0x8000000000000000ull)
            bits = ~bits;
         else
            bits ^= 0x8000000000000000ull;
         key_detail::write_big_endian(bits, stream);
      }
      else if constexpr (std::is_enum_v<T>)
      {
         to_key(static_cast<std::underlying_type_t<T>>(obj), stream);
      }
      else if constexpr (std::is_unsigned_v<T> && std::is_integral_v<T>)
      {
         key_detail::write_big_endian(obj, stream);
      }
      else if constexpr (std::is_signed_v<T> && std::is_integral_v<T>)
      {
         using U = std::make_unsigned_t<T>;
         U uv   = static_cast<U>(obj) ^ (U(1) << (sizeof(T) * 8 - 1));
         key_detail::write_big_endian(uv, stream);
      }
      else
      {
         // Reflected struct: serialize each member in order
         apply_members((typename reflect<T>::data_members*)nullptr,
                       [&](auto... member) { (to_key(obj.*member, stream), ...); });
      }
   }

   // ── Strings ──────────────────────────────────────────────────────────

   template <typename S>
   void to_key(std::string_view sv, S& stream)
   {
      key_detail::write_escaped(sv.data(), sv.size(), stream);
   }

   template <typename S>
   void to_key(const std::string& s, S& stream)
   {
      to_key(std::string_view(s), stream);
   }

   // ── Optionals ────────────────────────────────────────────────────────

   template <typename T, typename S>
   void to_key(const std::optional<T>& obj, S& stream)
   {
      if (obj)
      {
         stream.write('\1');
         to_key(*obj, stream);
      }
      else
      {
         stream.write('\0');
      }
   }

   // ── Vectors ──────────────────────────────────────────────────────────

   template <typename T, typename S>
   void to_key(const std::vector<T>& obj, S& stream)
   {
      if constexpr (key_detail::is_octet_v<T>)
      {
         // Octet containers: encode each element's key byte with null-escaping
         for (const auto& elem : obj)
         {
            // Key-encode the single byte (handles sign flip for int8_t)
            char buf[1];
            fixed_buf_stream fbs(buf, 1);
            to_key(elem, fbs);
            stream.write(buf[0]);
            if (buf[0] == '\0')
               stream.write('\1');
         }
         stream.write('\0');
         stream.write('\0');
      }
      else
      {
         // Non-octet: \1 prefix per element, \0 terminator
         for (const auto& elem : obj)
         {
            stream.write('\1');
            to_key(elem, stream);
         }
         stream.write('\0');
      }
   }

   // ── Variants ─────────────────────────────────────────────────────────

   template <typename... Ts, typename S>
   void to_key(const std::variant<Ts...>& obj, S& stream)
   {
      stream.write(static_cast<char>(obj.index()));
      std::visit([&](const auto& val) { to_key(val, stream); }, obj);
   }

   // ── Tuples / Pairs ───────────────────────────────────────────────────

   template <typename First, typename Second, typename S>
   void to_key(const std::pair<First, Second>& obj, S& stream)
   {
      to_key(obj.first, stream);
      to_key(obj.second, stream);
   }

   namespace key_detail
   {
      template <size_t I = 0, typename... Ts, typename S>
      void to_key_tuple(const std::tuple<Ts...>& obj, S& stream)
      {
         if constexpr (I < sizeof...(Ts))
         {
            to_key(std::get<I>(obj), stream);
            to_key_tuple<I + 1>(obj, stream);
         }
      }
   }  // namespace key_detail

   template <typename... Ts, typename S>
   void to_key(const std::tuple<Ts...>& obj, S& stream)
   {
      key_detail::to_key_tuple(obj, stream);
   }

   // ── Key size (first pass) ───────────────────────────────────────────

   // Forward declarations
   inline size_t key_size(std::string_view sv);
   inline size_t key_size(const std::string& s);
   template <typename T>
   size_t key_size(const std::optional<T>& obj);
   template <typename T>
   size_t key_size(const std::vector<T>& obj);
   template <typename... Ts>
   size_t key_size(const std::variant<Ts...>& obj);
   template <typename First, typename Second>
   size_t key_size(const std::pair<First, Second>& obj);
   template <typename... Ts>
   size_t key_size(const std::tuple<Ts...>& obj);
   template <typename T>
   size_t key_size(const T& obj);

   namespace key_detail
   {
      // Count null bytes in a buffer using memchr
      inline size_t count_nulls(const char* data, size_t len)
      {
         size_t      count = 0;
         const char* p     = data;
         const char* end   = data + len;
         while (p < end &&
                (p = static_cast<const char*>(std::memchr(p, '\0', end - p))) != nullptr)
         {
            ++count;
            ++p;
         }
         return count;
      }
   }  // namespace key_detail

   // Catch-all: scalars + reflected structs
   template <typename T>
   size_t key_size(const T& obj)
   {
      if constexpr (std::is_same_v<T, bool>)
         return 1;
      else if constexpr (std::is_same_v<T, float>)
         return 4;
      else if constexpr (std::is_same_v<T, double>)
         return 8;
      else if constexpr (std::is_enum_v<T>)
         return sizeof(std::underlying_type_t<T>);
      else if constexpr (std::is_integral_v<T>)
         return sizeof(T);
      else
      {
         // Reflected struct
         size_t total = 0;
         apply_members((typename reflect<T>::data_members*)nullptr,
                       [&](auto... member) { ((total += key_size(obj.*member)), ...); });
         return total;
      }
   }

   inline size_t key_size(std::string_view sv)
   {
      // content + one escape byte per null + \0\0 terminator
      return sv.size() + key_detail::count_nulls(sv.data(), sv.size()) + 2;
   }

   inline size_t key_size(const std::string& s)
   {
      return key_size(std::string_view(s));
   }

   template <typename T>
   size_t key_size(const std::optional<T>& obj)
   {
      return obj ? 1 + key_size(*obj) : 1;
   }

   template <typename T>
   size_t key_size(const std::vector<T>& obj)
   {
      if constexpr (key_detail::is_octet_v<T>)
      {
         // Count elements that produce \0 after key encoding
         size_t null_count;
         if constexpr (std::is_signed_v<T>)
         {
            // Signed: sign flip means min value (-128) → 0x00
            null_count = 0;
            for (auto elem : obj)
               null_count += (static_cast<uint8_t>(elem) ^ 0x80) == 0;
         }
         else
         {
            // Unsigned: key-encoded byte is identity
            null_count =
                key_detail::count_nulls(reinterpret_cast<const char*>(obj.data()), obj.size());
         }
         return obj.size() + null_count + 2;  // content + escapes + \0\0
      }
      else
      {
         // Non-octet: \1 prefix per element + element sizes + \0 terminator
         size_t total = 1;
         for (const auto& elem : obj)
            total += 1 + key_size(elem);
         return total;
      }
   }

   template <typename... Ts>
   size_t key_size(const std::variant<Ts...>& obj)
   {
      return 1 + std::visit([](const auto& val) -> size_t { return key_size(val); }, obj);
   }

   template <typename First, typename Second>
   size_t key_size(const std::pair<First, Second>& obj)
   {
      return key_size(obj.first) + key_size(obj.second);
   }

   namespace key_detail
   {
      template <size_t I = 0, typename... Ts>
      size_t key_size_tuple(const std::tuple<Ts...>& obj)
      {
         if constexpr (I < sizeof...(Ts))
            return key_size(std::get<I>(obj)) + key_size_tuple<I + 1>(obj);
         else
            return 0;
      }
   }  // namespace key_detail

   template <typename... Ts>
   size_t key_size(const std::tuple<Ts...>& obj)
   {
      return key_detail::key_size_tuple(obj);
   }

   // ── Convenience ──────────────────────────────────────────────────────

   // Two-pass: key_size() + fixed_buf_stream write (zero reallocation)
   template <typename T>
   std::vector<char> convert_to_key(const T& t)
   {
      size_t            sz = key_size(t);
      std::vector<char> result(sz);
      fixed_buf_stream  fbs(result.data(), sz);
      to_key(t, fbs);
      return result;
   }

}  // namespace psio
