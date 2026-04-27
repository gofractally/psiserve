#pragma once
//
// psio/pjson_view.hpp — zero-copy view over pjson bytes.
//
// A pjson value is (ptr, size). pjson_view holds exactly that pair.
// Every accessor is offset arithmetic + small decode of the tag byte.
// No allocation, no copies, no intermediate tree.
//
// Construct from a (ptr, size) pair (typically the buffer the caller
// owns), or from a span. Sub-views of nested children are produced by
// `find()` / `at()`, computing each child's `(ptr, size)` from the
// parent's slot table.

#include <psio/format.hpp>
#include <psio/pjson.hpp>

#include <ucc/lower_bound.hpp>  // ucc::find_byte — SWAR 8-byte hash scan

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>

namespace psio {

   template <>
   class dynamic_view<pjson_format>
   {
     public:
      using format = pjson_format;
      enum class kind : std::uint8_t
      {
         null,
         boolean,
         integer,
         decimal,
         floating,
         string,
         bytes,
         array,
         object,
         invalid
      };

      dynamic_view() = default;
      dynamic_view(const std::uint8_t* data, std::size_t size) noexcept
          : data_(data), size_(size)
      {
      }
      explicit dynamic_view(std::span<const std::uint8_t> b) noexcept
          : data_(b.data()), size_(b.size())
      {
      }

      bool                          valid() const noexcept { return data_ != nullptr; }
      const std::uint8_t*           data() const noexcept { return data_; }
      std::size_t                   size() const noexcept { return size_; }
      std::span<const std::uint8_t> raw() const noexcept
      {
         return {data_, size_};
      }

      kind type() const noexcept
      {
         if (!data_ || size_ == 0) return kind::invalid;
         std::uint8_t t = data_[0] >> 4;
         using namespace pjson_detail;
         switch (t)
         {
            case t_null:        return kind::null;
            case t_bool:        return kind::boolean;
            case t_uint_inline:
            case t_int:         return kind::integer;
            case t_decimal:     return kind::decimal;
            case t_ieee_float:  return kind::floating;
            case t_string:      return kind::string;
            case t_bytes:       return kind::bytes;
            // t_array's low nibble selects layout: 0 = generic, 1..10
            // = typed homogeneous. Both surface as kind::array; callers
            // that care about the difference (JSON emit, fast bulk
            // reads) test is_typed_array().
            case t_array:       return kind::array;
            case t_object:      return kind::object;
            default:            return kind::invalid;
         }
      }

      bool is_null() const noexcept { return type() == kind::null; }
      bool is_bool() const noexcept { return type() == kind::boolean; }
      bool is_integer() const noexcept { return type() == kind::integer; }
      bool is_decimal() const noexcept { return type() == kind::decimal; }
      bool is_floating() const noexcept { return type() == kind::floating; }
      bool is_number() const noexcept
      {
         auto k = type();
         return k == kind::integer || k == kind::decimal ||
                k == kind::floating;
      }
      bool is_string() const noexcept { return type() == kind::string; }
      bool is_bytes() const noexcept { return type() == kind::bytes; }
      bool is_array() const noexcept { return type() == kind::array; }
      bool is_object() const noexcept { return type() == kind::object; }

      // Distinguishes the typed homogeneous-primitive array form
      // (tag t_array, low nibble 1..10, raw little-endian element
      // bytes, no slot table) from the generic array (tag t_array,
      // low nibble = 0, per-element tags + slot table). Reserved
      // low-nibble values (11..15) return false — those buffers are
      // malformed and a validator should reject them.
      bool is_typed_array() const noexcept
      {
         if (!data_ || size_ == 0) return false;
         if ((data_[0] >> 4) != pjson_detail::t_array) return false;
         return pjson_detail::typed_array_code_from_low(
                    data_[0] & 0x0F) != pjson_detail::tac_invalid;
      }
      // Element type code (tac_i8 .. tac_f64). Undefined unless
      // is_typed_array().
      std::uint8_t typed_array_elem_code() const noexcept
      {
         return pjson_detail::typed_array_code_from_low(
             data_[0] & 0x0F);
      }
      // Element byte size. Undefined unless is_typed_array().
      std::size_t typed_array_elem_size() const noexcept
      {
         return pjson_detail::typed_array_elem_size(
             typed_array_elem_code());
      }

      // ── scalar accessors ────────────────────────────────────────────

      bool as_bool() const
      {
         require_(kind::boolean);
         // low nibble carries the boolean value
         return (data_[0] & 0x0F) != 0;
      }

      // Cross-tier: integer / decimal-with-scale-0 / integer-valued double.
      std::int64_t as_int64() const
      {
         using namespace pjson_detail;
         std::uint8_t t = data_[0] >> 4;
         if (t == t_uint_inline) return static_cast<std::int64_t>(data_[0] & 0x0F);
         if (t == t_int)
         {
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            if (bc > 8)
               throw std::out_of_range(
                   "pjson_view::as_int64: exceeds int64");
            std::uint64_t zz = 0;
            std::memcpy(&zz, data_ + 1, bc);
            return static_cast<std::int64_t>((zz >> 1) ^
                                             (~(zz & 1) + 1));
         }
         pjson_number n   = as_number();
         auto         opt = n.to_int64();
         if (opt) return *opt;
         throw std::out_of_range(
             "pjson_view::as_int64: not exactly representable");
      }

      __int128 as_int128() const
      {
         using namespace pjson_detail;
         std::uint8_t t = data_[0] >> 4;
         if (t == t_uint_inline)
            return static_cast<__int128>(data_[0] & 0x0F);
         if (t == t_int)
         {
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            __uint128_t zz = 0;
            std::memcpy(&zz, data_ + 1, bc);
            return zz128_decode(zz);
         }
         return as_number().mantissa;  // best-effort
      }

      double as_double() const
      {
         using namespace pjson_detail;
         std::uint8_t t = data_[0] >> 4;
         if (t == t_ieee_float)
         {
            double d;
            std::memcpy(&d, data_ + 1, 8);
            return d;
         }
         return as_number().to_double();
      }

      pjson_number as_number() const
      {
         using namespace pjson_detail;
         auto k = type();
         if (k == kind::integer) return pjson_number{as_int128(), 0};
         if (k == kind::decimal)
         {
            std::uint8_t bc =
                static_cast<std::uint8_t>((data_[0] & 0x0F) + 1);
            __uint128_t zz = 0;
            std::memcpy(&zz, data_ + 1, bc);
            std::int32_t scale;
            std::size_t  n =
                read_varint62(data_ + 1 + bc, size_ - 1 - bc, scale);
            if (n == 0) throw std::runtime_error("pjson_view: bad decimal");
            return pjson_number{zz128_decode(zz), scale};
         }
         if (k == kind::floating)
            return pjson_number::from_double(as_double());
         throw std::runtime_error("pjson_view::as_number: not a number");
      }

      std::string_view as_string() const
      {
         require_(kind::string);
         return std::string_view(
             reinterpret_cast<const char*>(data_ + 1), size_ - 1);
      }
      std::span<const std::uint8_t> as_bytes() const
      {
         require_(kind::bytes);
         return {data_ + 1, size_ - 1};
      }

      // String encoding flag (low nibble of the t_string tag).
      //   0 = raw_text       — bytes are unescaped text; emit must
      //                        run JSON escape detection
      //   1 = escape_form    — bytes are JSON-escape form already;
      //                        emit verbatim with surrounding quotes
      // Binary blobs use a separate t_bytes tag (kind::bytes), not a
      // string flag. Caller must check is_string() first; calling
      // this on a non-string value is undefined.
      std::uint8_t string_flag() const noexcept
      {
         return data_[0] & 0x0F;
      }

      // ── container access ────────────────────────────────────────────

      std::size_t count() const
      {
         using namespace pjson_detail;
         std::uint8_t t = data_[0] >> 4;
         if (t != t_array && t != t_object)
            throw std::runtime_error("pjson_view::count: not a container");
         if (size_ < 3)
            throw std::runtime_error("pjson_view: bad count");
         return static_cast<std::size_t>(data_[size_ - 2]) |
                (static_cast<std::size_t>(data_[size_ - 1]) << 8);
      }

      dynamic_view at(std::size_t i) const
      {
         using namespace pjson_detail;
         if (is_typed_array())
            throw std::runtime_error(
                "pjson_view::at: typed array — use typed_int64_at / "
                "typed_uint64_at / typed_double_at");
         require_(kind::array);
         std::size_t N = count();
         if (i >= N)
            throw std::out_of_range("pjson_view::at: array index");
         std::size_t slot_table_pos   = size_ - 2 - 4 * N;
         std::size_t value_data_start = 1;
         std::size_t value_data_size  = slot_table_pos - 1;
         std::uint32_t off_i =
             slot_offset(read_u32_le(data_ + slot_table_pos + i * 4));
         std::uint32_t off_next =
             i + 1 < N
                 ? slot_offset(read_u32_le(
                       data_ + slot_table_pos + (i + 1) * 4))
                 : static_cast<std::uint32_t>(value_data_size);
         return dynamic_view{data_ + value_data_start + off_i,
                             static_cast<std::size_t>(off_next - off_i)};
      }

      // Typed-array element accessors. Caller must check
      // is_typed_array() first; type code determines which accessor is
      // appropriate (signed / unsigned / float). Out-of-range index
      // throws.
      std::int64_t typed_int64_at(std::size_t i) const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_int64_at: not a typed array");
         std::size_t N  = count();
         if (i >= N)
            throw std::out_of_range("pjson_view::typed_int64_at");
         std::uint8_t       code = typed_array_elem_code();
         std::size_t        es   = pjson_detail::typed_array_elem_size(code);
         const std::uint8_t* eb  = data_ + 1 + i * es;
         switch (code)
         {
            case pjson_detail::tac_i8:
            { std::int8_t v;  std::memcpy(&v, eb, 1); return v; }
            case pjson_detail::tac_i16:
            { std::int16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_i32:
            { std::int32_t v; std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_i64:
            { std::int64_t v; std::memcpy(&v, eb, 8); return v; }
            case pjson_detail::tac_u8:
               return static_cast<std::int64_t>(eb[0]);
            case pjson_detail::tac_u16:
            { std::uint16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_u32:
            { std::uint32_t v; std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_u64:
            {
               std::uint64_t v;
               std::memcpy(&v, eb, 8);
               if (v > static_cast<std::uint64_t>(
                          std::numeric_limits<std::int64_t>::max()))
                  throw std::out_of_range(
                      "pjson_view::typed_int64_at: u64 exceeds i64");
               return static_cast<std::int64_t>(v);
            }
            default:
               throw std::runtime_error(
                   "pjson_view::typed_int64_at: float element");
         }
      }

      std::uint64_t typed_uint64_at(std::size_t i) const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_uint64_at: not a typed array");
         std::size_t N  = count();
         if (i >= N)
            throw std::out_of_range("pjson_view::typed_uint64_at");
         std::uint8_t       code = typed_array_elem_code();
         std::size_t        es   = pjson_detail::typed_array_elem_size(code);
         const std::uint8_t* eb  = data_ + 1 + i * es;
         switch (code)
         {
            case pjson_detail::tac_u8:  return eb[0];
            case pjson_detail::tac_u16:
            { std::uint16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_u32:
            { std::uint32_t v; std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_u64:
            { std::uint64_t v; std::memcpy(&v, eb, 8); return v; }
            // Signed → unsigned: reject negatives.
            case pjson_detail::tac_i8:
            {
               std::int8_t v; std::memcpy(&v, eb, 1);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            case pjson_detail::tac_i16:
            {
               std::int16_t v; std::memcpy(&v, eb, 2);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            case pjson_detail::tac_i32:
            {
               std::int32_t v; std::memcpy(&v, eb, 4);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            case pjson_detail::tac_i64:
            {
               std::int64_t v; std::memcpy(&v, eb, 8);
               if (v < 0) throw std::out_of_range(
                   "pjson_view::typed_uint64_at: negative");
               return static_cast<std::uint64_t>(v);
            }
            default:
               throw std::runtime_error(
                   "pjson_view::typed_uint64_at: float element");
         }
      }

      double typed_double_at(std::size_t i) const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_double_at: not a typed array");
         std::size_t N  = count();
         if (i >= N)
            throw std::out_of_range("pjson_view::typed_double_at");
         std::uint8_t       code = typed_array_elem_code();
         std::size_t        es   = pjson_detail::typed_array_elem_size(code);
         const std::uint8_t* eb  = data_ + 1 + i * es;
         switch (code)
         {
            case pjson_detail::tac_f32:
            { float v;  std::memcpy(&v, eb, 4); return v; }
            case pjson_detail::tac_f64:
            { double v; std::memcpy(&v, eb, 8); return v; }
            // Integer fall-throughs: explicit conversion to double.
            case pjson_detail::tac_i8:
            { std::int8_t v;  std::memcpy(&v, eb, 1); return v; }
            case pjson_detail::tac_i16:
            { std::int16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_i32:
            { std::int32_t v; std::memcpy(&v, eb, 4); return static_cast<double>(v); }
            case pjson_detail::tac_i64:
            { std::int64_t v; std::memcpy(&v, eb, 8); return static_cast<double>(v); }
            case pjson_detail::tac_u8:  return static_cast<double>(eb[0]);
            case pjson_detail::tac_u16:
            { std::uint16_t v; std::memcpy(&v, eb, 2); return v; }
            case pjson_detail::tac_u32:
            { std::uint32_t v; std::memcpy(&v, eb, 4); return static_cast<double>(v); }
            case pjson_detail::tac_u64:
            { std::uint64_t v; std::memcpy(&v, eb, 8); return static_cast<double>(v); }
            default:
               throw std::runtime_error(
                   "pjson_view::typed_double_at: invalid element code");
         }
      }

      // Raw typed-element span. Caller asserts the requested type
      // matches this view's element code; otherwise the bytes are
      // mis-interpreted. Useful for memcpy-style consumption when the
      // element type is known to match.
      template <typename T>
      std::span<const T> typed_array_span() const
      {
         if (!is_typed_array())
            throw std::runtime_error(
                "pjson_view::typed_array_span: not a typed array");
         if (typed_array_elem_code() != pjson_detail::tac_for<T>())
            throw std::runtime_error(
                "pjson_view::typed_array_span: element-type mismatch");
         std::size_t N = count();
         return std::span<const T>{
             reinterpret_cast<const T*>(data_ + 1), N};
      }

      // Object key lookup: hash prefilter + key verify.
      //
      // Prefilter uses `ucc::find_byte` (psitri's SWAR 8-byte-at-a-
      // time scan). Multi-trial bench (5M iterations × 5 trials):
      //
      //   ucc::find_byte    15.0-15.8 ns/access   (winner)
      //   libc memchr       16.1-16.3 ns/access
      //   scalar loop       17.9-18.9 ns/access
      //
      // For our typical N=5-30 hash bytes (all in one cache line,
      // already aligned), find_byte's tight SWAR loop has less setup
      // than libc memchr's general-purpose alignment + fallback
      // machinery. The tradeoff would flip around N=64+ where
      // memchr's larger-size optimizations pay off.
      std::optional<dynamic_view> find(std::string_view key) const
      {
         using namespace pjson_detail;
         if (type() != kind::object) return std::nullopt;
         std::size_t  N = count();
         std::size_t  slot_table_pos   = size_ - 2 - 4 * N;
         std::size_t  hash_table_pos   = slot_table_pos - N;
         std::size_t  value_data_start = 1;
         std::size_t  value_data_size  = hash_table_pos - 1;
         std::uint8_t want = key_hash8(key);

         const std::uint8_t* hashes    = data_ + hash_table_pos;
         std::size_t         remaining = N;
         std::size_t         base      = 0;
         while (remaining > 0)
         {
            int hit = ucc::find_byte(hashes + base, remaining, want);
            if (hit >= static_cast<int>(remaining)) return std::nullopt;
            std::size_t i = base + static_cast<std::size_t>(hit);
            std::uint32_t s_i =
                read_u32_le(data_ + slot_table_pos + i * 4);
            std::uint32_t off_i = slot_offset(s_i);
            std::uint8_t  ks    = slot_key_size(s_i);
            std::uint32_t off_next =
                i + 1 < N
                    ? slot_offset(read_u32_le(
                          data_ + slot_table_pos + (i + 1) * 4))
                    : static_cast<std::uint32_t>(value_data_size);
            const std::uint8_t* entry = data_ + value_data_start + off_i;
            std::size_t         entry_size = off_next - off_i;
            std::size_t         klen, klen_bytes;
            if (ks != 0xFF)
            {
               klen       = ks;
               klen_bytes = 0;
            }
            else
            {
               std::uint64_t excess;
               klen_bytes = read_varuint62(entry, entry_size, excess);
               if (klen_bytes == 0) return std::nullopt;
               klen = 0xFFu + static_cast<std::size_t>(excess);
            }
            if (klen == key.size() &&
                std::memcmp(entry + klen_bytes, key.data(), klen) == 0)
            {
               std::size_t value_off = klen_bytes + klen;
               return dynamic_view{entry + value_off,
                                   entry_size - value_off};
            }
            // Hash collision; advance past and re-scan.
            base      = i + 1;
            remaining = (i + 1 <= N) ? (N - (i + 1)) : 0;
         }
         return std::nullopt;
      }

      dynamic_view operator[](std::string_view key) const
      {
         auto v = find(key);
         if (!v) throw std::out_of_range("pjson_view: key not found");
         return *v;
      }
      dynamic_view operator[](std::size_t i) const { return at(i); }

      // Iterators — visit each child in encounter order.
      template <typename Fn>
      void for_each_field(Fn&& fn) const
      {
         using namespace pjson_detail;
         require_(kind::object);
         std::size_t N = count();
         std::size_t slot_table_pos   = size_ - 2 - 4 * N;
         std::size_t hash_table_pos   = slot_table_pos - N;
         std::size_t value_data_start = 1;
         std::size_t value_data_size  = hash_table_pos - 1;
         (void)hash_table_pos;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t s_i =
                read_u32_le(data_ + slot_table_pos + i * 4);
            std::uint32_t off_i = slot_offset(s_i);
            std::uint8_t  ks    = slot_key_size(s_i);
            std::uint32_t off_next =
                i + 1 < N
                    ? slot_offset(read_u32_le(
                          data_ + slot_table_pos + (i + 1) * 4))
                    : static_cast<std::uint32_t>(value_data_size);
            const std::uint8_t* entry = data_ + value_data_start + off_i;
            std::size_t         entry_size = off_next - off_i;
            std::size_t         klen, klen_bytes;
            if (ks != 0xFF) { klen = ks; klen_bytes = 0; }
            else
            {
               std::uint64_t excess;
               klen_bytes = read_varuint62(entry, entry_size, excess);
               klen = 0xFFu + static_cast<std::size_t>(excess);
            }
            std::string_view k(
                reinterpret_cast<const char*>(entry + klen_bytes), klen);
            std::size_t value_off = klen_bytes + klen;
            fn(k, dynamic_view{entry + value_off, entry_size - value_off});
         }
      }

      template <typename Fn>
      void for_each_element(Fn&& fn) const
      {
         using namespace pjson_detail;
         require_(kind::array);
         if (is_typed_array())
            throw std::runtime_error(
                "pjson_view::for_each_element: typed array — use "
                "typed_*_at or typed_array_span<T>()");
         std::size_t N = count();
         std::size_t slot_table_pos   = size_ - 2 - 4 * N;
         std::size_t value_data_start = 1;
         std::size_t value_data_size  = slot_table_pos - 1;
         for (std::size_t i = 0; i < N; ++i)
         {
            std::uint32_t off_i =
                slot_offset(read_u32_le(
                    data_ + slot_table_pos + i * 4));
            std::uint32_t off_next =
                i + 1 < N
                    ? slot_offset(read_u32_le(
                          data_ + slot_table_pos + (i + 1) * 4))
                    : static_cast<std::uint32_t>(value_data_size);
            fn(dynamic_view{data_ + value_data_start + off_i,
                            static_cast<std::size_t>(off_next - off_i)});
         }
      }

     private:
      void require_(kind k) const
      {
         if (type() != k)
            throw std::runtime_error("pjson_view: type mismatch");
      }
      const std::uint8_t* data_ = nullptr;
      std::size_t         size_ = 0;
   };

   // Backward-compat alias. New code should prefer
   // dynamic_view<pjson_format>; pjson_view stays for ergonomics in
   // pjson-only code.
   using pjson_view = dynamic_view<pjson_format>;

}  // namespace psio
