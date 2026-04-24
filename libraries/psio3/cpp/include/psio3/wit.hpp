#pragma once
//
// psio3/wit.hpp — WIT canonical ABI format tag.
//
// WebAssembly Component Model canonical ABI (GC-unaware subset):
// fixed-layout records with fields at compile-time offsets, strings
// and lists as (u32 ptr, u32 len) pointing into the same buffer.
// Deterministic — same value always produces the same bytes.
//
// MVP scope (Phase 14):
//   - primitives (arithmetic + bool), enums
//   - std::string (UTF-8, no trailing NUL)
//   - std::vector of: arithmetic / bool / enum / string / nested records
//   - reflected records (root or nested, recursive)
//
// Wire reference:
// https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md
//
// Not supported in this MVP: std::variant, std::optional, std::tuple,
// std::expected, std::array, resources (own<T>/borrow<T>). These land
// in a follow-up once the psio3 annotation + shape-wrapper surface is
// complete. Attempting to encode an unsupported shape is a compile
// error — the Record concept guards the tag_invoke entry points.

#include <psio3/cpo.hpp>
#include <psio3/error.hpp>
#include <psio3/format_tag_base.hpp>
#include <psio3/adapter.hpp>
#include <psio3/reflect.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psio3 {

   struct wit;

   namespace detail::wit_impl {

      // ── Shape classifiers ─────────────────────────────────────────────────

      template <typename T>
      struct is_vector : std::false_type {};
      template <typename E, typename A>
      struct is_vector<std::vector<E, A>> : std::true_type
      {
         using element_type = E;
      };

      template <typename T>
      struct is_array : std::false_type {};
      template <typename E, std::size_t N>
      struct is_array<std::array<E, N>> : std::true_type
      {
         using element_type             = E;
         static constexpr std::size_t n = N;
      };

      template <typename T>
      concept Record = ::psio3::Reflected<T> && !std::is_enum_v<T>;

      // Leaf = anything that fits in fixed-size cell (including string/vec
      // which have (ptr, len) as their fixed portion).
      template <typename T>
      concept Leaf = std::is_arithmetic_v<T> || std::is_enum_v<T> ||
                     std::is_same_v<T, std::string> ||
                     is_vector<T>::value || Record<T>;

      // ── Canonical alignment and size (compile-time) ───────────────────────

      template <typename T>
      consteval uint32_t canonical_align();

      template <typename T>
      consteval uint32_t canonical_size();

      template <typename T>
      consteval uint32_t canonical_align()
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool> ||
                       std::is_same_v<U, std::int8_t> ||
                       std::is_same_v<U, std::uint8_t> ||
                       std::is_same_v<U, char>)
            return 1;
         else if constexpr (std::is_same_v<U, std::int16_t> ||
                            std::is_same_v<U, std::uint16_t>)
            return 2;
         else if constexpr (std::is_same_v<U, std::int32_t> ||
                            std::is_same_v<U, std::uint32_t> ||
                            std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, std::int64_t> ||
                            std::is_same_v<U, std::uint64_t> ||
                            std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_enum_v<U>)
            return canonical_align<std::underlying_type_t<U>>();
         else if constexpr (std::is_same_v<U, std::string>)
            return 4;  // (u32 ptr, u32 len)
         else if constexpr (is_vector<U>::value)
            return 4;  // (u32 ptr, u32 len)
         else if constexpr (is_array<U>::value)
            return canonical_align<typename is_array<U>::element_type>();
         else if constexpr (Record<U>)
         {
            using R                 = ::psio3::reflect<U>;
            constexpr std::size_t N = R::member_count;
            return [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               uint32_t m = 1;
               ((m = std::max(
                    m,
                    canonical_align<typename R::template member_type<Is>>())),
                ...);
               return m;
            }(std::make_index_sequence<N>{});
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported type");
         }
      }

      template <typename T>
      consteval uint32_t canonical_size()
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool> ||
                       std::is_same_v<U, std::int8_t> ||
                       std::is_same_v<U, std::uint8_t> ||
                       std::is_same_v<U, char>)
            return 1;
         else if constexpr (std::is_same_v<U, std::int16_t> ||
                            std::is_same_v<U, std::uint16_t>)
            return 2;
         else if constexpr (std::is_same_v<U, std::int32_t> ||
                            std::is_same_v<U, std::uint32_t> ||
                            std::is_same_v<U, float>)
            return 4;
         else if constexpr (std::is_same_v<U, std::int64_t> ||
                            std::is_same_v<U, std::uint64_t> ||
                            std::is_same_v<U, double>)
            return 8;
         else if constexpr (std::is_enum_v<U>)
            return canonical_size<std::underlying_type_t<U>>();
         else if constexpr (std::is_same_v<U, std::string>)
            return 8;  // (u32 ptr, u32 len)
         else if constexpr (is_vector<U>::value)
            return 8;  // (u32 ptr, u32 len)
         else if constexpr (is_array<U>::value)
         {
            using E = typename is_array<U>::element_type;
            return static_cast<uint32_t>(is_array<U>::n) * canonical_size<E>();
         }
         else if constexpr (Record<U>)
         {
            using R                 = ::psio3::reflect<U>;
            constexpr std::size_t N = R::member_count;
            uint32_t size = 0;
            uint32_t max_align = 1;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               auto process = [&]<std::size_t I>() {
                  using FT                 = typename R::template member_type<I>;
                  constexpr uint32_t fa    = canonical_align<FT>();
                  constexpr uint32_t fs    = canonical_size<FT>();
                  max_align                = std::max(max_align, fa);
                  size                     = ((size + fa - 1) & ~(fa - 1)) + fs;
               };
               (process.template operator()<Is>(), ...);
            }(std::make_index_sequence<N>{});
            return (size + max_align - 1) & ~(max_align - 1);
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported type");
         }
      }

      template <typename T, std::size_t I>
      consteval uint32_t canonical_field_offset()
      {
         using R         = ::psio3::reflect<T>;
         uint32_t offset = 0;
         // Walk fields 0..I-1, accumulating offset with alignment padding.
         [&]<std::size_t... Js>(std::index_sequence<Js...>) {
            auto add = [&]<std::size_t J>() {
               using FT              = typename R::template member_type<J>;
               constexpr uint32_t fa = canonical_align<FT>();
               constexpr uint32_t fs = canonical_size<FT>();
               offset                = (offset + fa - 1) & ~(fa - 1);
               offset += fs;
            };
            (add.template operator()<Js>(), ...);
         }(std::make_index_sequence<I>{});
         // Pad up to field I's own alignment.
         using FT_I              = typename R::template member_type<I>;
         constexpr uint32_t fa_i = canonical_align<FT_I>();
         offset                  = (offset + fa_i - 1) & ~(fa_i - 1);
         return offset;
      }

      // ── Packed size (runtime — includes variable tails) ────────────────────

      template <typename T>
      uint32_t packed_size_of(const T& value);

      template <typename T>
      uint32_t variable_tail_bytes(const T& value)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>)
         {
            (void)value;
            return 0;
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            return static_cast<uint32_t>(value.size());
         }
         else if constexpr (is_vector<U>::value)
         {
            using E                = typename U::value_type;
            constexpr uint32_t es  = canonical_size<E>();
            constexpr uint32_t ea  = canonical_align<E>();
            // Align the list's base offset (conservative upper bound on
            // the padding the bump allocator will insert).
            uint32_t pad           = ea > 1 ? (ea - 1) : 0;
            uint32_t total         = pad + static_cast<uint32_t>(value.size()) * es;
            for (const auto& e : value)
               total += variable_tail_bytes(e);
            return total;
         }
         else if constexpr (is_array<U>::value)
         {
            // std::array is packed in-place, no variable tail.
            uint32_t total = 0;
            for (const auto& e : value)
               total += variable_tail_bytes(e);
            return total;
         }
         else if constexpr (Record<U>)
         {
            using R                 = ::psio3::reflect<U>;
            constexpr std::size_t N = R::member_count;
            uint32_t total = 0;
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((total += variable_tail_bytes(
                    value.*(R::template member_pointer<Is>))),
                ...);
            }(std::make_index_sequence<N>{});
            return total;
         }
         else
         {
            return 0;
         }
      }

      template <typename T>
      uint32_t packed_size_of(const T& value)
      {
         return canonical_size<T>() + variable_tail_bytes(value);
      }

      // ── Buffer helpers ────────────────────────────────────────────────────

      class bump_buf
      {
         std::vector<char> buf_;

        public:
         explicit bump_buf(uint32_t reserve = 0)
         {
            if (reserve)
               buf_.reserve(reserve);
         }

         uint32_t size() const { return static_cast<uint32_t>(buf_.size()); }

         // Allocate `n` bytes at current end, aligned up to `a`. Returns
         // the offset to the start of the allocation.
         uint32_t alloc(uint32_t a, uint32_t n)
         {
            uint32_t off = (size() + a - 1) & ~(a - 1);
            buf_.resize(off + n, 0);
            return off;
         }

         char*       data(uint32_t at) { return buf_.data() + at; }
         const char* data(uint32_t at) const { return buf_.data() + at; }

         std::vector<char> finish() { return std::move(buf_); }
      };

      template <typename T>
      void lower_leaf(bump_buf& buf, uint32_t at, const T& value);

      template <typename T>
      void lower_record(bump_buf& buf, uint32_t base, const T& value);

      template <typename E>
      void lower_list(bump_buf& buf, uint32_t at, const std::vector<E>& vec)
      {
         uint32_t count = static_cast<uint32_t>(vec.size());
         uint32_t ptr   = 0;
         if (count > 0)
         {
            constexpr uint32_t ea = canonical_align<E>();
            constexpr uint32_t es = canonical_size<E>();
            ptr                   = buf.alloc(ea, count * es);
            for (uint32_t i = 0; i < count; ++i)
               lower_leaf<E>(buf, ptr + i * es, vec[i]);
         }
         // Write header (ptr, count) at `at`.
         std::memcpy(buf.data(at), &ptr, 4);
         std::memcpy(buf.data(at + 4), &count, 4);
      }

      inline void lower_string(bump_buf& buf, uint32_t at,
                               std::string_view sv)
      {
         uint32_t len = static_cast<uint32_t>(sv.size());
         uint32_t ptr = 0;
         if (len > 0)
         {
            ptr = buf.alloc(1, len);
            std::memcpy(buf.data(ptr), sv.data(), len);
         }
         std::memcpy(buf.data(at), &ptr, 4);
         std::memcpy(buf.data(at + 4), &len, 4);
      }

      template <typename T>
      void lower_leaf(bump_buf& buf, uint32_t at, const T& value)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            uint8_t b = value ? 1 : 0;
            std::memcpy(buf.data(at), &b, 1);
         }
         else if constexpr (std::is_enum_v<U>)
         {
            using W = std::underlying_type_t<U>;
            W wire  = static_cast<W>(value);
            std::memcpy(buf.data(at), &wire, sizeof(W));
         }
         else if constexpr (std::is_arithmetic_v<U>)
         {
            std::memcpy(buf.data(at), &value, sizeof(U));
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            lower_string(buf, at, value);
         }
         else if constexpr (is_vector<U>::value)
         {
            lower_list(buf, at, value);
         }
         else if constexpr (is_array<U>::value)
         {
            using E               = typename is_array<U>::element_type;
            constexpr uint32_t es = canonical_size<E>();
            for (std::size_t i = 0; i < is_array<U>::n; ++i)
               lower_leaf<E>(buf, at + i * es, value[i]);
         }
         else if constexpr (Record<U>)
         {
            lower_record<U>(buf, at, value);
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported leaf");
         }
      }

      template <typename T>
      void lower_record(bump_buf& buf, uint32_t base, const T& value)
      {
         using R                 = ::psio3::reflect<T>;
         constexpr std::size_t N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (lower_leaf(
                 buf, base + canonical_field_offset<T, Is>(),
                 value.*(R::template member_pointer<Is>)),
             ...);
         }(std::make_index_sequence<N>{});
      }

      // ── Lifters (decode) ──────────────────────────────────────────────────

      template <typename T>
      void lift_leaf(std::span<const char> buf, uint32_t at, T& out);

      template <typename T>
      void lift_record(std::span<const char> buf, uint32_t base, T& out);

      inline uint32_t read_u32(std::span<const char> buf, uint32_t at)
      {
         uint32_t v;
         std::memcpy(&v, buf.data() + at, 4);
         return v;
      }

      template <typename E>
      void lift_list(std::span<const char> buf, uint32_t at,
                     std::vector<E>& out)
      {
         uint32_t ptr   = read_u32(buf, at);
         uint32_t count = read_u32(buf, at + 4);
         out.resize(count);
         if (count == 0)
            return;
         constexpr uint32_t es = canonical_size<E>();
         for (uint32_t i = 0; i < count; ++i)
            lift_leaf<E>(buf, ptr + i * es, out[i]);
      }

      inline void lift_string(std::span<const char> buf, uint32_t at,
                              std::string& out)
      {
         uint32_t ptr = read_u32(buf, at);
         uint32_t len = read_u32(buf, at + 4);
         out.assign(buf.data() + ptr, len);
      }

      template <typename T>
      void lift_leaf(std::span<const char> buf, uint32_t at, T& out)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_same_v<U, bool>)
         {
            uint8_t b;
            std::memcpy(&b, buf.data() + at, 1);
            out = (b != 0);
         }
         else if constexpr (std::is_enum_v<U>)
         {
            using W = std::underlying_type_t<U>;
            W wire;
            std::memcpy(&wire, buf.data() + at, sizeof(W));
            out = static_cast<U>(wire);
         }
         else if constexpr (std::is_arithmetic_v<U>)
         {
            std::memcpy(&out, buf.data() + at, sizeof(U));
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            lift_string(buf, at, out);
         }
         else if constexpr (is_vector<U>::value)
         {
            lift_list(buf, at, out);
         }
         else if constexpr (is_array<U>::value)
         {
            using E               = typename is_array<U>::element_type;
            constexpr uint32_t es = canonical_size<E>();
            for (std::size_t i = 0; i < is_array<U>::n; ++i)
               lift_leaf<E>(buf, at + i * es, out[i]);
         }
         else if constexpr (Record<U>)
         {
            lift_record<U>(buf, at, out);
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported leaf");
         }
      }

      template <typename T>
      void lift_record(std::span<const char> buf, uint32_t base, T& out)
      {
         using R                 = ::psio3::reflect<T>;
         constexpr std::size_t N = R::member_count;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            (lift_leaf(buf, base + canonical_field_offset<T, Is>(),
                       out.*(R::template member_pointer<Is>)),
             ...);
         }(std::make_index_sequence<N>{});
      }

      // ── Validate (bounds only for MVP) ────────────────────────────────────

      template <typename T>
      bool validate_leaf(std::span<const char> buf, uint32_t at);

      template <typename T>
      bool validate_record(std::span<const char> buf, uint32_t base)
      {
         using R                 = ::psio3::reflect<T>;
         constexpr std::size_t N = R::member_count;
         bool                  ok = true;
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((ok = ok && validate_leaf<typename R::template member_type<Is>>(
                            buf, base + canonical_field_offset<T, Is>())),
             ...);
         }(std::make_index_sequence<N>{});
         return ok;
      }

      template <typename T>
      bool validate_leaf(std::span<const char> buf, uint32_t at)
      {
         using U = std::remove_cvref_t<T>;
         if constexpr (std::is_arithmetic_v<U> || std::is_enum_v<U>)
         {
            return static_cast<std::size_t>(at) + canonical_size<U>() <=
                   buf.size();
         }
         else if constexpr (std::is_same_v<U, std::string>)
         {
            if (static_cast<std::size_t>(at) + 8 > buf.size())
               return false;
            uint32_t ptr = read_u32(buf, at);
            uint32_t len = read_u32(buf, at + 4);
            if (len == 0)
               return true;
            return static_cast<std::size_t>(ptr) + len <= buf.size();
         }
         else if constexpr (is_vector<U>::value)
         {
            if (static_cast<std::size_t>(at) + 8 > buf.size())
               return false;
            uint32_t ptr   = read_u32(buf, at);
            uint32_t count = read_u32(buf, at + 4);
            if (count == 0)
               return true;
            using E = typename U::value_type;
            constexpr uint32_t es = canonical_size<E>();
            if (static_cast<std::size_t>(ptr) +
                   static_cast<std::size_t>(count) * es >
                buf.size())
               return false;
            for (uint32_t i = 0; i < count; ++i)
               if (!validate_leaf<E>(buf, ptr + i * es))
                  return false;
            return true;
         }
         else if constexpr (is_array<U>::value)
         {
            using E               = typename is_array<U>::element_type;
            constexpr uint32_t es = canonical_size<E>();
            if (static_cast<std::size_t>(at) +
                   static_cast<std::size_t>(is_array<U>::n) * es >
                buf.size())
               return false;
            for (std::size_t i = 0; i < is_array<U>::n; ++i)
               if (!validate_leaf<E>(buf, at + i * es))
                  return false;
            return true;
         }
         else if constexpr (Record<U>)
         {
            return validate_record<U>(buf, at);
         }
         else
         {
            static_assert(!sizeof(U*), "wit: unsupported type");
         }
      }

   }  // namespace detail::wit_impl

   // ── Format tag ──────────────────────────────────────────────────────────

   struct wit : format_tag_base<wit>
   {
      using preferred_presentation_category = ::psio3::binary_category;

      template <typename T>
         requires detail::wit_impl::Record<T>
      friend std::vector<char> tag_invoke(decltype(::psio3::encode), wit,
                                          const T& v)
      {
         using namespace detail::wit_impl;
         bump_buf buf(packed_size_of(v));
         uint32_t root = buf.alloc(canonical_align<T>(), canonical_size<T>());
         lower_record<T>(buf, root, v);
         return buf.finish();
      }

      template <typename T>
         requires detail::wit_impl::Record<T>
      friend T tag_invoke(decltype(::psio3::decode<T>), wit, T*,
                          std::span<const char> bytes)
      {
         using namespace detail::wit_impl;
         T out{};
         lift_record<T>(bytes, 0, out);
         return out;
      }

      template <typename T>
         requires detail::wit_impl::Record<T>
      friend std::size_t tag_invoke(decltype(::psio3::size_of), wit,
                                    const T& v)
      {
         return detail::wit_impl::packed_size_of(v);
      }

      template <typename T>
         requires detail::wit_impl::Record<T>
      friend codec_status tag_invoke(decltype(::psio3::validate<T>), wit,
                                     T*,
                                     std::span<const char> bytes) noexcept
      {
         if (!detail::wit_impl::validate_record<T>(bytes, 0))
            return codec_fail("wit: invalid buffer", 0, "wit");
         return codec_ok();
      }

      template <typename T>
         requires detail::wit_impl::Record<T>
      friend codec_status
      tag_invoke(decltype(::psio3::validate_strict<T>), wit, T*,
                 std::span<const char> bytes) noexcept
      {
         return tag_invoke(::psio3::validate<T>, wit{}, (T*)nullptr,
                           bytes);
      }

      template <typename T>
         requires detail::wit_impl::Record<T>
      friend std::unique_ptr<T>
      tag_invoke(decltype(::psio3::make_boxed<T>), wit, T*,
                 std::span<const char> bytes) noexcept
      {
         using namespace detail::wit_impl;
         auto v = std::make_unique<T>();
         lift_record<T>(bytes, 0, *v);
         return v;
      }
   };

}  // namespace psio3
