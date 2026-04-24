#pragma once

#include <optional>
#include <psio/reflect.hpp>
#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/ext_int.hpp>
#include <psio/stream.hpp>
#include <psio/to_bin.hpp>   // for bin_detail traits shared with to_bin
#include <string_view>
#include <tuple>
#include <variant>
#include <vector>

namespace psio
{
   template <typename T, typename S>
   void from_bin(T& obj, S& stream);

   template <typename S>
   uint32_t varuint32_from_bin(S& stream)
   {
      uint32_t result = 0;
      int      shift  = 0;
      uint8_t  b      = 0;
      do
      {
         if (shift >= 35)
            abort_error(stream_error::invalid_varuint_encoding);
         from_bin(b, stream);
         result |= uint32_t(b & 0x7f) << shift;
         shift += 7;
      } while (b & 0x80);
      return result;
   }

   template <typename S>
   void varuint32_from_bin(uint32_t& dest, S& stream)
   {
      dest = varuint32_from_bin(stream);
   }

   template <typename S>
   uint64_t varuint64_from_bin(S& stream)
   {
      uint64_t result = 0;
      int      shift  = 0;
      uint8_t  b      = 0;
      do
      {
         if (shift >= 70)
            abort_error(stream_error::invalid_varuint_encoding);
         from_bin(b, stream);
         result |= uint64_t(b & 0x7f) << shift;
         shift += 7;
      } while (b & 0x80);
      return result;
   }

   template <typename S>
   void varuint64_from_bin(uint64_t& dest, S& stream)
   {
      dest = varuint64_from_bin(stream);
   }

   // zig-zag encoding
   template <typename S>
   void varint32_from_bin(int32_t& result, S& stream)
   {
      uint32_t v;
      varuint32_from_bin(v, stream);
      if (v & 1)
         result = ((~v) >> 1) | 0x8000'0000;
      else
         result = v >> 1;
   }

   // signed leb128 encoding
   template <typename Signed, typename S>
   Signed sleb_from_bin(S& stream)
   {
      using Unsigned  = std::make_unsigned_t<Signed>;
      Unsigned result = 0;
      int      shift  = 0;
      uint8_t  b      = 0;
      do
      {
         check(shift < sizeof(Unsigned) * 8, stream_error::invalid_varuint_encoding);
         from_bin(b, stream);
         result |= Unsigned(b & 0x7f) << shift;
         shift += 7;
      } while (b & 0x80);
      if (shift < sizeof(Unsigned) * 8 && (b & 0x40))
         result |= -(Unsigned(1) << shift);
      return result;
   }

   template <typename S>
   int64_t sleb64_from_bin(S& stream)
   {
      return sleb_from_bin<int64_t>(stream);
   }

   template <typename S>
   int64_t sleb32_from_bin(S& stream)
   {
      return sleb_from_bin<int32_t>(stream);
   }

   // ── Bit types ─────────────────────────────────────────────────────────────
   //
   // bitvector is auto-handled via has_bitwise_serialization (memcpy path).

   template <std::size_t MaxN, typename S>
   void from_bin(bitlist<MaxN>& v, S& stream)
   {
      std::uint32_t bit_count = 0;
      varuint32_from_bin(bit_count, stream);
      check(bit_count <= MaxN, "bitlist overflow on decode");
      std::size_t byte_count = (static_cast<std::size_t>(bit_count) + 7) / 8;
      std::vector<std::uint8_t> tmp(byte_count);
      if (byte_count)
         stream.read(reinterpret_cast<char*>(tmp.data()), byte_count);
      v.assign_raw(static_cast<std::size_t>(bit_count), tmp.data());
   }

   template <std::size_t N, typename S>
   void from_bin(std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8];
      stream.read(reinterpret_cast<char*>(buf), (N + 7) / 8);
      unpack_bitset_bytes(buf, bs);
   }

   template <typename S>
   void from_bin(std::vector<bool>& v, S& stream)
   {
      std::uint32_t bit_count = 0;
      varuint32_from_bin(bit_count, stream);
      std::size_t byte_count = (static_cast<std::size_t>(bit_count) + 7) / 8;
      std::vector<std::uint8_t> tmp(byte_count);
      if (byte_count)
         stream.read(reinterpret_cast<char*>(tmp.data()), byte_count);
      unpack_vector_bool(tmp.data(), static_cast<std::size_t>(bit_count), v);
   }

   // ── Bounded collections (validate bound on decode) ────────────────────────

   template <typename T, std::size_t N, typename S>
   void from_bin(bounded_list<T, N>& val, S& stream)
   {
      from_bin(val.storage(), stream);
      check(val.size() <= N, "bounded_list overflow on decode");
   }

   template <std::size_t N, typename S>
   void from_bin(bounded_string<N>& val, S& stream)
   {
      from_bin(val.storage(), stream);
      check(val.size() <= N, "bounded_string overflow on decode");
   }

   template <typename T, typename S>
   void from_bin(std::vector<T>& v, S& stream)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         uint32_t size;
         varuint32_from_bin(size, stream);
         read_vector_bitwise(v, stream, static_cast<std::size_t>(size));
      }
      else
      {
         uint32_t size;
         varuint32_from_bin(size, stream);
         v.resize(size);
         for (size_t i = 0; i < size; ++i)
         {
            from_bin(v[i], stream);
         }
      }
   }

   template <typename S>
   inline void from_bin(std::string& obj, S& stream)
   {
      uint32_t size;
      varuint32_from_bin(size, stream);
      read_string_bulk(obj, stream, static_cast<std::size_t>(size));
   }

   template <typename S>
   inline void from_bin(std::string_view& obj, S& stream)
   {
      uint32_t size;
      varuint32_from_bin(size, stream);
      obj = std::string_view(stream.get_pos(), size);
      stream.skip(size);
   }

   template <typename First, typename Second, typename S>
   void from_bin(std::pair<First, Second>& obj, S& stream)
   {
      from_bin(obj.first, stream);
      from_bin(obj.second, stream);
   }

   template <typename T, typename S>
   void from_bin(std::optional<T>& obj, S& stream)
   {
      bool present;
      from_bin(present, stream);
      if (!present)
      {
         obj.reset();
         return;
      }
      obj.emplace();
      from_bin(*obj, stream);
   }

   template <uint32_t I, typename... Ts, typename S>
   void variant_from_bin(std::variant<Ts...>& v, uint32_t i, S& stream)
   {
      if constexpr (I < std::variant_size_v<std::variant<Ts...>>)
      {
         if (i == I)
         {
            auto& x = v.template emplace<I>();
            from_bin(x, stream);
         }
         else
         {
            variant_from_bin<I + 1>(v, i, stream);
         }
      }
      else
      {
         abort_error(stream_error::bad_variant_index);
      }
   }

   template <typename... Ts, typename S>
   void from_bin(std::variant<Ts...>& obj, S& stream)
   {
      uint32_t u;
      varuint32_from_bin(u, stream);
      variant_from_bin<0>(obj, u, stream);
   }

   template <typename T, std::size_t N, typename S>
   void from_bin(std::array<T, N>& obj, S& stream)
   {
      for (T& elem : obj)
      {
         from_bin(elem, stream);
      }
   }

   template <int N, typename T, typename S>
   void from_bin_tuple(T& obj, S& stream)
   {
      if constexpr (N < std::tuple_size_v<T>)
      {
         from_bin(std::get<N>(obj), stream);
         from_bin_tuple<N + 1>(obj, stream);
      }
   }

   template <typename... T, typename S>
   void from_bin(std::tuple<T...>& obj, S& stream)
   {
      from_bin_tuple<0>(obj, stream);
   }


   template <typename T, typename S>
   void from_bin(T& obj, S& stream)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         stream.read(reinterpret_cast<char*>(&obj), sizeof(T));
      }
      else if constexpr (bin_detail::is_bitwise_dwnc_struct<T>())
      {
         // Compile-time verified: layout matches wire, read the whole thing.
         stream.read(reinterpret_cast<char*>(&obj), sizeof(T));
      }
      else if constexpr (reflect<T>::definitionWillNotChange)
      {
         // Fixed schema (DWNC): sequential fields, no header.
         psio::apply_members((typename reflect<T>::data_members*)nullptr,
                             [&](auto... member) { (from_bin(obj.*member, stream), ...); });
      }
      else
      {
         // Extensible schema: [varuint content_size][fields...][unknown ext bytes]
         std::uint32_t content_size = varuint32_from_bin(stream);
         if (stream.remaining() < content_size)
            abort_error(stream_error::overrun);
         const char* content_end = stream.pos + content_size;
         const char* saved_end   = stream.end;
         // Temporarily bound the stream so nested reads can't spill past
         // this struct's declared end. Restored after walking fields.
         stream.end = content_end;

         bool eof_hit = stream.pos >= stream.end;
         psio::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member)
             {
                auto process = [&](auto m)
                {
                   using FT = std::remove_cvref_t<decltype(obj.*m)>;
                   if (eof_hit)
                   {
                      // Already past the struct content; remaining fields
                      // must be optional (trimmed or unknown-new).
                      if constexpr (is_std_optional_v<FT>)
                         (obj.*m).reset();
                      else
                         abort_error(stream_error::underrun);
                   }
                   else
                   {
                      from_bin(obj.*m, stream);
                      if (stream.pos >= stream.end)
                         eof_hit = true;
                   }
                };
                (process(member), ...);
             });

         // Skip any unknown trailing bytes (newer-schema extension).
         stream.pos = content_end;
         stream.end = saved_end;
      }
   }

   template <typename T, typename S>
   T from_bin(S& stream)
   {
      T obj;
      from_bin(obj, stream);
      return obj;
   }

   template <typename T>
   void convert_from_bin(T& obj, const std::vector<char>& bin)
   {
      input_stream stream{bin};
      return from_bin(obj, stream);
   }

   template <typename T>
   T convert_from_bin(const std::vector<char>& bin)
   {
      T obj;
      convert_from_bin(obj, bin);
      return obj;
   }

}  // namespace psio
