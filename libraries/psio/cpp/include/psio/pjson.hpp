#pragma once
//
// psio/pjson.hpp — pjson value format.
//
// A pjson value is a (ptr, size) span where:
//
//   ptr[0]          tag byte  (high nibble = type, low nibble = payload-specific)
//   ptr[1 .. size)  raw value bits per the tag type
//
// Containers (array, object) embed an index at the *front* of their
// raw-value-bits region:
//
//   [num_fields:  2-bit-prefix varuint]      1..4 bytes
//   [hash[N]:     N × u8]                    objects only
//   [slot[N]:     N × u32 LE]                packed { offset:24, key_size:8 }
//                                            (key_size unused for arrays)
//   [value_data:  contiguous bytes]
//     for each i in 0..N:
//       object: [key bytes (key_size)][tag][raw value bits]
//               or, when key_size == 0xFF:
//                  [varuint excess][key bytes (0xFF + excess)][tag][raw]
//       array:  [tag][raw value bits]
//
// Entry size of child i: slot[i+1].offset - slot[i].offset
//                         (or, for the last child,
//                          container_value_data_size - slot[N-1].offset).
//
// Tag types:
//   0  null              raw = 0 bytes
//   1  bool              raw = 0;  low nibble: 0=false, 1=true
//   3  uint_inline       raw = 0;  unsigned value in low nibble (0..15)
//   4  int               raw = (low_nibble+1) bytes;  zigzag-LE mantissa
//   5  decimal           raw = (low_nibble+1) mantissa bytes + varscale (1..4)
//   6  ieee_float        raw = 8 bytes
//   8  string            raw = (size - 1) bytes; low nibble flag:
//                          0=raw_text, 1=escape_form, 2=binary
//   B  array             raw = container content
//   C  object            raw = container content
//   2, 7, 9, A, D..F     reserved
//
// No magic, no version, no flags. Versioning lives in the application
// wrapper (HTTP content-type, file header, RPC envelope). Buffer length
// is the value's size — caller's responsibility to provide.
//
// This file owns:
//   * pjson_number             — unified numeric value type (i128 + i32)
//   * pjson_value              — variant tree (test/build helper, not the
//                                primary consumer surface; that's pjson_view)
//   * pjson::encode / decode / validate
//   * Internal encode / decode helpers used by pjson_view, pjson_typed,
//     pjson_json.

#include <algorithm>
#include <bit>
#include <charconv>
#include <cmath>
#include <compare>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#define XXH_INLINE_ALL
#include <hash/xxhash.h>

namespace psio {

   // ── numeric value type ────────────────────────────────────────────────

   struct pjson_number
   {
      __int128     mantissa = 0;
      std::int32_t scale    = 0;

      pjson_number() = default;
      constexpr pjson_number(__int128 m, std::int32_t s = 0) noexcept
          : mantissa(m), scale(s)
      {
      }
      template <typename T>
         requires(std::is_integral_v<T> && !std::is_same_v<T, bool>)
      constexpr pjson_number(T v) noexcept
          : mantissa(static_cast<__int128>(v)), scale(0)
      {
      }

      static pjson_number from_double(double d);
      static pjson_number from_string(std::string_view s);

      bool is_integer() const noexcept { return scale == 0; }

      std::optional<std::int64_t> to_int64() const noexcept
      {
         if (scale != 0) return std::nullopt;
         if (mantissa < std::numeric_limits<std::int64_t>::min() ||
             mantissa > std::numeric_limits<std::int64_t>::max())
            return std::nullopt;
         return static_cast<std::int64_t>(mantissa);
      }

      double to_double() const noexcept
      {
         double m = static_cast<double>(mantissa);
         if (scale == 0) return m;
         double       p    = 1.0;
         std::int32_t s    = scale > 0 ? scale : -scale;
         double       base = 10.0;
         while (s)
         {
            if (s & 1) p *= base;
            base *= base;
            s >>= 1;
         }
         return scale > 0 ? m * p : m / p;
      }

      friend bool operator==(const pjson_number&,
                             const pjson_number&) noexcept = default;

      friend std::strong_ordering compare(const pjson_number& a,
                                          const pjson_number& b) noexcept
      {
         if (a.scale == b.scale) return a.mantissa <=> b.mantissa;
         std::int32_t diff =
             a.scale < b.scale ? b.scale - a.scale : a.scale - b.scale;
         __int128 mult = 1;
         for (std::int32_t i = 0; i < diff && mult > 0; ++i) mult *= 10;
         if (a.scale < b.scale) return a.mantissa <=> (b.mantissa * mult);
         return (a.mantissa * mult) <=> b.mantissa;
      }
      friend bool numerically_equal(const pjson_number& a,
                                    const pjson_number& b) noexcept
      {
         return compare(a, b) == 0;
      }
   };

   // ── pjson_value variant tree (build/test helper) ──────────────────────

   struct pjson_value;
   struct pjson_null
   {
      friend bool operator==(pjson_null, pjson_null) noexcept { return true; }
   };
   using pjson_object = std::vector<std::pair<std::string, pjson_value>>;
   using pjson_array  = std::vector<pjson_value>;
   using pjson_bytes  = std::vector<std::uint8_t>;

   struct pjson_value
   {
      using variant_t = std::variant<pjson_null,
                                     bool,
                                     std::int64_t,
                                     double,
                                     pjson_number,
                                     std::string,
                                     pjson_bytes,
                                     pjson_array,
                                     pjson_object>;
      variant_t v;

      pjson_value() : v(pjson_null{}) {}
      template <typename T>
         requires(!std::is_same_v<std::decay_t<T>, pjson_value>)
      pjson_value(T&& x) : v(std::forward<T>(x))
      {
      }

      template <typename T>
      bool holds() const noexcept
      {
         return std::holds_alternative<T>(v);
      }
      template <typename T>
      const T& as() const
      {
         return std::get<T>(v);
      }
      template <typename T>
      T& as()
      {
         return std::get<T>(v);
      }
      bool is_null() const noexcept { return holds<pjson_null>(); }

      friend bool operator==(const pjson_value&, const pjson_value&) = default;
   };

   // ── implementation detail ─────────────────────────────────────────────

   namespace pjson_detail {

      // Allocator that *skips* zero-init on default-construction.
      // std::vector<T>::resize(n) calls allocator::construct(p) with no
      // args for each new element, which for the default allocator
      // value-initializes T (zero-init for trivial types). When we
      // resize-then-overwrite (the common pattern in this encoder), the
      // zero-init is wasted work. With this allocator, construct(p) is
      // a no-op; subsequent writes (memcpy / direct stores) are the
      // ONLY initialization those bytes ever see.
      //
      // Forwards every other construct() form to standard placement new
      // so vector::insert, push_back, emplace, etc. still work.
      template <typename T>
      struct uninit_alloc
      {
         using value_type = T;

         uninit_alloc() noexcept = default;
         template <typename U>
         uninit_alloc(const uninit_alloc<U>&) noexcept
         {
         }

         T* allocate(std::size_t n)
         {
            return std::allocator<T>{}.allocate(n);
         }
         void deallocate(T* p, std::size_t n) noexcept
         {
            std::allocator<T>{}.deallocate(p, n);
         }

         template <typename U>
         void construct(U*) noexcept
         { /* skip default-init */
         }
         template <typename U, typename A, typename... Args>
         void construct(U* p, A&& a, Args&&... args)
         {
            ::new ((void*)p) U(std::forward<A>(a),
                               std::forward<Args>(args)...);
         }
         template <typename U>
         void destroy(U* p) noexcept
         {
            if constexpr (!std::is_trivially_destructible_v<U>) p->~U();
         }

         template <typename U>
         bool operator==(const uninit_alloc<U>&) const noexcept
         {
            return true;
         }
         template <typename U>
         bool operator!=(const uninit_alloc<U>&) const noexcept
         {
            return false;
         }
      };

      // Tag-byte type codes (high nibble of the tag byte).
      // Low-nibble usage is per-type; see notes below.
      enum : std::uint8_t
      {
         t_null        = 0,
         t_bool        = 1,    // low nibble: 0 = false, 1 = true
         // 2 reserved
         t_uint_inline = 3,    // low nibble: unsigned value 0..15
         t_int         = 4,    // low nibble: byte_count - 1 (1..16)
         t_decimal     = 5,    // low nibble: mantissa byte_count - 1
         t_ieee_float  = 6,
         t_string      = 8,    // low nibble: encoding flag (see below)
         // 9, 10 (was bytes), reserved — bytes is now t_string with
         //                              string_flag_binary
         t_array       = 0xB,
         t_object      = 0xC,
      };

      // Sub-encoding flags for t_string (low nibble of the tag).
      // Tells the JSON emitter what to do with the stored bytes;
      // tells consumers how to interpret them.
      enum : std::uint8_t
      {
         // Text bytes that have NOT been JSON-escaped — JSON emit
         // must run a per-character escape pass.
         string_flag_raw_text       = 0,
         // Text bytes already in JSON-escape form (`\n`, `\"`,
         // `\uXXXX` etc are LITERAL bytes in the buffer). JSON emit
         // wraps in quotes verbatim, no escape work.
         string_flag_escape_form    = 1,
         // Binary bytes — JSON emit must base64-encode them.
         string_flag_binary         = 2,
         // 3..15: reserved
      };

      // Strip a trailing ".tag" suffix from a key for the prefilter hash.
      // "amount.decimal" → hash bucket of "amount". Stored keys with a
      // presentation suffix and bare-name lookups land in the same hash
      // bucket; byte-exact verify still distinguishes them.
      inline std::string_view strip_key_suffix(std::string_view k) noexcept
      {
         auto dot = k.rfind('.');
         return dot == std::string_view::npos ? k : k.substr(0, dot);
      }

      // 8-bit prefilter hash: low byte of XXH3-64 over the suffix-stripped
      // key.
      inline std::uint8_t key_hash8(std::string_view k) noexcept
      {
         auto base = strip_key_suffix(k);
         return static_cast<std::uint8_t>(
             XXH3_64bits(base.data(), base.size()));
      }

      // ── 2-bit-prefix varuint ──────────────────────────────────────────
      //
      // First byte: high 2 bits = total byte count - 1 (0..3 → 1..4),
      //             low 6 bits  = lowest 6 bits of value.
      // Subsequent bytes: next 8 bits each (LE).
      //
      // Capacities: 6 / 14 / 22 / 30 bits.
      // Used for: num_fields counts, decimal varscale, long-key excess.

      inline std::size_t varuint62_byte_count(std::uint64_t v) noexcept
      {
         if (v <= 0x3Fu)        return 1;
         if (v <= 0x3FFFu)      return 2;
         if (v <= 0x3FFFFFu)    return 3;
         return 4;
      }
      inline std::size_t write_varuint62(std::uint8_t* dst,
                                         std::size_t   pos,
                                         std::uint64_t v) noexcept
      {
         std::size_t  n      = varuint62_byte_count(v);
         std::uint8_t prefix = static_cast<std::uint8_t>((n - 1) << 6);
         dst[pos]            = prefix | static_cast<std::uint8_t>(v & 0x3Fu);
         std::uint64_t hi    = v >> 6;
         for (std::size_t i = 1; i < n; ++i)
         {
            dst[pos + i] = static_cast<std::uint8_t>(hi & 0xFFu);
            hi >>= 8;
         }
         return n;
      }
      // Read into *out*; returns bytes consumed, 0 on failure.
      inline std::size_t read_varuint62(const std::uint8_t* buf,
                                        std::size_t         len,
                                        std::uint64_t&      out) noexcept
      {
         if (len == 0) return 0;
         std::uint8_t b0 = buf[0];
         std::size_t  n  = static_cast<std::size_t>(b0 >> 6) + 1;
         if (n > len) return 0;
         std::uint64_t v = b0 & 0x3Fu;
         for (std::size_t i = 1; i < n; ++i)
            v |= static_cast<std::uint64_t>(buf[i]) << (6 + (i - 1) * 8);
         out = v;
         return n;
      }

      // ── 2-bit-prefix varint (signed) — used for decimal scale ─────────
      inline std::size_t varint62_byte_count(std::int32_t v) noexcept
      {
         std::uint64_t zz =
             (static_cast<std::uint64_t>(v) << 1) ^
             static_cast<std::uint64_t>(static_cast<std::int64_t>(v) >> 31);
         return varuint62_byte_count(zz);
      }
      inline std::size_t write_varint62(std::uint8_t* dst,
                                        std::size_t   pos,
                                        std::int32_t  v) noexcept
      {
         std::uint64_t zz =
             (static_cast<std::uint64_t>(v) << 1) ^
             static_cast<std::uint64_t>(static_cast<std::int64_t>(v) >> 31);
         return write_varuint62(dst, pos, zz);
      }
      inline std::size_t read_varint62(const std::uint8_t* buf,
                                       std::size_t         len,
                                       std::int32_t&       out) noexcept
      {
         std::uint64_t zz;
         std::size_t   n = read_varuint62(buf, len, zz);
         if (n == 0) return 0;
         std::int64_t s = static_cast<std::int64_t>(
             (zz >> 1) ^ (~(zz & 1) + 1));
         out = static_cast<std::int32_t>(s);
         return n;
      }

      // ── zigzag (i128) ─────────────────────────────────────────────────
      inline __uint128_t zz128_encode(__int128 v) noexcept
      {
         return (static_cast<__uint128_t>(v) << 1) ^
                static_cast<__uint128_t>(v >> 127);
      }
      inline __int128 zz128_decode(__uint128_t v) noexcept
      {
         return static_cast<__int128>((v >> 1) ^ (~(v & 1) + 1));
      }
      inline std::uint8_t mantissa_byte_count(__uint128_t zz) noexcept
      {
         if (zz == 0) return 1;
         std::uint64_t hi = static_cast<std::uint64_t>(zz >> 64);
         std::uint64_t lo = static_cast<std::uint64_t>(zz);
         int bits = hi != 0 ? (128 - std::countl_zero(hi))
                            : (64 - std::countl_zero(lo));
         return static_cast<std::uint8_t>((bits + 7) / 8);
      }

      // ── slot packing { offset:24, key_size:8 } ───────────────────────
      inline std::uint32_t pack_slot(std::uint32_t offset,
                                     std::uint8_t  key_size) noexcept
      {
         return (offset & 0x00FFFFFFu) |
                (static_cast<std::uint32_t>(key_size) << 24);
      }
      inline std::uint32_t slot_offset(std::uint32_t s) noexcept
      {
         return s & 0x00FFFFFFu;
      }
      inline std::uint8_t slot_key_size(std::uint32_t s) noexcept
      {
         return static_cast<std::uint8_t>(s >> 24);
      }

      // LE u32 read/write.
      inline std::uint32_t read_u32_le(const std::uint8_t* p) noexcept
      {
         std::uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }
      inline void write_u32_le(std::uint8_t* p, std::uint32_t v) noexcept
      {
         std::memcpy(p, &v, 4);
      }

      // ── value sizing (recursion over pjson_value) ─────────────────────
      inline std::size_t value_size(const pjson_value& v) noexcept;

      inline std::size_t uint_inline_size() noexcept { return 1; }
      inline std::size_t int_size(std::int64_t i) noexcept
      {
         if (i >= 0 && i <= 15) return 1;
         std::uint64_t zz =
             (static_cast<std::uint64_t>(i) << 1) ^
             static_cast<std::uint64_t>(i >> 63);
         std::uint8_t bc = 1;
         std::uint64_t t = zz >> 8;
         while (t) { ++bc; t >>= 8; }
         return 1u + bc;
      }
      inline std::size_t number_size(const pjson_number& n) noexcept
      {
         if (n.scale == 0 && n.mantissa >= 0 && n.mantissa <= 15)
            return 1;
         __uint128_t  zz = zz128_encode(n.mantissa);
         std::uint8_t bc = mantissa_byte_count(zz);
         if (n.scale == 0) return 1u + bc;
         return 1u + bc + varint62_byte_count(n.scale);
      }
      inline std::size_t string_size(std::string_view s) noexcept
      {
         return 1u + s.size();
      }
      inline std::size_t bytes_size(const pjson_bytes& b) noexcept
      {
         return 1u + b.size();
      }

      // Container "content" size = num_fields_varuint + hash[N] + slot[N]*4
      //                          + sum(entry_sizes).
      // Container "value" size = 1 (tag) + content size.
      inline std::size_t container_value_data_size_array(
          const pjson_array& a) noexcept
      {
         std::size_t s = 0;
         for (const auto& el : a) s += value_size(el);
         return s;
      }
      inline std::size_t container_value_data_size_object(
          const pjson_object& o) noexcept
      {
         std::size_t s = 0;
         for (const auto& [k, v] : o)
         {
            std::size_t klen_extra = 0;
            if (k.size() >= 0xFFu)
               klen_extra = varuint62_byte_count(k.size() - 0xFFu);
            s += klen_extra + k.size() + value_size(v);
         }
         return s;
      }

      // Tail-indexed container layout:
      //   object:  [tag][value_data][hash[N]][slot[N]][count u16]
      //   array:   [tag][value_data][slot[N]][count u16]
      //
      // Index lives at the END of the container; readers walk backward
      // from container_end (provided by parent's slot or by the
      // top-level caller's buffer length).
      //
      // Single-pass forward encoding: write tag, stream value_data,
      // append hash[N] (objects only), append slot[N], append count.
      // No memmove, no header reservation, no backpatch beyond the
      // SBO of (hash, offset, key_size) tuples we record while
      // walking children.
      inline std::size_t array_size(const pjson_array& a) noexcept
      {
         std::size_t N  = a.size();
         std::size_t vd = container_value_data_size_array(a);
         return 1u + vd + 4 * N + 2;
      }
      inline std::size_t object_size(const pjson_object& o) noexcept
      {
         std::size_t N  = o.size();
         std::size_t vd = container_value_data_size_object(o);
         return 1u + vd + N + 4 * N + 2;
      }

      inline std::size_t value_size(const pjson_value& v) noexcept
      {
         return std::visit(
             [](const auto& x) -> std::size_t {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, pjson_null>) return 1;
                else if constexpr (std::is_same_v<T, bool>) return 1;
                else if constexpr (std::is_same_v<T, std::int64_t>)
                   return int_size(x);
                else if constexpr (std::is_same_v<T, double>)
                {
                   if (!std::isfinite(x)) return 9;
                   pjson_number n   = pjson_number::from_double(x);
                   std::size_t  dec = number_size(n);
                   return dec < 9 ? dec : 9;
                }
                else if constexpr (std::is_same_v<T, pjson_number>)
                   return number_size(x);
                else if constexpr (std::is_same_v<T, std::string>)
                   return string_size(x);
                else if constexpr (std::is_same_v<T, pjson_bytes>)
                   return bytes_size(x);
                else if constexpr (std::is_same_v<T, pjson_array>)
                   return array_size(x);
                else if constexpr (std::is_same_v<T, pjson_object>)
                   return object_size(x);
             },
             v.v);
      }

      // ── encoding ──────────────────────────────────────────────────────
      // All `encode_*_at` helpers write at dst[pos..pos+N) and return N.

      inline std::size_t encode_value_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_value& v);

      inline std::size_t encode_int64_at(std::uint8_t* dst,
                                         std::size_t   pos,
                                         std::int64_t  i) noexcept
      {
         if (i >= 0 && i <= 15)
         {
            dst[pos] = static_cast<std::uint8_t>(
                (t_uint_inline << 4) | static_cast<std::uint8_t>(i));
            return 1;
         }
         std::uint64_t zz =
             (static_cast<std::uint64_t>(i) << 1) ^
             static_cast<std::uint64_t>(i >> 63);
         std::uint8_t bc = 1;
         std::uint64_t t = zz >> 8;
         while (t) { ++bc; t >>= 8; }
         dst[pos] = static_cast<std::uint8_t>((t_int << 4) | (bc - 1));
         std::memcpy(dst + pos + 1, &zz, bc);
         return 1u + bc;
      }
      inline std::size_t encode_number_at(std::uint8_t*       dst,
                                          std::size_t         pos,
                                          const pjson_number& n) noexcept
      {
         if (n.scale == 0 && n.mantissa >= 0 && n.mantissa <= 15)
         {
            dst[pos] = static_cast<std::uint8_t>(
                (t_uint_inline << 4) | static_cast<std::uint8_t>(n.mantissa));
            return 1;
         }
         __uint128_t  zz = zz128_encode(n.mantissa);
         std::uint8_t bc = mantissa_byte_count(zz);
         if (n.scale == 0)
         {
            dst[pos] = static_cast<std::uint8_t>((t_int << 4) | (bc - 1));
            std::memcpy(dst + pos + 1, &zz, bc);
            return 1u + bc;
         }
         dst[pos] = static_cast<std::uint8_t>((t_decimal << 4) | (bc - 1));
         std::memcpy(dst + pos + 1, &zz, bc);
         std::size_t scale_bytes =
             write_varint62(dst, pos + 1 + bc, n.scale);
         return 1u + bc + scale_bytes;
      }
      inline std::size_t encode_double_raw_at(std::uint8_t* dst,
                                              std::size_t   pos,
                                              double        d) noexcept
      {
         dst[pos] = static_cast<std::uint8_t>(t_ieee_float << 4);
         std::memcpy(dst + pos + 1, &d, 8);
         return 9;
      }
      inline std::size_t encode_double_at(std::uint8_t* dst,
                                          std::size_t   pos,
                                          double        d)
      {
         if (!std::isfinite(d)) return encode_double_raw_at(dst, pos, d);
         pjson_number n   = pjson_number::from_double(d);
         std::size_t  dec = number_size(n);
         if (dec < 9) return encode_number_at(dst, pos, n);
         return encode_double_raw_at(dst, pos, d);
      }
      // Encode a text string. Default flag is raw_text (caller didn't
      // pre-escape the bytes; JSON emit will run a per-char escape).
      // Callers that already have escape-form bytes (e.g., from
      // simdjson's escaped_key()) should pass flag=escape_form to skip
      // emitter work later.
      inline std::size_t encode_string_at(std::uint8_t*    dst,
                                          std::size_t      pos,
                                          std::string_view s,
                                          std::uint8_t     flag =
                                              string_flag_raw_text) noexcept
      {
         dst[pos] = static_cast<std::uint8_t>((t_string << 4) | flag);
         if (!s.empty()) std::memcpy(dst + pos + 1, s.data(), s.size());
         return 1u + s.size();
      }
      // Encode raw binary bytes. JSON emit will base64-encode.
      inline std::size_t encode_bytes_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_bytes& b) noexcept
      {
         dst[pos] = static_cast<std::uint8_t>(
             (t_string << 4) | string_flag_binary);
         if (!b.empty()) std::memcpy(dst + pos + 1, b.data(), b.size());
         return 1u + b.size();
      }

      // Tail-indexed array encoder.
      //
      // Layout:  [tag][value_data][slot[N]][count u16]
      //
      // We pre-compute value_data size from container_value_data_size_*
      // so we can place the slot table and count at known absolute
      // positions in the pre-sized output buffer. Children are written
      // forward into value_data; slot[i] is filled directly during the
      // walk. Count is written last.
      inline std::size_t encode_array_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_array& a)
      {
         std::size_t start = pos;
         dst[pos++]        = static_cast<std::uint8_t>(t_array << 4);
         std::size_t N     = a.size();
         std::size_t vd    = container_value_data_size_array(a);
         std::size_t value_data_start = pos;
         std::size_t slot_table_pos   = value_data_start + vd;
         std::size_t count_pos        = slot_table_pos + 4 * N;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t off =
                static_cast<std::uint32_t>(pos - value_data_start);
            write_u32_le(dst + slot_table_pos + i * 4, pack_slot(off, 0));
            pos += encode_value_at(dst, pos, a[i]);
         }
         dst[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
         dst[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
         return count_pos + 2 - start;
      }

      // Tail-indexed object encoder.
      //
      // Layout:  [tag][value_data][hash[N]][slot[N]][count u16]
      //
      // value_data per field: [key bytes (key_size)][tag][raw bits]
      //   or with long-key escape: [varuint excess][key bytes][tag][bits]
      inline std::size_t encode_object_at(std::uint8_t*       dst,
                                          std::size_t         pos,
                                          const pjson_object& o)
      {
         std::size_t start = pos;
         dst[pos++]        = static_cast<std::uint8_t>(t_object << 4);
         std::size_t N     = o.size();
         std::size_t vd    = container_value_data_size_object(o);
         std::size_t value_data_start = pos;
         std::size_t hash_table_pos   = value_data_start + vd;
         std::size_t slot_table_pos   = hash_table_pos + N;
         std::size_t count_pos        = slot_table_pos + 4 * N;
         for (std::size_t i = 0; i < N; ++i)
         {
            const auto& [k, v] = o[i];
            std::uint32_t off  =
                static_cast<std::uint32_t>(pos - value_data_start);
            std::uint8_t ks_byte =
                k.size() < 0xFFu ? static_cast<std::uint8_t>(k.size())
                                 : static_cast<std::uint8_t>(0xFFu);
            write_u32_le(dst + slot_table_pos + i * 4,
                         pack_slot(off, ks_byte));
            dst[hash_table_pos + i] = key_hash8(k);
            if (k.size() >= 0xFFu)
               pos += write_varuint62(dst, pos, k.size() - 0xFFu);
            std::memcpy(dst + pos, k.data(), k.size());
            pos += k.size();
            pos += encode_value_at(dst, pos, v);
         }
         dst[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
         dst[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
         return count_pos + 2 - start;
      }

      inline std::size_t encode_value_at(std::uint8_t*      dst,
                                         std::size_t        pos,
                                         const pjson_value& v)
      {
         return std::visit(
             [&](const auto& x) -> std::size_t {
                using T = std::decay_t<decltype(x)>;
                if constexpr (std::is_same_v<T, pjson_null>)
                {
                   dst[pos] = static_cast<std::uint8_t>(t_null << 4);
                   return 1;
                }
                else if constexpr (std::is_same_v<T, bool>)
                {
                   // tag = (t_bool << 4) | (x ? 1 : 0)
                   dst[pos] = static_cast<std::uint8_t>(
                       (t_bool << 4) | (x ? 1u : 0u));
                   return 1;
                }
                else if constexpr (std::is_same_v<T, std::int64_t>)
                   return encode_int64_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, double>)
                   return encode_double_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_number>)
                   return encode_number_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, std::string>)
                   return encode_string_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_bytes>)
                   return encode_bytes_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_array>)
                   return encode_array_at(dst, pos, x);
                else if constexpr (std::is_same_v<T, pjson_object>)
                   return encode_object_at(dst, pos, x);
             },
             v.v);
      }

      // ── decoding (full materialize into pjson_value tree) ─────────────
      // The (ptr, size) pair fully describes the value.

      inline bool decode_value(const std::uint8_t* p,
                               std::size_t         size,
                               pjson_value&        out);

      // Tail-indexed array decoder. Reads count from the last 2 bytes,
      // then computes index positions backward.
      inline bool decode_array(const std::uint8_t* p,
                               std::size_t         size,
                               pjson_value&        out)
      {
         if (size < 3) return false;  // tag + count u16 minimum
         std::uint16_t N = static_cast<std::uint16_t>(p[size - 2]) |
                           (static_cast<std::uint16_t>(p[size - 1]) << 8);
         std::size_t slot_table_pos = size - 2 - 4 * N;
         if (slot_table_pos < 1 || slot_table_pos > size - 2)
            return false;  // overflow / impossible layout
         std::size_t value_data_start = 1;
         std::size_t value_data_size  = slot_table_pos - 1;

         pjson_array a;
         a.reserve(N);
         for (std::uint16_t i = 0; i < N; ++i)
         {
            std::uint32_t s_i =
                read_u32_le(p + slot_table_pos + i * 4);
            std::uint32_t off_i = slot_offset(s_i);
            std::uint32_t off_next =
                i + 1 < N
                    ? slot_offset(read_u32_le(
                          p + slot_table_pos + (i + 1) * 4))
                    : static_cast<std::uint32_t>(value_data_size);
            if (off_next < off_i || off_next > value_data_size)
               return false;
            pjson_value child;
            if (!decode_value(p + value_data_start + off_i,
                              off_next - off_i, child))
               return false;
            a.push_back(std::move(child));
         }
         out = pjson_value{std::move(a)};
         return true;
      }

      // Tail-indexed object decoder.
      inline bool decode_object(const std::uint8_t* p,
                                std::size_t         size,
                                pjson_value&        out)
      {
         if (size < 3) return false;
         std::uint16_t N = static_cast<std::uint16_t>(p[size - 2]) |
                           (static_cast<std::uint16_t>(p[size - 1]) << 8);
         std::size_t slot_table_pos = size - 2 - 4 * N;
         std::size_t hash_table_pos = slot_table_pos - N;
         if (slot_table_pos < 1 || slot_table_pos > size - 2 ||
             hash_table_pos < 1 || hash_table_pos > slot_table_pos)
            return false;
         std::size_t value_data_start = 1;
         std::size_t value_data_size  = hash_table_pos - 1;

         pjson_object o;
         o.reserve(N);
         for (std::uint16_t i = 0; i < N; ++i)
         {
            std::uint32_t s_i =
                read_u32_le(p + slot_table_pos + i * 4);
            std::uint32_t off_i = slot_offset(s_i);
            std::uint8_t  ks    = slot_key_size(s_i);
            std::uint32_t off_next =
                i + 1 < N
                    ? slot_offset(read_u32_le(
                          p + slot_table_pos + (i + 1) * 4))
                    : static_cast<std::uint32_t>(value_data_size);
            if (off_next < off_i || off_next > value_data_size)
               return false;
            const std::uint8_t* entry      = p + value_data_start + off_i;
            std::size_t         entry_size = off_next - off_i;
            std::size_t         klen_bytes;
            std::size_t         klen;
            if (ks != 0xFF)
            {
               klen       = ks;
               klen_bytes = 0;
            }
            else
            {
               std::uint64_t excess;
               klen_bytes =
                   read_varuint62(entry, entry_size, excess);
               if (klen_bytes == 0) return false;
               klen = 0xFFu + static_cast<std::size_t>(excess);
            }
            if (klen_bytes + klen > entry_size) return false;
            std::string_view k(
                reinterpret_cast<const char*>(entry + klen_bytes), klen);
            if (p[hash_table_pos + i] != key_hash8(k)) return false;
            pjson_value child;
            std::size_t value_off = klen_bytes + klen;
            if (!decode_value(entry + value_off,
                              entry_size - value_off, child))
               return false;
            o.emplace_back(std::string(k), std::move(child));
         }
         out = pjson_value{std::move(o)};
         return true;
      }

      inline bool decode_value(const std::uint8_t* p,
                               std::size_t         size,
                               pjson_value&        out)
      {
         if (size == 0) return false;
         std::uint8_t tag  = p[0];
         std::uint8_t type = tag >> 4;
         std::uint8_t low  = tag & 0x0F;

         switch (type)
         {
            case t_null: out = pjson_value{pjson_null{}}; return size == 1;
            case t_bool:
               // low nibble holds the value: 0 = false, 1 = true.
               // Anything else is an error.
               if (size != 1 || low > 1) return false;
               out = pjson_value{low == 1};
               return true;
            case t_uint_inline:
               out = pjson_value{static_cast<std::int64_t>(low)};
               return size == 1;
            case t_int:
            {
               std::uint8_t bc = static_cast<std::uint8_t>(low + 1);
               if (1u + bc != size) return false;
               if (bc <= 8)
               {
                  std::uint64_t zz = 0;
                  std::memcpy(&zz, p + 1, bc);
                  out = pjson_value{static_cast<std::int64_t>(
                      (zz >> 1) ^ (~(zz & 1) + 1))};
               }
               else
               {
                  __uint128_t zz = 0;
                  std::memcpy(&zz, p + 1, bc);
                  out = pjson_value{
                      pjson_number{zz128_decode(zz), 0}};
               }
               return true;
            }
            case t_decimal:
            {
               std::uint8_t bc = static_cast<std::uint8_t>(low + 1);
               if (1u + bc > size) return false;
               __uint128_t zz = 0;
               std::memcpy(&zz, p + 1, bc);
               std::int32_t scale;
               std::size_t  scale_bytes =
                   read_varint62(p + 1 + bc, size - 1 - bc, scale);
               if (scale_bytes == 0) return false;
               if (1u + bc + scale_bytes != size) return false;
               pjson_number n{zz128_decode(zz), scale};
               // Variant normalize: tag 5 → double when value fits exactly.
               __int128 m_abs =
                   n.mantissa < 0 ? -n.mantissa : n.mantissa;
               if (m_abs < (static_cast<__int128>(1) << 53) &&
                   scale >= -22 && scale <= 22)
               {
                  out = pjson_value{n.to_double()};
                  return true;
               }
               double d = n.to_double();
               if (std::isfinite(d))
               {
                  pjson_number rt = pjson_number::from_double(d);
                  if (rt == n)
                  {
                     out = pjson_value{d};
                     return true;
                  }
               }
               out = pjson_value{n};
               return true;
            }
            case t_ieee_float:
            {
               if (size != 9) return false;
               double d;
               std::memcpy(&d, p + 1, 8);
               out = pjson_value{d};
               return true;
            }
            case t_string:
            {
               // Low-nibble flag tells us whether the bytes are
               // text (variant: std::string) or binary (variant:
               // pjson_bytes). Reject reserved flag values.
               if (low > string_flag_binary) return false;
               if (low == string_flag_binary)
                  out = pjson_value{pjson_bytes(p + 1, p + size)};
               else
                  out = pjson_value{std::string(
                      reinterpret_cast<const char*>(p + 1), size - 1)};
               return true;
            }
            case t_array:  return decode_array(p, size, out);
            case t_object: return decode_object(p, size, out);
            default:       return false;
         }
      }

   }  // namespace pjson_detail

   // ── pjson_number out-of-line definitions ─────────────────────────────

   inline pjson_number pjson_number::from_double(double d)
   {
      if (!std::isfinite(d))
         throw std::invalid_argument("pjson: NaN/Inf not representable");
      char buf[32];
      auto r = std::to_chars(buf, buf + sizeof(buf), d);
      if (r.ec != std::errc{})
         throw std::runtime_error("pjson: to_chars failed");
      std::string_view s(buf, r.ptr - buf);
      bool        neg = false;
      std::size_t i   = 0;
      if (i < s.size() && s[i] == '-') { neg = true; ++i; }
      __int128     mant     = 0;
      std::int32_t scale    = 0;
      bool         seen_dot = false;
      for (; i < s.size(); ++i)
      {
         char c = s[i];
         if (c == '.') { seen_dot = true; continue; }
         if (c == 'e' || c == 'E')
         {
            ++i;
            int sign = 1;
            if (i < s.size() && (s[i] == '+' || s[i] == '-'))
            {
               if (s[i] == '-') sign = -1;
               ++i;
            }
            std::int32_t exp = 0;
            for (; i < s.size(); ++i) exp = exp * 10 + (s[i] - '0');
            scale += sign * exp;
            break;
         }
         mant = mant * 10 + (c - '0');
         if (seen_dot) --scale;
      }
      if (neg) mant = -mant;
      return pjson_number{mant, scale};
   }

   inline pjson_number pjson_number::from_string(std::string_view s)
   {
      std::size_t i = 0, n = s.size();
      auto skip_ws = [&] {
         while (i < n && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' ||
                          s[i] == '\r'))
            ++i;
      };
      skip_ws();
      bool neg = false;
      if (i < n && s[i] == '-') { neg = true; ++i; }
      if (i >= n || s[i] < '0' || s[i] > '9')
         throw std::invalid_argument("pjson: expected digit");
      __int128     mant     = 0;
      std::int32_t scale    = 0;
      bool         seen_dot = false;
      while (i < n)
      {
         char c = s[i];
         if (c == '.')
         {
            if (seen_dot)
               throw std::invalid_argument("pjson: stray '.'");
            seen_dot = true;
            ++i;
            continue;
         }
         if (c == 'e' || c == 'E')
         {
            ++i;
            int sign = 1;
            if (i < n && (s[i] == '+' || s[i] == '-'))
            {
               if (s[i] == '-') sign = -1;
               ++i;
            }
            std::int32_t exp = 0;
            if (i >= n || s[i] < '0' || s[i] > '9')
               throw std::invalid_argument("pjson: bad exponent");
            while (i < n && s[i] >= '0' && s[i] <= '9')
            {
               exp = exp * 10 + (s[i] - '0');
               ++i;
            }
            scale += sign * exp;
            break;
         }
         if (c < '0' || c > '9') break;
         mant = mant * 10 + (c - '0');
         if (seen_dot) --scale;
         ++i;
      }
      skip_ws();
      if (i != n)
         throw std::invalid_argument("pjson: trailing garbage");
      if (mant != 0)
      {
         while (scale < 0 && (mant % 10) == 0)
         {
            mant /= 10;
            ++scale;
         }
      }
      if (neg) mant = -mant;
      return pjson_number{mant, scale};
   }

   // ── public façade ────────────────────────────────────────────────────

   struct pjson
   {
      static std::vector<std::uint8_t> encode(const pjson_value& v)
      {
         std::size_t total = pjson_detail::value_size(v);
         std::vector<std::uint8_t> out(total);
         pjson_detail::encode_value_at(out.data(), 0, v);
         return out;
      }

      static std::size_t encoded_size(const pjson_value& v) noexcept
      {
         return pjson_detail::value_size(v);
      }

      static std::optional<pjson_value>
      try_decode(std::span<const std::uint8_t> bytes)
      {
         pjson_value v;
         if (!pjson_detail::decode_value(bytes.data(), bytes.size(), v))
            return std::nullopt;
         return v;
      }
      static pjson_value decode(std::span<const std::uint8_t> bytes)
      {
         auto v = try_decode(bytes);
         if (!v)
            throw std::runtime_error("pjson: invalid buffer");
         return std::move(*v);
      }
      static bool validate(std::span<const std::uint8_t> bytes) noexcept
      {
         pjson_value v;
         return pjson_detail::decode_value(bytes.data(), bytes.size(), v);
      }
   };

}  // namespace psio
