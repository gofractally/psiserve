#pragma once
// Borsh decoding — driven by PSIO1_REFLECT, same types, different wire format.
// See to_borsh.hpp for encoding rules.

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
   void from_borsh(T& obj, S& stream);

   // ── Scalars (fixed-width little-endian) ───────────────────────────────────

   template <typename S>
   void from_borsh(bool& val, S& stream)
   {
      uint8_t b;
      stream.read(reinterpret_cast<char*>(&b), 1);
      val = (b != 0);
   }

   template <typename S>
   void from_borsh(int8_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(uint8_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(int16_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(uint16_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(int32_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(uint32_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(int64_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(uint64_t& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(float& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(double& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   // ── Extended integer types ────────────────────────────────────────────────

   template <typename S>
   void from_borsh(unsigned __int128& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(__int128& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_borsh(uint256& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   // Scoped enums
   template <typename T, typename S>
      requires std::is_enum_v<T>
   void from_borsh(T& val, S& stream)
   {
      std::underlying_type_t<T> underlying;
      stream.read(reinterpret_cast<char*>(&underlying), sizeof(underlying));
      val = static_cast<T>(underlying);
   }

   // ── Strings ───────────────────────────────────────────────────────────────

   template <typename S>
   void from_borsh(std::string& obj, S& stream)
   {
      uint32_t len;
      stream.read(reinterpret_cast<char*>(&len), sizeof(len));
      read_string_bulk(obj, stream, static_cast<std::size_t>(len));
   }

   // ── Bit types ─────────────────────────────────────────────────────────────

   template <std::size_t N, typename S>
   void from_borsh(bitvector<N>& v, S& stream)
   {
      stream.read(reinterpret_cast<char*>(v.data()), bitvector<N>::byte_count);
   }

   template <std::size_t MaxN, typename S>
   void from_borsh(bitlist<MaxN>& v, S& stream)
   {
      std::uint32_t bit_count = 0;
      stream.read(reinterpret_cast<char*>(&bit_count), sizeof(bit_count));
      check(bit_count <= MaxN, "bitlist overflow on decode");
      std::size_t byte_count = (static_cast<std::size_t>(bit_count) + 7) / 8;
      std::vector<std::uint8_t> tmp(byte_count);
      if (byte_count)
         stream.read(reinterpret_cast<char*>(tmp.data()), byte_count);
      v.assign_raw(static_cast<std::size_t>(bit_count), tmp.data());
   }

   template <std::size_t N, typename S>
   void from_borsh(std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8];
      stream.read(reinterpret_cast<char*>(buf), (N + 7) / 8);
      unpack_bitset_bytes(buf, bs);
   }

   template <typename S>
   void from_borsh(std::vector<bool>& v, S& stream)
   {
      std::uint32_t bit_count = 0;
      stream.read(reinterpret_cast<char*>(&bit_count), sizeof(bit_count));
      std::size_t byte_count = (static_cast<std::size_t>(bit_count) + 7) / 8;
      std::vector<std::uint8_t> tmp(byte_count);
      if (byte_count)
         stream.read(reinterpret_cast<char*>(tmp.data()), byte_count);
      unpack_vector_bool(tmp.data(), static_cast<std::size_t>(bit_count), v);
   }

   // ── Bounded collections (validate bound on decode) ────────────────────────

   template <typename T, std::size_t N, typename S>
   void from_borsh(bounded_list<T, N>& val, S& stream)
   {
      from_borsh(val.storage(), stream);
      check(val.size() <= N, "bounded_list overflow on decode");
   }

   template <std::size_t N, typename S>
   void from_borsh(bounded_string<N>& val, S& stream)
   {
      from_borsh(val.storage(), stream);
      check(val.size() <= N, "bounded_string overflow on decode");
   }

   // ── Vectors ───────────────────────────────────────────────────────────────

   template <typename T, typename S>
   void from_borsh(std::vector<T>& v, S& stream)
   {
      uint32_t len;
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
            from_borsh(v[i], stream);
         }
      }
   }

   // ── Fixed-length arrays ───────────────────────────────────────────────────

   template <typename T, std::size_t N, typename S>
   void from_borsh(std::array<T, N>& obj, S& stream)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         stream.read(reinterpret_cast<char*>(obj.data()), N * sizeof(T));
      }
      else
      {
         for (T& elem : obj)
         {
            from_borsh(elem, stream);
         }
      }
   }

   // ── Optionals ─────────────────────────────────────────────────────────────

   template <typename T, typename S>
   void from_borsh(std::optional<T>& obj, S& stream)
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
         from_borsh(*obj, stream);
      }
   }

   // ── Variants ──────────────────────────────────────────────────────────────
   // u8 discriminant + value

   template <uint8_t I, typename... Ts, typename S>
   void borsh_variant_from(std::variant<Ts...>& v, uint8_t i, S& stream)
   {
      if constexpr (I < sizeof...(Ts))
      {
         if (I == i)
         {
            auto& x = v.template emplace<I>();
            from_borsh(x, stream);
         }
         else
         {
            borsh_variant_from<I + 1>(v, i, stream);
         }
      }
      else
      {
         abort_error(stream_error::bad_variant_index);
      }
   }

   template <typename... Ts, typename S>
   void from_borsh(std::variant<Ts...>& obj, S& stream)
   {
      static_assert(sizeof...(Ts) <= 256, "Borsh enum discriminant is u8");
      uint8_t idx;
      stream.read(reinterpret_cast<char*>(&idx), sizeof(idx));
      borsh_variant_from<0>(obj, idx, stream);
   }

   // ── Tuples ────────────────────────────────────────────────────────────────

   template <int i, typename T, typename S>
   void from_borsh_tuple(T& obj, S& stream)
   {
      if constexpr (i < std::tuple_size_v<T>)
      {
         from_borsh(std::get<i>(obj), stream);
         from_borsh_tuple<i + 1>(obj, stream);
      }
   }

   template <typename... Ts, typename S>
   void from_borsh(std::tuple<Ts...>& obj, S& stream)
   {
      from_borsh_tuple<0>(obj, stream);
   }

   // ── Pairs ─────────────────────────────────────────────────────────────────

   template <typename First, typename Second, typename S>
   void from_borsh(std::pair<First, Second>& obj, S& stream)
   {
      from_borsh(obj.first, stream);
      from_borsh(obj.second, stream);
   }

   // ── Structs (via reflection) ──────────────────────────────────────────────

   template <typename T, typename S>
   void from_borsh(T& obj, S& stream)
   {
      psio1::apply_members((typename reflect<T>::data_members*)nullptr,
                          [&](auto... member) { (from_borsh(obj.*member, stream), ...); });
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_from_borsh(T& obj, const std::vector<char>& bin)
   {
      input_stream stream{bin};
      from_borsh(obj, stream);
   }

   template <typename T>
   T convert_from_borsh(const std::vector<char>& bin)
   {
      T obj;
      convert_from_borsh(obj, bin);
      return obj;
   }

}  // namespace psio1
