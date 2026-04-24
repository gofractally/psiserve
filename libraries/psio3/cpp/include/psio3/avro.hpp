#pragma once
//
// psio3/avro.hpp — `avro` format tag (Apache Avro binary).
//
// Wire (MVP scope):
//   bool:              1 byte (0/1)
//   signed/unsigned integrals: zig-zag varint (Avro long)
//   float:             raw 4-byte LE IEEE-754
//   double:            raw 8-byte LE IEEE-754
//   std::string:       varint(length) + utf-8 bytes
//   std::vector<T>:    block encoding — varint(count) + items, then 0-count
//   std::array<byte,N>: raw N bytes (Avro "fixed" when element is byte-sized)
//   std::array<other,N>: block encoding identical to vector
//   std::optional<T>:  union{null,T}: varint(0) or varint(1)+value
//   record:            fields concatenated in reflected order

#include <psio3/cpo.hpp>
#include <psio3/detail/variant_util.hpp>
#include <psio3/error.hpp>
#include <psio3/ext_int.hpp>
#include <psio3/format_tag_base.hpp>
#include <psio3/adapter.hpp>
#include <psio3/reflect.hpp>
#include <psio3/wrappers.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio3 {

   struct avro;

   namespace detail::avro_impl {

      template <typename T>
      concept Record = ::psio3::Reflected<T>;

      template <typename T>
      struct is_std_array : std::false_type {};
      template <typename T, std::size_t N>
      struct is_std_array<std::array<T, N>> : std::true_type {};
      template <typename T>
      struct is_std_vector : std::false_type {};
      template <typename T, typename A>
      struct is_std_vector<std::vector<T, A>> : std::true_type {};
      template <typename T>
      struct is_std_optional : std::false_type {};
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type {};

      using ::psio3::detail::is_std_variant;

      template <typename T>
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio3::bitvector<N>> : std::true_type {};

      template <typename T>
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio3::bitlist<N>> : std::true_type {};

      using sink_t = std::vector<char>;

      inline void write_long(sink_t& s, std::int64_t v)
      {
         std::uint64_t zz = (static_cast<std::uint64_t>(v) << 1) ^
                            static_cast<std::uint64_t>(v >> 63);
         do
         {
            std::uint8_t b = zz & 0x7f;
            zz >>= 7;
            b |= ((zz > 0) << 7);
            s.push_back(static_cast<char>(b));
         } while (zz);
      }

      inline std::int64_t read_long(std::span<const char> src,
                                    std::size_t&          pos)
      {
         std::uint64_t r     = 0;
         int           shift = 0;
         std::uint8_t  b     = 0;
         do
         {
            b = static_cast<std::uint8_t>(src[pos++]);
            r |= static_cast<std::uint64_t>(b & 0x7f) << shift;
            shift += 7;
         } while (b & 0x80);
         return static_cast<std::int64_t>((r >> 1) ^ (~(r & 1) + 1));
      }

      template <typename T>
      void encode_value(const T& v, sink_t& s)
      {
         if constexpr (std::is_same_v<T, bool>)
            s.push_back(v ? '\x01' : '\x00');
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
         {
            // Treat as Avro "fixed" 32-byte field — raw LE bytes,
            // no varint. Matches v1 ext-int behavior in multiformat
            // tests.
            s.insert(s.end(),
                     reinterpret_cast<const char*>(v.limb),
                     reinterpret_cast<const char*>(v.limb) + 32);
         }
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
         {
            s.insert(s.end(),
                     reinterpret_cast<const char*>(&v),
                     reinterpret_cast<const char*>(&v) + 16);
         }
         else if constexpr (std::is_same_v<T, float>)
         {
            char buf[4];
            std::memcpy(buf, &v, 4);
            s.insert(s.end(), buf, buf + 4);
         }
         else if constexpr (std::is_same_v<T, double>)
         {
            char buf[8];
            std::memcpy(buf, &v, 8);
            s.insert(s.end(), buf, buf + 8);
         }
         else if constexpr (std::is_enum_v<T>)
         {
            write_long(s,
                       static_cast<std::int64_t>(
                          static_cast<std::underlying_type_t<T>>(v)));
         }
         else if constexpr (std::is_integral_v<T>)
         {
            write_long(s, static_cast<std::int64_t>(v));
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (sizeof(E) == 1 && std::is_arithmetic_v<E>)
            {
               s.insert(s.end(),
                        reinterpret_cast<const char*>(v.data()),
                        reinterpret_cast<const char*>(v.data()) + v.size());
            }
            else
            {
               if (v.size() > 0)
               {
                  write_long(s, static_cast<std::int64_t>(v.size()));
                  for (const auto& x : v)
                     encode_value(x, s);
               }
               write_long(s, 0);
            }
         }
         else if constexpr (is_std_vector<T>::value)
         {
            if (!v.empty())
            {
               write_long(s, static_cast<std::int64_t>(v.size()));
               for (const auto& x : v)
                  encode_value(x, s);
            }
            write_long(s, 0);
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            write_long(s, static_cast<std::int64_t>(v.size()));
            s.insert(s.end(), v.data(), v.data() + v.size());
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (v.has_value())
            {
               write_long(s, 1);
               encode_value(*v, s);
            }
            else
            {
               write_long(s, 0);
            }
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // Avro union: long(branch index) + value.
            write_long(s, static_cast<std::int64_t>(v.index()));
            std::visit([&](const auto& alt) { encode_value(alt, s); }, v);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            // Avro fixed: raw N bytes (no length prefix).
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.insert(s.end(),
                        reinterpret_cast<const char*>(v.data()),
                        reinterpret_cast<const char*>(v.data()) + nbytes);
         }
         else if constexpr (is_bitlist<T>::value)
         {
            // Avro long(bit_count) + packed bytes.
            write_long(s, static_cast<std::int64_t>(v.size()));
            auto bytes = v.bytes();
            if (!bytes.empty())
               s.insert(s.end(),
                        reinterpret_cast<const char*>(bytes.data()),
                        reinterpret_cast<const char*>(bytes.data()) +
                           bytes.size());
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (encode_value(v.*(R::template member_pointer<Is>), s), ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::avro: unsupported type in encode_value");
         }
      }

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos);

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos)
      {
         if constexpr (std::is_same_v<T, bool>)
            return static_cast<unsigned char>(src[pos++]) != 0;
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
         {
            T out{};
            std::memcpy(out.limb, src.data() + pos, 32);
            pos += 32;
            return out;
         }
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, 16);
            pos += 16;
            return out;
         }
         else if constexpr (std::is_same_v<T, float>)
         {
            float out;
            std::memcpy(&out, src.data() + pos, 4);
            pos += 4;
            return out;
         }
         else if constexpr (std::is_same_v<T, double>)
         {
            double out;
            std::memcpy(&out, src.data() + pos, 8);
            pos += 8;
            return out;
         }
         else if constexpr (std::is_enum_v<T>)
         {
            return static_cast<T>(
               static_cast<std::underlying_type_t<T>>(read_long(src, pos)));
         }
         else if constexpr (std::is_integral_v<T>)
         {
            return static_cast<T>(read_long(src, pos));
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            T out{};
            if constexpr (sizeof(E) == 1 && std::is_arithmetic_v<E>)
            {
               std::memcpy(out.data(), src.data() + pos, out.size());
               pos += out.size();
            }
            else
            {
               std::size_t i = 0;
               while (true)
               {
                  std::int64_t count = read_long(src, pos);
                  if (count < 0)
                  {
                     // Avro optional size-prefixed block variant — treat as abs
                     count = -count;
                     // skip size-in-bytes long
                     (void)read_long(src, pos);
                  }
                  if (count == 0)
                     break;
                  for (std::int64_t j = 0; j < count; ++j, ++i)
                  {
                     if (i < out.size())
                        out[i] = decode_value<E>(src, pos);
                  }
               }
            }
            return out;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            std::vector<E> out;
            while (true)
            {
               std::int64_t count = read_long(src, pos);
               if (count < 0)
               {
                  count = -count;
                  (void)read_long(src, pos);  // skip byte size
               }
               if (count == 0)
                  break;
               out.reserve(out.size() + static_cast<std::size_t>(count));
               for (std::int64_t j = 0; j < count; ++j)
                  out.push_back(decode_value<E>(src, pos));
            }
            return out;
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            std::int64_t n = read_long(src, pos);
            std::string  out(src.data() + pos,
                            src.data() + pos + static_cast<std::size_t>(n));
            pos += static_cast<std::size_t>(n);
            return out;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V      = typename T::value_type;
            std::int64_t branch = read_long(src, pos);
            if (branch == 0)
               return std::optional<V>{};
            return std::optional<V>{decode_value<V>(src, pos)};
         }
         else if constexpr (is_std_variant<T>::value)
         {
            const auto idx =
               static_cast<std::size_t>(read_long(src, pos));
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
                                  std::variant_alternative_t<Is, T>>(src,
                                                                       pos)},
                       true)
                    : false) ||
                  ...);
               (void)found;
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            return out;
         }
         else if constexpr (is_bitvector<T>::value)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            T                     out{};
            if constexpr (nbytes > 0)
            {
               std::memcpy(out.data(), src.data() + pos, nbytes);
               pos += nbytes;
            }
            return out;
         }
         else if constexpr (is_bitlist<T>::value)
         {
            const std::size_t bit_count =
               static_cast<std::size_t>(read_long(src, pos));
            T                 out;
            const std::size_t byte_count = (bit_count + 7) / 8;
            auto&             bits       = out.storage();
            bits.resize(bit_count);
            for (std::size_t i = 0; i < bit_count; ++i)
            {
               const unsigned char b =
                  static_cast<unsigned char>(src[pos + (i >> 3)]);
               bits[i] = (b >> (i & 7)) & 1;
            }
            pos += byte_count;
            return out;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            T       out{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               (((out.*(R::template member_pointer<Is>)) =
                    decode_value<typename R::template member_type<Is>>(src,
                                                                         pos)),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return out;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::avro: unsupported type in decode_value");
         }
      }

   }  // namespace detail::avro_impl

   struct avro : format_tag_base<avro>
   {
      using preferred_presentation_category = ::psio3::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio3::encode), avro, const T& v,
                             std::vector<char>& sink)
      {
         detail::avro_impl::encode_value(v, sink);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio3::encode), avro,
                                          const T& v)
      {
         std::vector<char> out;
         detail::avro_impl::encode_value(v, out);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio3::decode<T>), avro, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::avro_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio3::size_of), avro,
                                    const T& v)
      {
         std::vector<char> tmp;
         detail::avro_impl::encode_value(v, tmp);
         return tmp.size();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate<T>), avro, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("avro: empty buffer", 0, "avro");
         return codec_ok();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate_strict<T>),
                                     avro, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("avro: empty buffer", 0, "avro");
         return codec_ok();
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio3::make_boxed<T>),
                                           avro, T*,
                                           std::span<const char> bytes) noexcept
      {
         std::size_t pos = 0;
         return std::make_unique<T>(
            detail::avro_impl::decode_value<T>(bytes, pos));
      }
   };

}  // namespace psio3
