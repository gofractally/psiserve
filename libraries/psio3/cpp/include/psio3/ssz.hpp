#pragma once
//
// psio3/ssz.hpp — SSZ (Simple Serialize) format tag.
//
// SSZ is the Ethereum consensus-layer canonical format. This header
// implements the v3-surface SSZ codec: `psio3::ssz` is the format tag,
// with hidden-friend tag_invoke overloads for every CPO
// (encode/decode/size_of/validate/validate_strict).
//
// Scope (Phase 6 MVP) — types supported here:
//   - bool, integer primitives (u8..u64, i8..i64), float, double
//   - std::array<T, N>             (SSZ Vector[T, N])
//   - std::vector<T>               (SSZ List[T, *])
//   - std::string                  (byte-list)
//   - std::optional<T>             (0 bytes for none; encoded payload for some)
//   - Records reflected via PSIO3_REFLECT (SSZ Container)
//
// Out of scope for the first cut (land in follow-ups):
//   uint256 / __int128, std::bitset / bitvector / bitlist, std::variant
//   (SSZ Union), wrappers like bounded<T, N>, schema-bound validation.
//
// Wire rules reproduced verbatim from the v1 SSZ implementation (see
// libraries/psio/cpp/include/psio/{to,from}_ssz.hpp). Integers are raw
// little-endian; vectors/arrays of fixed-size elements are packed tight;
// vectors of variable-size elements use a front offset table of uint32
// slot offsets (relative to the start of the container), with payloads
// appended after. Containers follow the same fixed/variable split.

#include <psio3/cpo.hpp>
#include <psio3/detail/variant_util.hpp>
#include <psio3/error.hpp>
#include <psio3/ext_int.hpp>
#include <psio3/format_tag_base.hpp>
#include <psio3/adapter.hpp>
#include <psio3/reflect.hpp>
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

   struct ssz;  // fwd — used by adapter-dispatch trait below.

   namespace detail::ssz_impl {

      // ── Shape classification ──────────────────────────────────────────────
      //
      // "Fixed-size" (in SSZ parlance) means the wire length is a
      // compile-time constant. Primitives, arrays of fixed elements, and
      // reflected records all-of-fixed-fields are fixed. std::vector,
      // std::string, std::optional, and any record containing a variable
      // field are variable.

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

      // psio3::bitvector<N>: SSZ Bitvector[N], ceil(N/8) bytes, fixed.
      template <std::size_t N>
      struct is_fixed<::psio3::bitvector<N>> : std::true_type
      {
      };
      // psio3::bitlist<MaxN>: SSZ Bitlist[N], variable (trailing
      // delimiter bit determines the bit count at decode time).

      // 128/256-bit integers — fixed-width raw LE bytes.
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

      // std::vector, std::string, std::optional are variable.
      // Reflected records: fixed iff every field is fixed.

      template <typename T>
      inline constexpr bool is_fixed_v = is_fixed<T>::value;

      template <typename T>
      concept Record = ::psio3::Reflected<T>;

      // Helper: does every reflected field of T have is_fixed_v == true?
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

      // Projected types are runtime-sized opaque bytes — not fixed.
      template <typename T>
      inline constexpr bool has_binary_adapter_v =
         ::psio3::has_adapter_v<T, ::psio3::binary_category>;

      template <Record T>
         requires(!has_binary_adapter_v<T>)
      struct is_fixed<T>
         : std::bool_constant<record_all_fixed<T>()>
      {
      };

      template <typename T>
         requires(has_binary_adapter_v<T>)
      struct is_fixed<T> : std::false_type
      {
      };

      // ── Fixed-size byte count ─────────────────────────────────────────────

      template <typename T>
      constexpr std::size_t fixed_size_of() noexcept;

      template <Record T, std::size_t... Is>
      consteval std::size_t sum_record_fields(std::index_sequence<Is...>)
      {
         using R = ::psio3::reflect<T>;
         return (fixed_size_of<typename R::template member_type<Is>>() + ... + 0);
      }

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
         else if constexpr (is_bitvector_v<T>)
            return (T::size_value + 7) / 8;
         else if constexpr (requires { typename T::value_type; } &&
                            requires { T{}.size(); })
         {
            // std::array<T, N>: N * fixed_size_of<T>
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
            return 0;  // unreachable for is_fixed_v<T> types
         }
      }

      // ── Dynamic size probe (for variable types) ───────────────────────────
      //
      // Forward-declared traits (is_std_array_v / is_std_vector_v /
      // is_std_optional_v) live below the encode section; forward-declare
      // them here so size_of_v can use them in its constexpr-if.

      template <typename T>
      struct is_std_array;
      template <typename T>
      struct is_std_vector;
      template <typename T>
      struct is_std_optional;

      template <typename T>
      std::size_t size_of_v(const T& v) noexcept
      {
         if constexpr (is_fixed_v<T>)
         {
            return fixed_size_of<T>();
         }
         else if constexpr (is_std_array<T>::value)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
               return N * fixed_size_of<E>();
            else
            {
               std::size_t total = N * 4;
               for (const auto& x : v)
                  total += size_of_v(x);
               return total;
            }
         }
         else if constexpr (is_std_vector<T>::value)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
               return v.size() * fixed_size_of<E>();
            else
            {
               std::size_t total = v.size() * 4;
               for (const auto& x : v)
                  total += size_of_v(x);
               return total;
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            return v.size();
         }
         else if constexpr (is_std_optional<T>::value)
         {
            return v.has_value() ? size_of_v(*v) : 0;
         }
         else if constexpr (is_bitlist_v<T>)
         {
            // SSZ bitlist: (bit_count + 8) / 8 bytes — data bytes
            // plus one for the trailing delimiter bit.
            return (v.size() + 8) / 8;
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // SSZ Union: 1 byte selector + size of the chosen alternative.
            std::size_t total = 1;
            std::visit([&](const auto& alt) { total += size_of_v(alt); }, v);
            return total;
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) noexcept {
               std::size_t total = 0;
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      const auto& fref =
                         v.*(R::template member_pointer<Is>);
                      if constexpr (is_fixed_v<F>)
                         total += fixed_size_of<F>();
                      else
                         total += 4 + size_of_v(fref);
                   }()),
                  ...);
               return total;
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::ssz: size_of_v: unsupported type");
            return 0;
         }
      }

      // ── Encoding ──────────────────────────────────────────────────────────
      //
      // Sink is `std::vector<char>` throughout — simplest interface that
      // satisfies backpatching (we write offsets by updating a known index
      // after the tail has been emitted).

      using sink_t = std::vector<char>;

      // Resize-then-memcpy is faster than `insert(end, ptr, ptr+n)` when
      // the vector is pre-reserved: skips iterator-validity checks and
      // the hint-is-at-end branch that std::vector::insert walks through.
      // For ssz encode with size_of_v()-reserved output, every
      // append_bytes call becomes bump-size + direct memcpy.
      inline void append_bytes(sink_t& s, const void* p, std::size_t n)
      {
         const std::size_t at = s.size();
         s.resize(at + n);
         std::memcpy(s.data() + at, p, n);
      }

      // Trait helpers for the single-dispatcher encode_value.
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

      // Single dispatcher — the only encode_value template. All per-type
      // logic is gated by `if constexpr` so name lookup at template-parse
      // time only needs to find this function, not per-type overloads.
      template <typename T>
      void encode_value(const T& v, sink_t& s)
      {
         // Adapter dispatch — projected types emit their opaque
         // bytes; SSZ's container walkers treat the result as a
         // variable-size payload (offset-addressed) through the same
         // heap mechanism they use for strings and vectors. Skip
         // self-delegating adapters.
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::ssz, T>)
         {
            using Proj = ::psio3::adapter<std::remove_cvref_t<T>,
                                             ::psio3::binary_category>;
            Proj::encode(v, s);
            return;
         }

         if constexpr (std::is_same_v<T, bool>)
         {
            s.push_back(v ? '\x01' : '\x00');
         }
         else if constexpr (std::is_same_v<T, ::psio3::uint256>)
         {
            append_bytes(s, v.limb, 32);
         }
         else if constexpr (std::is_same_v<T, ::psio3::uint128> ||
                            std::is_same_v<T, ::psio3::int128>)
         {
            append_bytes(s, &v, 16);
         }
         else if constexpr (std::is_arithmetic_v<T>)
         {
            append_bytes(s, &v, sizeof(T));
         }
         else if constexpr (is_std_array_v<T>)
         {
            using E                    = typename T::value_type;
            constexpr std::size_t N    = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
            {
               if constexpr (std::is_arithmetic_v<E> &&
                             !std::is_same_v<E, bool>)
               {
                  if constexpr (N > 0)
                     append_bytes(s, v.data(), N * sizeof(E));
               }
               else
               {
                  for (const auto& x : v)
                     encode_value(x, s);
               }
            }
            else
            {
               const std::size_t table_index = s.size();
               s.resize(s.size() + N * 4, 0);
               for (std::size_t i = 0; i < N; ++i)
               {
                  const std::uint32_t rel =
                     static_cast<std::uint32_t>(s.size() - table_index);
                  std::memcpy(s.data() + table_index + i * 4, &rel, 4);
                  encode_value(v[i], s);
               }
            }
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
            {
               // Bulk-memcpy fast path for arithmetic elements — the
               // wire layout is just the raw little-endian bytes of
               // the vector's contiguous storage. Matches v1's
               // ssz_memcpy_ok_v path; without this psio3 was ~9x
               // slower than v1 on a 512-element vector<u32>.
               if constexpr (std::is_arithmetic_v<E> &&
                             !std::is_same_v<E, bool>)
               {
                  if (!v.empty())
                     append_bytes(s, v.data(), v.size() * sizeof(E));
               }
               else
               {
                  for (const auto& x : v)
                     encode_value(x, s);
               }
            }
            else
            {
               const std::size_t n           = v.size();
               const std::size_t table_index = s.size();
               s.resize(s.size() + n * 4, 0);
               for (std::size_t i = 0; i < n; ++i)
               {
                  const std::uint32_t rel =
                     static_cast<std::uint32_t>(s.size() - table_index);
                  std::memcpy(s.data() + table_index + i * 4, &rel, 4);
                  encode_value(v[i], s);
               }
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            append_bytes(s, v.data(), v.size());
         }
         else if constexpr (is_std_optional_v<T>)
         {
            // SSZ encodes optional as Union[null, T]:
            //   None    → 0x00
            //   Some(x) → 0x01 || serialized(x)
            // Matches v1 wire format (canonical SSZ Union).
            if (v.has_value())
            {
               s.push_back('\x01');
               encode_value(*v, s);
            }
            else
            {
               s.push_back('\x00');
            }
         }
         else if constexpr (is_bitvector_v<T>)
         {
            // SSZ bitvector: ceil(N/8) raw bytes, LSB-first. psio3's
            // bitvector storage already matches the wire layout.
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               append_bytes(s, v.data(), nbytes);
         }
         else if constexpr (is_bitlist_v<T>)
         {
            // SSZ bitlist: data bits + trailing '1' delimiter at the
            // bit_count-th position. Total = (bit_count + 8) / 8.
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
            // SSZ Union: 1-byte selector + value (canonical). Variant
            // is always variable — records addressing it go through the
            // offset table (see record walker, variable-field branch).
            static_assert(std::variant_size_v<T> <= 256,
                          "ssz variant selector is u8 (≤ 256 alternatives)");
            s.push_back(static_cast<char>(v.index()));
            std::visit([&](const auto& alt) { encode_value(alt, s); }, v);
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const std::size_t container_start = s.size();
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
                         fixed_region += 4;  // offset slot
                   }()),
                  ...);

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
                         // Fast paths that bypass the tmp-vector alloc.
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
                            // std::array — second level of dispatch, so
                            // F::value_type is only named when F is
                            // actually a std::array.
                            using E = typename F::value_type;
                            if constexpr (std::is_arithmetic_v<E> &&
                                          !std::is_same_v<E, bool>)
                            {
                               std::memcpy(s.data() + fixed_cursor,
                                           fref.data(), fixed_size_of<F>());
                            }
                            else
                            {
                               sink_t tmp;
                               encode_value(fref, tmp);
                               std::memcpy(s.data() + fixed_cursor,
                                           tmp.data(), tmp.size());
                            }
                         }
                         else
                         {
                            // Fallback for nested fixed records.
                            sink_t tmp;
                            encode_value(fref, tmp);
                            std::memcpy(s.data() + fixed_cursor, tmp.data(),
                                        tmp.size());
                         }
                         fixed_cursor += fixed_size_of<F>();
                      }
                      else
                      {
                         const std::uint32_t rel = static_cast<std::uint32_t>(
                            s.size() - container_start);
                         std::memcpy(s.data() + fixed_cursor, &rel, 4);
                         fixed_cursor += 4;
                         if constexpr (override_v)
                         {
                            using Tag = ::psio3::adapter_tag_of_t<eff>;
                            using Proj = ::psio3::adapter<
                               std::remove_cvref_t<F>, Tag>;
                            Proj::encode(fref, s);
                         }
                         else
                         {
                            encode_value(fref, s);
                         }
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::ssz: encode_value: unsupported type");
         }
      }

      // ── Decoding ──────────────────────────────────────────────────────────
      //
      // All decoders take [pos, end) — pos is the byte index of the value's
      // start within the full input buffer, end is the byte index of the
      // value's end. Fixed decoders ignore `end`; variable decoders use it.

      template <typename T>
      T decode_value(std::span<const char> src,
                     std::size_t            pos,
                     std::size_t            end);

      template <typename T>
         requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
      T decode_arith(std::span<const char> src, std::size_t pos)
      {
         T out{};
         std::memcpy(&out, src.data() + pos, sizeof(T));
         return out;
      }

      template <typename T, std::size_t N>
      std::array<T, N> decode_array(std::span<const char> src,
                                    std::size_t            pos,
                                    std::size_t            end)
      {
         std::array<T, N> out{};
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            for (std::size_t i = 0; i < N; ++i)
               out[i] = decode_value<T>(src, pos + i * esz, pos + (i + 1) * esz);
         }
         else
         {
            if constexpr (N > 0)
            {
               std::array<std::uint32_t, N> offsets{};
               for (std::size_t i = 0; i < N; ++i)
                  std::memcpy(&offsets[i], src.data() + pos + i * 4, 4);
               for (std::size_t i = 0; i < N; ++i)
               {
                  const std::size_t beg = pos + offsets[i];
                  const std::size_t fin =
                     (i + 1 < N) ? (pos + offsets[i + 1]) : end;
                  out[i] = decode_value<T>(src, beg, fin);
               }
            }
         }
         return out;
      }

      template <typename T>
      std::vector<T> decode_vector(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end)
      {
         std::vector<T> out;
         if (pos >= end)
            return out;
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            const std::size_t n   = (end - pos) / esz;
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
               out.push_back(
                  decode_value<T>(src, pos + i * esz, pos + (i + 1) * esz));
         }
         else
         {
            // First offset tells us the count.
            if (end - pos < 4)
               return out;
            std::uint32_t first = 0;
            std::memcpy(&first, src.data() + pos, 4);
            const std::size_t n = first / 4;
            std::vector<std::uint32_t> offsets(n);
            for (std::size_t i = 0; i < n; ++i)
               std::memcpy(&offsets[i], src.data() + pos + i * 4, 4);
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
            {
               const std::size_t beg = pos + offsets[i];
               const std::size_t fin =
                  (i + 1 < n) ? (pos + offsets[i + 1]) : end;
               out.push_back(decode_value<T>(src, beg, fin));
            }
         }
         return out;
      }

      inline std::string decode_string(std::span<const char> src,
                                       std::size_t            pos,
                                       std::size_t            end)
      {
         return std::string(src.data() + pos, src.data() + end);
      }

      template <typename T>
      std::optional<T> decode_optional(std::span<const char> src,
                                       std::size_t            pos,
                                       std::size_t            end)
      {
         // SSZ Union[null, T] decoding: first byte is the selector.
         // 0x00 → None, 0x01 → Some(payload follows).
         if (pos >= end)
            return std::nullopt;
         const std::uint8_t sel =
            static_cast<unsigned char>(src[pos]);
         if (sel == 0x00)
            return std::nullopt;
         // sel == 0x01 (or anything non-zero for tolerant decode);
         // payload occupies [pos+1, end).
         return decode_value<T>(src, pos + 1, end);
      }

      template <Record T, std::size_t... Is>
      T record_decode(std::span<const char> src,
                      std::size_t            pos,
                      std::size_t            end,
                      std::index_sequence<Is...>)
      {
         using R = ::psio3::reflect<T>;

         // Walk once to collect fixed cursor + offsets of variable fields.
         // Fields with a member-level presentation override are treated
         // as variable (heap-slot + offset).
         std::array<std::uint32_t, R::member_count> var_offsets{};
         std::array<bool, R::member_count>          is_var{};
         std::size_t                                 cursor = pos;
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
                   is_var[Is]       = false;
                   var_offsets[Is]  = 0;
                   cursor          += fixed_size_of<F>();
                }
                else
                {
                   is_var[Is] = true;
                   std::uint32_t off = 0;
                   std::memcpy(&off, src.data() + cursor, 4);
                   var_offsets[Is]  = off;
                   cursor          += 4;
                }
             }()),
            ...);

         // Second pass: decode each field. Fixed fields read directly;
         // variable fields span [pos + off_i, pos + off_{next_var} or end).
         T out{};

         // Compute the end of each variable field by finding the next
         // variable field's start.
         std::array<std::size_t, R::member_count> var_end{};
         {
            std::size_t last_end = end;
            for (std::size_t i = R::member_count; i-- > 0;)
            {
               if (is_var[i])
               {
                  var_end[i] = last_end;
                  last_end   = pos + var_offsets[i];
               }
            }
         }

         std::size_t fixed_cursor = pos;
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
                   fref = decode_value<F>(src, fixed_cursor,
                                          fixed_cursor + fixed_size_of<F>());
                   fixed_cursor += fixed_size_of<F>();
                }
                else
                {
                   const std::size_t beg = pos + var_offsets[Is];
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
                      fref = decode_value<F>(src, beg, var_end[Is]);
                   }
                   fixed_cursor += 4;
                }
             }()),
            ...);

         return out;
      }

      // ── decode_value dispatch ─────────────────────────────────────────────
      //
      // Single generic entry that selects the right specialization.

      template <typename T>
      T decode_value(std::span<const char> src,
                     std::size_t            pos,
                     std::size_t            end)
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::ssz, T>)
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
            return decode_arith<T>(src, pos);
         else if constexpr (std::is_same_v<T, std::string>)
            return decode_string(src, pos, end);
         else if constexpr (requires { typename T::value_type; } &&
                            requires { std::tuple_size<T>::value; })
         {
            return decode_array<typename T::value_type,
                                std::tuple_size<T>::value>(src, pos, end);
         }
         else if constexpr (is_std_variant<T>::value)
         {
            // SSZ Union: 1-byte selector + value (value occupies
            // [pos+1, end)).
            const auto idx = static_cast<std::size_t>(
               static_cast<unsigned char>(src[pos]));
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
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
            // Walk the trailing delimiter bit: scan the last byte
            // high-to-low to find the highest set bit; that's the
            // end marker and the bit positions below it are data.
            T out;
            if (end <= pos)
               return out;
            const std::size_t total_bytes = end - pos;
            // Find delimiter in the last byte.
            const unsigned char last =
               static_cast<unsigned char>(src[end - 1]);
            if (last == 0)
               return out;  // malformed; treat as empty
            int delim_bit = 7;
            while (delim_bit >= 0 && !((last >> delim_bit) & 1))
               --delim_bit;
            const std::size_t bit_count =
               (total_bytes - 1) * 8 + static_cast<std::size_t>(delim_bit);
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
         else if constexpr (requires { typename T::value_type; } &&
                            !Record<T> &&
                            !std::is_same_v<T, std::string>)
         {
            using V = typename T::value_type;
            if constexpr (requires(T t) { t.has_value(); })
               return decode_optional<V>(src, pos, end);
            else
               return decode_vector<V>(src, pos, end);
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return record_decode<T>(src, pos, end,
                                    std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0, "psio3::ssz: unsupported type");
         }
      }

      // ── Validation (structural) ───────────────────────────────────────────
      //
      // MVP: checks buffer size >= minimum for fixed types; for variable
      // types and containers just verifies there's enough room for the
      // fixed region. A complete walk lands in follow-up work — for now
      // this is the same shape as v1's decode path without the memcpy.

      template <typename T>
      codec_status validate_value(std::span<const char> src,
                                  std::size_t            pos,
                                  std::size_t            end) noexcept;

      template <typename T>
         requires(is_fixed_v<T> && !Record<T>)
      codec_status validate_value(std::span<const char> /*src*/,
                                  std::size_t            pos,
                                  std::size_t            end) noexcept
      {
         if (end - pos < fixed_size_of<T>())
            return codec_fail("ssz: buffer too small for fixed primitive",
                              static_cast<std::uint32_t>(pos), "ssz");
         return codec_ok();
      }

      inline codec_status validate_string(std::span<const char> /*src*/,
                                          std::size_t            pos,
                                          std::size_t            end) noexcept
      {
         if (pos > end)
            return codec_fail("ssz: negative string span",
                              static_cast<std::uint32_t>(pos), "ssz");
         return codec_ok();
      }

      template <typename T>
      codec_status validate_vector(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end) noexcept
      {
         if (pos > end)
            return codec_fail("ssz: negative vector span",
                              static_cast<std::uint32_t>(pos), "ssz");
         if (pos == end)
            return codec_ok();

         if constexpr (is_fixed_v<T>)
         {
            if ((end - pos) % fixed_size_of<T>() != 0)
               return codec_fail("ssz: vector length not a multiple "
                                 "of element size",
                                 static_cast<std::uint32_t>(pos), "ssz");
            return codec_ok();
         }
         else
         {
            if (end - pos < 4)
               return codec_fail("ssz: variable vector missing offset table",
                                 static_cast<std::uint32_t>(pos), "ssz");
            std::uint32_t first = 0;
            std::memcpy(&first, src.data() + pos, 4);
            if (first % 4 != 0)
               return codec_fail("ssz: first vector offset not multiple of 4",
                                 static_cast<std::uint32_t>(pos), "ssz");
            return codec_ok();
         }
      }

      template <Record T, std::size_t... Is>
      codec_status validate_record(std::span<const char> /*src*/,
                                   std::size_t            pos,
                                   std::size_t            end,
                                   std::index_sequence<Is...>) noexcept
      {
         using R = ::psio3::reflect<T>;
         std::size_t needed = 0;
         (
            ([&]
             {
                using F = typename R::template member_type<Is>;
                if constexpr (is_fixed_v<F>)
                   needed += fixed_size_of<F>();
                else
                   needed += 4;
             }()),
            ...);
         if (end - pos < needed)
            return codec_fail("ssz: record buffer too small for fixed region",
                              static_cast<std::uint32_t>(pos), "ssz");
         return codec_ok();
      }

      template <typename T>
      codec_status validate_value(std::span<const char> src,
                                  std::size_t            pos,
                                  std::size_t            end) noexcept
      {
         if constexpr (std::is_same_v<T, std::string>)
            return validate_string(src, pos, end);
         else if constexpr (requires { typename T::value_type; } &&
                            requires { std::tuple_size<T>::value; })
         {
            // std::array: fixed; dispatched via is_fixed path above when
            // its element is fixed. For variable-element arrays, check
            // offset table size.
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
               return (end - pos) >= std::tuple_size<T>::value * fixed_size_of<E>()
                         ? codec_ok()
                         : codec_fail("ssz: array buffer too small",
                                      static_cast<std::uint32_t>(pos), "ssz");
            else
               return (end - pos) >= std::tuple_size<T>::value * 4
                         ? codec_ok()
                         : codec_fail(
                              "ssz: variable array offset table truncated",
                              static_cast<std::uint32_t>(pos), "ssz");
         }
         else if constexpr (requires { typename T::value_type; } &&
                            !Record<T>)
         {
            using V = typename T::value_type;
            if constexpr (requires(T t) { t.has_value(); })
               return codec_ok();  // optional: empty or one payload
            else
               return validate_vector<V>(src, pos, end);
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return validate_record<T>(src, pos, end,
                                       std::make_index_sequence<R::member_count>{});
         }
         else
         {
            return codec_fail("ssz: unsupported type in validate",
                              static_cast<std::uint32_t>(pos), "ssz");
         }
      }

   }  // namespace detail::ssz_impl

   // ── Format tag ─────────────────────────────────────────────────────────

   struct ssz : format_tag_base<ssz>
   {
      // Identifier for validate() error messages / tool routing.
      static constexpr const char* name = "ssz";

      using preferred_presentation_category = ::psio3::binary_category;

      // ── encode ─────────────────────────────────────────────────────────
      template <typename T>
      friend void tag_invoke(decltype(::psio3::encode),
                             ssz,
                             const T&              v,
                             std::vector<char>&    sink)
      {
         detail::ssz_impl::encode_value(v, sink);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio3::encode),
                                          ssz,
                                          const T& v)
      {
         // Pre-size the output buffer to match v1's `convert_to_ssz`:
         // ssz_size is O(1) for fixed types and O(variable-field count)
         // for containers, so the probe cost is dwarfed by the savings
         // from skipping mid-encode reallocations. Without this the
         // encode path was ~1.5x slower than v1 on the mixed-record
         // workload; with it, parity is within ±5%.
         std::vector<char> out;
         if constexpr (detail::ssz_impl::is_fixed_v<T>)
            out.reserve(detail::ssz_impl::fixed_size_of<T>());
         else
            out.reserve(detail::ssz_impl::size_of_v(v));
         detail::ssz_impl::encode_value(v, out);
         return out;
      }

      // ── decode ─────────────────────────────────────────────────────────
      template <typename T>
      friend T tag_invoke(decltype(::psio3::decode<T>),
                          ssz,
                          T*,
                          std::span<const char> bytes)
      {
         return detail::ssz_impl::decode_value<T>(bytes, 0, bytes.size());
      }

      // ── size_of ────────────────────────────────────────────────────────
      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio3::size_of),
                                    ssz,
                                    const T& v)
      {
         if constexpr (detail::ssz_impl::is_fixed_v<T>)
            return detail::ssz_impl::fixed_size_of<T>();
         else
            return detail::ssz_impl::size_of_v(v);
      }

      // ── validate (structural only) ─────────────────────────────────────
      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate<T>),
                                     ssz,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::ssz_impl::validate_value<T>(bytes, 0, bytes.size());
      }

      // ── validate_strict — phase-6 MVP aliases structural ───────────────
      //
      // Semantic (spec-carried) checks that depend on annotations land in
      // the dynamic / schema phase. Until then strict delegates.
      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate_strict<T>),
                                     ssz,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::ssz_impl::validate_value<T>(bytes, 0, bytes.size());
      }

      // ── make_boxed — default decode + std::make_unique ─────────────────
      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio3::make_boxed<T>),
                                           ssz,
                                           T*,
                                           std::span<const char> bytes) noexcept
      {
         return std::make_unique<T>(
            detail::ssz_impl::decode_value<T>(bytes, 0, bytes.size()));
      }
   };

}  // namespace psio3
