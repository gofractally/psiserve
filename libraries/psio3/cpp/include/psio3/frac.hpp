#pragma once
//
// psio3/frac.hpp — fracpack format tag family.
//
// Byte-compatible with v1 psio::fracpack on the MVP shape set —
// primitives, std::array, std::vector, std::string, std::optional,
// std::variant, reflected records. Wire details:
//
//   1. Records use pointer-relative offsets (heap_pos − slot_pos) with
//      a u16 header holding the fixed_region size. Sentinel slot values:
//        0 → Some(empty container) / non-optional empty container
//        1 → None (std::optional only)
//   2. Vectors and strings use a W-byte byte-count prefix.
//   3. Variant encoding (v1 parity): 1-byte tag (high bit MUST be
//      clear, ≤ 128 alternatives) + W-byte size_type + payload bytes.
//
// Exposed tags:
//   psio3::frac_<4>  (alias psio3::frac32)
//   psio3::frac_<2>  (alias psio3::frac16)
//
// Pending follow-ups (not required for byte-parity tests to pass):
//   - DWNC (definition_will_not_change) raw-memcpy fast path
//   - header_type selection per PSIO_FRAC_MAX_FIXED_SIZE for structs
//     whose fixed region exceeds 64 KiB

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
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace psio3 {

   template <std::size_t W>
   struct frac_;  // fwd — used by adapter-dispatch trait below.

   namespace detail::frac_impl {

      template <std::size_t W>
      struct width_info;
      template <>
      struct width_info<2>
      {
         using word_t = std::uint16_t;
      };
      template <>
      struct width_info<4>
      {
         using word_t = std::uint32_t;
      };

      template <typename T>
      concept Record = ::psio3::Reflected<T>;

      // Shape classification (same flavor as ssz/pssz).
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

      // Record-of-all-fixed is fixed *unless* the type has a binary
      // adapter that frac itself is willing to honor — in which
      // case frac sees opaque runtime-sized bytes. A adapter that
      // delegates back to frac_<W> doesn't count (frac would recurse);
      // in that case frac walks the shape as normal.
      //
      // Since the "willing to dispatch" predicate depends on the
      // specific frac_<W> width, is_fixed stays width-agnostic (just
      // "is there any adapter for which this is fixed?") and the
      // encode/decode dispatch does the self-delegation check with
      // the proper Fmt.
      template <typename T>
      inline constexpr bool has_binary_adapter_v =
         ::psio3::has_adapter_v<T, ::psio3::binary_category>;

      template <Record T>
         requires(!has_binary_adapter_v<T>)
      struct is_fixed<T> : std::bool_constant<record_all_fixed<T>()>
      {
      };

      template <typename T>
         requires(has_binary_adapter_v<T>)
      struct is_fixed<T> : std::false_type
      {
      };

      // std-container trait helpers.
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

      // v1 bitlist wire uses bounded_length_t<MaxN> — the smallest
      // unsigned integer type that fits the declared bound.
      template <std::size_t MaxN>
      constexpr std::size_t bitlist_len_bytes() noexcept
      {
         if constexpr (MaxN <= 0xFFu)
            return 1;
         else if constexpr (MaxN <= 0xFFFFu)
            return 2;
         else if constexpr (MaxN <= 0xFFFFFFFFu)
            return 4;
         else
            return 8;
      }

      template <typename T>
      inline constexpr bool is_std_array_v    = is_std_array<T>::value;
      template <typename T>
      inline constexpr bool is_std_vector_v   = is_std_vector<T>::value;
      template <typename T>
      inline constexpr bool is_std_optional_v = is_std_optional<T>::value;
      template <typename T>
      inline constexpr bool is_std_variant_v  = is_std_variant<T>::value;

      // Fixed byte count for fixed types.
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
         else if constexpr (is_bitvector<T>::value)
            return (T::size_value + 7) / 8;
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            return N * fixed_size_of<E>();
         }
         else if constexpr (Record<T>)
         {
            using R = ::psio3::reflect<T>;
            return sum_record_fields<T>(
               std::make_index_sequence<R::member_count>{});
         }
         else
            return 0;
      }

      using sink_t = std::vector<char>;

      template <std::size_t W>
      void write_word(sink_t& s, std::size_t pos, std::size_t value)
      {
         using O = typename width_info<W>::word_t;
         O ov   = static_cast<O>(value);
         std::memcpy(s.data() + pos, &ov, W);
      }

      template <std::size_t W>
      std::uint32_t read_word(std::span<const char> src, std::size_t pos)
      {
         using O = typename width_info<W>::word_t;
         O v{};
         std::memcpy(&v, src.data() + pos, W);
         return static_cast<std::uint32_t>(v);
      }

      // Resize+memcpy beats insert(end, …) on a pre-reserved vector —
      // same win as in ssz.hpp / pssz.hpp.
      template <std::size_t W>
      void append_word(sink_t& s, std::size_t value)
      {
         using O              = typename width_info<W>::word_t;
         O                 ov = static_cast<O>(value);
         const std::size_t at = s.size();
         s.resize(at + W);
         std::memcpy(s.data() + at, &ov, W);
      }

      inline void append_bytes(sink_t& s, const void* p, std::size_t n)
      {
         const std::size_t at = s.size();
         s.resize(at + n);
         std::memcpy(s.data() + at, p, n);
      }

      // ── Encoding ──────────────────────────────────────────────────────────
      //
      // Variable payloads are written as [u{W} length][payload]. Records
      // emit a [u{W} header][fixed_region][heap] triple where the header
      // carries the fixed_region size for forward-compatibility.

      template <std::size_t W, typename T>
      void encode_value(const T& v, sink_t& s);

      template <std::size_t W, typename T>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end);

      // Mirror of encode_record_fixed_inline for the read path.
      template <std::size_t W, Record T>
      T decode_record_fixed_inline(std::span<const char> src,
                                   std::size_t cursor)
      {
         using R = ::psio3::reflect<T>;
         T out{};
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (([&]
              {
                 using F = typename R::template member_type<Is>;
                 static_assert(is_fixed_v<F>,
                               "decode_record_fixed_inline called on a "
                               "record with a variable field");
                 auto& fref = out.*(R::template member_pointer<Is>);
                 if constexpr (Record<F>)
                    fref = decode_record_fixed_inline<W, F>(src, cursor);
                 else
                    fref = decode_value<W, F>(src, cursor,
                                              cursor + fixed_size_of<F>());
                 cursor += fixed_size_of<F>();
              }()),
             ...);
         }(std::make_index_sequence<R::member_count>{});
         return out;
      }

      // Inline a nested fixed Record into a parent's fixed_region —
      // writes each field's bytes at absolute positions starting at
      // `cursor` in `s`, without emitting the record's own u16 header
      // or heap frame. Used when a fixed record appears as a field of
      // a parent record: v1 fracpack inlines the bytes directly; the
      // nested record has no independent encoding unit.
      template <std::size_t W, Record T>
      void encode_record_fixed_inline(const T& v, sink_t& s,
                                      std::size_t cursor)
      {
         using R = ::psio3::reflect<T>;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (([&]
              {
                 using F = typename R::template member_type<Is>;
                 static_assert(is_fixed_v<F>,
                               "encode_record_fixed_inline called on a "
                               "record with a variable field");
                 const auto& fref =
                    v.*(R::template member_pointer<Is>);
                 if constexpr (std::is_same_v<F, bool>)
                    s[cursor] = fref ? '\x01' : '\x00';
                 else if constexpr (std::is_same_v<F, ::psio3::uint256>)
                    std::memcpy(s.data() + cursor, fref.limb, 32);
                 else if constexpr (std::is_same_v<F, ::psio3::uint128> ||
                                    std::is_same_v<F, ::psio3::int128>)
                    std::memcpy(s.data() + cursor, &fref, 16);
                 else if constexpr (std::is_arithmetic_v<F>)
                    std::memcpy(s.data() + cursor, &fref, sizeof(F));
                 else if constexpr (is_bitvector<F>::value)
                 {
                    constexpr std::size_t n = (F::size_value + 7) / 8;
                    if constexpr (n > 0)
                       std::memcpy(s.data() + cursor, fref.data(), n);
                 }
                 else if constexpr (is_std_array_v<F>)
                 {
                    using E = typename F::value_type;
                    if constexpr (std::is_arithmetic_v<E> &&
                                  !std::is_same_v<E, bool>)
                       std::memcpy(s.data() + cursor, fref.data(),
                                   fixed_size_of<F>());
                    else
                    {
                       sink_t tmp;
                       encode_value<W>(fref, tmp);
                       std::memcpy(s.data() + cursor, tmp.data(),
                                   tmp.size());
                    }
                 }
                 else if constexpr (Record<F>)
                    encode_record_fixed_inline<W>(fref, s, cursor);
                 else
                 {
                    sink_t tmp;
                    encode_value<W>(fref, tmp);
                    std::memcpy(s.data() + cursor, tmp.data(),
                                tmp.size());
                 }
                 cursor += fixed_size_of<F>();
              }()),
             ...);
         }(std::make_index_sequence<R::member_count>{});
      }

      template <std::size_t W, typename T>
      void encode_value(const T& v, sink_t& s)
      {
         // Adapter check — if the type registered a binary
         // adapter that isn't a self-delegation back to this frac_<W>
         // instance, write its opaque bytes. Frac's variable-payload
         // framing is added by the caller's own walk (we only emit the
         // payload here; the outer record / vector field slot
         // length-prefixes it). When frac is called at the top level
         // (buffer<T, frac>) there is no outer framing — the adapter
         // bytes are the whole wire form, which is the documented
         // behavior.
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::frac_<W>, T>)
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
            using E = typename T::value_type;
            if constexpr (is_fixed_v<E>)
            {
               for (const auto& x : v)
                  encode_value<W>(x, s);
            }
            else
            {
               // Array of variable: length-prefixed payloads.
               for (const auto& x : v)
               {
                  const std::size_t len_pos = s.size();
                  s.resize(s.size() + W, 0);
                  const std::size_t start = s.size();
                  encode_value<W>(x, s);
                  write_word<W>(s, len_pos, s.size() - start);
               }
            }
         }
         else if constexpr (is_std_vector_v<T>)
         {
            // v1 wire: [W-byte byte_count][element bytes concatenated].
            // Byte count, NOT element count. For arithmetic T this is
            // one memcpy of v.size()*sizeof(E) bytes.
            using E = typename T::value_type;
            if constexpr (std::is_arithmetic_v<E> &&
                          !std::is_same_v<E, bool>)
            {
               append_word<W>(s, v.size() * sizeof(E));
               if (!v.empty())
                  append_bytes(s, v.data(), v.size() * sizeof(E));
            }
            else
            {
               // Fixed non-arithmetic element (bool, nested fixed record):
               // still byte-count prefix over the packed content.
               static_assert(is_fixed_v<E>,
                             "psio3::frac: vector<variable-element> not "
                             "yet supported (needs offset-table walker)");
               const std::size_t lenpos = s.size();
               s.resize(s.size() + W, 0);
               const std::size_t start = s.size();
               for (const auto& x : v)
                  encode_value<W>(x, s);
               write_word<W>(s, lenpos, s.size() - start);
            }
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            // v1 wire: [W-byte byte_count][bytes].
            append_word<W>(s, v.size());
            if (!v.empty())
               append_bytes(s, v.data(), v.size());
         }
         else if constexpr (is_std_optional_v<T>)
         {
            // v1 fracpack std::optional is a W-byte slot at the record's
            // fixed region. At the top level the same layout is reused:
            //   None    → the W-byte slot holds the sentinel value `1`.
            //   Some(x) → the slot holds a pointer-relative offset
            //             (heap_pos − slot_pos); payload x follows.
            //
            // The slot is written first as 1 (placeholder); on Some we
            // rewrite it with the actual offset and then append x's pack.
            using V                  = typename T::value_type;
            const std::size_t slot   = s.size();
            s.resize(s.size() + W, 0);
            if (!v.has_value())
            {
               write_word<W>(s, slot, 1);
            }
            else
            {
               // is_empty_container check — for containers, skip payload
               // and leave slot at 0 (matches v1's "Some(empty) → 0").
               bool empty = false;
               if constexpr (is_std_vector_v<V> ||
                             std::is_same_v<V, std::string>)
                  empty = (*v).empty();
               if (empty)
               {
                  write_word<W>(s, slot, 0);
               }
               else
               {
                  const std::size_t heap = s.size();
                  write_word<W>(s, slot, heap - slot);
                  encode_value<W>(*v, s);
               }
            }
         }
         else if constexpr (is_std_variant_v<T>)
         {
            // v1 fracpack variant wire format:
            //   [u8 tag]                      — alternative index; high
            //                                   bit reserved (≤ 128 alts)
            //   [u{W} size]                   — byte length of payload
            //   [payload bytes]               — encoded alternative
            //
            // See libraries/psio/cpp/include/psio/fracpack.hpp:1952.
            static_assert(std::variant_size_v<T> < 128,
                          "fracpack variant tag high bit is reserved");
            s.push_back(static_cast<char>(v.index()));
            const std::size_t size_pos = s.size();
            s.resize(s.size() + W, 0);
            const std::size_t content_pos = s.size();
            std::visit(
               [&](const auto& alt) { encode_value<W>(alt, s); }, v);
            write_word<W>(s, size_pos, s.size() - content_pos);
         }
         else if constexpr (is_bitvector<T>::value)
         {
            // Fixed — raw ceil(N/8) bytes.
            constexpr std::size_t nbytes = (T::size_value + 7) / 8;
            if constexpr (nbytes > 0)
               append_bytes(s, v.data(), nbytes);
         }
         else if constexpr (is_bitlist<T>::value)
         {
            // v1 wire: [bounded_length_t<MaxN> bit_count][packed bytes].
            constexpr std::size_t LB =
               bitlist_len_bytes<T::max_size_value>();
            const std::size_t bit_count = v.size();
            if constexpr (LB == 1)
            {
               auto len8 = static_cast<std::uint8_t>(bit_count);
               append_bytes(s, &len8, 1);
            }
            else if constexpr (LB == 2)
            {
               auto len16 = static_cast<std::uint16_t>(bit_count);
               append_bytes(s, &len16, 2);
            }
            else if constexpr (LB == 4)
            {
               auto len32 = static_cast<std::uint32_t>(bit_count);
               append_bytes(s, &len32, 4);
            }
            else
            {
               auto len64 = static_cast<std::uint64_t>(bit_count);
               append_bytes(s, &len64, 8);
            }
            auto bytes = v.bytes();
            if (!bytes.empty())
               append_bytes(s, bytes.data(), bytes.size());
         }
         else if constexpr (Record<T>)
         {
            // v1 fracpack record wire format:
            //   [u{W} header = fixed_region size]
            //   [fixed_region: for each field in source order]
            //      - fixed field: inlined at the cursor
            //      - variable field: W-byte offset slot (initially 0 or
            //        1 depending on empty-container / optional-None)
            //   [heap: variable-field payloads, concatenated]
            //
            // Variable-field offsets are POINTER-RELATIVE: offset =
            // heap_pos − slot_pos. A slot holding `1` means "None"
            // (only produced by std::optional fields). A slot holding
            // `0` means "Some(empty container)" — payload is absent
            // but the slot value disambiguates from None.
            //
            // Empty-container fields (empty std::vector / std::string)
            // that are NOT wrapped in std::optional also leave the
            // slot at 0 and skip the heap write, matching v1's
            // is_empty_container logic.

            using R = ::psio3::reflect<T>;

            constexpr auto field_has_override = []<std::size_t I>(
                                                   std::integral_constant<
                                                      std::size_t, I>) constexpr
            {
               using F = typename R::template member_type<I>;
               using eff =
                  typename ::psio3::effective_annotations_for<
                     T, F, R::template member_pointer<I>>::value_t;
               return ::psio3::has_as_override_v<eff>;
            };

            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               // Phase 1: compute fixed_region size.
               std::size_t fixed_region = 0;
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      constexpr bool override_v =
                         field_has_override(
                            std::integral_constant<std::size_t, Is>{});
                      if constexpr (!override_v && is_fixed_v<F>)
                         fixed_region += fixed_size_of<F>();
                      else
                         fixed_region += W;  // offset slot
                   }()),
                  ...);

               // [u16 header] — v1 default header width is always 2
               // bytes regardless of offset width W. Wider headers
               // (u32 / u64) are a follow-up for types with
               // PSIO_FRAC_MAX_FIXED_SIZE commitments beyond 64 KiB.
               append_word<2>(s, fixed_region);

               const std::size_t fixed_start = s.size();
               s.resize(fixed_start + fixed_region, 0);

               // Phase 2: embedded_fixed_pack — inline fixed fields;
               // write initial slot placeholder for variable fields.
               // Placeholder is 1 for std::optional (matches v1's None
               // sentinel), 0 for other variable types (v1 default
               // placeholder).
               std::array<std::size_t, R::member_count> slot_pos{};
               std::size_t cursor = fixed_start;
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      constexpr bool override_v =
                         field_has_override(
                            std::integral_constant<std::size_t, Is>{});
                      const auto& fref =
                         v.*(R::template member_pointer<Is>);

                      if constexpr (!override_v && is_fixed_v<F>)
                      {
                         // Inline fixed. Fast paths bypass the tmp
                         // vector allocation used by v1's embedded_
                         // fixed_pack generic path.
                         if constexpr (std::is_same_v<F, bool>)
                            s[cursor] = fref ? '\x01' : '\x00';
                         else if constexpr (std::is_arithmetic_v<F>)
                            std::memcpy(s.data() + cursor, &fref,
                                        sizeof(F));
                         else if constexpr (is_std_array_v<F>)
                         {
                            using E = typename F::value_type;
                            if constexpr (std::is_arithmetic_v<E> &&
                                          !std::is_same_v<E, bool>)
                               std::memcpy(s.data() + cursor, fref.data(),
                                           fixed_size_of<F>());
                            else
                            {
                               sink_t tmp;
                               encode_value<W>(fref, tmp);
                               std::memcpy(s.data() + cursor, tmp.data(),
                                           tmp.size());
                            }
                         }
                         else if constexpr (Record<F>)
                         {
                            // v1 fracpack: nested fixed records are
                            // INLINED into the parent's fixed_region —
                            // no own u16 header, no heap (a fixed
                            // record has no variable fields by
                            // definition). Writing via encode_value
                            // would emit a header + heap framing,
                            // overflowing the slot. Walk fields
                            // directly instead.
                            encode_record_fixed_inline<W>(fref,
                                                          s, cursor);
                         }
                         else
                         {
                            sink_t tmp;
                            encode_value<W>(fref, tmp);
                            std::memcpy(s.data() + cursor, tmp.data(),
                                        tmp.size());
                         }
                         slot_pos[Is] = 0;  // not a slot
                         cursor += fixed_size_of<F>();
                      }
                      else
                      {
                         // Variable field: record slot position; seed
                         // with 1 if optional-None, 0 otherwise.
                         // (Phase 3 overwrites with actual offset on Some
                         // / non-empty; leaves as-is for None / empty.)
                         slot_pos[Is] = cursor;
                         if constexpr (is_std_optional_v<F>)
                         {
                            if (!fref.has_value())
                               write_word<W>(s, cursor, 1);
                         }
                         cursor += W;
                      }
                   }()),
                  ...);

               // Phase 3: embedded_variable_pack — append each variable
               // field's payload to the heap, rewriting the slot with
               // the pointer-relative offset (heap_pos − slot_pos).
               // Empty containers are skipped, leaving the slot at 0.
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      constexpr bool override_v =
                         field_has_override(
                            std::integral_constant<std::size_t, Is>{});
                      if constexpr (!override_v && is_fixed_v<F>)
                         return;

                      const auto& fref =
                         v.*(R::template member_pointer<Is>);

                      // Presentation override: always emit payload
                      // (adapters have no notion of empty / None).
                      if constexpr (override_v)
                      {
                         const std::size_t heap = s.size();
                         write_word<W>(s, slot_pos[Is],
                                       heap - slot_pos[Is]);
                         using eff = typename ::psio3::
                            effective_annotations_for<
                               T, F,
                               R::template member_pointer<Is>>::value_t;
                         using Tag  = ::psio3::adapter_tag_of_t<eff>;
                         using Proj = ::psio3::adapter<
                            std::remove_cvref_t<F>, Tag>;
                         Proj::encode(fref, s);
                         return;
                      }

                      // std::optional<V>:
                      //   None    → slot already 1, no payload.
                      //   Some(v) → if v is empty container, slot stays
                      //             0; else write offset + v.pack().
                      if constexpr (is_std_optional_v<F>)
                      {
                         if (!fref.has_value()) return;
                         using V = typename F::value_type;
                         bool empty = false;
                         if constexpr (is_std_vector_v<V> ||
                                       std::is_same_v<V, std::string>)
                            empty = (*fref).empty();
                         if (empty) return;
                         const std::size_t heap = s.size();
                         write_word<W>(s, slot_pos[Is],
                                       heap - slot_pos[Is]);
                         encode_value<W>(*fref, s);
                         return;
                      }

                      // Plain variable container:
                      //   empty → slot stays 0, no payload.
                      //   non-empty → write offset + pack().
                      bool empty = false;
                      if constexpr (is_std_vector_v<F> ||
                                    std::is_same_v<F, std::string>)
                         empty = fref.empty();
                      if (empty) return;

                      const std::size_t heap = s.size();
                      write_word<W>(s, slot_pos[Is],
                                    heap - slot_pos[Is]);
                      encode_value<W>(fref, s);
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::frac: unsupported type in encode_value");
         }
      }

      // ── Decoding ──────────────────────────────────────────────────────────

      template <std::size_t W, typename T>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end);

      // Read a record's [u16 header][fixed_region][heap] frame directly
      // into `out`. Mirrors v1's from_frac path that takes T& by reference.
      // Used by decode_record_with_header (returning T) and decode_vector
      // (filling pre-resized slots without per-element move-construct).
      template <std::size_t W, Record T>
      void decode_record_with_header_into(std::span<const char> src,
                                          std::size_t            pos,
                                          std::size_t            end,
                                          T&                     out)
      {
         using R = ::psio3::reflect<T>;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            // Read u16 header (fixed_region size). Matches v1.
            const std::uint32_t fixed_region = read_word<2>(src, pos);
            const std::size_t   fixed_start  = pos + 2;
            const std::size_t   fixed_end    = fixed_start + fixed_region;
            (void)fixed_end;
            std::size_t cursor = fixed_start;
            (
               ([&]
                {
                   using F = typename R::template member_type<Is>;
                   auto& fref = out.*(R::template member_pointer<Is>);
                   using eff =
                      typename ::psio3::effective_annotations_for<
                         T, F,
                         R::template member_pointer<Is>>::value_t;
                   constexpr bool override_v =
                      ::psio3::has_as_override_v<eff>;

                   if constexpr (!override_v && is_fixed_v<F>)
                   {
                      if constexpr (Record<F>)
                      {
                         fref = decode_record_fixed_inline<W, F>(
                            src, cursor);
                      }
                      else
                      {
                         fref = decode_value<W, F>(
                            src, cursor, cursor + fixed_size_of<F>());
                      }
                      cursor += fixed_size_of<F>();
                   }
                   else
                   {
                      const std::size_t   slot_pos = cursor;
                      const std::uint32_t slot     = read_word<W>(src, slot_pos);
                      cursor += W;

                      if constexpr (override_v)
                      {
                         using Tag = ::psio3::adapter_tag_of_t<eff>;
                         using Proj = ::psio3::adapter<
                            std::remove_cvref_t<F>, Tag>;
                         const std::size_t payload_pos = slot_pos + slot;
                         fref = Proj::decode(std::span<const char>(
                            src.data() + payload_pos,
                            end - payload_pos));
                         return;
                      }

                      if constexpr (is_std_optional_v<F>)
                      {
                         using V = typename F::value_type;
                         if (slot == 1) { fref = std::optional<V>{}; return; }
                         if (slot == 0)
                         {
                            if constexpr (is_std_vector_v<V> ||
                                          std::is_same_v<V, std::string>)
                               fref = std::optional<V>{V{}};
                            else
                               fref = std::optional<V>{};
                            return;
                         }
                         const std::size_t payload_pos = slot_pos + slot;
                         fref = std::optional<V>{
                            decode_value<W, V>(src, payload_pos, end)};
                         return;
                      }

                      if (slot == 0)
                      {
                         fref = F{};
                         return;
                      }
                      const std::size_t payload_pos = slot_pos + slot;
                      fref = decode_value<W, F>(src, payload_pos, end);
                   }
                }()),
               ...);
         }(std::make_index_sequence<R::member_count>{});
      }

      template <std::size_t W, Record T>
      T decode_record_with_header(std::span<const char> src,
                                  std::size_t            pos,
                                  std::size_t            end)
      {
         T out{};
         decode_record_with_header_into<W, T>(src, pos, end, out);
         return out;
      }

      template <std::size_t W, typename T, std::size_t N>
      std::array<T, N> decode_array(std::span<const char> src, std::size_t pos,
                                    std::size_t end)
      {
         std::array<T, N> out{};
         std::size_t      cursor = pos;
         if constexpr (is_fixed_v<T>)
         {
            const std::size_t esz = fixed_size_of<T>();
            for (std::size_t i = 0; i < N; ++i)
            {
               out[i] = decode_value<W, T>(src, cursor, cursor + esz);
               cursor += esz;
            }
         }
         else
         {
            for (std::size_t i = 0; i < N; ++i)
            {
               const std::uint32_t len = read_word<W>(src, cursor);
               cursor += W;
               out[i] = decode_value<W, T>(src, cursor, cursor + len);
               cursor += len;
            }
         }
         return out;
      }

      template <std::size_t W, typename T>
      std::vector<T> decode_vector(std::span<const char> src,
                                   std::size_t            pos,
                                   std::size_t            end)
      {
         // v1 wire: [W-byte byte_count][element bytes]
         const std::uint32_t byte_count = read_word<W>(src, pos);
         std::size_t         cursor     = pos + W;
         std::vector<T>      out;
         if constexpr (std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
         {
            const std::uint32_t n = byte_count / sizeof(T);
            out.resize(n);
            if (n > 0)
               std::memcpy(out.data(), src.data() + cursor, n * sizeof(T));
         }
         else if constexpr (is_fixed_v<T>)
         {
            const std::size_t   esz = fixed_size_of<T>();
            const std::uint32_t n   = byte_count / esz;
            if constexpr (Record<T>)
            {
               // In-place decode — single bulk zero-init via resize, then
               // write directly into each slot. Avoids the per-element
               // `T tmp{}` + move-construct that push_back incurs.
               out.resize(n);
               for (std::uint32_t i = 0; i < n; ++i)
               {
                  decode_record_with_header_into<W, T>(
                     src, cursor, cursor + esz, out[i]);
                  cursor += esz;
               }
            }
            else
            {
               out.reserve(n);
               for (std::uint32_t i = 0; i < n; ++i)
               {
                  out.push_back(decode_value<W, T>(src, cursor, cursor + esz));
                  cursor += esz;
               }
            }
         }
         else
         {
            static_assert(is_fixed_v<T>,
                          "psio3::frac: vector<variable-element> decode "
                          "not yet supported");
         }
         return out;
      }

      template <std::size_t W, typename T>
      T decode_value(std::span<const char> src, std::size_t pos,
                     std::size_t end)
      {
         // Adapter check — payload span belongs to the adapter;
         // frac only framed it. Self-delegating adapters fall
         // through to the shape walk.
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::frac_<W>, T>)
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
         {
            T out{};
            std::memcpy(&out, src.data() + pos, sizeof(T));
            return out;
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            // v1 wire: [W-byte byte_count][bytes]
            const std::uint32_t byte_count = read_word<W>(src, pos);
            return std::string(src.data() + pos + W,
                               src.data() + pos + W + byte_count);
         }
         else if constexpr (is_std_array_v<T>)
            return decode_array<W, typename T::value_type,
                                std::tuple_size<T>::value>(src, pos, end);
         else if constexpr (is_std_vector_v<T>)
            return decode_vector<W, typename T::value_type>(src, pos, end);
         else if constexpr (is_std_optional_v<T>)
         {
            // v1 wire at top level:
            //   [W-byte slot]
            //     slot == 1      → None
            //     slot == 0      → Some(empty container) — payload absent
            //     slot == offset → Some, payload at pos + offset
            using V = typename T::value_type;
            const std::uint32_t slot = read_word<W>(src, pos);
            if (slot == 1)
               return std::optional<V>{};
            if (slot == 0)
            {
               if constexpr (is_std_vector_v<V> ||
                             std::is_same_v<V, std::string>)
                  return std::optional<V>{V{}};
               else
                  return std::optional<V>{
                     decode_value<W, V>(src, pos + W, end)};
            }
            return std::optional<V>{
               decode_value<W, V>(src, pos + slot, end)};
         }
         else if constexpr (is_std_variant_v<T>)
         {
            // v1 wire: [u8 tag][u{W} size][payload bytes]
            const auto idx = static_cast<std::size_t>(
               static_cast<unsigned char>(src[pos]));
            const std::uint32_t size_bytes = read_word<W>(src, pos + 1);
            const std::size_t content_pos = pos + 1 + W;
            const std::size_t content_end = content_pos + size_bytes;
            T out;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               const bool found = ((idx == Is
                    ? (out = T{std::in_place_index<Is>,
                               decode_value<
                                  W,
                                  std::variant_alternative_t<Is, T>>(
                                  src, content_pos, content_end)},
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
               std::memcpy(out.data(), src.data() + pos, nbytes);
            return out;
         }
         else if constexpr (is_bitlist<T>::value)
         {
            constexpr std::size_t LB =
               bitlist_len_bytes<T::max_size_value>();
            std::size_t bit_count = 0;
            if constexpr (LB == 1)
            {
               std::uint8_t v8;
               std::memcpy(&v8, src.data() + pos, 1);
               bit_count = v8;
            }
            else if constexpr (LB == 2)
            {
               std::uint16_t v16;
               std::memcpy(&v16, src.data() + pos, 2);
               bit_count = v16;
            }
            else if constexpr (LB == 4)
            {
               std::uint32_t v32;
               std::memcpy(&v32, src.data() + pos, 4);
               bit_count = v32;
            }
            else
            {
               std::uint64_t v64;
               std::memcpy(&v64, src.data() + pos, 8);
               bit_count = static_cast<std::size_t>(v64);
            }
            T                 out;
            auto&             bits = out.storage();
            bits.resize(bit_count);
            for (std::size_t i = 0; i < bit_count; ++i)
            {
               const unsigned char b = static_cast<unsigned char>(
                  src[pos + LB + (i >> 3)]);
               bits[i] = (b >> (i & 7)) & 1;
            }
            return out;
         }
         else if constexpr (Record<T>)
         {
            return decode_record_with_header<W, T>(src, pos, end);
         }
         else
         {
            static_assert(sizeof(T) == 0,
                          "psio3::frac: unsupported type in decode_value");
         }
      }

      // ── Sizing — real packsize walker ─────────────────────────────────────
      //
      // Replaces the old "encode into a tmp vector and measure" path.
      // Mirrors `encode_value<W>` one branch at a time and returns the
      // same byte count without producing the bytes. Fixed types
      // short-circuit to a compile-time constant; variable types read
      // only `.size()` / optional discriminant / variant index.

      template <std::size_t W, typename T>
      std::size_t size_of_v(const T& v) noexcept;

      template <std::size_t W, typename T>
      std::size_t size_of_v(const T& v) noexcept
      {
         if constexpr (::psio3::format_should_dispatch_adapter_v<
                          ::psio3::frac_<W>, T>)
         {
            using Proj = ::psio3::adapter<std::remove_cvref_t<T>,
                                          ::psio3::binary_category>;
            return Proj::packsize(v);
         }
         else if constexpr (is_fixed_v<T>)
         {
            return fixed_size_of<T>();
         }
         else if constexpr (std::is_same_v<T, std::string>)
         {
            return W + v.size();
         }
         else if constexpr (is_std_vector_v<T>)
         {
            using E = typename T::value_type;
            if constexpr (std::is_arithmetic_v<E> &&
                          !std::is_same_v<E, bool>)
               return W + v.size() * sizeof(E);
            else if constexpr (is_fixed_v<E>)
               return W + v.size() * fixed_size_of<E>();
            else
            {
               std::size_t total = W;
               for (const auto& x : v)
                  total += size_of_v<W>(x);
               return total;
            }
         }
         else if constexpr (is_std_array_v<T>)
         {
            using E                 = typename T::value_type;
            constexpr std::size_t N = std::tuple_size<T>::value;
            if constexpr (is_fixed_v<E>)
               return N * fixed_size_of<E>();
            else
            {
               std::size_t total = 0;
               for (const auto& x : v)
                  total += W + size_of_v<W>(x);
               return total;
            }
         }
         else if constexpr (is_std_optional_v<T>)
         {
            if (!v.has_value())
               return W;  // slot holds sentinel 1
            using V = typename T::value_type;
            bool empty = false;
            if constexpr (is_std_vector_v<V> ||
                          std::is_same_v<V, std::string>)
               empty = (*v).empty();
            if (empty)
               return W;  // slot holds 0
            return W + size_of_v<W>(*v);
         }
         else if constexpr (is_std_variant_v<T>)
         {
            std::size_t total = 1 + W;  // tag + size word
            std::visit(
               [&](const auto& alt) { total += size_of_v<W>(alt); }, v);
            return total;
         }
         else if constexpr (is_bitlist<T>::value)
         {
            constexpr std::size_t LB =
               bitlist_len_bytes<T::max_size_value>();
            return LB + v.bytes().size();
         }
         else if constexpr (Record<T>)
         {
            // Fracpack record wire:
            //   [u16 header = fixed_region size]
            //   [fixed_region: inline-fixed fields or W-byte slots]
            //   [heap: variable payloads, each appended contiguously]

            using R = ::psio3::reflect<T>;

            // Phase 1 — fixed_region size (compile-time; fully folded
            // when every field is fixed).
            constexpr std::size_t fixed_region =
               [&]<std::size_t... Is>(std::index_sequence<Is...>) {
                  std::size_t total = 0;
                  (
                     ([&]<std::size_t I>() {
                        using F = typename R::template member_type<I>;
                        if constexpr (is_fixed_v<F>)
                           total += fixed_size_of<F>();
                        else
                           total += W;  // offset slot
                     }.template operator()<Is>()),
                     ...);
                  return total;
               }(std::make_index_sequence<R::member_count>{});

            // Phase 2 — heap payload per variable field (runtime).
            std::size_t heap = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               (
                  ([&]
                   {
                      using F = typename R::template member_type<Is>;
                      if constexpr (!is_fixed_v<F>)
                      {
                         const auto& fref =
                            v.*(R::template member_pointer<Is>);
                         if constexpr (is_std_optional_v<F>)
                         {
                            if (fref.has_value())
                            {
                               using V = typename F::value_type;
                               bool empty = false;
                               if constexpr (is_std_vector_v<V> ||
                                             std::is_same_v<V,
                                                            std::string>)
                                  empty = (*fref).empty();
                               if (!empty)
                                  heap += size_of_v<W>(*fref);
                            }
                         }
                         else if constexpr (is_std_vector_v<F> ||
                                            std::is_same_v<F, std::string>)
                         {
                            if (!fref.empty())
                               heap += size_of_v<W>(fref);
                         }
                         else
                         {
                            heap += size_of_v<W>(fref);
                         }
                      }
                   }()),
                  ...);
            }(std::make_index_sequence<R::member_count>{});

            return 2 /* u16 header */ + fixed_region + heap;
         }
         else
         {
            // Conservative fallback: produce-then-measure.
            sink_t tmp;
            encode_value<W>(v, tmp);
            return tmp.size();
         }
      }

      // ── Validation (structural) ───────────────────────────────────────────

      template <std::size_t W, typename T>
      codec_status validate_value(std::span<const char> src, std::size_t pos,
                                  std::size_t end) noexcept
      {
         if constexpr (is_fixed_v<T>)
            return (end - pos) >= fixed_size_of<T>()
                      ? codec_ok()
                      : codec_fail("frac: buffer too small for fixed type",
                                   static_cast<std::uint32_t>(pos), "frac");
         else if constexpr (std::is_same_v<T, std::string>)
            return codec_ok();
         else if constexpr (is_std_vector_v<T>)
            return (end - pos) >= W
                      ? codec_ok()
                      : codec_fail("frac: vector length prefix truncated",
                                   static_cast<std::uint32_t>(pos), "frac");
         else if constexpr (Record<T>)
            return (end - pos) >= W
                      ? codec_ok()
                      : codec_fail("frac: record header truncated",
                                   static_cast<std::uint32_t>(pos), "frac");
         else
            return codec_ok();
      }

   }  // namespace detail::frac_impl

   template <std::size_t W>
   struct frac_ : format_tag_base<frac_<W>>
   {
      static constexpr std::size_t word_width = W;

      // fracpack is a binary format — delegates to the binary
      // adapter slot when a type registers one.
      using preferred_presentation_category = ::psio3::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio3::encode), frac_<W>,
                             const T& v, std::vector<char>& sink)
      {
         detail::frac_impl::encode_value<W>(v, sink);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio3::encode),
                                          frac_<W>, const T& v)
      {
         // Pre-size from the real packsize walker (no longer a
         // produce-then-measure). With resize-backed append_bytes,
         // every byte lands via direct memcpy — no reallocations
         // during the three-phase pack.
         std::vector<char> out;
         out.reserve(detail::frac_impl::size_of_v<W>(v));
         detail::frac_impl::encode_value<W>(v, out);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio3::decode<T>), frac_<W>, T*,
                          std::span<const char> bytes)
      {
         return detail::frac_impl::decode_value<W, T>(bytes, 0, bytes.size());
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio3::size_of), frac_<W>,
                                    const T& v)
      {
         return detail::frac_impl::size_of_v<W>(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate<T>),
                                     frac_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::frac_impl::validate_value<W, T>(bytes, 0,
                                                         bytes.size());
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio3::validate_strict<T>),
                                     frac_<W>, T*,
                                     std::span<const char> bytes) noexcept
      {
         return detail::frac_impl::validate_value<W, T>(bytes, 0,
                                                         bytes.size());
      }

      template <typename T>
      friend std::unique_ptr<T> tag_invoke(decltype(::psio3::make_boxed<T>),
                                           frac_<W>, T*,
                                           std::span<const char> bytes) noexcept
      {
         return std::make_unique<T>(
            detail::frac_impl::decode_value<W, T>(bytes, 0, bytes.size()));
      }
   };

   using frac16 = frac_<2>;
   using frac32 = frac_<4>;

}  // namespace psio3
