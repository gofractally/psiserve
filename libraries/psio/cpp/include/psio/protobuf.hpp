#pragma once
//
// psio/protobuf.hpp — Schema-driven Protocol Buffers codec.
//
// Protobuf is the canonical "schema-required, length-delimited
// tag-and-value" wire format.  Each field is encoded as
//
//     varint(tag = (field_number << 3) | wire_type)
//     <payload, depending on wire_type>
//
// where `wire_type ∈ {0=varint, 1=fixed64, 2=length-delimited,
// 5=fixed32}`.  Field numbers — NOT positions, NOT names — are the
// stable wire identity, so old senders / new receivers can interop
// across schema evolution as long as the field number stays put.
//
// Field-number assignment (this codec):
//
//   The N-th field in declaration order gets field_number N+1.  That
//   makes PSIO_REFLECT(T, a, b, c) produce a := 1, b := 2, c := 3 on
//   the wire — the same convention every protobuf compiler defaults
//   to when the .proto specifies fields without explicit numbers.
//   When the schema needs to evolve (insert / remove / reorder a
//   field while keeping wire compatibility) the
//   psio::protobuf_field_number<&T::m> annotation is the override.
//
// Type mapping (C++ → wire_type, default integer encoding):
//
//   bool / [u]int{8,16,32,64} / enum  → varint     (wire_type 0)
//   float                              → fixed32    (wire_type 5)
//   double                             → fixed64    (wire_type 1)
//   std::string / std::string_view     → length-delim (wire_type 2)
//   std::vector<uint8_t> / std::byte   → length-delim (bytes)
//   std::vector<scalar>                → length-delim, PACKED
//                                        (wire_type 2 — concatenated
//                                        varint or fixed payload, no
//                                        per-element tag)
//   std::vector<std::string>           → REPEATED, unpacked (one
//                                        wire_type-2 record per
//                                        element)
//   std::vector<message>               → REPEATED, unpacked
//   std::optional<T>                   → field is omitted when
//                                        empty; present otherwise
//   PSIO_REFLECT'd struct              → length-delim sub-message
//
// Out of scope for the first cut (every entry below has a clear
// landing spot — sint32/sint64 zigzag, fixed-width ints, map<K,V>,
// std::variant→oneof — all annotation-driven additions to the
// dispatch).  Unknown fields are dropped on decode (a strict mode
// is a separate hook).

#include <psio/adapter.hpp>
#include <psio/cpo.hpp>
#include <psio/error.hpp>
#include <psio/format_tag_base.hpp>
#include <psio/reflect.hpp>
#include <psio/stream.hpp>
#include <psio/varint/leb128.hpp>

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio
{

   //  Per-field protobuf field-number override.  Default convention
   //  is "N-th declared field = field_number N+1"; when schema
   //  evolution requires fixing field numbers explicitly, specialise
   //  this template:
   //
   //      template <>
   //      inline constexpr std::uint32_t
   //         psio::protobuf_field_number<&MyMsg::renamed_field> = 7;
   //
   //  (The default fallback below returns 0, which the codec
   //  interprets as "use declaration order.")
   template <auto MemberPtr>
   inline constexpr std::uint32_t protobuf_field_number = 0;

   namespace detail::pb_impl
   {
      // ── Wire-type constants ─────────────────────────────────────
      namespace wt
      {
         constexpr std::uint8_t varint        = 0;
         constexpr std::uint8_t fixed64       = 1;
         constexpr std::uint8_t length_delim  = 2;
         constexpr std::uint8_t fixed32       = 5;
      }  // namespace wt

      // ── varint = LEB128 (matches the protobuf wire format) ─────
      //
      //  Decode goes through psio::varint::leb128::fast::decode_u64,
      //  which picks NEON on aarch64, SSE/SWAR on x86_64, and scalar
      //  elsewhere; ~2x the throughput of the byte-at-a-time loop on
      //  long varints, and zero overhead on the common 1-byte tags
      //  since the fast path checks `avail < 16` and falls back to
      //  scalar for short reads.
      //
      //  Encode is left scalar: the leb128 library's measurements
      //  (varint/leb128.hpp:248) show the encode side is dominated
      //  by the writer's store + advance, with vectorised packing
      //  pulled from below the level the wire-rate measurements care
      //  about.  Keeping the scalar form here avoids dragging
      //  alignment/padding concerns into the protobuf write path.

      inline std::size_t varint_size(std::uint64_t v) noexcept
      {
         return ::psio::varint::leb128::scalar::size_u64(v);
      }

      template <typename Sink>
      inline void emit_varint(std::uint64_t v, Sink& s)
      {
         std::uint8_t buf[10];
         std::size_t  n =
            ::psio::varint::leb128::scalar::encode_u64(buf, v);
         s.write(buf, n);
      }

      inline std::uint64_t read_varint(std::span<const char> src,
                                       std::size_t&          pos)
      {
         //  fast::decode_u64 returns {.value, .size, .ok}.  Up to 16
         //  bytes ahead are read by the SIMD path so the bounds
         //  check is per-call, not per-byte.
         auto avail = src.size() - pos;
         auto r     = ::psio::varint::leb128::fast::decode_u64(
            reinterpret_cast<const std::uint8_t*>(src.data() + pos),
            avail);
         if (!r.ok)
            throw std::runtime_error("protobuf: malformed varint");
         pos += r.len;
         return r.value;
      }

      // Tag = (field_number << 3) | wire_type, varint-encoded.
      template <typename Sink>
      inline void emit_tag(std::uint32_t field, std::uint8_t wt, Sink& s)
      {
         emit_varint((static_cast<std::uint64_t>(field) << 3) | wt, s);
      }

      inline std::size_t tag_size(std::uint32_t field, std::uint8_t /*wt*/) noexcept
      {
         return varint_size(static_cast<std::uint64_t>(field) << 3);
      }

      // ── Endian-fixed payload helpers ────────────────────────────
      template <typename T>
         requires std::is_trivially_copyable_v<T>
      inline T to_le(T v) noexcept
      {
         if constexpr (sizeof(T) == 1 ||
                       std::endian::native == std::endian::little)
            return v;
         else
         {
            unsigned char buf[sizeof(T)];
            std::memcpy(buf, &v, sizeof(T));
            for (std::size_t i = 0; i < sizeof(T) / 2; ++i)
               std::swap(buf[i], buf[sizeof(T) - 1 - i]);
            T out;
            std::memcpy(&out, buf, sizeof(T));
            return out;
         }
      }

      // ── std-container detectors ─────────────────────────────────
      template <typename T>
      struct pb_is_vector : std::false_type {};
      template <typename E, typename A>
      struct pb_is_vector<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename T>
      struct pb_is_optional : std::false_type {};
      template <typename T>
      struct pb_is_optional<std::optional<T>> : std::true_type
      {
         using elem = T;
      };

      template <typename T>
      constexpr bool is_byte_vector_v =
         std::is_same_v<T, std::vector<std::uint8_t>> ||
         std::is_same_v<T, std::vector<std::byte>>;

      template <typename T>
      constexpr bool is_packed_scalar_v =
         std::is_arithmetic_v<T> && !std::is_same_v<T, bool>;

      // ── size_of pass ────────────────────────────────────────────

      //  Forward-decls for recursive message + repeated handling.
      template <typename T>
      std::size_t pb_message_size(const T& v) noexcept;

      template <typename T>
      std::size_t pb_value_size(const T& v) noexcept;

      template <typename T>
      std::size_t pb_value_size(const T& v) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
            return 1;
         else if constexpr (std::is_signed_v<U> && std::is_integral_v<U>)
         {
            //  proto3 default: varint with sign-extended u64 for
            //  negatives.  Negative int32 always costs 10 bytes.
            std::int64_t s = static_cast<std::int64_t>(v);
            return varint_size(static_cast<std::uint64_t>(s));
         }
         else if constexpr (std::is_unsigned_v<U> && std::is_integral_v<U>)
            return varint_size(static_cast<std::uint64_t>(v));
         else if constexpr (std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return varint_size(v.size()) + v.size();
         else if constexpr (Reflected<U>)
         {
            std::size_t n = pb_message_size(v);
            return varint_size(n) + n;
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported value type");
            return 0;
         }
      }

      template <typename T, auto MemberPtr>
      constexpr std::uint32_t resolve_field_number(std::size_t source_idx) noexcept
      {
         constexpr auto override_v =
            ::psio::protobuf_field_number<MemberPtr>;
         if constexpr (override_v != 0)
            return override_v;
         else
            return static_cast<std::uint32_t>(source_idx + 1);
      }

      template <typename T>
      std::size_t pb_field_size(const T& v, std::uint32_t field) noexcept
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (pb_is_optional<U>::value)
         {
            if (!v.has_value())
               return 0;
            return pb_field_size<typename pb_is_optional<U>::elem>(*v, field);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            //  bytes: tag + varint(len) + raw bytes.
            return tag_size(field, wt::length_delim) +
                   varint_size(v.size()) + v.size();
         }
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if (v.empty())
               return 0;
            if constexpr (is_packed_scalar_v<E>)
            {
               //  Packed: tag + varint(payload_len) + payload.
               std::size_t payload = 0;
               for (auto const& x : v)
                  payload += pb_value_size(x);
               return tag_size(field, wt::length_delim) +
                      varint_size(payload) + payload;
            }
            else
            {
               //  Unpacked: per-element (tag + value-with-its-own-length).
               std::size_t total = 0;
               for (auto const& x : v)
                  total += tag_size(field, wt::length_delim) +
                           pb_value_size(x);
               return total;
            }
         }
         else if constexpr (std::is_same_v<U, bool> ||
                            std::is_integral_v<U>)
            return tag_size(field, wt::varint) + pb_value_size(v);
         else if constexpr (std::is_same_v<U, float>)
            return tag_size(field, wt::fixed32) + 4;
         else if constexpr (std::is_same_v<U, double>)
            return tag_size(field, wt::fixed64) + 8;
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
            return tag_size(field, wt::length_delim) +
                   varint_size(v.size()) + v.size();
         else if constexpr (Reflected<U>)
            return tag_size(field, wt::length_delim) + pb_value_size(v);
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported field type");
            return 0;
         }
      }

      template <typename T>
      std::size_t pb_message_size(const T& v) noexcept
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         std::size_t    total = 0;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            ((total += pb_field_size(
                 v.*(R::template member_pointer<Is>),
                 resolve_field_number<T, R::template member_pointer<Is>>(Is))),
             ...);
         }(std::make_index_sequence<N>{});
         return total;
      }

      // ── encode pass ─────────────────────────────────────────────

      template <typename T, typename Sink>
      void pb_write_value(const T& v, Sink& s);

      template <typename T, typename Sink>
      void pb_write_message(const T& v, Sink& s);

      template <typename T, typename Sink>
      void pb_write_value(const T& v, Sink& s)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            std::uint8_t b = v ? 1 : 0;
            s.write(&b, 1);
         }
         else if constexpr (std::is_signed_v<U> && std::is_integral_v<U>)
         {
            std::int64_t i = static_cast<std::int64_t>(v);
            emit_varint(static_cast<std::uint64_t>(i), s);
         }
         else if constexpr (std::is_unsigned_v<U> && std::is_integral_v<U>)
            emit_varint(static_cast<std::uint64_t>(v), s);
         else if constexpr (std::is_same_v<U, float>)
         {
            std::uint32_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            std::uint32_t le = to_le(bits);
            s.write(&le, sizeof(le));
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            std::uint64_t bits;
            std::memcpy(&bits, &v, sizeof(bits));
            std::uint64_t le = to_le(bits);
            s.write(&le, sizeof(le));
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
         {
            emit_varint(v.size(), s);
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (Reflected<U>)
         {
            //  length-delimited sub-message: varint(size) then body.
            emit_varint(pb_message_size(v), s);
            pb_write_message(v, s);
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported value type");
         }
      }

      template <typename T, typename Sink>
      void pb_write_field(const T& v, std::uint32_t field, Sink& s)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (pb_is_optional<U>::value)
         {
            if (!v.has_value())
               return;
            pb_write_field<typename pb_is_optional<U>::elem>(*v, field, s);
         }
         else if constexpr (is_byte_vector_v<U>)
         {
            emit_tag(field, wt::length_delim, s);
            emit_varint(v.size(), s);
            if (!v.empty())
               s.write(v.data(), v.size());
         }
         else if constexpr (pb_is_vector<U>::value)
         {
            using E = typename pb_is_vector<U>::elem;
            if (v.empty())
               return;
            if constexpr (is_packed_scalar_v<E>)
            {
               //  Packed: one tag, varint(payload_len), values.
               std::size_t payload = 0;
               for (auto const& x : v)
                  payload += pb_value_size(x);
               emit_tag(field, wt::length_delim, s);
               emit_varint(payload, s);
               for (auto const& x : v)
                  pb_write_value(x, s);
            }
            else
            {
               //  Unpacked: one tag per element.
               for (auto const& x : v)
               {
                  emit_tag(field, wt::length_delim, s);
                  pb_write_value(x, s);
               }
            }
         }
         else if constexpr (std::is_same_v<U, bool> ||
                            std::is_integral_v<U>)
         {
            emit_tag(field, wt::varint, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            emit_tag(field, wt::fixed32, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            emit_tag(field, wt::fixed64, s);
            pb_write_value(v, s);
         }
         else if constexpr (std::is_same_v<U, std::string> ||
                            std::is_same_v<U, std::string_view>)
         {
            emit_tag(field, wt::length_delim, s);
            pb_write_value(v, s);
         }
         else if constexpr (Reflected<U>)
         {
            emit_tag(field, wt::length_delim, s);
            pb_write_value(v, s);
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported field type");
         }
      }

      template <typename T, typename Sink>
      void pb_write_message(const T& v, Sink& s)
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            ((pb_write_field(
                 v.*(R::template member_pointer<Is>),
                 resolve_field_number<T, R::template member_pointer<Is>>(Is),
                 s)),
             ...);
         }(std::make_index_sequence<N>{});
      }

      // ── decode pass ─────────────────────────────────────────────
      //
      //  Streaming walk over a length-delimited message body.  At
      //  each iteration we read a tag, look up the matching field by
      //  number on the C++ side (compile-time fold over the
      //  reflected fields), and route the value into the
      //  corresponding member.  Unknown fields are skipped according
      //  to wire_type so the stream stays aligned.

      inline void skip_field(std::span<const char> src,
                             std::size_t&          pos,
                             std::uint8_t          wire_type)
      {
         switch (wire_type)
         {
            case wt::varint:
               (void)read_varint(src, pos);
               return;
            case wt::fixed64:
               if (pos + 8 > src.size())
                  throw std::runtime_error(
                     "protobuf: truncated fixed64");
               pos += 8;
               return;
            case wt::length_delim:
            {
               std::uint64_t n = read_varint(src, pos);
               if (pos + n > src.size())
                  throw std::runtime_error(
                     "protobuf: length-delim overruns buffer");
               pos += n;
               return;
            }
            case wt::fixed32:
               if (pos + 4 > src.size())
                  throw std::runtime_error(
                     "protobuf: truncated fixed32");
               pos += 4;
               return;
            default:
               throw std::runtime_error(
                  "protobuf: deprecated/unknown wire type");
         }
      }

      template <typename T>
      void pb_read_value(std::span<const char> src, std::size_t& pos,
                         std::uint8_t wire_type, T& out);

      template <typename T>
      void pb_decode_message(T&                    out,
                             std::span<const char> src,
                             std::size_t           start,
                             std::size_t           end);

      template <typename T>
      void pb_read_value(std::span<const char> src, std::size_t& pos,
                         std::uint8_t wire_type, T& out)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            if (wire_type != wt::varint)
               throw std::runtime_error("protobuf: bool wire-type mismatch");
            out = read_varint(src, pos) != 0;
         }
         else if constexpr (std::is_integral_v<U>)
         {
            if (wire_type != wt::varint)
               throw std::runtime_error("protobuf: int wire-type mismatch");
            std::uint64_t raw = read_varint(src, pos);
            //  Truncate / sign-extend to T as the wire intends —
            //  proto3 sends negative signed ints sign-extended to
            //  u64, so a simple cast back to T recovers the value.
            out = static_cast<U>(raw);
         }
         else if constexpr (std::is_same_v<U, float>)
         {
            if (wire_type != wt::fixed32)
               throw std::runtime_error(
                  "protobuf: float wire-type mismatch");
            if (pos + 4 > src.size())
               throw std::runtime_error("protobuf: truncated f32");
            std::uint32_t bits;
            std::memcpy(&bits, src.data() + pos, 4);
            bits = to_le(bits);
            std::memcpy(&out, &bits, 4);
            pos += 4;
         }
         else if constexpr (std::is_same_v<U, double>)
         {
            if (wire_type != wt::fixed64)
               throw std::runtime_error(
                  "protobuf: double wire-type mismatch");
            if (pos + 8 > src.size())
               throw std::runtime_error("protobuf: truncated f64");
            std::uint64_t bits;
            std::memcpy(&bits, src.data() + pos, 8);
            bits = to_le(bits);
            std::memcpy(&out, &bits, 8);
            pos += 8;
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            if (wire_type != wt::length_delim)
               throw std::runtime_error(
                  "protobuf: string wire-type mismatch");
            std::uint64_t n = read_varint(src, pos);
            if (pos + n > src.size())
               throw std::runtime_error(
                  "protobuf: string overruns buffer");
            out.assign(src.data() + pos, n);
            pos += n;
         }
         else if constexpr (Reflected<U>)
         {
            if (wire_type != wt::length_delim)
               throw std::runtime_error(
                  "protobuf: message wire-type mismatch");
            std::uint64_t n = read_varint(src, pos);
            if (pos + n > src.size())
               throw std::runtime_error(
                  "protobuf: sub-message overruns buffer");
            pb_decode_message(out, src, pos, pos + n);
            pos += n;
         }
         else
         {
            static_assert(sizeof(U) == 0,
                          "psio::protobuf: unsupported value type");
         }
      }

      //  Read a packed-or-unpacked repeated field's incoming chunk
      //  into the provided vector reference.  The vector accumulates
      //  across multiple wire-tag occurrences (proto3 allows a
      //  repeated field's elements to arrive via mixed packed +
      //  unpacked chunks, all concatenated).
      template <typename V>
      void pb_read_repeated(std::span<const char> src, std::size_t& pos,
                            std::uint8_t wire_type, V& out)
      {
         using E = typename pb_is_vector<V>::elem;
         if constexpr (is_packed_scalar_v<E>)
         {
            //  Packed: wire is length-delim with concatenated values
            //  and no per-element tag.
            if (wire_type == wt::length_delim)
            {
               std::uint64_t n   = read_varint(src, pos);
               std::size_t   end = pos + n;
               if (end > src.size())
                  throw std::runtime_error(
                     "protobuf: packed array overruns buffer");
               while (pos < end)
               {
                  E v;
                  //  Each packed element uses the wire_type implied
                  //  by E (varint / fixed32 / fixed64), not 2.
                  std::uint8_t inner_wt =
                     std::is_floating_point_v<E>
                        ? (sizeof(E) == 4 ? wt::fixed32 : wt::fixed64)
                        : wt::varint;
                  pb_read_value(src, pos, inner_wt, v);
                  out.push_back(std::move(v));
               }
            }
            else
            {
               //  Permit a sender that opted out of packing.
               E v;
               pb_read_value(src, pos, wire_type, v);
               out.push_back(std::move(v));
            }
         }
         else
         {
            //  Unpacked: one element per tag occurrence.
            E v;
            pb_read_value(src, pos, wire_type, v);
            out.push_back(std::move(v));
         }
      }

      template <typename T>
      void pb_decode_message(T&                    out,
                             std::span<const char> src,
                             std::size_t           start,
                             std::size_t           end)
      {
         using R          = ::psio::reflect<T>;
         constexpr auto N = R::member_count;
         std::size_t pos  = start;
         while (pos < end)
         {
            std::uint64_t tag        = read_varint(src, pos);
            std::uint32_t field_num  = static_cast<std::uint32_t>(tag >> 3);
            std::uint8_t  wire_type  = static_cast<std::uint8_t>(tag & 0x7);

            //  Compile-time fold over reflected fields: the matching
            //  one routes the value, the rest fall through to a
            //  skip().
            bool handled = false;
            [&]<std::size_t... Is>(std::index_sequence<Is...>)
            {
               ((
                  [&]
                  {
                     if (handled)
                        return;
                     constexpr auto fn = resolve_field_number<
                        T, R::template member_pointer<Is>>(Is);
                     if (field_num != fn)
                        return;
                     using FT = std::remove_cvref_t<
                        typename R::template member_type<Is>>;
                     auto& fref = out.*(R::template member_pointer<Is>);

                     if constexpr (pb_is_optional<FT>::value)
                     {
                        using E = typename pb_is_optional<FT>::elem;
                        E inner{};
                        pb_read_value(src, pos, wire_type, inner);
                        fref = std::move(inner);
                     }
                     else if constexpr (is_byte_vector_v<FT>)
                     {
                        if (wire_type != wt::length_delim)
                           throw std::runtime_error(
                              "protobuf: bytes wire-type mismatch");
                        std::uint64_t n = read_varint(src, pos);
                        if (pos + n > src.size())
                           throw std::runtime_error(
                              "protobuf: bytes overrun");
                        fref.assign(
                           reinterpret_cast<const std::uint8_t*>(
                              src.data() + pos),
                           reinterpret_cast<const std::uint8_t*>(
                              src.data() + pos + n));
                        pos += n;
                     }
                     else if constexpr (pb_is_vector<FT>::value)
                     {
                        pb_read_repeated(src, pos, wire_type, fref);
                     }
                     else
                     {
                        pb_read_value(src, pos, wire_type, fref);
                     }
                     handled = true;
                  }()),
                ...);
            }(std::make_index_sequence<N>{});

            if (!handled)
               skip_field(src, pos, wire_type);
         }
         if (pos != end)
            throw std::runtime_error(
               "protobuf: message body length mismatch");
      }

   }  // namespace detail::pb_impl

   struct protobuf : format_tag_base<protobuf>
   {
      using preferred_presentation_category = ::psio::binary_category;

      template <typename T>
      friend void tag_invoke(decltype(::psio::encode), protobuf,
                             const T& v, std::vector<char>& sink)
      {
         ::psio::vector_stream vs{sink};
         detail::pb_impl::pb_write_message(v, vs);
      }

      template <typename T>
      friend std::vector<char> tag_invoke(decltype(::psio::encode),
                                          protobuf, const T& v)
      {
         std::vector<char>       out(detail::pb_impl::pb_message_size(v));
         ::psio::fast_buf_stream fbs{out.data(), out.size()};
         detail::pb_impl::pb_write_message(v, fbs);
         return out;
      }

      template <typename T>
      friend T tag_invoke(decltype(::psio::decode<T>), protobuf, T*,
                          std::span<const char> bytes)
      {
         T out{};
         detail::pb_impl::pb_decode_message(out, bytes, 0, bytes.size());
         return out;
      }

      template <typename T>
      friend std::size_t tag_invoke(decltype(::psio::size_of), protobuf,
                                    const T& v)
      {
         return detail::pb_impl::pb_message_size(v);
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate<T>),
                                     protobuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         try
         {
            T tmp{};
            detail::pb_impl::pb_decode_message(tmp, bytes, 0, bytes.size());
            return codec_ok();
         }
         catch (const std::exception& e)
         {
            return codec_fail(e.what(), 0, "protobuf");
         }
      }

      template <typename T>
      friend codec_status tag_invoke(decltype(::psio::validate_strict<T>),
                                     protobuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         //  protobuf has no canonical encoding (e.g. unknown fields
         //  are spec-permitted), so strict mode collapses to the
         //  same check as validate today.  A separate hook can layer
         //  on "reject unknown fields" semantics when needed.
         return tag_invoke(::psio::validate<T>, protobuf{},
                           static_cast<T*>(nullptr), bytes);
      }
   };

}  // namespace psio
