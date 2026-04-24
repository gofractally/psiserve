#pragma once
// Avro binary decoding — driven by PSIO_REFLECT, same types, different wire format.

#include <psio/bounded.hpp>
#include <psio/check.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <array>
#include <optional>
#include <string>
#include <tuple>
#include <variant>
#include <vector>

namespace psio
{
   // ── Avro zig-zag varint (signed long) decoder ─────────────────────────────

   template <typename S>
   int64_t avro_long_from_bin(S& stream)
   {
      uint64_t result = 0;
      int      shift  = 0;
      uint8_t  b      = 0;
      do
      {
         if (shift >= 70)
            abort_error(stream_error::invalid_varuint_encoding);
         stream.read(reinterpret_cast<char*>(&b), 1);
         result |= uint64_t(b & 0x7f) << shift;
         shift += 7;
      } while (b & 0x80);
      // Zig-zag decode: (result >> 1) ^ -(result & 1)
      return static_cast<int64_t>((result >> 1) ^ (~(result & 1) + 1));
   }

   // ── Forward declarations ──────────────────────────────────────────────────

   template <typename T, typename S>
   void from_avro(T& obj, S& stream);

   // ── Scalars ───────────────────────────────────────────────────────────────

   template <typename S>
   void from_avro(bool& val, S& stream)
   {
      uint8_t b;
      stream.read(reinterpret_cast<char*>(&b), 1);
      val = (b != 0);
   }

   template <typename S>
   void from_avro(int8_t& val, S& stream)
   {
      val = static_cast<int8_t>(avro_long_from_bin(stream));
   }

   template <typename S>
   void from_avro(int16_t& val, S& stream)
   {
      val = static_cast<int16_t>(avro_long_from_bin(stream));
   }

   template <typename S>
   void from_avro(int32_t& val, S& stream)
   {
      val = static_cast<int32_t>(avro_long_from_bin(stream));
   }

   template <typename S>
   void from_avro(int64_t& val, S& stream)
   {
      val = avro_long_from_bin(stream);
   }

   template <typename S>
   void from_avro(uint8_t& val, S& stream)
   {
      val = static_cast<uint8_t>(avro_long_from_bin(stream));
   }

   template <typename S>
   void from_avro(uint16_t& val, S& stream)
   {
      val = static_cast<uint16_t>(avro_long_from_bin(stream));
   }

   template <typename S>
   void from_avro(uint32_t& val, S& stream)
   {
      val = static_cast<uint32_t>(avro_long_from_bin(stream));
   }

   template <typename S>
   void from_avro(uint64_t& val, S& stream)
   {
      val = static_cast<uint64_t>(avro_long_from_bin(stream));
   }

   template <typename S>
   void from_avro(float& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename S>
   void from_avro(double& val, S& stream)
   {
      stream.read(reinterpret_cast<char*>(&val), sizeof(val));
   }

   template <typename T, typename S>
      requires std::is_enum_v<T>
   void from_avro(T& val, S& stream)
   {
      val = static_cast<T>(static_cast<std::underlying_type_t<T>>(avro_long_from_bin(stream)));
   }

   // ── Strings ───────────────────────────────────────────────────────────────

   template <typename S>
   void from_avro(std::string& obj, S& stream)
   {
      int64_t len = avro_long_from_bin(stream);
      if (len < 0)
         abort_error(stream_error::overrun);
      read_string_bulk(obj, stream, static_cast<std::size_t>(len));
   }

   // Bounded string — same wire as std::string, bound enforced on decode.
   template <std::size_t N, typename S>
   void from_avro(bounded_string<N>& obj, S& stream)
   {
      from_avro(obj.storage(), stream);
      check(obj.storage().size() <= N, "avro: bounded_string overflow");
   }

   // ── Arrays (block decoding) ───────────────────────────────────────────────

   template <typename T, typename S>
   void from_avro(std::vector<T>& v, S& stream)
   {
      v.clear();
      for (;;)
      {
         int64_t block_count = avro_long_from_bin(stream);
         if (block_count == 0)
            break;
         // Negative count means the block is preceded by a byte-size
         // (which we skip since we decode element-by-element)
         if (block_count < 0)
         {
            block_count = -block_count;
            avro_long_from_bin(stream);  // skip byte-size
         }
         for (int64_t i = 0; i < block_count; ++i)
         {
            v.emplace_back();
            from_avro(v.back(), stream);
         }
      }
   }

   template <typename T, std::size_t N, typename S>
   void from_avro(bounded_list<T, N>& v, S& stream)
   {
      from_avro(v.storage(), stream);
      check(v.storage().size() <= N, "avro: bounded_list overflow");
   }

   // ── Fixed-length arrays ───────────────────────────────────────────────────

   template <typename T, std::size_t N, typename S>
   void from_avro(std::array<T, N>& obj, S& stream)
   {
      if constexpr (sizeof(T) == 1 && has_bitwise_serialization<T>())
      {
         // Avro "fixed": raw bytes
         stream.read(reinterpret_cast<char*>(obj.data()), N);
      }
      else
      {
         // Decode as Avro array
         size_t idx = 0;
         for (;;)
         {
            int64_t block_count = avro_long_from_bin(stream);
            if (block_count == 0)
               break;
            if (block_count < 0)
            {
               block_count = -block_count;
               avro_long_from_bin(stream);  // skip byte-size
            }
            for (int64_t i = 0; i < block_count; ++i)
            {
               if (idx >= N)
                  abort_error(stream_error::array_size_mismatch);
               from_avro(obj[idx++], stream);
            }
         }
         if (idx != N)
            abort_error(stream_error::array_size_mismatch);
      }
   }

   // ── Optionals → union{null, T} ───────────────────────────────────────────

   template <typename T, typename S>
   void from_avro(std::optional<T>& obj, S& stream)
   {
      int64_t branch = avro_long_from_bin(stream);
      if (branch == 0)
      {
         obj.reset();  // null
      }
      else if (branch == 1)
      {
         obj.emplace();
         from_avro(*obj, stream);
      }
      else
      {
         abort_error(stream_error::bad_variant_index);
      }
   }

   // ── Variants → union ─────────────────────────────────────────────────────

   template <uint32_t I, typename... Ts, typename S>
   void avro_variant_from(std::variant<Ts...>& v, int64_t i, S& stream)
   {
      if constexpr (I < sizeof...(Ts))
      {
         if (static_cast<int64_t>(I) == i)
         {
            auto& x = v.template emplace<I>();
            from_avro(x, stream);
         }
         else
         {
            avro_variant_from<I + 1>(v, i, stream);
         }
      }
      else
      {
         abort_error(stream_error::bad_variant_index);
      }
   }

   template <typename... Ts, typename S>
   void from_avro(std::variant<Ts...>& obj, S& stream)
   {
      int64_t idx = avro_long_from_bin(stream);
      avro_variant_from<0>(obj, idx, stream);
   }

   // ── Tuples ────────────────────────────────────────────────────────────────

   template <int i, typename T, typename S>
   void from_avro_tuple(T& obj, S& stream)
   {
      if constexpr (i < std::tuple_size_v<T>)
      {
         from_avro(std::get<i>(obj), stream);
         from_avro_tuple<i + 1>(obj, stream);
      }
   }

   template <typename... Ts, typename S>
   void from_avro(std::tuple<Ts...>& obj, S& stream)
   {
      from_avro_tuple<0>(obj, stream);
   }

   // ── Pairs ─────────────────────────────────────────────────────────────────

   template <typename First, typename Second, typename S>
   void from_avro(std::pair<First, Second>& obj, S& stream)
   {
      from_avro(obj.first, stream);
      from_avro(obj.second, stream);
   }

   // ── Structs (via reflection) ──────────────────────────────────────────────

   template <typename T, typename S>
   void from_avro(T& obj, S& stream)
   {
      psio::apply_members((typename reflect<T>::data_members*)nullptr,
                          [&](auto... member) { (from_avro(obj.*member, stream), ...); });
   }

   // ── Public API ────────────────────────────────────────────────────────────

   template <typename T>
   void convert_from_avro(T& obj, const std::vector<char>& bin)
   {
      input_stream stream{bin};
      from_avro(obj, stream);
   }

   template <typename T>
   T convert_from_avro(const std::vector<char>& bin)
   {
      T obj;
      convert_from_avro(obj, bin);
      return obj;
   }

}  // namespace psio
