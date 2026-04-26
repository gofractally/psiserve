#pragma once
// Bincode decoding — driven by PSIO1_REFLECT, same types, different wire format.

#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/reflect.hpp>
#include <psio1/stream.hpp>
#include <array>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace psio1
{
   // ── Forward declarations ──────────────────────────────────────────────────

   template <typename T, typename S>
   void from_bincode(T& obj, S& stream);

   // ── Scalars (fixed-width little-endian) ───────────────────────────────────

   template <typename S>
   void from_bincode(bool& val, S& stream)
   {
      uint8_t b;
      stream.read(reinterpret_cast<char*>(&b), 1);
      val = (b != 0);
   }

   template <typename S>
   void from_bincode(int8_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(uint8_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(int16_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(uint16_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(int32_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(uint32_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(int64_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(uint64_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(float& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(double& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   // ── Extended integer types ────────────────────────────────────────────────

   template <typename S>
   void from_bincode(unsigned __int128& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(__int128& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_bincode(uint256& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   // Scoped enums
   template <typename T, typename S>
      requires std::is_enum_v<T>
   void from_bincode(T& val, S& stream)
   {
      std::underlying_type_t<T> underlying;
      stream.read(reinterpret_cast<char*>(&underlying), sizeof(underlying));
      val = static_cast<T>(underlying);
   }

   // ── Strings ───────────────────────────────────────────────────────────────

   template <typename S>
   void from_bincode(std::string& obj, S& stream)
   {
      uint64_t len;
      stream.read(reinterpret_cast<char*>(&len), sizeof(len));
      read_string_bulk(obj, stream, static_cast<std::size_t>(len));
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N, typename S>
   void from_bincode(bitvector<N>& v, S& stream)
   {
      stream.read(reinterpret_cast<char*>(v.data()), bitvector<N>::byte_count);
   }

   template <std::size_t MaxN, typename S>
   void from_bincode(bitlist<MaxN>& v, S& stream)
   {
      std::uint64_t bit_count = 0;
      stream.read(reinterpret_cast<char*>(&bit_count), sizeof(bit_count));
      check(bit_count <= MaxN, "bitlist overflow on decode");
      std::size_t byte_count = (static_cast<std::size_t>(bit_count) + 7) / 8;
      std::vector<std::uint8_t> tmp(byte_count);
      if (byte_count)
         stream.read(reinterpret_cast<char*>(tmp.data()), byte_count);
      v.assign_raw(static_cast<std::size_t>(bit_count), tmp.data());
   }

   template <std::size_t N, typename S>
   void from_bincode(std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8];
      stream.read(reinterpret_cast<char*>(buf), (N + 7) / 8);
      unpack_bitset_bytes(buf, bs);
   }

   template <typename S>
   void from_bincode(std::vector<bool>& v, S& stream)
   {
      std::uint64_t bit_count = 0;
      stream.read(reinterpret_cast<char*>(&bit_count), sizeof(bit_count));
      std::size_t byte_count = (static_cast<std::size_t>(bit_count) + 7) / 8;
      std::vector<std::uint8_t> tmp(byte_count);
      if (byte_count)
         stream.read(reinterpret_cast<char*>(tmp.data()), byte_count);
      unpack_vector_bool(tmp.data(), static_cast<std::size_t>(bit_count), v);
   }

   // ── Bounded collections (validate bound on decode) ────────────────────────

   template <typename T, std::size_t N, typename S>
   void from_bincode(bounded_list<T, N>& val, S& stream)
   {
      from_bincode(val.storage(), stream);
      check(val.size() <= N, "bounded_list overflow on decode");
   }

   template <std::size_t N, typename S>
   void from_bincode(bounded_string<N>& val, S& stream)
   {
      from_bincode(val.storage(), stream);
      check(val.size() <= N, "bounded_string overflow on decode");
   }

   // ── Vectors ───────────────────────────────────────────────────────────────

   template <typename T, typename S>
   void from_bincode(std::vector<T>& v, S& stream)
   {
      uint64_t len;
      stream.read(reinterpret_cast<char*>(&len), sizeof(len));
      if constexpr (has_bitwise_serialization<T>())
      {
         read_vector_bitwise(v, stream, static_cast<std::size_t>(len));
      }
      else
      {
         v.resize(static_cast<size_t>(len));
         for (size_t i = 0; i < static_cast<size_t>(len); ++i)
         {
            from_bincode(v[i], stream);
         }
      }
   }

   // ── Fixed-length arrays ───────────────────────────────────────────────────

   template <typename T, std::size_t N, typename S>
   void from_bincode(std::array<T, N>& obj, S& stream)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         stream.read(reinterpret_cast<char*>(obj.data()), N * sizeof(T));
      }
      else
      {
         for (T& elem : obj)
         {
            from_bincode(elem, stream);
         }
      }
   }

   // ── Optionals ─────────────────────────────────────────────────────────────

   template <typename T, typename S>
   void from_bincode(std::optional<T>& obj, S& stream)
   {
      uint8_t tag;
      stream.read(reinterpret_cast<char*>(&tag), 1);
      if (tag == 0)
      {
         obj.reset();
      }
      else
      {
         obj.emplace();
         from_bincode(*obj, stream);
      }
   }

   // ── Variants ──────────────────────────────────────────────────────────────

   template <uint32_t I, typename... Ts, typename S>
   void bincode_variant_from(std::variant<Ts...>& v, uint32_t i, S& stream)
   {
      if constexpr (I < sizeof...(Ts))
      {
         if (I == i)
         {
            auto& x = v.template emplace<I>();
            from_bincode(x, stream);
         }
         else
         {
            bincode_variant_from<I + 1>(v, i, stream);
         }
      }
      else
      {
         abort_error(stream_error::bad_variant_index);
      }
   }

   template <typename... Ts, typename S>
   void from_bincode(std::variant<Ts...>& obj, S& stream)
   {
      uint32_t idx;
      stream.read(reinterpret_cast<char*>(&idx), sizeof(idx));
      bincode_variant_from<0>(obj, idx, stream);
   }

   // ── Tuples ────────────────────────────────────────────────────────────────

   template <int i, typename T, typename S>
   void from_bincode_tuple(T& obj, S& stream)
   {
      if constexpr (i < std::tuple_size_v<T>)
      {
         from_bincode(std::get<i>(obj), stream);
         from_bincode_tuple<i + 1>(obj, stream);
      }
   }

   template <typename... Ts, typename S>
   void from_bincode(std::tuple<Ts...>& obj, S& stream)
   {
      from_bincode_tuple<0>(obj, stream);
   }

   // ── Pairs ─────────────────────────────────────────────────────────────────

   template <typename First, typename Second, typename S>
   void from_bincode(std::pair<First, Second>& obj, S& stream)
   {
      from_bincode(obj.first, stream);
      from_bincode(obj.second, stream);
   }

   // ── Structs (via reflection) ──────────────────────────────────────────────

   template <typename T, typename S>
   void from_bincode(T& obj, S& stream)
   {
      psio1::apply_members((typename reflect<T>::data_members*)nullptr,
                          [&](auto... member) { (from_bincode(obj.*member, stream), ...); });
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_from_bincode(T& obj, const std::vector<char>& bin)
   {
      input_stream stream{bin};
      from_bincode(obj, stream);
   }

   template <typename T>
   T convert_from_bincode(const std::vector<char>& bin)
   {
      T obj;
      convert_from_bincode(obj, bin);
      return obj;
   }

}  // namespace psio1
