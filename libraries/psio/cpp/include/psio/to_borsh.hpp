#pragma once
// Borsh encoding — driven by PSIO_REFLECT, same types, different wire format.
//
// Borsh (Binary Object Representation Serializer for Hashing) is NEAR's and
// Solana's canonical serialization format. It's a close cousin of bincode with
// three meaningful wire differences:
//
//   * Length prefixes are u32, not u64 (strings, vecs).
//   * Enum/variant discriminants are u8, not u32.
//   * Maps/sets are canonically sorted (we don't ship specialized containers).
//
// Key encoding rules:
//   - Integers:   fixed-width little-endian (u8..u64, i8..i64)
//   - Floats:     raw IEEE 754 little-endian (f32, f64) — spec disallows NaN
//   - Booleans:   single byte (0x00 or 0x01)
//   - Strings:    u32 length + UTF-8 bytes
//   - Bytes/Vec:  u32 length + elements
//   - Option:     u8 tag (0=None, 1=Some) + value if Some
//   - Variant:    u8 index + value
//   - Structs:    fields concatenated in order
//   - Tuples:     fields concatenated in order
//   - Arrays:     elements concatenated (no length prefix — length is type)

#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/detail/run_detector.hpp>
#include <psio/ext_int.hpp>
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
   // ── Forward declarations ──────────────────────────────────────────────────

   template <typename S>
   void to_borsh(std::string_view sv, S& stream);

   template <typename S>
   void to_borsh(const std::string& s, S& stream);

   template <typename T, typename S>
   void to_borsh(const std::vector<T>& obj, S& stream);

   template <typename T, typename S>
   void to_borsh(const std::optional<T>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_borsh(const std::variant<Ts...>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_borsh(const std::tuple<Ts...>& obj, S& stream);

   template <typename T, typename S>
   void to_borsh(const T& obj, S& stream);

   // ── Scalars (fixed-width little-endian) ───────────────────────────────────

   template <typename S>
   void to_borsh(bool val, S& stream)
   {
      uint8_t b = val ? 1 : 0;
      stream.write(reinterpret_cast<const char*>(&b), 1);
   }

   template <typename S>
   void to_borsh(int8_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(uint8_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(int16_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(uint16_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(int32_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(uint32_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(int64_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(uint64_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(float val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(double val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // ── Extended integer types ────────────────────────────────────────────────

   template <typename S>
   void to_borsh(unsigned __int128 val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(__int128 val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_borsh(const uint256& val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // Scoped enums: encode as underlying integer type (same as the underlying int,
   // distinct from tagged-variant discriminants which are always u8 in Borsh).
   template <typename T, typename S>
      requires std::is_enum_v<T>
   void to_borsh(T val, S& stream)
   {
      auto underlying = static_cast<std::underlying_type_t<T>>(val);
      stream.write(reinterpret_cast<const char*>(&underlying), sizeof(underlying));
   }

   // ── Strings ───────────────────────────────────────────────────────────────

   template <typename S>
   void to_borsh(std::string_view sv, S& stream)
   {
      uint32_t len = static_cast<uint32_t>(sv.size());
      stream.write(reinterpret_cast<const char*>(&len), sizeof(len));
      stream.write(sv.data(), sv.size());
   }

   template <typename S>
   void to_borsh(const std::string& s, S& stream)
   {
      to_borsh(std::string_view{s}, stream);
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N, typename S>
   void to_borsh(const bitvector<N>& v, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(v.data()), bitvector<N>::byte_count);
   }

   template <std::size_t MaxN, typename S>
   void to_borsh(const bitlist<MaxN>& v, S& stream)
   {
      std::uint32_t bit_count = static_cast<std::uint32_t>(v.size());
      stream.write(reinterpret_cast<const char*>(&bit_count), sizeof(bit_count));
      auto data = v.bytes();
      if (!data.empty())
         stream.write(reinterpret_cast<const char*>(data.data()), data.size());
   }

   template <std::size_t N, typename S>
   void to_borsh(const std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8];
      pack_bitset_bytes(bs, buf);
      stream.write(reinterpret_cast<const char*>(buf), (N + 7) / 8);
   }

   template <typename S>
   void to_borsh(const std::vector<bool>& v, S& stream)
   {
      std::uint32_t bit_count = static_cast<std::uint32_t>(v.size());
      stream.write(reinterpret_cast<const char*>(&bit_count), sizeof(bit_count));
      auto packed = pack_vector_bool(v);
      if (!packed.empty())
         stream.write(reinterpret_cast<const char*>(packed.data()), packed.size());
   }

   // ── Bounded collections (delegate to std::vector/std::string encoding) ────

   template <typename T, std::size_t N, typename S>
   void to_borsh(const bounded_list<T, N>& val, S& stream)
   {
      to_borsh(val.storage(), stream);
   }

   template <std::size_t N, typename S>
   void to_borsh(const bounded_string<N>& val, S& stream)
   {
      to_borsh(val.storage(), stream);
   }

   // ── Vectors ───────────────────────────────────────────────────────────────

   template <typename T, typename S>
   void to_borsh(const std::vector<T>& obj, S& stream)
   {
      uint32_t len = static_cast<uint32_t>(obj.size());
      stream.write(reinterpret_cast<const char*>(&len), sizeof(len));
      if constexpr (has_bitwise_serialization<T>())
      {
         if (!obj.empty())
            stream.write(reinterpret_cast<const char*>(obj.data()), obj.size() * sizeof(T));
      }
      else
      {
         for (auto& x : obj)
         {
            to_borsh(x, stream);
         }
      }
   }

   // ── Fixed-length arrays ───────────────────────────────────────────────────
   // Borsh: no length prefix, elements concatenated (length is part of type)

   template <typename T, std::size_t N, typename S>
   void to_borsh(const std::array<T, N>& obj, S& stream)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         stream.write(reinterpret_cast<const char*>(obj.data()), N * sizeof(T));
      }
      else
      {
         for (auto& x : obj)
         {
            to_borsh(x, stream);
         }
      }
   }

   // ── Optionals ─────────────────────────────────────────────────────────────
   // u8 tag: 0=None, 1=Some + value

   template <typename T, typename S>
   void to_borsh(const std::optional<T>& obj, S& stream)
   {
      if (!obj)
      {
         uint8_t tag = 0;
         stream.write(reinterpret_cast<const char*>(&tag), 1);
      }
      else
      {
         uint8_t tag = 1;
         stream.write(reinterpret_cast<const char*>(&tag), 1);
         to_borsh(*obj, stream);
      }
   }

   // ── Variants ──────────────────────────────────────────────────────────────
   // u8 discriminant + value (Borsh caps enums at 256 variants)

   template <typename... Ts, typename S>
   void to_borsh(const std::variant<Ts...>& obj, S& stream)
   {
      static_assert(sizeof...(Ts) <= 256, "Borsh enum discriminant is u8");
      uint8_t idx = static_cast<uint8_t>(obj.index());
      stream.write(reinterpret_cast<const char*>(&idx), sizeof(idx));
      std::visit([&](auto& x) { to_borsh(x, stream); }, obj);
   }

   // ── Tuples ────────────────────────────────────────────────────────────────

   template <int i, typename T, typename S>
   void to_borsh_tuple(const T& obj, S& stream)
   {
      if constexpr (i < std::tuple_size_v<T>)
      {
         to_borsh(std::get<i>(obj), stream);
         to_borsh_tuple<i + 1>(obj, stream);
      }
   }

   template <typename... Ts, typename S>
   void to_borsh(const std::tuple<Ts...>& obj, S& stream)
   {
      to_borsh_tuple<0>(obj, stream);
   }

   // ── Pairs ─────────────────────────────────────────────────────────────────

   template <typename First, typename Second, typename S>
   void to_borsh(const std::pair<First, Second>& obj, S& stream)
   {
      to_borsh(obj.first, stream);
      to_borsh(obj.second, stream);
   }

   // ── Structs (via reflection) ──────────────────────────────────────────────

   template <typename T, typename S>
   void to_borsh(const T& obj, S& stream)
   {
      if constexpr (run_detail::has_batchable_run<T>())
      {
         auto op = [&](auto const& val) { to_borsh(val, stream); };
         run_detail::walk_with_runs(
             obj, stream,
             static_cast<int>(std::tuple_size_v<struct_tuple_t<T>>), op);
      }
      else
      {
         psio::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member) { (to_borsh(obj.*member, stream), ...); });
      }
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_to_borsh(const T& t, std::vector<char>& bin)
   {
      size_stream ss;
      to_borsh(t, ss);

      auto orig_size = bin.size();
      bin.resize(orig_size + ss.size);
      fixed_buf_stream fbs(bin.data() + orig_size, ss.size);
      to_borsh(t, fbs);
      check(fbs.pos == fbs.end, stream_error::underrun);
   }

   template <typename T>
   std::vector<char> convert_to_borsh(const T& t)
   {
      std::vector<char> result;
      convert_to_borsh(t, result);
      return result;
   }

}  // namespace psio
