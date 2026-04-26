#pragma once
// Bincode encoding — driven by PSIO1_REFLECT, same types, different wire format.
//
// Bincode is the default binary serialization format for Rust's serde ecosystem.
// This implements bincode v1 (legacy) format, which is the most widely deployed.
//
// Key encoding rules:
//   - Integers:   fixed-width little-endian (u8, u16, u32, u64, i8, i16, i32, i64)
//   - Floats:     raw IEEE 754 little-endian (f32, f64)
//   - Booleans:   single byte (0x00 or 0x01)
//   - Strings:    u64 length + UTF-8 bytes
//   - Bytes/Vec:  u64 length + elements
//   - Option:     u8 tag (0=None, 1=Some) + value if Some
//   - Variant:    u32 index + value
//   - Structs:    fields concatenated in order
//   - Tuples:     fields concatenated in order
//   - Arrays:     elements concatenated (no length prefix — length is type)

#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/detail/run_detector.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/reflect.hpp>
#include <psio1/stream.hpp>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace psio1
{
   // ── Forward declarations ──────────────────────────────────────────────────

   template <typename S>
   void to_bincode(std::string_view sv, S& stream);

   template <typename S>
   void to_bincode(const std::string& s, S& stream);

   template <typename T, typename S>
   void to_bincode(const std::vector<T>& obj, S& stream);

   template <typename T, typename S>
   void to_bincode(const std::optional<T>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_bincode(const std::variant<Ts...>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_bincode(const std::tuple<Ts...>& obj, S& stream);

   template <typename T, typename S>
   void to_bincode(const T& obj, S& stream);

   // ── Scalars (fixed-width little-endian) ───────────────────────────────────

   template <typename S>
   void to_bincode(bool val, S& stream)
   {
      uint8_t b = val ? 1 : 0;
      stream.write(reinterpret_cast<const char*>(&b), 1);
   }

   template <typename S>
   void to_bincode(int8_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(uint8_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(int16_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(uint16_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(int32_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(uint32_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(int64_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(uint64_t val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(float val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(double val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // ── Extended integer types (GCC/Clang __int128, psio1::uint256) ────────────

   template <typename S>
   void to_bincode(unsigned __int128 val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(__int128 val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   template <typename S>
   void to_bincode(const uint256& val, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(&val), sizeof(val));
   }

   // Scoped enums: encode as underlying integer type
   template <typename T, typename S>
      requires std::is_enum_v<T>
   void to_bincode(T val, S& stream)
   {
      auto underlying = static_cast<std::underlying_type_t<T>>(val);
      stream.write(reinterpret_cast<const char*>(&underlying), sizeof(underlying));
   }

   // ── Strings ───────────────────────────────────────────────────────────────

   template <typename S>
   void to_bincode(std::string_view sv, S& stream)
   {
      uint64_t len = sv.size();
      stream.write(reinterpret_cast<const char*>(&len), sizeof(len));
      stream.write(sv.data(), sv.size());
   }

   template <typename S>
   void to_bincode(const std::string& s, S& stream)
   {
      to_bincode(std::string_view{s}, stream);
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N, typename S>
   void to_bincode(const bitvector<N>& v, S& stream)
   {
      stream.write(reinterpret_cast<const char*>(v.data()), bitvector<N>::byte_count);
   }

   template <std::size_t MaxN, typename S>
   void to_bincode(const bitlist<MaxN>& v, S& stream)
   {
      std::uint64_t bit_count = v.size();
      stream.write(reinterpret_cast<const char*>(&bit_count), sizeof(bit_count));
      auto data = v.bytes();
      if (!data.empty())
         stream.write(reinterpret_cast<const char*>(data.data()), data.size());
   }

   // std::bitset<N>: identical wire format to psio1::bitvector<N>.
   template <std::size_t N, typename S>
   void to_bincode(const std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8];
      pack_bitset_bytes(bs, buf);
      stream.write(reinterpret_cast<const char*>(buf), (N + 7) / 8);
   }

   // std::vector<bool>: unbounded bitlist analogue.
   template <typename S>
   void to_bincode(const std::vector<bool>& v, S& stream)
   {
      std::uint64_t bit_count = v.size();
      stream.write(reinterpret_cast<const char*>(&bit_count), sizeof(bit_count));
      auto packed = pack_vector_bool(v);
      if (!packed.empty())
         stream.write(reinterpret_cast<const char*>(packed.data()), packed.size());
   }

   // ── Bounded collections (delegate to std::vector/std::string encoding) ────

   template <typename T, std::size_t N, typename S>
   void to_bincode(const bounded_list<T, N>& val, S& stream)
   {
      to_bincode(val.storage(), stream);
   }

   template <std::size_t N, typename S>
   void to_bincode(const bounded_string<N>& val, S& stream)
   {
      to_bincode(val.storage(), stream);
   }

   // ── Vectors ───────────────────────────────────────────────────────────────

   template <typename T, typename S>
   void to_bincode(const std::vector<T>& obj, S& stream)
   {
      uint64_t len = obj.size();
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
            to_bincode(x, stream);
         }
      }
   }

   // ── Fixed-length arrays ───────────────────────────────────────────────────
   // Bincode: no length prefix, elements concatenated (length is part of type)

   template <typename T, std::size_t N, typename S>
   void to_bincode(const std::array<T, N>& obj, S& stream)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         stream.write(reinterpret_cast<const char*>(obj.data()), N * sizeof(T));
      }
      else
      {
         for (auto& x : obj)
         {
            to_bincode(x, stream);
         }
      }
   }

   // ── Optionals ─────────────────────────────────────────────────────────────
   // u8 tag: 0=None, 1=Some + value

   template <typename T, typename S>
   void to_bincode(const std::optional<T>& obj, S& stream)
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
         to_bincode(*obj, stream);
      }
   }

   // ── Variants ──────────────────────────────────────────────────────────────
   // u32 index + value

   template <typename... Ts, typename S>
   void to_bincode(const std::variant<Ts...>& obj, S& stream)
   {
      uint32_t idx = static_cast<uint32_t>(obj.index());
      stream.write(reinterpret_cast<const char*>(&idx), sizeof(idx));
      std::visit([&](auto& x) { to_bincode(x, stream); }, obj);
   }

   // ── Tuples ────────────────────────────────────────────────────────────────

   template <int i, typename T, typename S>
   void to_bincode_tuple(const T& obj, S& stream)
   {
      if constexpr (i < std::tuple_size_v<T>)
      {
         to_bincode(std::get<i>(obj), stream);
         to_bincode_tuple<i + 1>(obj, stream);
      }
   }

   template <typename... Ts, typename S>
   void to_bincode(const std::tuple<Ts...>& obj, S& stream)
   {
      to_bincode_tuple<0>(obj, stream);
   }

   // ── Pairs ─────────────────────────────────────────────────────────────────

   template <typename First, typename Second, typename S>
   void to_bincode(const std::pair<First, Second>& obj, S& stream)
   {
      to_bincode(obj.first, stream);
      to_bincode(obj.second, stream);
   }

   // ── Structs (via reflection) ──────────────────────────────────────────────

   template <typename T, typename S>
   void to_bincode(const T& obj, S& stream)
   {
      if constexpr (run_detail::has_batchable_run<T>())
      {
         // Contiguous bitwise fields get batched into a single stream.write;
         // non-bitwise fields fall through to per-field to_bincode.
         auto op = [&](auto const& val) { to_bincode(val, stream); };
         run_detail::walk_with_runs(
             obj, stream,
             static_cast<int>(std::tuple_size_v<struct_tuple_t<T>>), op);
      }
      else
      {
         psio1::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member) { (to_bincode(obj.*member, stream), ...); });
      }
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_to_bincode(const T& t, std::vector<char>& bin)
   {
      size_stream ss;
      to_bincode(t, ss);

      auto orig_size = bin.size();
      bin.resize(orig_size + ss.size);
      fixed_buf_stream fbs(bin.data() + orig_size, ss.size);
      to_bincode(t, fbs);
      check(fbs.pos == fbs.end, stream_error::underrun);
   }

   template <typename T>
   std::vector<char> convert_to_bincode(const T& t)
   {
      std::vector<char> result;
      convert_to_bincode(t, result);
      return result;
   }

}  // namespace psio1
