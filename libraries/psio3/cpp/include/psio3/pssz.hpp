#pragma once
//
// psio3/pssz.hpp — pSSZ (PsiSSZ) format tag.
//
// pSSZ is a variable-width variant of SSZ — offsets and container
// headers use 1, 2, or 4 bytes rather than always 4. The width is
// picked automatically per type by `auto_pssz_width_v<T>` from
// `max_encoded_size<T>()` (see max_size.hpp):
//
//   max ≤ 0xff   → W = 1
//   max ≤ 0xffff → W = 2
//   else         → W = 4
//
// Users shrink the width by attaching `length_bound{.max = N}` to
// variable-length fields (member annotation) or types (type annotation
// / wrapper). Unbounded types fall back to W = 4.
//
// Only one public tag: `psio3::pssz`. The width choice is a compile-
// time detail of the encode/decode path, not a user-facing knob.
//
// `psio3::pssz_<W>` exists as an explicit-width tag for
// byte-parity regression tests against v1; production code should
// always use `pssz` and let width selection happen automatically.
//
// Scope (Phase 7 MVP) matches Phase 6: primitives, std::array,
// std::vector, std::string, std::optional, reflected records.
// Out of scope (follow-ups): the "skip_header on DWNC" optimization,
// runtime size assertions against `max_encoded_size<T>()`.

#include <psio3/cpo.hpp>
#include <psio3/detail/variant_util.hpp>
#include <psio3/error.hpp>
#include <psio3/ext_int.hpp>
#include <psio3/format_tag_base.hpp>
#include <psio3/adapter.hpp>
#include <psio3/max_size.hpp>
#include <psio3/reflect.hpp>
#include <psio3/validate_strict_walker.hpp>
#include <psio3/wrappers.hpp>  // effective_annotations_for

#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <variant>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psio3 {

   struct pssz;  // fwd — used by adapter-dispatch trait below.
   template <std::size_t W>
   struct pssz_;  // explicit-width tag (byte-parity testing).

   namespace detail::pssz_impl {

      // ── Width → offset integer type ───────────────────────────────────────
      template <std::size_t W>
      struct width_info;

      template <>
      struct width_info<1>
      {
         using offset_t = std::uint8_t;
      };
      template <>
      struct width_info<2>
      {
         using offset_t = std::uint16_t;
      };
      template <>
      struct width_info<4>
      {
         using offset_t = std::uint32_t;
      };

      // ── Shape classification (same as SSZ) ────────────────────────────────

      template <typename T>
      struct is_fixed : std::false_type
      {
      };
      template <>
      struct is_fixed<bool> : std::true_type
      {
      };
      template <typename T>
         requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
      struct is_fixed<T> : std::true_type
      {
      };
      template <typename T, std::size_t N>
      struct is_fixed<std::array<T, N>> : is_fixed<T>
      {
      };

      template <std::size_t N>
      struct is_fixed<::psio3::bitvector<N>> : std::true_type
      {
      };

      // 128 / 256-bit integers — fixed-width raw LE bytes.
      template <>
      struct is_fixed<::psio3::uint128> : std::true_type
      {
      };
      template <>
      struct is_fixed<::psio3::int128> : std::true_type
      {
      };
      template <>
      struct is_fixed<::psio3::uint256> : std::true_type
      {
      };

      template <typename T>
      concept Record = ::psio3::Reflected<T>;

      template <typename T>
      inline constexpr bool is_fixed_v = is_fixed<T>::value;

      template <Record T, std::size_t... Is>
      consteval bool record_fields_all_fixed(std::index_sequence<Is...>)
      {
         using R = ::psio3::reflect<T>;
         return (is_fixed_v<typename R::template member_type<Is>> && ...);
      }

      template <Record T>
      consteval bool record_all_fixed()
      {
         using R = ::psio3::reflect<T>;
         return record_fields_all_fixed<T>(
            std::make_index_sequence<R::member_count>{});
      }

      template <typename T>
      inline constexpr bool has_binary_adapter_v =
         ::psio3::has_adapter_v<T, ::psio3::binary_category>;

      // A Record is pssz-fixed iff it is DWNC AND all fields are fixed.
      // DWNC skips the u{W} header, so a DWNC all-fixed record is
      // truly constant-size on the wire. Non-DWNC records carry the
      // header and (in a future trailing-pruning implementation)
      // variable-size fixed regions, so they go through the record
      // encode path rather than being inlined as fixed.
      template <Record T>
         requires(!has_binary_adapter_v<T>)
      struct is_fixed<T>
         : std::bool_constant<::psio3::is_dwnc_v<T> && record_all_fixed<T>()>
      {
      };

      template <typename T>
         requires(has_binary_adapter_v<T>)
      struct is_fixed<T> : std::false_type
      {
      };

      // ── fixed_size_of ─────────────────────────────────────────────────────
      template <typename T>
      constexpr std::size_t fixed_size_of() noexcept;

      template <Record T, std::size_t... Is>
      consteval std::size_t sum_record_fields(std::index_sequence<Is...>)
      {
         using R = ::psio3::reflect<T>;
         return (fixed_size_of<typename R::template member_type<Is>>() + ... + 0);
      }

      template <typename T>
      constexpr std::size_t fixed_size_of() noexcept
      {
         if constexpr (std::is_same_v<T, bool>)
            return 1;
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
            return 32;
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
            return 16;
         else if constexpr (std::is_arithmetic_v<T>)
            return sizeof(T);
         else if constexpr (requires { T::bytes_value; T::size_value; })
         {
            // psio3::bitvector<N> — declares both size_value and
            // bytes_value; the latter is (N+7)/8.
            return T::bytes_value;
         }
         else if constexpr (requires { typename T::value_type; } &&
                            requires { std::tuple_size<T>::value; })
         {
            using E = typename T::value_type;
            return std::tuple_size<T>::value * fixed_size_of<E>();
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return sum_record_fields<T>(
               std::make_index_sequence<R::member_count>{});
         }
         else
         {
            return 0;
         }
      }

      // ── std-container trait helpers ───────────────────────────────────────
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
      inline constexpr bool is_std_array_v    = is_std_array<T>::value;
      template <typename T>
      inline constexpr bool is_std_vector_v   = is_std_vector<T>::value;
      template <typename T>
      inline constexpr bool is_std_optional_v = is_std_optional<T>::value;
      template <typename T>
      inline constexpr bool is_std_variant_v  = is_std_variant<T>::value;

      template <typename T>
      struct is_bitvector : std::false_type {};
      template <std::size_t N>
      struct is_bitvector<::psio3::bitvector<N>> : std::true_type {};

      template <typename T>
      struct is_bitlist : std::false_type {};
      template <std::size_t N>
      struct is_bitlist<::psio3::bitlist<N>> : std::true_type {};

      template <typename T>
      inline constexpr bool is_bitvector_v = is_bitvector<T>::value;
      template <typename T>
      inline constexpr bool is_bitlist_v = is_bitlist<T>::value;

      // ── Sizing ────────────────────────────────────────────────────────────
      //
      // For pSSZ, offsets cost W bytes. The shape remains the same as SSZ.

      template <std::size_t W, typename T>
      std::size_t size_of_v(const T& v) noexcept
      {
         if constexpr (is_fixed_v<T>)
            return fixed_size_of<T>();
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            std::size_t           total = N * W;
            for (const auto& x : v)
               total += size_of_v<W>(x);
            return total;
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
               return v.size() * fixed_size_of<E>();
            else
            {
               std::size_t total = v.size() * W;
               for (const auto& x : v)
                  total += size_of_v<W>(x);
               return total;
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
            return v.size();
         else if constexpr (is_std_optional_v<T>)
            return v.has_value() ? size_of_v<W>(*v) : 0;
         else if constexpr (is_bitlist_v<T>)
            return (v.size() + 8) / 8;
         else if constexpr (is_std_variant_v<T>)
         {
            std::size_t total = 1;
            std::visit([&](const auto& alt) { total += size_of_v<W>(alt); },
                       v);
            return total;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) noexcept {
               std::size_t total = 0;
               // u{W} fixed_size header — DWNC types skip it.
               if constexpr (!::psio3::is_dwnc_v<T>)
                  total += W;
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      const auto& fref = v.*(R::template member_pointer<Is>);
                      if constexpr (is_fixed_v<F>)
                         total += fixed_size_of<F>();
                      else
                         total += W + size_of_v<W>(fref);
                   }()),
                  ...);
               return total;
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::pssz: unsupported type in size_of_v");
            return 0;
         }
      }

      // ── Encoding ──────────────────────────────────────────────────────────

      using sink_t = std::vector<char>;

      // Resize+memcpy beats insert(end, …) on a pre-reserved vector —
      // same win as in ssz.hpp. Saves ~3 ns per append on hot paths.
      inline void append_bytes(sink_t& s, const void* p, std::size_t n)
      {
         const std::size_t at = s.size();
         s.resize(at + n);
         std::memcpy(s.data() + at, p, n);
      }

      template <std::size_t W>
      void write_offset(sink_t& s, std::size_t pos, std::size_t value)
      {
         using O = typename width_info<W>::offset_t;
         O ov   = static_cast<O>(value);
         std::memcpy(s.data() + pos, &ov, W);
      }

      template <std::size_t W, typename T>
      void encode_value(const T& v, sink_t& s)
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::pssz, T>)
         {
            using Proj = ::psio3::adapter<std::remove_cvref_t<T>,
                                             ::psio3::binary_category>;
            Proj::encode(v, s);
            return;
         }

         if constexpr (std::is_same_v<T, bool>)
            s.push_back(v ? '\x01' : '\x00');
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
            append_bytes(s, v.limb, 32);
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
            append_bytes(s, &v, 16);
         else if constexpr (std::is_arithmetic_v<T>)
            append_bytes(s, &v, sizeof(T));
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
            {
               for (const auto& x : v)
                  encode_value<W>(x, s);
            }
            else
            {
               const std::size_t table = s.size();
               s.resize(s.size() + N * W, 0);
               for (std::size_t i = 0; i < N; ++i)
               {
                  write_offset<W>(s, table + i * W, s.size() - table);
                  encode_value<W>(v[i], s);
               }
            }
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
            {
               // Bulk memcpy for arithmetic OR memcpy-layout records.
               constexpr bool is_arith =
                  std::is_arithmetic_v<E> && !std::is_same_v<E, bool>;
               constexpr bool is_memcpy_record =
                  Record<E> && std::is_trivially_copyable_v<E> &&
                  fixed_size_of<E>() == sizeof(E);
               if constexpr (is_arith || is_memcpy_record)
               {
                  if (!v.empty())
                     append_bytes(s, v.data(), v.size() * sizeof(E));
               }
               else
               {
                  for (const auto& x : v)
                     encode_value<W>(x, s);
               }
            }
            else
            {
               const std::size_t n     = v.size();
               const std::size_t table = s.size();
               s.resize(s.size() + n * W, 0);
               for (std::size_t i = 0; i < n; ++i)
               {
                  write_offset<W>(s, table + i * W, s.size() - table);
                  encode_value<W>(v[i], s);
               }
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
            append_bytes(s, v.data(), v.size());
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            // pSSZ optional selector rule (matches v1):
            //   min_encoded_size(V) > 0 (fixed-size V) → no selector;
            //     the byte-span length (0 vs fixed_size) disambiguates.
            //   min_encoded_size(V) == 0 (variable V)  → 1-byte Union
            //     selector (0x00 = None, 0x01 + payload = Some).
            if constexpr (is_fixed_v<V>)
            {
               if (v.has_value())
                  encode_value<W>(*v, s);
               // None → no bytes appended.
            }
            else
            {
               if (v.has_value())
               {
                  s.push_back('\x01');
                  encode_value<W>(*v, s);
               }
               else
               {
                  s.push_back('\x00');
               }
            }
         }
         else if constexpr (is_bitvector_v<T>)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               append_bytes(s, v.data(), nbytes);
         }
         else if constexpr (is_bitlist_v<T>)
         {
            const std::size_t bit_count   = v.size();
            const std::size_t total_bytes = (bit_count + 8) / 8;
            const std::size_t at          = s.size();
            s.resize(at + total_bytes, 0);
            auto bytes = v.bytes();
            if (!bytes.empty())
               std::memcpy(s.data() + at, bytes.data(),
                           std::min(bytes.size(), total_bytes));
            s[at + (bit_count >> 3)] |=
               static_cast<char>(1u << (bit_count & 7u));
         }
         else if constexpr (is_std_variant_v<T>)
         {
            // pSSZ Union: 1-byte selector + value (same shape as SSZ;
            // offsets widths only affect containers holding variable
            // fields, not the selector byte itself).
            static_assert(std::variant_size_v<T> <= 256,
                          "pssz variant selector is u8 (≤ 256 alternatives)");
            s.push_back(static_cast<char>(v.index()));
            std::visit(
               [&](const auto& alt) { encode_value<W>(alt, s); }, v);
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               std::size_t       fixed_region    = 0;
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      using eff =
                         typename ::psio3::effective_annotations_for<
                            T, F,
                            R::template member_pointer<Is>>::value_t;
                      constexpr bool override_v =
                         ::psio3::has_as_override_v<eff>;
                      if constexpr (!override_v && is_fixed_v<F>)
                         fixed_region += fixed_size_of<F>();
                      else
                         fixed_region += W;
                   }()),
                  ...);

               // u{W} fixed_size header — fracpack's forward-compat
               // slot. DWNC types skip it (matches v1 pssz: the header
               // width is header_bytes == W). Non-DWNC records always
               // carry the header so a newer decoder with more fields
               // knows how many bytes of fixed region were actually
               // written.
               if constexpr (!::psio3::is_dwnc_v<T>)
               {
                  using O = typename width_info<W>::offset_t;
                  O hdr   = static_cast<O>(fixed_region);
                  const std::size_t hp = s.size();
                  s.resize(hp + W);
                  std::memcpy(s.data() + hp, &hdr, W);
               }

               const std::size_t container_start = s.size();
               s.resize(container_start + fixed_region, 0);
               std::size_t fixed_cursor = container_start;

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
                      constexpr bool override_v =
                         ::psio3::has_as_override_v<eff>;

                      if constexpr (!override_v && is_fixed_v<F>)
                      {
                         // Direct-to-position writes bypass tmp
                         // allocation — same pattern SSZ uses. The
                         // ~9× regression seen on pssz32 Header-encode
                         // came from the unconditional sink_t{}.
                         if constexpr (std::is_same_v<F, bool>)
                         {
                            s[fixed_cursor] = fref ? '\x01' : '\x00';
                         }
                         else if constexpr (std::is_arithmetic_v<F>)
                         {
                            std::memcpy(s.data() + fixed_cursor, &fref,
                                        sizeof(F));
                         }
                         else if constexpr (is_std_array_v<F>)
                         {
                            using E = typename F::value_type;
                            if constexpr (std::is_arithmetic_v<E> &&
                                          !std::is_same_v<E, bool>)
                            {
                               std::memcpy(s.data() + fixed_cursor,
                                           fref.data(),
                                           fixed_size_of<F>());
                            }
                            else
                            {
                               sink_t tmp;
                               encode_value<W>(fref, tmp);
                               std::memcpy(s.data() + fixed_cursor,
                                           tmp.data(), tmp.size());
                            }
                         }
                         else
                         {
                            sink_t tmp;
                            encode_value<W>(fref, tmp);
                            std::memcpy(s.data() + fixed_cursor,
                                        tmp.data(), tmp.size());
                         }
                         fixed_cursor += fixed_size_of<F>();
                      }
                      else
                      {
                         write_offset<W>(s, fixed_cursor,
                                         s.size() - container_start);
                         fixed_cursor += W;
                         if constexpr (override_v)
                         {
                            using Tag = ::psio3::adapter_tag_of_t<eff>;
                            using Proj = ::psio3::adapter<
                               std::remove_cvref_t<F>, Tag>;
                            Proj::encode(fref, s);
                         }
                         else
                         {
                            encode_value<W>(fref, s);
                         }
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::pssz: unsupported type in encode_value");
         }
      }

      // ── Decoding ──────────────────────────────────────────────────────────

      template <std::size_t W>
      std::uint32_t read_offset(std::span<const char> src, std::size_t pos)
      {
         using O = typename width_info<W>::offset_t;
         O v{};
         std::memcpy(&v, src.data() + pos, W);
         return static_cast<std::uint32_t>(v);
      }

      template <std::size_t W, typename T>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end);

      template <std::size_t W, typename T>
         requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
      T decode_arith(std::span<const char> src, std::size_t pos)
      {
         T out{};
         std::memcpy(&out, src.data() + pos, sizeof(T));
         return out;
      }

      template <std::size_t W, typename T, std::size_t N>
      std::array<T, N> decode_array(std::span<const char> src, std::size_t pos,
                                    std::size_t end)
      {
         std::array<T, N> out{};
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            for (std::size_t i = 0; i < N; ++i)
               out[i] = decode_value<W, T>(src, pos + i * esz,
                                           pos + (i + 1) * esz);
         }
         else if constexpr (N > 0)
         {
            std::array<std::uint32_t, N> offsets{};
            for (std::size_t i = 0; i < N; ++i)
               offsets[i] = read_offset<W>(src, pos + i * W);
            for (std::size_t i = 0; i < N; ++i)
            {
               const std::size_t beg = pos + offsets[i];
               const std::size_t fin =
                  (i + 1 < N) ? (pos + offsets[i + 1]) : end;
               out[i] = decode_value<W, T>(src, beg, fin);
            }
         }
         return out;
      }

      // Forward declaration — used by decode_vector to decode record
      // elements in-place. Definition appears later in this file.
      template <std::size_t W, Record T, std::size_t... Is>
      void record_decode_into(std::span<const char> src, std::size_t pos,
                              std::size_t end, T& out,
                              std::index_sequence<Is...>);

      template <std::size_t W, typename T>
      std::vector<T> decode_vector(std::span<const char> src, std::size_t pos,
                                   std::size_t end)
      {
         std::vector<T> out;
         if (pos >= end)
            return out;
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            const std::size_t n   = (end - pos) / esz;
            // Bulk-memcpy fast path covers arithmetic primitives AND
            // memcpy-layout DWNC packed records (sizeof matches wire).
            constexpr bool is_arith =
               std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;
            constexpr bool is_memcpy_record =
               Record<T> && std::is_trivially_copyable_v<T> &&
               fixed_size_of<T>() == sizeof(T);
            if constexpr (is_arith || is_memcpy_record)
            {
               const T* first = reinterpret_cast<const T*>(src.data() + pos);
               out.assign(first, first + n);
            }
            else if constexpr (Record<T>)
            {
               // In-place decode — single bulk zero-init via resize, then
               // write directly into each slot. Mirrors the ssz path.
               out.resize(n);
               using R = ::psio3::reflect<T>;
               for (std::size_t i = 0; i < n; ++i)
                  record_decode_into<W, T>(
                     src, pos + i * esz, pos + (i + 1) * esz, out[i],
                     std::make_index_sequence<R::member_count>{});
            }
            else
            {
               out.reserve(n);
               for (std::size_t i = 0; i < n; ++i)
                  out.push_back(decode_value<W, T>(src, pos + i * esz,
                                                    pos + (i + 1) * esz));
            }
         }
         else
         {
            if (end - pos < W)
               return out;
            const std::uint32_t first = read_offset<W>(src, pos);
            const std::size_t   n     = first / W;
            std::vector<std::uint32_t> offsets(n);
            for (std::size_t i = 0; i < n; ++i)
               offsets[i] = read_offset<W>(src, pos + i * W);
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
            {
               const std::size_t beg = pos + offsets[i];
               const std::size_t fin =
                  (i + 1 < n) ? (pos + offsets[i + 1]) : end;
               out.push_back(decode_value<W, T>(src, beg, fin));
            }
         }
         return out;
      }

      // Decode in-place: write fields directly into `out`. Used by
      // decode_vector to avoid the per-element `T tmp{}` + move-construct
      // when filling a pre-resized destination.
      template <std::size_t W, Record T, std::size_t... Is>
      void record_decode_into(std::span<const char> src, std::size_t pos,
                              std::size_t end, T& out,
                              std::index_sequence<Is...>)
      {
         using R = ::psio3::reflect<T>;

         // Non-DWNC records carry a u{W} fixed_size header; skip it
         // before reading the fixed region. Offsets inside the record
         // are relative to the container_start — the byte after the
         // header — so the rest of the walker uses that.
         const std::size_t container_start =
            ::psio3::is_dwnc_v<T> ? pos : pos + W;

         std::array<std::uint32_t, R::member_count> var_offsets{};
         std::array<bool, R::member_count>          is_var{};
         std::size_t                                 cursor = container_start;
         (
            ([&]
             {
                using F = typename R::template member_type<Is>;
                using eff =
                   typename ::psio3::effective_annotations_for<
                      T, F, R::template member_pointer<Is>>::value_t;
                constexpr bool override_v =
                   ::psio3::has_as_override_v<eff>;
                if constexpr (!override_v && is_fixed_v<F>)
                {
                   is_var[Is] = false;
                   cursor    += fixed_size_of<F>();
                }
                else
                {
                   is_var[Is]      = true;
                   var_offsets[Is] = read_offset<W>(src, cursor);
                   cursor         += W;
                }
             }()),
            ...);

         std::array<std::size_t, R::member_count> var_end{};
         {
            std::size_t last_end = end;
            for (std::size_t i = R::member_count; i-- > 0;)
            {
               if (is_var[i])
               {
                  var_end[i] = last_end;
                  last_end   = container_start + var_offsets[i];
               }
            }
         }

         std::size_t fixed_cursor = container_start;
         (
            ([&]
             {
                using F = typename R::template member_type<Is>;
                auto& fref = out.*(R::template member_pointer<Is>);
                using eff =
                   typename ::psio3::effective_annotations_for<
                      T, F, R::template member_pointer<Is>>::value_t;
                constexpr bool override_v =
                   ::psio3::has_as_override_v<eff>;

                if constexpr (!override_v && is_fixed_v<F>)
                {
                   fref = decode_value<W, F>(src, fixed_cursor,
                                              fixed_cursor + fixed_size_of<F>());
                   fixed_cursor += fixed_size_of<F>();
                }
                else
                {
                   const std::size_t beg = container_start + var_offsets[Is];
                   if constexpr (override_v)
                   {
                      using Tag = ::psio3::adapter_tag_of_t<eff>;
                      using Proj = ::psio3::adapter<
                         std::remove_cvref_t<F>, Tag>;
                      fref = Proj::decode(std::span<const char>(
                         src.data() + beg, var_end[Is] - beg));
                   }
                   else
                   {
                      fref = decode_value<W, F>(src, beg, var_end[Is]);
                   }
                   fixed_cursor += W;
                }
             }()),
            ...);
      }

      template <std::size_t W, Record T, std::size_t... Is>
      T record_decode(std::span<const char> src, std::size_t pos,
                      std::size_t end, std::index_sequence<Is...> seq)
      {
         T out{};
         record_decode_into<W, T>(src, pos, end, out, seq);
         return out;
      }

      template <std::size_t W, typename T>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end)
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::pssz, T>)
         {
            using Proj = ::psio3::adapter<std::remove_cvref_t<T>,
                                             ::psio3::binary_category>;
            return Proj::decode(
               std::span<const char>(src.data() + pos, end - pos));
         }

         if constexpr (std::is_same_v<T, bool>)
            return static_cast<unsigned char>(src[pos]) != 0;
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
         {
            T out{};
            std::memcpy(out.limb, src.data() + pos, 32);
            return out;
         }
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
         {
            T out{};
            std::memcpy(&out, src.data() + pos, 16);
            return out;
         }
         else if constexpr (std::is_arithmetic_v<T>)
            return decode_arith<W, T>(src, pos);
         else if constexpr (std::is_same_v<T, std::string>)
            return std::string(src.data() + pos, src.data() + end);
         else if constexpr (is_std_array_v<T>)
            return decode_array<W, typename T::value_type,
                                std::tuple_size<T>::value>(src, pos, end);
         else if constexpr (is_std_vector_v<T>)
            return decode_vector<W, typename T::value_type>(src, pos, end);
         else if constexpr (is_std_optional_v<T>)
         {
            using V = typename T::value_type;
            if constexpr (is_fixed_v<V>)
            {
               // No selector: length alone disambiguates (0 vs fixed size).
               if (pos >= end)
                  return std::optional<V>{};
               return std::optional<V>{decode_value<W, V>(src, pos, end)};
            }
            else
            {
               // 1-byte Union selector at pos: 0x00 = None, 0x01 = Some.
               if (pos >= end)
                  return std::optional<V>{};
               const std::uint8_t sel =
                  static_cast<unsigned char>(src[pos]);
               if (sel == 0)
                  return std::optional<V>{};
               return std::optional<V>{
                  decode_value<W, V>(src, pos + 1, end)};
            }
         }
         else if constexpr (is_std_variant_v<T>)
         {
            const auto idx = static_cast<std::size_t>(
               static_cast<unsigned char>(src[pos]));
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
                                  W,
                                  std::variant_alternative_t<Is, T>>(
                                  src, pos + 1, end)},
                       true)
                    : false) ||
                  ...);
               (void)found;
            }(std::make_index_sequence<std::variant_size_v<T>>{});
            return out;
         }
         else if constexpr (is_bitvector_v<T>)
         {
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            T                     out{};
            if constexpr (nbytes > 0)
               std::memcpy(out.data(), src.data() + pos, nbytes);
            return out;
         }
         else if constexpr (is_bitlist_v<T>)
         {
            T out;
            if (end <= pos)
               return out;
            const std::size_t total_bytes = end - pos;
            const unsigned char last =
               static_cast<unsigned char>(src[end - 1]);
            if (last == 0)
               return out;
            int delim_bit = 7;
            while (delim_bit >= 0 && !((last >> delim_bit) & 1))
               --delim_bit;
            const std::size_t bit_count =
               (total_bytes - 1) * 8 +
               static_cast<std::size_t>(delim_bit);
            auto& bits = out.storage();
            bits.resize(bit_count);
            for (std::size_t i = 0; i < bit_count; ++i)
            {
               const unsigned char b =
                  static_cast<unsigned char>(src[pos + (i >> 3)]);
               bits[i] = (b >> (i & 7)) & 1;
            }
            return out;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return record_decode<W, T>(
               src, pos, end, std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::pssz: unsupported type in decode_value");
         }
      }

      // ── Validation (structural) ───────────────────────────────────────────

      template <std::size_t W, typename T>
      codec_status validate_value(std::span<const char> src, std::size_t pos,
                                  std::size_t end) noexcept
      {
         if constexpr (is_fixed_v<T>)
         {
            return (end - pos) >= fixed_size_of<T>()
                      ? codec_ok()
                      : codec_fail("pssz: buffer too small for fixed type",
                                   static_cast<std::uint32_t>(pos), "pssz");
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            return pos <= end
                      ? codec_ok()
                      : codec_fail("pssz: string span invalid",
                                   static_cast<std::uint32_t>(pos), "pssz");
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if (pos > end)
               return codec_fail("pssz: negative vector span",
                                 static_cast<std::uint32_t>(pos), "pssz");
            if (pos == end)
               return codec_ok();
            if constexpr (is_fixed_v<E>)
               return (end - pos) % fixed_size_of<E>() == 0
                         ? codec_ok()
                         : codec_fail("pssz: vector length not a multiple of "
                                      "element size",
                                      static_cast<std::uint32_t>(pos), "pssz");
            else
               return (end - pos) >= W
                         ? codec_ok()
                         : codec_fail("pssz: variable vector offset missing",
                                      static_cast<std::uint32_t>(pos), "pssz");
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) noexcept {
               std::size_t needed = 0;
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      if constexpr (is_fixed_v<F>)
                         needed += fixed_size_of<F>();
                      else
                         needed += W;
                   }()),
                  ...);
               return (end - pos) >= needed
                         ? codec_ok()
                         : codec_fail("pssz: record fixed region truncated",
                                      static_cast<std::uint32_t>(pos), "pssz");
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            return codec_ok();
         }
      }

   }  // namespace detail::pssz_impl

   // ── Format tags ───────────────────────────────────────────────────────────

   // ── Width selection ───────────────────────────────────────────────────
   //
   // pssz is one format tag. Its internal offset width (1, 2, or 4
   // bytes) is selected at compile time from `max_encoded_size<T>()`:
   //   max ≤ 0xff   → W = 1  (pssz8-equivalent wire)
   //   max ≤ 0xffff → W = 2  (pssz16-equivalent wire)
   //   else / unbounded → W = 4  (pssz32-equivalent wire)
   //
   // Narrower widths kick in automatically when a record's variable
   // fields carry `length_bound` annotations that prove the encoded
   // output fits in the narrower offset range.

   template <typename T>
   consteval std::size_t auto_pssz_width() noexcept
   {
      constexpr auto m = ::psio3::max_encoded_size<T>();
      if constexpr (m.has_value())
      {
         if constexpr (*m <= 0xFFu)
            return 1;
         else if constexpr (*m <= 0xFFFFu)
            return 2;
         else
            return 4;
      }
      else
      {
         return 4;
      }
   }

   template <typename T>
   inline constexpr std::size_t auto_pssz_width_v = auto_pssz_width<T>();

   // Explicit-width tag — `psio3::pssz_<1|2|4>`. Not on the public
   // surface; retained so byte-parity regressions vs. v1's
   // `frac_format_pssz{8,16,32}` can name the exact width at the call
   // site. Production code uses `psio3::pssz` which picks W via
   // `auto_pssz_width_v<T>`.
   template <std::size_t W>
   struct pssz_ : format_tag_base<pssz_<W>>
   {
      static constexpr std::size_t offset_width = W;
      using preferred_presentation_category = ::psio3::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio3::encode), pssz_<W>,
                             const T& v, std::vector<char>& sink)
      {
         detail::pssz_impl::encode_value<W>(v, sink);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio3::encode),
                                          pssz_<W>, const T& v)
      {
         std::vector<char> out;
         out.reserve(detail::pssz_impl::size_of_v<W>(v));
         detail::pssz_impl::encode_value<W>(v, out);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio3::decode<T>), pssz_<W>, T*,
                          std::span<const char> bytes)
      {
         return detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size());
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio3::size_of),
                                    pssz_<W>, const T& v)
      {
         return detail::pssz_impl::size_of_v<W>(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate<T>),
                                     pssz_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::pssz_impl::validate_value<W, T>(bytes, 0,
                                                        bytes.size());
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate_strict<T>),
                                     pssz_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         auto st = detail::pssz_impl::validate_value<W, T>(bytes, 0,
                                                            bytes.size());
         if (!st.ok())
            return st;
         if constexpr (::psio3::Reflected<T>)
         {
            try
            {
               T decoded = detail::pssz_impl::decode_value<W, T>(
                  bytes, 0, bytes.size());
               return ::psio3::validate_specs_on_value(decoded);
            }
            catch (...)
            {
               return codec_fail(
                  "pssz: decode failed during validate_strict", 0,
                  "pssz");
            }
         }
         return st;
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio3::make_boxed<T>),
                                           pssz_<W>, T*,
                                           std::span<const char> bytes) noexcept
      {
         return std::make_unique<T>(
            detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size()));
      }
   };

   // Internal aliases — used by byte-parity tests only. Not part of
   // the public surface; do not reference from production code.
   using pssz8  = pssz_<1>;
   using pssz16 = pssz_<2>;
   using pssz32 = pssz_<4>;

   struct pssz : format_tag_base<pssz>
   {
      using preferred_presentation_category = ::psio3::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio3::encode), pssz,
                             const T& v, std::vector<char>& sink)
      {
         detail::pssz_impl::encode_value<auto_pssz_width_v<T>>(v, sink);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio3::encode),
                                          pssz, const T& v)
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         std::vector<char> out;
         out.reserve(detail::pssz_impl::size_of_v<W>(v));
         detail::pssz_impl::encode_value<W>(v, out);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio3::decode<T>), pssz, T*,
                          std::span<const char> bytes)
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size());
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio3::size_of),
                                    pssz, const T& v)
      {
         return detail::pssz_impl::size_of_v<auto_pssz_width_v<T>>(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate<T>),
                                     pssz, T*,
                                     std::span<const char> bytes) noexcept
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return detail::pssz_impl::validate_value<W, T>(bytes, 0,
                                                        bytes.size());
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate_strict<T>),
                                     pssz, T*,
                                     std::span<const char> bytes) noexcept
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         auto st = detail::pssz_impl::validate_value<W, T>(bytes, 0,
                                                            bytes.size());
         if (!st.ok())
            return st;
         if constexpr (::psio3::Reflected<T>)
         {
            try
            {
               T decoded = detail::pssz_impl::decode_value<W, T>(
                  bytes, 0, bytes.size());
               return ::psio3::validate_specs_on_value(decoded);
            }
            catch (...)
            {
               return codec_fail(
                  "pssz: decode failed during validate_strict", 0,
                  "pssz");
            }
         }
         return st;
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio3::make_boxed<T>),
                                           pssz, T*,
                                           std::span<const char> bytes) noexcept
      {
         constexpr std::size_t W = auto_pssz_width_v<T>;
         return std::make_unique<T>(
            detail::pssz_impl::decode_value<W, T>(bytes, 0, bytes.size()));
      }
   };

}  // namespace psio3
