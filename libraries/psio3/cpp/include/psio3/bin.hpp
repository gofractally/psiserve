#pragma once
//
// psio3/bin.hpp — `bin` format tag.
//
// `bin` is the simplest binary format: primitives serialize as raw LE
// bytes; std::string and std::vector get a u32 length prefix; std::array
// and reflected records concatenate their elements. No offset tables,
// no headers, no heap region.
//
// Scope (Phase 10 MVP): primitives, std::array, std::vector, std::string,
// std::optional, reflected records. Matches the other-format MVPs.

#include <psio3/cpo.hpp>
#include <psio3/detail/variant_util.hpp>
#include <psio3/error.hpp>
#include <psio3/ext_int.hpp>
#include <psio3/format_tag_base.hpp>
#include <psio3/adapter.hpp>
#include <psio3/reflect.hpp>
#include <psio3/stream.hpp>
#include <psio3/wrappers.hpp>  // effective_annotations_for

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

   struct bin;  // fwd — the dispatch trait below needs the complete
                // format-tag identity, but only at instantiation time.

   namespace detail::bin_impl {

      template <typename T>
      concept Record = ::psio3::Reflected<T>;

      template <typename T>
      struct is_std_array : std::false_type
      {
      };
      template <typename T, std::size_t N>
      struct is_std_array<std::array<T, N>> : std::true_type
      {
      };
      template <typename T>
      struct is_std_vector : std::false_type
      {
      };
      template <typename T, typename A>
      struct is_std_vector<std::vector<T, A>> : std::true_type
      {
      };
      template <typename T>
      struct is_std_optional : std::false_type
      {
      };
      template <typename T>
      struct is_std_optional<std::optional<T>> : std::true_type
      {
      };

      using ::psio3::detail::is_std_variant;

      template <typename T>
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio3::bitvector<N>> : std::true_type {};

      template <typename T>
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio3::bitlist<N>> : std::true_type {};

      inline std::uint32_t read_u32(std::span<const char> src, std::size_t pos)
      {
         std::uint32_t v{};
         std::memcpy(&v, src.data() + pos, 4);
         return v;
      }

      // Sinks that only count bytes (psio3::size_stream) skip the actual
      // adapter encode in the dispatch path and just advance by the
      // adapter's packsize. Other sinks encode into a scratch vector
      // and copy the bytes through.
      template <typename S>
      struct sink_counts_only : std::false_type {};
      template <>
      struct sink_counts_only<::psio3::size_stream> : std::true_type {};
      template <typename S>
      inline constexpr bool sink_counts_only_v = sink_counts_only<S>::value;

      // ── Fixed / variable packsize split ─────────────────────────────────
      //
      // `fixed_contrib<T>()` is the minimum byte count T contributes
      // regardless of runtime content — all compile-time constant. For
      // types with no variable-length parts it's the entire packsize.
      //
      // `variable_contrib(v)` reads only what it must from `v`: string
      // sizes, vector sizes (and their payloads when the element type is
      // variable), optional discriminants, variant active-alt content.
      // Types that are fully fixed produce zero variable contribution
      // and the function compiles down to `return 0;` at every call
      // site.
      //
      // `packed_size_of(v) = fixed_contrib<T>() + variable_contrib(v)`.
      // This avoids the naive walker's per-field memory traversal for
      // deeply nested records dominated by fixed-size leaf types.

      template <typename T>
      consteval std::size_t fixed_contrib();

      template <typename T>
      std::size_t variable_contrib(const T& v);

      template <typename T>
      consteval bool fully_fixed();

      template <typename T>
      consteval bool fully_fixed()
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::bin, T>)
            return false;  // adapter payload is runtime-sized
         else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                            std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128> ||
                            std::is_same_v<T, ::psio3::uint256>)
            return true;
         else if constexpr (is_std_array<T>::value)
            return fully_fixed<typename T::value_type>();
         else if constexpr (is_bitvector<T>::value)
            return true;
         else if constexpr (std::is_same_v<T, std::string> ||
                            is_std_vector<T>::value ||
                            is_std_optional<T>::value ||
                            is_std_variant<T>::value ||
                            is_bitlist<T>::value)
            return false;
         else if constexpr (Record<T>)
         {
            using R  = ::psio3::reflect<T>;
            bool all = true;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((all = all && fully_fixed<
                                  typename R::template member_type<Is>>()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return all;
         }
         else
            return false;
      }

      template <typename T>
      consteval std::size_t fixed_contrib()
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::bin, T>)
            return 4;  // u32 length prefix; payload is variable
         else if constexpr (std::is_same_v<T, bool>)
            return 1;
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
            return 32;
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
            return 16;
         else if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>)
            return sizeof(T);
         else if constexpr (is_std_array<T>::value)
            return std::tuple_size<T>::value *
                   fixed_contrib<typename T::value_type>();
         else if constexpr (is_bitvector<T>::value)
            return (T::size_value + 7) / 8;
         else if constexpr (std::is_same_v<T, std::string>)
            return 4;
         else if constexpr (is_std_vector<T>::value)
            return 4;
         else if constexpr (is_std_optional<T>::value)
            return 1;
         else if constexpr (is_std_variant<T>::value)
            return 4;
         else if constexpr (is_bitlist<T>::value)
            return 4;
         else if constexpr (Record<T>)
         {
            std::size_t total = 0;
            using R           = ::psio3::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((total += fixed_contrib<
                             typename R::template member_type<Is>>()),
                ...);
            }(std::make_index_sequence<R::member_count>{});
            return total;
         }
         else
            return 0;
      }

      template <typename T>
      std::size_t variable_contrib(const T& v)
      {
         if constexpr (fully_fixed<T>())
            return 0;
         else if constexpr (::psio3::format_should_dispatch_adapter_v<
                               ::psio3::bin, T>)
         {
            using Proj = ::psio3::adapter<std::remove_cvref_t<T>,
                                          ::psio3::binary_category>;
            return Proj::packsize(v);
         }
         else if constexpr (std::is_same_v<T, std::string>)
            return v.size();
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (fully_fixed<E>())
               return v.size() * fixed_contrib<E>();
            else
            {
               std::size_t total = 0;
               for (const auto& x : v)
                  total += fixed_contrib<E>() + variable_contrib(x);
               return total;
            }
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            // Fully-fixed array short-circuits via the top guard.
            std::size_t total = 0;
            for (const auto& x : v)
               total += variable_contrib(x);
            return total;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            if (!v.has_value())
               return 0;
            using E = typename T::value_type;
            return fixed_contrib<E>() + variable_contrib(*v);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            std::size_t total = 0;
            std::visit(
               [&](const auto& alt)
               {
                  using A = std::remove_cvref_t<decltype(alt)>;
                  total   = fixed_contrib<A>() + variable_contrib(alt);
               },
               v);
            return total;
         }
         else if constexpr (is_bitlist<T>::value)
            return v.bytes().size();
         else if constexpr (Record<T>)
         {
            using R           = ::psio3::reflect<T>;
            std::size_t total = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      if constexpr (!fully_fixed<F>())
                         total += variable_contrib(
                            v.*(R::template member_pointer<Is>));
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return total;
         }
         else
            return 0;
      }

      template <typename T>
      std::size_t packed_size_of(const T& v)
      {
         return fixed_contrib<T>() + variable_contrib(v);
      }

      // ── Unified walker ───────────────────────────────────────────────────
      //
      // One template handles both packsize (Sink = psio3::size_stream) and
      // encode (Sink = psio3::fast_buf_stream / vector_stream). The sink
      // concept is uniform: `.put<T>(T)` writes sizeof(T) bytes,
      // `.write(ptr, n)` writes n bytes, `.write(ch)` writes a single
      // byte. `size_stream` specializations of those just sum sizes.

      template <typename T, typename Sink>
      void write_value(const T& v, Sink& s)
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::bin, T>)
         {
            using Proj = ::psio3::adapter<std::remove_cvref_t<T>,
                                          ::psio3::binary_category>;
            if constexpr (sink_counts_only_v<Sink>)
            {
               s.put(std::uint32_t{0});
               s.write(nullptr, Proj::packsize(v));
            }
            else
            {
               std::vector<char> tmp;
               Proj::encode(v, tmp);
               s.put(static_cast<std::uint32_t>(tmp.size()));
               s.write(tmp.data(), tmp.size());
            }
            return;
         }

         if constexpr (std::is_same_v<T, bool>)
         {
            std::uint8_t b = v ? 1 : 0;
            s.write(reinterpret_cast<const char*>(&b), 1);
         }
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
            s.write(v.limb, 32);
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
            s.write(&v, 16);
         else if constexpr (std::is_arithmetic_v<T>)
            s.put(v);
         else if constexpr (is_std_array<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (std::is_arithmetic_v<E>)
               s.write(v.data(), v.size() * sizeof(E));
            else
               for (const auto& x : v)
                  write_value(x, s);
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            s.put(static_cast<std::uint32_t>(v.size()));
            if constexpr (std::is_arithmetic_v<E> &&
                          !std::is_same_v<E, bool>)
            {
               if (!v.empty())
                  s.write(v.data(), v.size() * sizeof(E));
            }
            else
            {
               for (const auto& x : v)
                  write_value(x, s);
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            s.put(static_cast<std::uint32_t>(v.size()));
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (is_std_optional<T>::value)
         {
            std::uint8_t tag = v.has_value() ? 1 : 0;
            s.write(reinterpret_cast<const char*>(&tag), 1);
            if (v.has_value())
               write_value(*v, s);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            s.put(static_cast<std::uint32_t>(v.index()));
            std::visit([&](const auto& alt) { write_value(alt, s); }, v);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               s.write(v.data(), nbytes);
         }
         else if constexpr (is_bitlist<T>::value)
         {
            s.put(static_cast<std::uint32_t>(v.size()));
            auto bytes = v.bytes();
            if (!bytes.empty())
               s.write(bytes.data(), bytes.size());
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      const auto& fref =
                         v.*(R::template member_pointer<Is>);
                      using eff =
                         typename ::psio3::effective_annotations_for<
                            T, F,
                            R::template member_pointer<Is>>::value_t;
                      if constexpr (::psio3::has_as_override_v<eff>)
                      {
                         using Tag = ::psio3::adapter_tag_of_t<eff>;
                         using Proj = ::psio3::adapter<
                            std::remove_cvref_t<F>, Tag>;
                         if constexpr (sink_counts_only_v<Sink>)
                         {
                            s.put(std::uint32_t{0});
                            s.write(nullptr, Proj::packsize(fref));
                         }
                         else
                         {
                            std::vector<char> tmp;
                            Proj::encode(fref, tmp);
                            s.put(static_cast<std::uint32_t>(tmp.size()));
                            s.write(tmp.data(), tmp.size());
                         }
                      }
                      else
                      {
                         write_value(fref, s);
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::bin: unsupported type in write_value");
         }
      }

      // Cursor-based decoder — every decode_value advances `pos`.
      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos);

      template <typename T>
      T decode_value(std::span<const char> src, std::size_t& pos)
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::bin, T>)
         {
            using Proj = ::psio3::adapter<std::remove_cvref_t<T>,
                                             ::psio3::binary_category>;
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            auto out = Proj::decode(
               std::span<const char>(src.data() + pos, n));
            pos += n;
            return out;
         }

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
         else if constexpr (std::is_arithmetic_v<T>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, sizeof(T));
            pos += sizeof(T);
            return out;
         }
         else if constexpr (is_std_array<T>::value)
         {
            T out{};
            for (std::size_t i = 0; i < std::tuple_size<T>::value; ++i)
               out[i] = decode_value<typename T::value_type>(src, pos);
            return out;
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E             = typename T::value_type;
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            // Bulk-memcpy fast path for arithmetic elements — wire layout
            // is contiguous little-endian raw bytes, same as the element's
            // memory representation. assign(p, p+n) avoids resize's
            // zero-init pass and the per-element decode_value call.
            if constexpr (std::is_arithmetic_v<E> &&
                          !std::is_same_v<E, bool>)
            {
               const E* first = reinterpret_cast<const E*>(src.data() + pos);
               std::vector<E> out(first, first + n);
               pos += n * sizeof(E);
               return out;
            }
            else
            {
               std::vector<E> out;
               out.reserve(n);
               for (std::uint32_t i = 0; i < n; ++i)
                  out.push_back(decode_value<E>(src, pos));
               return out;
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            const std::uint32_t n = read_u32(src, pos);
            pos += 4;
            std::string out(src.data() + pos, src.data() + pos + n);
            pos += n;
            return out;
         }
         else if constexpr (is_std_optional<T>::value)
         {
            using V            = typename T::value_type;
            const bool present = static_cast<unsigned char>(src[pos++]) != 0;
            if (!present)
               return std::optional<V>{};
            return std::optional<V>{decode_value<V>(src, pos)};
         }
         else if constexpr (is_std_variant<T>::value)
         {
            const auto idx = static_cast<std::size_t>(read_u32(src, pos));
            pos += 4;
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
            const std::uint32_t bit_count = read_u32(src, pos);
            pos += 4;
            T                 out;
            const std::size_t byte_count = (bit_count + 7) / 8;
            auto&             bits       = out.storage();
            bits.resize(bit_count);
            for (std::uint32_t i = 0; i < bit_count; ++i)
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
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      auto& fref =
                         out.*(R::template member_pointer<Is>);
                      using eff =
                         typename ::psio3::effective_annotations_for<
                            T, F,
                            R::template member_pointer<Is>>::value_t;
                      if constexpr (::psio3::has_as_override_v<eff>)
                      {
                         using Tag = ::psio3::adapter_tag_of_t<eff>;
                         using Proj = ::psio3::adapter<
                            std::remove_cvref_t<F>, Tag>;
                         const std::uint32_t n = read_u32(src, pos);
                         pos += 4;
                         fref = Proj::decode(
                            std::span<const char>(src.data() + pos, n));
                         pos += n;
                      }
                      else
                      {
                         fref = decode_value<F>(src, pos);
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
            return out;
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::bin: unsupported type in decode_value");
         }
      }

   }  // namespace detail::bin_impl

   struct bin : format_tag_base<bin>
   {
      using preferred_presentation_category = ::psio3::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio3::encode), bin, const T& v,
                             std::vector<char>& sink)
      {
         ::psio3::vector_stream vs{sink};
         detail::bin_impl::write_value(v, vs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio3::encode), bin,
                                          const T& v)
      {
         const std::size_t       n = detail::bin_impl::packed_size_of(v);
         std::vector<char>        out(n);
         ::psio3::fast_buf_stream fbs{out.data(), out.size()};
         detail::bin_impl::write_value(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio3::decode<T>), bin, T*,
                          std::span<const char> bytes)
      {
         std::size_t pos = 0;
         return detail::bin_impl::decode_value<T>(bytes, pos);
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio3::size_of), bin,
                                    const T& v)
      {
         return detail::bin_impl::packed_size_of(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate<T>), bin, T*,
                                     std::span<const char> bytes) noexcept
      {
         // Minimal structural check: non-empty for non-void types.
         if (bytes.empty())
            return codec_fail("bin: empty buffer", 0, "bin");
         return codec_ok();
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate_strict<T>),
                                     bin, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.empty())
            return codec_fail("bin: empty buffer", 0, "bin");
         return codec_ok();
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio3::make_boxed<T>),
                                           bin, T*,
                                           std::span<const char> bytes) noexcept
      {
         std::size_t pos = 0;
         return std::make_unique<T>(
            detail::bin_impl::decode_value<T>(bytes, pos));
      }
   };

}  // namespace psio3

// ── Forward declarations — dynamic CPO friend overloads live in
// dynamic_bin.hpp (it has to see dynamic_value + schema). These forward
// declarations are here only so ADL finds the name from ssz-tag-only
// call sites; the definitions are the separate header.

