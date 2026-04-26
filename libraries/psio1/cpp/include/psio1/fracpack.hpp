// TODO: The interaction between checking for no extra data (no gaps)
//       and the possible presence of unknown fields and variant tags
//       has some unsolved border cases. It might be best to only check
//       for gaps when in a mode which prohibits unknown fields and skip
//       checking for gaps when in a mode which allows unknown fields.

#pragma once

#include <psio1/bitset.hpp>
#include <psio1/bounded.hpp>
#include <psio1/detail/layout.hpp>
#include <psio1/detail/run_detector.hpp>
#include <psio1/ext_int.hpp>
#include <psio1/reflect.hpp>
#include <psio1/stream.hpp>

#include <cstdint>
#include <type_traits>

namespace psio1
{
   // ── Fracpack max fixed-region size per type ───────────────────────────────
   //
   // Extensible (non-DWNC) structs prepend a header whose width caps the
   // size of the struct's fixed region. By default fracpack uses a u16
   // header (64 KiB cap). Types that anticipate a larger fixed region must
   // commit up front — same pattern as bounded_list<T, N>'s compile-time
   // bound — by specializing frac_max_fixed_size<T>. The chosen header
   // width (u16 / u32 / u64) is locked for that type's wire format and
   // must be stable across schema evolution.
   //
   // Use the convenience macro:
   //   PSIO1_FRAC_MAX_FIXED_SIZE(MyType, 200'000)
   // or specialize directly:
   //   template <> struct psio1::frac_max_fixed_size<MyType>
   //       : std::integral_constant<std::size_t, 200'000> {};
   //
   // DWNC types are unaffected (they emit no header regardless).

   template <typename T>
   struct frac_max_fixed_size : std::integral_constant<std::size_t, 0xffffu>
   {
   };

   template <typename T>
   inline constexpr std::size_t frac_max_fixed_size_v = frac_max_fixed_size<T>::value;

   // Smallest unsigned int type that can hold N (u16/u32/u64).
   template <std::size_t N>
   using frac_header_type_t =
       std::conditional_t<(N <= 0xffffu), std::uint16_t,
       std::conditional_t<(N <= 0xffffffffull), std::uint32_t, std::uint64_t>>;

}  // namespace psio1

// PSIO1_FRAC_MAX_FIXED_SIZE(Type, N) — opt-in for fixed regions > 64 KiB.
//   Expands to a full specialization of psio1::frac_max_fixed_size in the
//   psio namespace. Use at namespace scope, after PSIO1_REFLECT(Type, ...).
#define PSIO1_FRAC_MAX_FIXED_SIZE(T, N)                                 \
   namespace psio1                                                       \
   {                                                                    \
      template <>                                                       \
      struct frac_max_fixed_size<T> : std::integral_constant<std::size_t, (N)> \
      {                                                                 \
      };                                                                \
   }

#include <cassert>
#include <cstring>

namespace psio1
{
   // Wire-format width tags. frac32 is the default (4-byte offsets/sizes).
   // frac16 uses 2-byte offsets/sizes for records that fit in 64KB.
   struct frac_format_32
   {
      using size_type                              = std::uint32_t;
      static constexpr std::size_t   size_bytes    = 4;
      static constexpr std::uint64_t max_buf_bytes = 0xFFFFFFFFull;
   };
   struct frac_format_16
   {
      using size_type                              = std::uint16_t;
      static constexpr std::size_t   size_bytes    = 2;
      static constexpr std::uint64_t max_buf_bytes = 0xFFFFull;
   };

   // If a type T supports the expressions `psio_unwrap_packable(T&)`,
   // which returns a `T2&`, and `psio_unwrap_packable(const T&)`, which
   // returns `const T2&`, then packing or unpacking T packs or unpacks
   // the returned reference instead.
   template <typename T>
   concept PackableWrapper = requires(T& x, const T& cx) {
      psio_unwrap_packable(x);
      psio_unwrap_packable(cx);
   };

   template <typename T, bool Reflected, typename Format = frac_format_32>
   struct is_packable_reflected;

   template <typename T, typename Format>
   struct is_packable_reflected<T, false, Format> : std::bool_constant<false>
   {
   };

   template <typename T, typename Format>
   struct is_packable_reflected<T, true, Format>;

   template <typename T, typename Format = frac_format_32>
   struct is_packable : is_packable_reflected<T, Reflected<T>, Format>
   {
   };

   // Checking Reflected here is necessary to handle recursive structures
   // because is_packable<T> might already be on the instantiation stack
   template <typename T>
   concept Packable = Reflected<T> || is_packable<T>::value;

   template <typename T>
   concept PackableValidatedObject = requires(T& x) { psio_validate_packable(x); };

   template <typename T>
      requires Packable<std::remove_cv_t<T>>
   class frac_validation_view;  // placeholder — not yet implemented

   template <typename T>
   concept PackableValidatedView =
       requires(frac_validation_view<const T>& p) { psio_validate_packable(p); };

   template <typename T>
   concept PackableNumeric =                       //
       std::is_same_v<T, std::byte> ||             //
       std::is_same_v<T, char> ||                  //
       std::is_same_v<T, uint8_t> ||               //
       std::is_same_v<T, uint16_t> ||              //
       std::is_same_v<T, uint32_t> ||              //
       std::is_same_v<T, uint64_t> ||              //
       std::is_same_v<T, int8_t> ||                //
       std::is_same_v<T, int16_t> ||               //
       std::is_same_v<T, int32_t> ||               //
       std::is_same_v<T, int64_t> ||               //
       std::is_same_v<T, unsigned __int128> ||     //
       std::is_same_v<T, __int128> ||              //
       std::is_same_v<T, float> ||                 //
       std::is_same_v<T, double>;

   template <bool Verify, PackableNumeric T, bool SkipCheck = false>
   [[nodiscard]] bool unpack_numeric(T* value, const char* src, uint32_t& pos, uint32_t end_pos);

   template <typename T>
   struct is_packable_memcpy : std::bool_constant<false>
   {
   };

   template <typename T>
   concept PackableMemcpy = is_packable_memcpy<T>::value;

   template <PackableNumeric T>
   struct is_packable_memcpy<T> : std::bool_constant<true>
   {
   };

   template <typename T>
      requires(!PackableNumeric<T> && PackableNumeric<std::underlying_type_t<T>>)
   struct is_packable_memcpy<T> : std::bool_constant<true>
   {
   };

   template <PackableMemcpy T, std::size_t N>
   struct is_packable_memcpy<std::array<T, N>> : std::bool_constant<true>
   {
      static_assert(sizeof(std::array<T, N>) == N * sizeof(T));
   };

   // psio1::uint256 is a 32-byte bitwise-copy struct; opt it into the memcpy path.
   template <>
   struct is_packable_memcpy<uint256> : std::bool_constant<true>
   {
   };

   // psio1::bitvector<N> is a fixed-size packed bit array; opt into memcpy path.
   template <std::size_t N>
   struct is_packable_memcpy<bitvector<N>> : std::bool_constant<true>
   {
   };

   // Reflected DWNC structs whose in-memory layout matches the wire layout
   // (all fields bitwise + sizeof == sum of field sizes, i.e. no alignment
   // padding anywhere) can be packed and unpacked as a single memcpy. Most
   // commonly this fires when the user applies __attribute__((packed)) to a
   // struct of byte-like fields (e.g. beacon Validator: 48+32+8+1+8+8+8+8 =
   // 121 B). Enables std::vector<T> to take the bulk-memcpy path.
   //
   // DWNC gate: non-DWNC reflected structs participate in fracpack's
   // extensibility semantics and rely on the reflected-struct pack path for
   // wire consistency (even when the bytes happen to be identical). The
   // layout-match optimization is safe only for DWNC types where the
   // compile-time struct layout IS the wire contract.
   template <typename T>
      requires(Reflected<T> && reflect<T>::definitionWillNotChange &&
               !is_bitvector_v<T> && !is_bitlist_v<T> && !is_std_bitset_v<T> &&
               layout_detail::is_memcpy_layout_struct<T>())
   struct is_packable_memcpy<T> : std::bool_constant<true>
   {
   };

   template <typename T>
   concept RefPackable = Packable<std::remove_cvref_t<T>>;

   template <typename Format>
   struct is_packable<std::string, Format>;

   template <typename Format>
   struct is_packable<std::string_view, Format>;

   template <PackableMemcpy T, typename Format>
   struct is_packable<std::span<T>, Format>;

   template <Packable T, typename Format>
   struct is_packable<std::vector<T>, Format>;

   template <Packable T, std::size_t N, typename Format>
      requires(!is_packable_memcpy<T>::value)
   struct is_packable<std::array<T, N>, Format>;

   template <Packable T, typename Format>
   struct is_packable<std::optional<T>, Format>;

   template <typename Format, RefPackable... Ts>
   struct is_packable<std::tuple<Ts...>, Format>;

   template <typename Format, Packable... Ts>
   struct is_packable<std::variant<Ts...>, Format>;

   template <bool Unpack, bool Pointer, typename T, typename Format = frac_format_32>
   bool user_validate(T* value, const char* src, std::uint32_t orig_pos);

   // Default implementations for is_packable<T>
   template <typename T, typename Derived, typename Format = frac_format_32>
   struct base_packable_impl : std::bool_constant<true>
   {
      // // Pack object into a single contiguous region
      // template <typename S>
      // static void pack(const T& value, S& stream);

      // True if T is a variable-sized container and it is empty
      static bool is_empty_container(const T& value) { return false; }

      template <bool Verify>
      static bool clear_container(T* value)
      {
         value->clear();
         return true;
      }

      // Pack either:
      // * Object content if T is fixed size
      // * Space for offset if T is variable size. Must write 0 if is_empty_container().
      template <typename S>
      static void embedded_fixed_pack(const T& value, S& stream)
      {
         if constexpr (Derived::is_variable_size)
            stream.write_raw(typename Format::size_type(0));
         else
            Derived::pack(value, stream);
      }

      // Repack offset if T is variable size
      template <typename S>
      static void embedded_fixed_repack(const T& value,
                                        uint32_t fixed_pos,
                                        uint32_t heap_pos,
                                        S&       stream)
      {
         if (Derived::is_variable_size && !Derived::is_empty_container(value))
            stream.rewrite_raw(fixed_pos,
                               static_cast<typename Format::size_type>(heap_pos - fixed_pos));
      }

      // Pack object content if T is variable size
      template <typename S>
      static void embedded_variable_pack(const T& value, S& stream)
      {
         if (Derived::is_variable_size && !Derived::is_empty_container(value))
            Derived::pack(value, stream);
      }

      // Compute total packed size without writing
      static uint32_t packed_size(const T& value)
      {
         size_stream ss;
         Derived::pack(value, ss);
         return static_cast<uint32_t>(ss.size);
      }

      // Compute the size that embedded_variable_pack would write
      static uint32_t embedded_variable_packed_size(const T& value)
      {
         size_stream ss;
         Derived::embedded_variable_pack(value, ss);
         return static_cast<uint32_t>(ss.size);
      }

      // // Unpack and/or Verify object in a single contiguous region
      // template <bool Unpack, bool Verify>
      // [[nodiscard]] static bool unpack(T*          value,
      //                                  bool&       has_unknown,
      //                                  bool&       known_end,
      //                                  const char* src,
      //                                  uint32_t&   pos,
      //                                  uint32_t    end_pos);

      // Unpack and/or Verify object which is pointed to by an offset.
      // `SkipOffsetCheck = true` tells us the caller has already bounds-checked
      // the offset slot (e.g. we're inside a pre-validated struct fixed region).
      template <bool Unpack, bool Verify, bool SkipOffsetCheck = false>
      [[nodiscard]] static bool embedded_variable_unpack(T*          value,
                                                         bool&       has_unknown,
                                                         bool&       known_pos,
                                                         const char* src,
                                                         uint32_t&   fixed_pos,
                                                         uint32_t    end_fixed_pos,
                                                         uint32_t&   heap_pos,
                                                         uint32_t    end_heap_pos)
      {
         uint32_t                   orig_pos = fixed_pos;
         typename Format::size_type offset;
         if (!unpack_numeric<Verify, typename Format::size_type, SkipOffsetCheck>(
                 &offset, src, fixed_pos, end_fixed_pos))
            return false;
         if constexpr (Derived::supports_0_offset)
         {
            if (offset == 0)
            {
               if constexpr (Unpack)
                  if (!Derived::template clear_container<Verify>(value))
                     return false;
               if constexpr (Verify)
                  known_pos = true;
               return true;
            }
         }
         uint32_t new_heap_pos = orig_pos + offset;
         if constexpr (Verify)
         {
            if (offset < Format::size_bytes || new_heap_pos < heap_pos ||
                new_heap_pos > end_heap_pos)
               return false;
            if (new_heap_pos != heap_pos && known_pos)
               return false;
            if constexpr (Derived::supports_0_offset)
               if (Derived::is_empty_container(src, new_heap_pos, end_heap_pos))
                  return false;
         }
         heap_pos = new_heap_pos;
         return Derived::template unpack<Unpack, Verify>(value, has_unknown, known_pos, src,
                                                         heap_pos, end_heap_pos);
      }

      // Unpack and/or verify either:
      // * Object at fixed_pos if T is fixed size
      // * Object at offset if T is variable size
      // `SkipOffsetCheck = true` is forwarded to the variable path.
      template <bool Unpack, bool Verify, bool SkipOffsetCheck = false>
      [[nodiscard]] static bool embedded_unpack(T*          value,
                                                bool&       has_unknown,
                                                bool&       known_pos,
                                                const char* src,
                                                uint32_t&   fixed_pos,
                                                uint32_t    end_fixed_pos,
                                                uint32_t&   heap_pos,
                                                uint32_t    end_heap_pos)
      {
         if constexpr (Derived::is_variable_size)
            return Derived::template embedded_variable_unpack<Unpack, Verify, SkipOffsetCheck>(
                value, has_unknown, known_pos, src, fixed_pos, end_fixed_pos, heap_pos,
                end_heap_pos);
         else
            return Derived::template unpack<Unpack, Verify>(value, has_unknown, known_pos, src,
                                                            fixed_pos, end_fixed_pos);
      }
   };  // base_packable_impl

   template <PackableMemcpy T, typename Format>
   struct is_packable<T, Format> : base_packable_impl<T, is_packable<T, Format>, Format>
   {
      static constexpr uint32_t fixed_size        = sizeof(T);
      static constexpr bool     is_variable_size  = false;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = false;

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         stream.write_raw(value);
      }

      static uint32_t packed_size(const T&) { return sizeof(T); }
      static uint32_t embedded_variable_packed_size(const T&) { return 0; }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T*          value,
                                       bool&       has_unknown,
                                       bool&       known_end,
                                       const char* src,
                                       uint32_t&   pos,
                                       uint32_t    end_pos)
      {
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < sizeof(T))
               return false;
         }
         if constexpr (Unpack)
            std::memcpy(value, src + pos, sizeof(T));
         pos += sizeof(T);
         return true;
      }
   };  // is_packable<PackableMemcpy>

   template <bool Verify, PackableNumeric T, bool SkipCheck>
   [[nodiscard]] bool unpack_numeric(T* value, const char* src, uint32_t& pos, uint32_t end_pos)
   {
      if constexpr (SkipCheck && Verify)
      {
         // Pre-validated region: caller guarantees `pos + sizeof(T) <= end_pos`.
         // Skip the bounds check; just read.
         std::memcpy(value, src + pos, sizeof(T));
         pos += sizeof(T);
         return true;
      }
      else
      {
         bool has_unknown, known_end;
         return is_packable<T>::template unpack<true, Verify>(value, has_unknown, known_end, src,
                                                              pos, end_pos);
      }
   }

   template <typename Format>
   struct is_packable<bool, Format> : base_packable_impl<bool, is_packable<bool, Format>, Format>
   {
      static constexpr uint32_t fixed_size        = 1;
      static constexpr bool     is_variable_size  = false;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = false;

      template <typename S>
      static void pack(const bool& value, S& stream)
      {
         stream.write_raw(value);
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(bool*       value,
                                       bool&       has_unknown,
                                       bool&       known_end,
                                       const char* src,
                                       uint32_t&   pos,
                                       uint32_t    end_pos)
      {
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < 1 || static_cast<unsigned char>(src[pos]) > 1)
               return false;
         }
         if constexpr (Unpack)
            *value = src[pos] != 0;
         pos += 1;
         return true;
      }
   };  // is_packable<bool>

   template <PackableWrapper T, typename Format>
   struct is_packable<T, Format> : std::bool_constant<true>
   {
      using inner = std::remove_cvref_t<decltype(psio_unwrap_packable(std::declval<T&>()))>;
      using is_p  = is_packable<inner, Format>;

      static constexpr uint32_t fixed_size        = is_p::fixed_size;
      static constexpr bool     is_variable_size  = is_p::is_variable_size;
      static constexpr bool     is_optional       = is_p::is_optional;
      static constexpr bool     supports_0_offset = is_p::supports_0_offset;

      static bool has_value(const T& value) { return is_p::has_value(psio_unwrap_packable(value)); }
      template <bool Verify>
      static bool has_value(const char* src, uint32_t pos, uint32_t end_pos)
      {
         return is_p::template has_value<Verify>(src, pos, end_pos);
      }

      template <bool Unpack>
      static inner* ptr(T* value)
      {
         if constexpr (Unpack)
            return &psio_unwrap_packable(*value);
         else
            return nullptr;
      }

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         return is_p::pack(psio_unwrap_packable(value), stream);
      }

      static bool is_empty_container(const T& value)
      {
         return is_p::is_empty_container(psio_unwrap_packable(value));
      }
      static bool is_empty_container(const char* src, uint32_t pos, uint32_t end_pos)
      {
         return is_p::is_empty_container(src, pos, end_pos);
      }

      static uint32_t packed_size(const T& value)
      {
         return is_p::packed_size(psio_unwrap_packable(value));
      }
      static uint32_t embedded_variable_packed_size(const T& value)
      {
         return is_p::embedded_variable_packed_size(psio_unwrap_packable(value));
      }

      template <typename S>
      static void embedded_fixed_pack(const T& value, S& stream)
      {
         return is_p::embedded_fixed_pack(psio_unwrap_packable(value), stream);
      }

      template <typename S>
      static void embedded_fixed_repack(const T& value,
                                        uint32_t fixed_pos,
                                        uint32_t heap_pos,
                                        S&       stream)
      {
         return is_p::embedded_fixed_repack(psio_unwrap_packable(value), fixed_pos, heap_pos,
                                            stream);
      }

      template <typename S>
      static void embedded_variable_pack(const T& value, S& stream)
      {
         return is_p::embedded_variable_pack(psio_unwrap_packable(value), stream);
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T*          value,
                                       bool&       has_unknown,
                                       bool&       known_end,
                                       const char* src,
                                       uint32_t&   pos,
                                       uint32_t    end_pos)
      {
         auto orig_pos = pos;
         bool result   = is_p::template unpack<Unpack, Verify>(ptr<Unpack>(value), has_unknown,
                                                               known_end, src, pos, end_pos);
         if constexpr (Verify && (PackableValidatedObject<T> || PackableValidatedView<T>))
         {
            return result && user_validate<Unpack, false, T, Format>(value, src, orig_pos);
         }
         return result;
      }

      template <bool Unpack, bool Verify, bool SkipOffsetCheck = false>
      [[nodiscard]] static bool embedded_variable_unpack(T*          value,
                                                         bool&       has_unknown,
                                                         bool&       known_end,
                                                         const char* src,
                                                         uint32_t&   fixed_pos,
                                                         uint32_t    end_fixed_pos,
                                                         uint32_t&   heap_pos,
                                                         uint32_t    end_heap_pos)
      {
         auto orig_pos = fixed_pos;
         bool result   = is_p::template embedded_variable_unpack<Unpack, Verify, SkipOffsetCheck>(
             ptr<Unpack>(value), has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
             end_heap_pos);
         if constexpr (Verify && (PackableValidatedObject<T> || PackableValidatedView<T>))
         {
            return result && user_validate<Unpack, true, T, Format>(value, src, orig_pos);
         }
         return result;
      }

      template <bool Unpack, bool Verify, bool SkipOffsetCheck = false>
      [[nodiscard]] static bool embedded_unpack(T*          value,
                                                bool&       has_unknown,
                                                bool&       known_end,
                                                const char* src,
                                                uint32_t&   fixed_pos,
                                                uint32_t    end_fixed_pos,
                                                uint32_t&   heap_pos,
                                                uint32_t    end_heap_pos)
      {
         auto orig_pos = fixed_pos;
         bool result   = is_p::template embedded_unpack<Unpack, Verify, SkipOffsetCheck>(
             ptr<Unpack>(value), has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
             end_heap_pos);
         if constexpr (Verify && (PackableValidatedObject<T> || PackableValidatedView<T>))
         {
            return result && user_validate<Unpack, !is_optional && is_variable_size, T, Format>(
                                 value, src, orig_pos);
         }
         return result;
      }
   };  // is_packable<PackableWrapper>

   template <typename T, typename Derived, typename Format = frac_format_32>
   struct packable_container_memcpy_impl : base_packable_impl<T, Derived, Format>
   {
      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = true;

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         is_packable<typename Format::size_type, Format>::pack(
             static_cast<typename Format::size_type>(value.size() * sizeof(typename T::value_type)),
             stream);
         if (value.size())
            stream.write(value.data(), value.size() * sizeof(typename T::value_type));
      }

      static bool is_empty_container(const T& value) { return value.empty(); }
      static bool is_empty_container(const char* src, uint32_t pos, uint32_t end_pos)
      {
         typename Format::size_type fixed_size = 0;
         if (!unpack_numeric<true>(&fixed_size, src, pos, end_pos))
            return false;
         return fixed_size == 0;
      }

      static uint32_t packed_size(const T& value)
      {
         return Format::size_bytes +
                static_cast<uint32_t>(value.size() * sizeof(typename T::value_type));
      }
      static uint32_t embedded_variable_packed_size(const T& value)
      {
         return value.empty() ? 0 : packed_size(value);
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T*          value,
                                       bool&       has_unknown,
                                       bool&       known_end,
                                       const char* src,
                                       uint32_t&   pos,
                                       uint32_t    end_pos)
      {
         typename Format::size_type fixed_size = 0;
         if (!unpack_numeric<Verify>(&fixed_size, src, pos, end_pos))
            return false;
         uint32_t size    = fixed_size / sizeof(typename T::value_type);
         uint32_t new_pos = pos + fixed_size;
         if constexpr (Verify)
         {
            known_end = true;
            if ((fixed_size % sizeof(typename T::value_type)) || new_pos < pos || new_pos > end_pos)
               return false;
         }
         if constexpr (Unpack)
         {
            // resize(n) on std::vector<trivial> value-initializes (zero-fills)
            // every element, doubling write bandwidth on the subsequent
            // memcpy. assign(first, last) with pointer iterators of a
            // trivially-copyable T lowers to a single memcpy via
            // __uninitialized_copy_a, with no zero-init pass. Measured 2×
            // decode speedup on a 260 MiB validator list (libstdc++).
            using elem_t = typename T::value_type;
            if constexpr (requires(T& v, const elem_t* p) {
                             v.assign(p, p + 0);
                          })
            {
               const elem_t* first =
                   reinterpret_cast<const elem_t*>(src + pos);
               value->assign(first, first + size);
            }
            else
            {
               value->resize(size);
               if (size)
                  std::memcpy(value->data(), src + pos, fixed_size);
            }
         }
         pos = new_pos;
         return true;
      }
   };  // packable_container_memcpy_impl

   template <typename Format>
   struct is_packable<std::string, Format>
       : packable_container_memcpy_impl<std::string, is_packable<std::string, Format>, Format>
   {
   };

   template <typename Format>
   struct is_packable<std::string_view, Format>
       : packable_container_memcpy_impl<std::string_view,
                                         is_packable<std::string_view, Format>,
                                         Format>
   {
   };

   template <PackableMemcpy T, typename Format>
   struct is_packable<std::span<T>, Format>
       : packable_container_memcpy_impl<std::span<T>, is_packable<std::span<T>, Format>, Format>
   {
   };

   template <PackableMemcpy T, typename Format>
   struct is_packable<std::span<const T>, Format>
       : packable_container_memcpy_impl<std::span<const T>,
                                         is_packable<std::span<const T>, Format>,
                                         Format>
   {
   };

   template <Packable T, typename Format>
      requires(is_packable_memcpy<T>::value)
   struct is_packable<std::vector<T>, Format>
       : packable_container_memcpy_impl<std::vector<T>,
                                         is_packable<std::vector<T>, Format>,
                                         Format>
   {
   };

   // ── Bounded containers: compact length prefix (smallest uint fitting N) ───
   //
   // Fracpack encoding: [LengthT count_bytes][data...]
   // where LengthT = bounded_length_t<N * sizeof(T::value_type)>. Compared to
   // std::vector's 4-byte prefix, this saves 3 B for N<256, 2 B for N<65536,
   // 0 B for larger bounds. For N ≥ 2^32 we use u64 — 4 B larger than vector,
   // but necessary to represent the bound.

   template <typename T, std::size_t MaxBytes, typename Derived,
             typename Format = frac_format_32>
   struct packable_bounded_memcpy_impl : base_packable_impl<T, Derived, Format>
   {
      using elem_t = typename T::value_type;
      using length_t = bounded_length_t<MaxBytes>;

      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = true;

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         length_t len = static_cast<length_t>(value.size() * sizeof(elem_t));
         stream.write(reinterpret_cast<const char*>(&len), sizeof(len));
         if (value.size())
            stream.write(value.data(), value.size() * sizeof(elem_t));
      }

      static bool is_empty_container(const T& value) { return value.empty(); }
      static bool is_empty_container(const char* src, uint32_t pos, uint32_t end_pos)
      {
         if (end_pos - pos < sizeof(length_t))
            return false;
         length_t len = 0;
         std::memcpy(&len, src + pos, sizeof(length_t));
         return len == 0;
      }

      static uint32_t packed_size(const T& value)
      {
         return sizeof(length_t) +
                static_cast<uint32_t>(value.size() * sizeof(elem_t));
      }
      static uint32_t embedded_variable_packed_size(const T& value)
      {
         return value.empty() ? 0 : packed_size(value);
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T* value, bool& has_unknown, bool& known_end,
                                       const char* src, uint32_t& pos, uint32_t end_pos)
      {
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < sizeof(length_t))
               return false;
         }
         length_t len = 0;
         std::memcpy(&len, src + pos, sizeof(length_t));
         pos += sizeof(length_t);
         std::uint32_t byte_count = static_cast<std::uint32_t>(len);
         std::uint32_t count      = byte_count / sizeof(elem_t);
         std::uint32_t new_pos    = pos + byte_count;
         if constexpr (Verify)
         {
            if ((byte_count % sizeof(elem_t)) || new_pos < pos || new_pos > end_pos)
               return false;
         }
         if constexpr (Unpack)
         {
            value->storage().resize(count);
            if (count)
               std::memcpy(value->data(), src + pos, byte_count);
         }
         pos = new_pos;
         return true;
      }
   };

   template <PackableMemcpy T, std::size_t N, typename Format>
   struct is_packable<bounded_list<T, N>, Format>
       : packable_bounded_memcpy_impl<bounded_list<T, N>,
                                       N * sizeof(T),
                                       is_packable<bounded_list<T, N>, Format>,
                                       Format>
   {
   };

   template <std::size_t N, typename Format>
   struct is_packable<bounded_string<N>, Format>
       : packable_bounded_memcpy_impl<bounded_string<N>,
                                       N,
                                       is_packable<bounded_string<N>, Format>,
                                       Format>
   {
   };

   template <typename T>
   struct make_mutable
   {
      using type = T;
   };
   template <typename T>
   using make_mutable_t = typename make_mutable<T>::type;
   template <typename T>
   struct make_mutable<const T> : make_mutable<T>
   {
   };
   template <typename T, typename U>
   struct make_mutable<std::pair<T, U>>
   {
      using type = std::pair<make_mutable_t<T>, make_mutable_t<U>>;
   };

   template <typename T>
   concept PackableAssociativeContainer = requires(T& t, const typename T::value_type& v) {
      t.begin();
      t.end();
      typename T::value_type;
      requires Packable<typename T::value_type>;
      requires requires(const typename T::value_type& v) { t.insert(v); };
   };

   template <typename T, typename Derived, typename Format = frac_format_32>
   struct packable_container_impl : base_packable_impl<T, Derived, Format>
   {
      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = true;

      using value_type = typename T::value_type;

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         uint32_t num_bytes = value.size() * is_packable<value_type, Format>::fixed_size;
         assert((num_bytes == value.size() * is_packable<value_type, Format>::fixed_size));
         is_packable<typename Format::size_type, Format>::pack(
             static_cast<typename Format::size_type>(num_bytes), stream);
         stream.about_to_write(num_bytes);
         uint32_t fixed_pos = stream.written();
         for (const auto& x : value)
            is_packable<value_type, Format>::embedded_fixed_pack(x, stream);
         for (const auto& x : value)
         {
            is_packable<value_type, Format>::embedded_fixed_repack(x, fixed_pos, stream.written(),
                                                                    stream);
            is_packable<value_type, Format>::embedded_variable_pack(x, stream);
            fixed_pos += is_packable<T, Format>::fixed_size;
         }
      }

      static bool is_empty_container(const T& value) { return value.empty(); }
      static bool is_empty_container(const char* src, uint32_t pos, uint32_t end_pos)
      {
         typename Format::size_type fixed_size;
         if (!unpack_numeric<true>(&fixed_size, src, pos, end_pos))
            return false;
         return fixed_size == 0;
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T*          value,
                                       bool&       has_unknown,
                                       bool&       known_end,
                                       const char* src,
                                       uint32_t&   pos,
                                       uint32_t    end_pos)
      {
         typename Format::size_type fixed_size;
         if (!unpack_numeric<Verify>(&fixed_size, src, pos, end_pos))
            return false;
         uint32_t size          = fixed_size / is_packable<value_type, Format>::fixed_size;
         uint32_t fixed_pos     = pos;
         uint32_t heap_pos      = pos + fixed_size;
         uint32_t end_fixed_pos = heap_pos;
         if constexpr (Verify)
         {
            known_end = true;
            if ((fixed_size % is_packable<value_type, Format>::fixed_size) || heap_pos < pos ||
                heap_pos > end_pos)
               return false;
         }
         if constexpr (Unpack)
         {
            if (!Derived::template unpack_items<Verify>(value, size, has_unknown, known_end, src,
                                                        fixed_pos, end_fixed_pos, heap_pos,
                                                        end_pos))
               return false;
         }
         else
         {
            for (uint32_t i = 0; i < size; ++i)
               if (!is_packable<make_mutable_t<value_type>,
                                 Format>::template embedded_unpack<Unpack, Verify>(
                       nullptr, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
                       end_pos))
                  return false;
         }
         pos = heap_pos;
         return true;
      }  // unpack
   };

   template <typename T, typename Derived, typename Format = frac_format_32>
   struct packable_associative_container_impl : packable_container_impl<T, Derived, Format>
   {
      using value_type = typename T::value_type;
      template <bool Verify>
      [[nodiscard]] static bool unpack_items(T*             value,
                                             std::uint32_t  size,
                                             bool&          has_unknown,
                                             bool&          known_end,
                                             const char*    src,
                                             std::uint32_t  fixed_pos,
                                             std::uint32_t  end_fixed_pos,
                                             std::uint32_t& heap_pos,
                                             std::uint32_t  end_pos)
      {
         value->clear();
         for (std::size_t i = 0; i < size; ++i)
         {
            make_mutable_t<value_type> item;
            if (!is_packable<make_mutable_t<value_type>,
                              Format>::template embedded_unpack<true, Verify>(
                    &item, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
                    end_pos))
               return false;
            value->insert(std::move(item));
         }
         return true;
      }
   };

   template <typename T, typename Derived, typename Format = frac_format_32>
   struct packable_sequence_container_impl : packable_container_impl<T, Derived, Format>
   {
      using value_type = typename T::value_type;
      template <bool Verify>
      [[nodiscard]] static bool unpack_items(T*             value,
                                             std::uint32_t  size,
                                             bool&          has_unknown,
                                             bool&          known_end,
                                             const char*    src,
                                             std::uint32_t  fixed_pos,
                                             std::uint32_t  end_fixed_pos,
                                             std::uint32_t& heap_pos,
                                             std::uint32_t  end_pos)
      {
         value->resize(size);
         for (auto& x : *value)
            if (!is_packable<value_type, Format>::template embedded_unpack<true, Verify>(
                    &x, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos, end_pos))
               return false;
         return true;
      }
   };

   template <Packable T, typename Format>
      requires(!is_packable_memcpy<T>::value)
   struct is_packable<std::vector<T>, Format>
       : packable_sequence_container_impl<std::vector<T>,
                                           is_packable<std::vector<T>, Format>,
                                           Format>
   {
   };

   // ── Variable-element bounded container ────────────────────────────────────
   //
   // Mirrors packable_container_impl / packable_sequence_container_impl but
   // uses a bounded length prefix (smallest uint fitting MaxBytes) instead of
   // Format::size_type. `MaxBytes` should be N × fixed_size of the element's
   // per-element slot (u32 offset for variable elements, i.e. N × 4 for
   // frac_format_32). For T that is itself memcpy-safe, the caller should use
   // packable_bounded_memcpy_impl instead.

   template <typename T, std::size_t MaxBytes, typename Derived,
             typename Format = frac_format_32>
   struct packable_bounded_container_impl : base_packable_impl<T, Derived, Format>
   {
      using elem_t   = typename T::value_type;
      using length_t = bounded_length_t<MaxBytes>;

      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = true;

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         uint32_t num_bytes = static_cast<uint32_t>(
             value.size() * is_packable<elem_t, Format>::fixed_size);
         length_t len = static_cast<length_t>(num_bytes);
         stream.write(reinterpret_cast<const char*>(&len), sizeof(len));
         stream.about_to_write(num_bytes);
         uint32_t fixed_pos = stream.written();
         for (const auto& x : value)
            is_packable<elem_t, Format>::embedded_fixed_pack(x, stream);
         for (const auto& x : value)
         {
            is_packable<elem_t, Format>::embedded_fixed_repack(x, fixed_pos, stream.written(),
                                                                stream);
            is_packable<elem_t, Format>::embedded_variable_pack(x, stream);
            fixed_pos += is_packable<elem_t, Format>::fixed_size;
         }
      }

      static bool is_empty_container(const T& value) { return value.empty(); }
      static bool is_empty_container(const char* src, uint32_t pos, uint32_t end_pos)
      {
         if (end_pos - pos < sizeof(length_t))
            return false;
         length_t len = 0;
         std::memcpy(&len, src + pos, sizeof(length_t));
         return len == 0;
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T* value, bool& has_unknown, bool& known_end,
                                       const char* src, uint32_t& pos, uint32_t end_pos)
      {
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < sizeof(length_t))
               return false;
         }
         length_t len = 0;
         std::memcpy(&len, src + pos, sizeof(length_t));
         pos += sizeof(length_t);
         uint32_t num_bytes     = static_cast<uint32_t>(len);
         uint32_t size          = num_bytes / is_packable<elem_t, Format>::fixed_size;
         uint32_t fixed_pos     = pos;
         uint32_t heap_pos      = pos + num_bytes;
         uint32_t end_fixed_pos = heap_pos;
         if constexpr (Verify)
         {
            if ((num_bytes % is_packable<elem_t, Format>::fixed_size) || heap_pos < pos ||
                heap_pos > end_pos)
               return false;
         }
         if constexpr (Unpack)
         {
            if (!Derived::template unpack_items<Verify>(value, size, has_unknown, known_end, src,
                                                         fixed_pos, end_fixed_pos, heap_pos,
                                                         end_pos))
               return false;
         }
         else
         {
            for (uint32_t i = 0; i < size; ++i)
               if (!is_packable<make_mutable_t<elem_t>,
                                 Format>::template embedded_unpack<Unpack, Verify>(
                       nullptr, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
                       end_pos))
                  return false;
         }
         pos = heap_pos;
         return true;
      }
   };

   template <typename T, std::size_t MaxBytes, typename Derived,
             typename Format = frac_format_32>
   struct packable_bounded_sequence_container_impl
       : packable_bounded_container_impl<T, MaxBytes, Derived, Format>
   {
      using elem_t = typename T::value_type;
      template <bool Verify>
      [[nodiscard]] static bool unpack_items(T*             value,
                                             std::uint32_t  size,
                                             bool&          has_unknown,
                                             bool&          known_end,
                                             const char*    src,
                                             std::uint32_t  fixed_pos,
                                             std::uint32_t  end_fixed_pos,
                                             std::uint32_t& heap_pos,
                                             std::uint32_t  end_pos)
      {
         value->storage().resize(size);
         for (auto& x : value->storage())
            if (!is_packable<elem_t, Format>::template embedded_unpack<true, Verify>(
                    &x, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos, end_pos))
               return false;
         return true;
      }
   };

   // bounded_list<T, N> for variable-size T: N × elem_slot_size bytes max.
   // For variable-size T, the element's fixed_size is the offset-slot width
   // (Format::size_bytes) since each element gets an offset in the fixed region.
   template <Packable T, std::size_t N, typename Format>
      requires(!is_packable_memcpy<T>::value)
   struct is_packable<bounded_list<T, N>, Format>
       : packable_bounded_sequence_container_impl<
             bounded_list<T, N>,
             N * Format::size_bytes,
             is_packable<bounded_list<T, N>, Format>,
             Format>
   {
   };

   // ── bitlist<N> ────────────────────────────────────────────────────────────
   //
   // Wire format:
   //   [bit_count: bounded_length_t<N>][ceil(bit_count/8) bytes packed]
   //
   // Empty bitlist encodes via the offset=0 sentinel (supports_0_offset=true).
   // Validation rejects bit_count > N and rejects non-zero trailing bits in
   // the final byte.

   template <std::size_t MaxN, typename Derived, typename Format = frac_format_32>
   struct packable_bitlist_impl : base_packable_impl<bitlist<MaxN>, Derived, Format>
   {
      using T        = bitlist<MaxN>;
      using length_t = bounded_length_t<MaxN>;

      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = true;

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         length_t len = static_cast<length_t>(value.size());
         stream.write(reinterpret_cast<const char*>(&len), sizeof(len));
         auto data = value.bytes();
         if (!data.empty())
            stream.write(reinterpret_cast<const char*>(data.data()), data.size());
      }

      static bool is_empty_container(const T& value) { return value.empty(); }
      static bool is_empty_container(const char* src, uint32_t pos, uint32_t end_pos)
      {
         if (end_pos - pos < sizeof(length_t))
            return false;
         length_t len = 0;
         std::memcpy(&len, src + pos, sizeof(length_t));
         return len == 0;
      }

      static uint32_t packed_size(const T& value)
      {
         return sizeof(length_t) + static_cast<uint32_t>(value.byte_count());
      }
      static uint32_t embedded_variable_packed_size(const T& value)
      {
         return value.empty() ? 0 : packed_size(value);
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T* value, bool& has_unknown, bool& known_end,
                                       const char* src, uint32_t& pos, uint32_t end_pos)
      {
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < sizeof(length_t))
               return false;
         }
         length_t len = 0;
         std::memcpy(&len, src + pos, sizeof(length_t));
         pos += sizeof(length_t);
         std::uint32_t bit_count  = static_cast<std::uint32_t>(len);
         std::uint32_t byte_count = (bit_count + 7) / 8;
         std::uint32_t new_pos    = pos + byte_count;
         if constexpr (Verify)
         {
            if (bit_count > MaxN || new_pos < pos || new_pos > end_pos)
               return false;
            if (byte_count > 0 && (bit_count & 7u) != 0)
            {
               std::uint8_t mask =
                   static_cast<std::uint8_t>((1u << (bit_count & 7u)) - 1u);
               std::uint8_t last =
                   static_cast<std::uint8_t>(src[pos + byte_count - 1]);
               if ((last & ~mask) != 0)
                  return false;
            }
         }
         if constexpr (Unpack)
         {
            value->assign_raw(bit_count, reinterpret_cast<const std::uint8_t*>(src + pos));
         }
         pos = new_pos;
         return true;
      }
   };

   template <std::size_t MaxN, typename Format>
   struct is_packable<bitlist<MaxN>, Format>
       : packable_bitlist_impl<MaxN, is_packable<bitlist<MaxN>, Format>, Format>
   {
   };

   // ── std::bitset<N>: fixed-size, packed via pack_bitset_bytes helpers ──────
   // Wire format identical to psio1::bitvector<N>. Not a memcpy type because
   // std::bitset's internal layout is implementation-defined.

   template <std::size_t N, typename Format>
   struct is_packable<std::bitset<N>, Format>
       : base_packable_impl<std::bitset<N>, is_packable<std::bitset<N>, Format>, Format>
   {
      static constexpr uint32_t fixed_size        = (N + 7) / 8;
      static constexpr bool     is_variable_size  = false;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = false;

      template <typename S>
      static void pack(const std::bitset<N>& bs, S& stream)
      {
         std::uint8_t buf[(N + 7) / 8];
         pack_bitset_bytes(bs, buf);
         stream.write(reinterpret_cast<const char*>(buf), (N + 7) / 8);
      }

      static std::uint32_t packed_size(const std::bitset<N>&) { return (N + 7) / 8; }
      static std::uint32_t embedded_variable_packed_size(const std::bitset<N>&) { return 0; }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(std::bitset<N>* value, bool& has_unknown, bool& known_end,
                                       const char* src, uint32_t& pos, uint32_t end_pos)
      {
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < (N + 7) / 8)
               return false;
         }
         if constexpr (Unpack)
            unpack_bitset_bytes(reinterpret_cast<const std::uint8_t*>(src + pos), *value);
         pos += (N + 7) / 8;
         return true;
      }
   };

   // ── std::vector<bool>: unbounded bitlist analogue ─────────────────────────
   // Wire format: [Format::size_type num_bytes][ceil(bit_count/8) bytes]
   // We store the BYTE count (not bit count) so it's ambiguous between the
   // last byte being full or having trailing zero bits. To resolve, we add a
   // secondary byte that stores bit_count % 8 as the leading byte. Simpler:
   // emit as [size_type bit_count][ceil/8 data]. That matches bitlist's shape.

   template <typename Format>
   struct is_packable<std::vector<bool>, Format>
       : base_packable_impl<std::vector<bool>, is_packable<std::vector<bool>, Format>, Format>
   {
      using length_t = typename Format::size_type;

      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = true;

      template <typename S>
      static void pack(const std::vector<bool>& value, S& stream)
      {
         length_t bit_count = static_cast<length_t>(value.size());
         stream.write(reinterpret_cast<const char*>(&bit_count), sizeof(bit_count));
         auto packed = pack_vector_bool(value);
         if (!packed.empty())
            stream.write(reinterpret_cast<const char*>(packed.data()), packed.size());
      }

      static bool is_empty_container(const std::vector<bool>& value) { return value.empty(); }
      static bool is_empty_container(const char* src, uint32_t pos, uint32_t end_pos)
      {
         if (end_pos - pos < sizeof(length_t))
            return false;
         length_t bc = 0;
         std::memcpy(&bc, src + pos, sizeof(length_t));
         return bc == 0;
      }

      static std::uint32_t packed_size(const std::vector<bool>& value)
      {
         return sizeof(length_t) + static_cast<std::uint32_t>((value.size() + 7) / 8);
      }
      static std::uint32_t embedded_variable_packed_size(const std::vector<bool>& value)
      {
         return value.empty() ? 0 : packed_size(value);
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(std::vector<bool>* value, bool& has_unknown,
                                       bool& known_end, const char* src, uint32_t& pos,
                                       uint32_t end_pos)
      {
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < sizeof(length_t))
               return false;
         }
         length_t bc_raw = 0;
         std::memcpy(&bc_raw, src + pos, sizeof(length_t));
         pos += sizeof(length_t);
         std::uint32_t bit_count  = static_cast<std::uint32_t>(bc_raw);
         std::uint32_t byte_count = (bit_count + 7) / 8;
         std::uint32_t new_pos    = pos + byte_count;
         if constexpr (Verify)
         {
            if (new_pos < pos || new_pos > end_pos)
               return false;
            if (byte_count > 0 && (bit_count & 7u) != 0)
            {
               std::uint8_t mask =
                   static_cast<std::uint8_t>((1u << (bit_count & 7u)) - 1u);
               std::uint8_t last =
                   static_cast<std::uint8_t>(src[pos + byte_count - 1]);
               if ((last & ~mask) != 0)
                  return false;
            }
         }
         if constexpr (Unpack)
         {
            unpack_vector_bool(reinterpret_cast<const std::uint8_t*>(src + pos), bit_count,
                               *value);
         }
         pos = new_pos;
         return true;
      }
   };

   template <PackableAssociativeContainer T, typename Format>
   struct is_packable<T, Format>
       : packable_associative_container_impl<T, is_packable<T, Format>, Format>
   {
   };

   template <Packable T, std::size_t N, typename Format>
      requires(!is_packable_memcpy<T>::value)
   struct is_packable<std::array<T, N>, Format>
       : base_packable_impl<std::array<T, N>, is_packable<std::array<T, N>, Format>, Format>
   {
      static constexpr uint32_t fixed_size    = is_packable<T, Format>::is_variable_size
                                                    ? Format::size_bytes
                                                    : is_packable<T, Format>::fixed_size * N;
      static constexpr bool is_variable_size  = is_packable<T, Format>::is_variable_size;
      static constexpr bool is_optional       = false;
      static constexpr bool supports_0_offset = false;

      template <typename S>
      static void pack(const std::array<T, N>& value, S& stream)
      {
         stream.about_to_write(is_packable<T, Format>::fixed_size * N);
         uint32_t fixed_pos = stream.written();
         for (const auto& x : value)
            is_packable<T, Format>::embedded_fixed_pack(x, stream);
         for (const auto& x : value)
         {
            is_packable<T, Format>::embedded_fixed_repack(x, fixed_pos, stream.written(), stream);
            is_packable<T, Format>::embedded_variable_pack(x, stream);
            fixed_pos += is_packable<T, Format>::fixed_size;
         }
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(std::array<T, N>* value,
                                       bool&             has_unknown,
                                       bool&             known_end,
                                       const char*       src,
                                       uint32_t&         pos,
                                       uint32_t          end_pos)
      {
         uint32_t fixed_pos     = pos;
         uint32_t heap_pos      = pos + is_packable<T, Format>::fixed_size * N;
         uint32_t end_fixed_pos = heap_pos;
         if constexpr (Verify)
         {
            known_end = true;
            if (heap_pos < pos || heap_pos > end_pos)
               return false;
         }
         if constexpr (Unpack)
         {
            for (auto& x : *value)
               if (!is_packable<T, Format>::template embedded_unpack<Unpack, Verify>(
                       &x, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
                       end_pos))
                  return false;
         }
         else
         {
            for (uint32_t i = 0; i < N; ++i)
               if (!is_packable<T, Format>::template embedded_unpack<Unpack, Verify>(
                       nullptr, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
                       end_pos))
                  return false;
         }
         pos = heap_pos;
         return true;
      }
   };

   template <Packable T, typename Format>
   struct is_packable<std::optional<T>, Format>
       : base_packable_impl<std::optional<T>, is_packable<std::optional<T>, Format>, Format>
   {
      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = true;
      static constexpr bool     supports_0_offset = false;

      static bool has_value(const std::optional<T>& value) { return value.has_value(); }
      template <bool Verify>
      static bool has_value(const char* src, uint32_t pos, uint32_t end_pos)
      {
         typename Format::size_type offset;
         if (!unpack_numeric<Verify>(&offset, src, pos, end_pos))
            return false;
         return offset != 1;
      }

      template <typename S>
      static void pack(const std::optional<T>& value, S& stream)
      {
         uint32_t fixed_pos = stream.written();
         embedded_fixed_pack(value, stream);
         uint32_t heap_pos = stream.written();
         embedded_fixed_repack(value, fixed_pos, heap_pos, stream);
         embedded_variable_pack(value, stream);
      }

      template <typename S>
      static void embedded_fixed_pack(const std::optional<T>& value, S& stream)
      {
         if (!is_packable<T, Format>::is_optional && is_packable<T, Format>::is_variable_size &&
             value.has_value())
            is_packable<T, Format>::embedded_fixed_pack(*value, stream);
         else
            stream.write_raw(typename Format::size_type(1));
      }

      template <typename S>
      static void embedded_fixed_repack(const std::optional<T>& value,
                                        uint32_t                fixed_pos,
                                        uint32_t                heap_pos,
                                        S&                      stream)
      {
         if (value.has_value())
         {
            if (!is_packable<T, Format>::is_optional && is_packable<T, Format>::is_variable_size)
               is_packable<T, Format>::embedded_fixed_repack(*value, fixed_pos, heap_pos, stream);
            else
               stream.rewrite_raw(fixed_pos,
                                  static_cast<typename Format::size_type>(heap_pos - fixed_pos));
         }
      }

      template <typename S>
      static void embedded_variable_pack(const std::optional<T>& value, S& stream)
      {
         if (value.has_value() && !is_packable<T, Format>::is_empty_container(*value))
            is_packable<T, Format>::pack(*value, stream);
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(std::optional<T>* value,
                                       bool&             has_unknown,
                                       bool&             known_end,
                                       const char*       src,
                                       uint32_t&         pos,
                                       uint32_t          end_pos)
      {
         uint32_t fixed_pos = pos;
         if constexpr (Verify)
         {
            known_end = true;
            if (end_pos - pos < Format::size_bytes)
            {
               return false;
            }
         }
         pos += Format::size_bytes;
         uint32_t end_fixed_pos = pos;
         return embedded_unpack<Unpack, Verify>(value, has_unknown, known_end, src, fixed_pos,
                                                end_fixed_pos, pos, end_pos);
      }

      template <bool Unpack, bool Verify, bool SkipOffsetCheck = false>
      [[nodiscard]] static bool embedded_unpack(std::optional<T>* value,
                                                bool&             has_unknown,
                                                bool&             known_pos,
                                                const char*       src,
                                                uint32_t&         fixed_pos,
                                                uint32_t          end_fixed_pos,
                                                uint32_t&         heap_pos,
                                                uint32_t          end_heap_pos)
      {
         uint32_t                   orig_pos = fixed_pos;
         typename Format::size_type offset;
         if (!unpack_numeric<Verify, typename Format::size_type, SkipOffsetCheck>(
                 &offset, src, fixed_pos, end_fixed_pos))
            return false;
         if (offset == 1)
         {
            if constexpr (Unpack)
            {
               *value = std::nullopt;
            }
            return true;
         }
         fixed_pos = orig_pos;
         if constexpr (Unpack)
         {
            value->emplace();
         }
         // Inner's embedded_variable_unpack will re-read the offset from
         // fixed_pos; same slot, still pre-validated if SkipOffsetCheck.
         return is_packable<T, Format>::template embedded_variable_unpack<Unpack, Verify,
                                                                           SkipOffsetCheck>(
             Unpack ? &**value : nullptr, has_unknown, known_pos, src, fixed_pos, end_fixed_pos,
             heap_pos, end_heap_pos);
      }
   };  // is_packable<std::optional<T>>

   template <typename Format = frac_format_32>
   inline bool verify_extensions(const char* src,
                                 bool        known_pos,
                                 bool&       last_has_value,
                                 uint32_t    fixed_pos,
                                 uint32_t    end_fixed_pos,
                                 uint32_t&   heap_pos,
                                 uint32_t    end_heap_pos)
   {
      if ((end_fixed_pos - fixed_pos) % Format::size_bytes != 0)
         return false;
      while (fixed_pos < end_fixed_pos)
      {
         typename Format::size_type offset;
         auto                       base = fixed_pos;
         if (!unpack_numeric<false>(&offset, src, fixed_pos, end_fixed_pos))
            return false;
         last_has_value = offset != 1;
         if (offset >= 2)
         {
            if (offset < Format::size_bytes)
               return false;
            if (known_pos && offset + base != heap_pos)
               return false;
            if (offset > end_heap_pos - base)
               return false;
            if (offset < heap_pos - base)
               return false;
            heap_pos  = base + offset;
            known_pos = false;
         }
      }
      return true;
   }

   namespace detail
   {
      template <bool DefWillNotChange, typename Format = frac_format_32>
      struct num_present_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            ++i;
            if constexpr (is_packable<T, Format>::is_optional)
            {
               if (is_packable<T, Format>::has_value(member))
                  num_present = i;
            }
            else
            {
               num_present = i;
            }
         }
         int i           = 0;
         int num_present = 0;
      };
      template <typename Format>
      struct num_present_fn<true, Format>
      {
         template <typename T>
         void operator()(const T&)
         {
            ++num_present;
         }
         int num_present = 0;
      };

      template <typename Format = frac_format_32>
      struct fixed_size_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            if (i < num_present)
               fixed_size += is_packable<T, Format>::fixed_size;
            ++i;
         }
         int           num_present;
         int           i          = 0;
         std::uint32_t fixed_size = 0;  // u32 covers PSIO1_FRAC_MAX_FIXED_SIZE commitments
      };

      template <typename S, typename Format = frac_format_32>
      struct embedded_fixed_pack_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            if (i < num_present)
               is_packable<T, Format>::embedded_fixed_pack(member, stream);
            ++i;
         }
         int num_present;
         S&  stream;
         int i = 0;
      };

      template <typename S, typename Format = frac_format_32>
      struct embedded_variable_pack_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            if (i < num_present)
            {
               using is_p = is_packable<T, Format>;
               is_p::embedded_fixed_repack(member, fixed_pos, stream.written(), stream);
               is_p::embedded_variable_pack(member, stream);
               fixed_pos += is_p::fixed_size;
            }
            ++i;
         }
         int           num_present;
         std::uint32_t fixed_pos;
         S&            stream;
         int           i = 0;
      };

      template <typename S, typename Format = frac_format_32>
      struct pack_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            is_packable<T, Format>::pack(member, stream);
         }
         S& stream;
      };

      // Branchless pack functors — all members present, no i < num_present guard
      template <typename S, typename Format = frac_format_32>
      struct embedded_fixed_pack_all_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            is_packable<T, Format>::embedded_fixed_pack(member, stream);
         }
         S& stream;
      };

      template <typename S, typename Format = frac_format_32>
      struct embedded_variable_pack_all_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            using is_p = is_packable<T, Format>;
            is_p::embedded_fixed_repack(member, fixed_pos, stream.written(), stream);
            is_p::embedded_variable_pack(member, stream);
            fixed_pos += is_p::fixed_size;
         }
         std::uint32_t fixed_pos;
         S&            stream;
      };

      // Merged num_present + fixed_size in a single iteration
      template <typename Format = frac_format_32>
      struct num_present_and_size_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            tentative_fixed += is_packable<T, Format>::fixed_size;
            ++i;
            if constexpr (is_packable<T, Format>::is_optional)
            {
               if (is_packable<T, Format>::has_value(member))
               {
                  num_present = i;
                  fixed_size  = tentative_fixed;
               }
            }
            else
            {
               num_present = i;
               fixed_size  = tentative_fixed;
            }
         }
         std::uint32_t tentative_fixed = 0;  // u32 covers PSIO1_FRAC_MAX_FIXED_SIZE commitments
         std::uint32_t fixed_size      = 0;
         int           num_present     = 0;
         int           i               = 0;
      };

      // Single-pass packed size computation for definitionWillNotChange structs
      template <bool DefWillNotChange, typename Format = frac_format_32>
      struct packed_size_fn
      {
         template <typename T>
         void operator()(const T& member)
         {
            tentative_size += is_packable<T, Format>::fixed_size;
            tentative_size += is_packable<T, Format>::embedded_variable_packed_size(member);
            if constexpr (is_packable<T, Format>::is_optional)
            {
               if (is_packable<T, Format>::has_value(member))
                  committed_size = tentative_size;
            }
            else
            {
               committed_size = tentative_size;
            }
         }
         uint32_t tentative_size = 0;
         uint32_t committed_size = 0;
      };

      template <typename Format>
      struct packed_size_fn<true, Format>
      {
         template <typename T>
         void operator()(const T& member)
         {
            total_size += is_packable<T, Format>::fixed_size;
            total_size += is_packable<T, Format>::embedded_variable_packed_size(member);
         }
         uint32_t total_size = 0;
      };

   }  // namespace detail

   template <typename Format, RefPackable... Ts>
   struct is_packable<std::tuple<Ts...>, Format>
       : base_packable_impl<std::tuple<Ts...>, is_packable<std::tuple<Ts...>, Format>, Format>
   {
      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = false;

      template <typename S>
      static void pack(const std::tuple<Ts...>& value, S& stream)
      {
         // TODO: verify fixed_size doesn't overflow
         detail::num_present_fn<false, Format> num_present_fn;
         tuple_foreach(  //
             value, num_present_fn);
         auto                          num_present = num_present_fn.num_present;
         detail::fixed_size_fn<Format> fixed_size_fn{num_present};
         tuple_foreach(  //
             value, fixed_size_fn);
         auto fixed_size = fixed_size_fn.fixed_size;
         is_packable<uint16_t, Format>::pack(fixed_size, stream);
         uint32_t fixed_pos = stream.written();
         tuple_foreach(  //
             value, detail::embedded_fixed_pack_fn<S, Format>{num_present, stream});
         tuple_foreach(  //
             value, detail::embedded_variable_pack_fn<S, Format>{num_present, fixed_pos, stream});
      }  // pack

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(std::tuple<Ts...>* value,
                                       bool&              has_unknown,
                                       bool&              known_end,
                                       const char*        src,
                                       uint32_t&          pos,
                                       uint32_t           end_pos)
      {
         uint16_t fixed_size;
         if (!unpack_numeric<Verify>(&fixed_size, src, pos, end_pos))
            return false;
         uint32_t fixed_pos     = pos;
         uint32_t heap_pos      = pos + fixed_size;
         uint32_t end_fixed_pos = heap_pos;
         if constexpr (Verify)
         {
            known_end = true;
            if (heap_pos < pos || heap_pos > end_pos)
               return false;
         }
         bool ok             = true;
         bool last_has_value = true;
         if constexpr (Unpack)
         {
            tuple_foreach(  //
                *value,
                [&](auto& x)
                {
                   using is_p = is_packable<std::remove_cvref_t<decltype(x)>, Format>;
                   if (fixed_pos < end_fixed_pos || !is_p::is_optional)
                   {
                      ok &= is_p::template embedded_unpack<Unpack, Verify>(
                          &x, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
                          end_pos);
                      if constexpr (Verify)
                      {
                         if constexpr (is_p::is_optional)
                         {
                            last_has_value = is_p::has_value(x);
                         }
                         else
                         {
                            last_has_value = true;
                         }
                      }
                   }
                });
         }
         else
         {
            tuple_foreach_type(  //
                (std::tuple<Ts...>*)nullptr,
                [&](auto* p)
                {
                   using is_p = is_packable<std::remove_cvref_t<decltype(*p)>, Format>;
                   if (fixed_pos < end_fixed_pos || !is_p::is_optional)
                   {
                      if constexpr (Verify)
                      {
                         if constexpr (is_p::is_optional)
                         {
                            last_has_value =
                                is_p::template has_value<Verify>(src, fixed_pos, end_fixed_pos);
                         }
                         else
                         {
                            last_has_value = true;
                         }
                      }
                      ok &= is_p::template embedded_unpack<Unpack, Verify>(
                          nullptr, has_unknown, known_end, src, fixed_pos, end_fixed_pos, heap_pos,
                          end_pos);
                   }
                });
         }
         if (!ok)
            return false;
         if constexpr (Verify)
         {
            if (fixed_pos < end_fixed_pos)
            {
               if (!verify_extensions<Format>(src, known_end, last_has_value, fixed_pos,
                                               end_fixed_pos, heap_pos, end_pos))
                  return false;
               has_unknown = true;
               known_end   = false;
            }
            if (!last_has_value)
               return false;
         }
         pos = heap_pos;
         return true;
      }  // unpack
   };  // is_packable<std::tuple<Ts...>>

   template <bool Unpack, bool Verify, size_t I, typename Format, typename... Ts>
   [[nodiscard]] bool unpack_variant_impl(size_t               tag,
                                          std::variant<Ts...>* value,
                                          bool&                has_unknown,
                                          bool&                end_known,
                                          const char*          src,
                                          uint32_t&            pos,
                                          uint32_t             end_pos)
   {
      if constexpr (I < sizeof...(Ts))
      {
         using is_p = is_packable<std::variant_alternative_t<I, std::variant<Ts...>>, Format>;
         if (tag == I)
         {
            if constexpr (Unpack)
            {
               value->template emplace<I>();
               return is_p::template unpack<Unpack, Verify>(&std::get<I>(*value), has_unknown,
                                                            end_known, src, pos, end_pos);
            }
            else
            {
               return is_p::template unpack<Unpack, Verify>(nullptr, has_unknown, end_known, src,
                                                            pos, end_pos);
            }
         }
         else
         {
            return unpack_variant_impl<Unpack, Verify, I + 1, Format>(
                tag, value, has_unknown, end_known, src, pos, end_pos);
         }
      }
      else
      {
         return false;
      }
   }

   template <typename Format, Packable... Ts>
   struct is_packable<std::variant<Ts...>, Format>
       : base_packable_impl<std::variant<Ts...>, is_packable<std::variant<Ts...>, Format>, Format>
   {
      static_assert(sizeof...(Ts) < 128);

      static constexpr uint32_t fixed_size        = Format::size_bytes;
      static constexpr bool     is_variable_size  = true;
      static constexpr bool     is_optional       = false;
      static constexpr bool     supports_0_offset = false;

      template <typename S>
      static void pack(const std::variant<Ts...>& value, S& stream)
      {
         is_packable<uint8_t, Format>::pack(value.index(), stream);
         uint32_t size_pos = stream.written();
         is_packable<typename Format::size_type, Format>::pack(0, stream);
         uint32_t content_pos = stream.written();
         std::visit(
             [&](const auto& x)
             { is_packable<std::remove_cvref_t<decltype(x)>, Format>::pack(x, stream); },
             value);
         stream.rewrite_raw(
             size_pos,
             static_cast<typename Format::size_type>(stream.written() - content_pos));
      }

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(std::variant<Ts...>* value,
                                       bool&                has_unknown,
                                       bool&                known_end,
                                       const char*          src,
                                       uint32_t&            pos,
                                       uint32_t             end_pos)
      {
         uint8_t tag;
         if (!unpack_numeric<Verify>(&tag, src, pos, end_pos))
            return false;
         if constexpr (Verify)
            if (tag & 0x80)
               return false;
         typename Format::size_type size;
         if (!unpack_numeric<Verify>(&size, src, pos, end_pos))
            return false;
         uint32_t content_pos = pos;
         uint32_t content_end = pos + size;
         if constexpr (Verify)
            if (content_end < content_pos || content_end > end_pos)
               return false;
         bool inner_known_end;
         if (!unpack_variant_impl<Unpack, Verify, 0, Format>(tag, value, has_unknown,
                                                              inner_known_end, src, content_pos,
                                                              content_end))
            return false;
         if constexpr (Verify)
         {
            known_end = true;
            if (inner_known_end && content_pos != content_end)
               return false;
         }
         pos = content_end;
         return true;
      }
   };  // is_packable<std::variant<Ts...>>

   template <typename T, typename Format>
   struct is_packable_reflected<T, true, Format>
       : base_packable_impl<T, is_packable<T, Format>, Format>
   {
      static constexpr uint32_t get_members_fixed_size()
      {
         return psio1::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... member)
             {
                return (0 + ... +
                        is_packable<std::remove_cvref_t<decltype(std::declval<T>().*member)>,
                                     Format>::fixed_size);
             });
      }

      static constexpr bool get_is_var_size()
      {
         if constexpr (!reflect<T>::definitionWillNotChange)
            return true;
         else
            return psio1::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [](auto... member)
                {
                   return (false || ... ||
                           is_packable<std::remove_cvref_t<decltype(std::declval<T>().*member)>,
                                        Format>::is_variable_size);
                });
      }

      // True if the last field is optional (trailing optionals may be trimmed)
      static constexpr bool get_has_trailing_optionals()
      {
         return psio1::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... member)
             {
                if constexpr (sizeof...(member) == 0)
                   return false;
                else
                {
                   // Array of is_optional for each member; check the last one
                   constexpr bool opts[] = {
                       is_packable<std::remove_cvref_t<decltype(std::declval<T>().*member)>,
                                    Format>::is_optional...};
                   return opts[sizeof...(member) - 1];
                }
             });
      }

      static constexpr uint32_t members_fixed_size    = get_members_fixed_size();
      static constexpr bool     is_variable_size      = get_is_var_size();
      static constexpr uint32_t fixed_size =
          is_variable_size ? Format::size_bytes : members_fixed_size;
      static constexpr bool is_optional            = false;
      static constexpr bool supports_0_offset      = false;
      static constexpr bool has_trailing_optionals = get_has_trailing_optionals();
      // All members always present: either definitionWillNotChange or last field non-optional
      static constexpr bool all_members_present =
          reflect<T>::definitionWillNotChange || !has_trailing_optionals;

      // Extensible (non-DWNC) structs prepend a header with the fixed-region
      // byte count. Default header is u16 (cap 64 KiB). Types needing more
      // specialize psio1::frac_max_fixed_size<T> (or use the
      // PSIO1_FRAC_MAX_FIXED_SIZE(T, N) macro) to commit to a larger cap
      // and wider header. DWNC types emit no header — their cap is lifted
      // regardless.
      static_assert(reflect<T>::definitionWillNotChange ||
                        members_fixed_size <= frac_max_fixed_size_v<T>,
                    "Extensible struct's fixed region exceeds the declared "
                    "maximum. Either mark with definitionWillNotChange() to "
                    "remove the header entirely, or use "
                    "PSIO1_FRAC_MAX_FIXED_SIZE(T, N) to commit to a wider "
                    "header (u32 / u64).");

      // Header wire type — u16 default; upgrades to u32/u64 per commitment.
      using header_type =
          std::conditional_t<reflect<T>::definitionWillNotChange,
                             std::uint16_t,  // unused in DWNC path; placeholder
                             frac_header_type_t<frac_max_fixed_size_v<T>>>;

      static uint32_t packed_size(const T& value)
      {
         if constexpr (!is_variable_size)
         {
            return members_fixed_size;
         }
         else if constexpr (all_members_present)
         {
            constexpr auto header_size =
                reflect<T>::definitionWillNotChange ? std::uint32_t{0}
                                                    : std::uint32_t{sizeof(header_type)};
            return header_size +
                   psio1::for_each_member(
                       &value, (typename reflect<T>::data_members*)nullptr,
                       detail::packed_size_fn<true, Format>{})
                       .total_size;
         }
         else
         {
            return sizeof(header_type) +
                   psio1::for_each_member(&value,
                                         (typename reflect<T>::data_members*)nullptr,
                                         detail::packed_size_fn<false, Format>{})
                       .committed_size;
         }
      }

      static uint32_t embedded_variable_packed_size(const T& value)
      {
         if constexpr (is_variable_size)
            return packed_size(value);
         return 0;
      }

      template <typename S>
      static void pack(const T& value, S& stream)
      {
         if constexpr (!is_variable_size)
         {
            if constexpr (run_detail::has_batchable_run<T>())
            {
               // Fully-fixed struct with at least one batchable run: walk
               // members with compile-time run detection.
               auto op = [&](auto const& member)
               {
                  using FT = std::remove_cvref_t<decltype(member)>;
                  is_packable<FT, Format>::pack(member, stream);
               };
               constexpr auto N = std::tuple_size_v<struct_tuple_t<T>>;
               run_detail::walk_with_runs(value, stream, static_cast<int>(N), op);
            }
            else
            {
               // No batchable runs — avoid walker overhead, use the simple
               // per-member path.
               psio1::for_each_member(&value, (typename reflect<T>::data_members*)nullptr,
                                     detail::pack_fn<S, Format>{stream});
            }
         }
         else if constexpr (all_members_present)
         {
            // num_present = N and fixed_size = members_fixed_size are compile-time constants.
            // Skip num_present scan and fixed_size computation entirely.
            // 2 iterations instead of 4.
            if constexpr (!reflect<T>::definitionWillNotChange)
               is_packable<header_type, Format>::pack(header_type(members_fixed_size), stream);
            uint32_t fixed_pos = stream.written();
            if constexpr (run_detail::has_batchable_run<T>())
            {
               // Fixed-region walk with run batching.
               auto op = [&](auto const& member)
               {
                  using FT = std::remove_cvref_t<decltype(member)>;
                  is_packable<FT, Format>::embedded_fixed_pack(member, stream);
               };
               constexpr auto N = std::tuple_size_v<struct_tuple_t<T>>;
               run_detail::walk_with_runs(value, stream, static_cast<int>(N), op);
            }
            else
            {
               psio1::for_each_member(&value, (typename reflect<T>::data_members*)nullptr,
                                     detail::embedded_fixed_pack_all_fn<S, Format>{stream});
            }
            psio1::for_each_member(
                &value, (typename reflect<T>::data_members*)nullptr,
                detail::embedded_variable_pack_all_fn<S, Format>{fixed_pos, stream});
         }
         else
         {
            // Last field is optional — need runtime num_present scan.
            // Merge num_present + fixed_size into single iteration.
            // 3 iterations instead of 4.
            auto result =
                psio1::for_each_member(&value, (typename reflect<T>::data_members*)nullptr,
                                      detail::num_present_and_size_fn<Format>{});
            is_packable<header_type, Format>::pack(header_type(result.fixed_size), stream);
            uint32_t fixed_pos = stream.written();
            psio1::for_each_member(
                &value, (typename reflect<T>::data_members*)nullptr,
                detail::embedded_fixed_pack_fn<S, Format>{result.num_present, stream});
            psio1::for_each_member(
                &value, (typename reflect<T>::data_members*)nullptr,
                detail::embedded_variable_pack_fn<S, Format>{result.num_present, fixed_pos,
                                                              stream});
         }
      }  // pack

      template <bool Unpack, bool Verify>
      [[nodiscard]] static bool unpack(T*          value,
                                       bool&       has_unknown,
                                       bool&       known_end,
                                       const char* src,
                                       uint32_t&   pos,
                                       uint32_t    end_pos)
      {
         if constexpr (is_variable_size)
         {
            // DWNC types have no wire header — their fixed region is
            // members_fixed_size by construction. Extensible types read a
            // header whose width matches header_type (u16 default, u32/u64
            // if PSIO1_FRAC_MAX_FIXED_SIZE committed to a wider type).
            // fracpack's absolute buffer address space is u32 (pos is u32),
            // so the value must fit in u32 regardless of the header width;
            // a u64 header with the high bits set is a corrupt buffer.
            std::uint32_t fixed_size = 0;
            if constexpr (reflect<T>::definitionWillNotChange)
               fixed_size = members_fixed_size;
            else
            {
               header_type hdr = 0;
               if (!unpack_numeric<Verify>(&hdr, src, pos, end_pos))
                  return false;
               if constexpr (sizeof(header_type) > 4)
               {
                  if constexpr (Verify)
                     if (hdr > 0xffffffffull)
                        return false;
               }
               fixed_size = static_cast<std::uint32_t>(hdr);
            }
            uint32_t fixed_pos     = pos;
            uint32_t heap_pos      = pos + fixed_size;
            uint32_t end_fixed_pos = heap_pos;
            if constexpr (Verify)
            {
               if (heap_pos < pos || heap_pos > end_pos)
                  return false;
               known_end = true;
            }

            if constexpr (all_members_present)
            {
               // Fast path: no trailing-optional trimming possible, so wire's
               // fixed_size must be >= members_fixed_size (equal = exact schema
               // match; greater = extensions to verify at tail).
               // The single struct-entry bounds check above already covered
               // the whole fixed region — per-field bounds checks on fixed
               // fields inside the walker are redundant and get skipped here.
               if constexpr (Verify && !reflect<T>::definitionWillNotChange)
               {
                  if (fixed_size < members_fixed_size)
                     return false;
               }
               bool ok = true;
               psio1::for_each_member_ptr<!Unpack>(
                   value, (typename reflect<T>::data_members*)nullptr,
                   [&](auto* member)
                   {
                      using FieldT = std::remove_cvref_t<decltype(*member)>;
                      using is_p   = is_packable<FieldT, Format>;
                      if constexpr (is_p::is_variable_size)
                      {
                         // Variable field: embedded_unpack dispatches to the
                         // type-specific handler. SkipOffsetCheck=true tells
                         // it the offset slot is inside the pre-validated
                         // fixed region (no re-check needed).
                         ok &= is_p::template embedded_unpack<Unpack, Verify,
                                                                /*SkipOffsetCheck=*/true>(
                             member, has_unknown, known_end, src, fixed_pos, end_fixed_pos,
                             heap_pos, end_pos);
                      }
                      else if constexpr (is_packable_memcpy<FieldT>::value)
                      {
                         // Fixed primitive/enum/memcpy-eligible array: bytewise copy.
                         // No per-field bounds check — region was pre-validated
                         // at struct entry. Compiler coalesces consecutive adds.
                         if constexpr (Unpack)
                            std::memcpy(member, src + fixed_pos, is_p::fixed_size);
                         fixed_pos += is_p::fixed_size;
                      }
                      else
                      {
                         // Fixed but not bytewise-copyable (bool, nested fixed
                         // reflected struct): defer to the field's own unpack
                         // which knows how to validate/assemble its layout.
                         ok &= is_p::template unpack<Unpack, Verify>(
                             member, has_unknown, known_end, src, fixed_pos, end_fixed_pos);
                      }
                   });
               if (!ok)
                  return false;
               if constexpr (Verify)
               {
                  if (fixed_pos < end_fixed_pos)
                  {
                     bool last_has_value_dummy = true;  // no trailing optionals here
                     if (!verify_extensions<Format>(src, known_end, last_has_value_dummy,
                                                     fixed_pos, end_fixed_pos, heap_pos, end_pos))
                        return false;
                     has_unknown = true;
                     known_end   = false;
                  }
               }
               pos = heap_pos;
               return true;
            }
            else
            {
               // Slow path: has_trailing_optionals, wire may have trimmed some
               // trailing optionals — per-field gating needed.
               bool ok             = true;
               bool last_has_value = true;
               psio1::for_each_member_ptr<!Unpack>(
                   value, (typename reflect<T>::data_members*)nullptr,
                   [&](auto* member)
                   {
                      using is_p =
                          is_packable<std::remove_cvref_t<decltype(*member)>, Format>;
                      if (fixed_pos < end_fixed_pos || !is_p::is_optional)
                      {
                         if constexpr (Verify)
                         {
                            if constexpr (is_p::is_optional &&
                                          !reflect<T>::definitionWillNotChange)
                            {
                               last_has_value =
                                   is_p::template has_value<Verify>(src, fixed_pos,
                                                                    end_fixed_pos);
                            }
                            else
                            {
                               last_has_value = true;
                            }
                         }
                         ok &= is_p::template embedded_unpack<Unpack, Verify>(
                             member, has_unknown, known_end, src, fixed_pos, end_fixed_pos,
                             heap_pos, end_pos);
                      }
                   });
               if (!ok)
                  return false;
               if constexpr (Verify)
               {
                  if (fixed_pos < end_fixed_pos)
                  {
                     if (!verify_extensions<Format>(src, known_end, last_has_value, fixed_pos,
                                                     end_fixed_pos, heap_pos, end_pos))
                        return false;
                     has_unknown = true;
                     known_end   = false;
                  }
                  if (!last_has_value)
                     return false;
               }
               pos = heap_pos;
               return true;
            }
         }  // is_variable_size
         else
         {
            // Fully fixed-size struct: one bounds check covers the whole struct,
            // then per-field logic without re-checking bounds.
            if constexpr (Verify)
            {
               known_end = true;
               if (end_pos - pos < members_fixed_size)
                  return false;
            }
            bool ok = true;
            psio1::for_each_member_ptr<!Unpack>(
                value, (typename reflect<T>::data_members*)nullptr,
                [&](auto* member)
                {
                   using FieldT = std::remove_cvref_t<decltype(*member)>;
                   using is_p   = is_packable<FieldT, Format>;
                   if constexpr (is_packable_memcpy<FieldT>::value)
                   {
                      if constexpr (Unpack)
                         std::memcpy(member, src + pos, is_p::fixed_size);
                      pos += is_p::fixed_size;
                   }
                   else
                   {
                      ok &= is_p::template unpack<Unpack, Verify>(
                          member, has_unknown, known_end, src, pos, end_pos);
                   }
                });
            return ok;
         }
      }  // unpack
   };  // is_packable_reflected

   template <Packable T, typename Format = frac_format_32, typename S>
   void to_frac(const T& value, S& stream)
   {
      psio1::is_packable<T, Format>::pack(value, stream);
   }

   template <Packable T, typename Format = frac_format_32>
   std::uint32_t fracpack_size(const T& value)
   {
      return psio1::is_packable<T, Format>::packed_size(value);
   }

   template <Packable T, typename Format = frac_format_32>
   std::vector<char> to_frac(const T& value)
   {
      auto                   sz = fracpack_size<T, Format>(value);
      std::vector<char>      result(sz);
      psio1::fixed_buf_stream fbs(result.data(), sz);
      psio1::to_frac<T, Format>(value, fbs);
      return result;
   }

   template <Packable T>
   auto convert_to_frac(const T& value)
   {
      return to_frac(value);
   }

   // frac16 convenience aliases
   template <Packable T, typename S>
   void to_frac16(const T& value, S& stream)
   {
      to_frac<T, frac_format_16>(value, stream);
   }

   template <Packable T>
   std::vector<char> to_frac16(const T& value)
   {
      return to_frac<T, frac_format_16>(value);
   }

   enum validation_t : std::uint8_t
   {
      invalid,
      valid,
      extended,
   };
   template <Packable T, typename Format = frac_format_32>
   validation_t validate_frac(std::span<const char> data)
   {
      bool          has_unknown = false;
      bool          known_end;
      std::uint32_t pos = 0;
      if (!is_packable<T, Format>::template unpack<false, true>(nullptr, has_unknown, known_end,
                                                                 data.data(), pos, data.size()))
         return validation_t::invalid;
      if (known_end && pos != data.size())
         return validation_t::invalid;
      return has_unknown ? validation_t::extended : validation_t::valid;
   }
   template <Packable T, typename Format = frac_format_32>
   bool validate_frac_compatible(std::span<const char> data)
   {
      return validate_frac<T, Format>(data) != validation_t::invalid;
   }
   template <Packable T, typename Format = frac_format_32>
   bool validate_frac_strict(std::span<const char> data)
   {
      return validate_frac<T, Format>(data) == validation_t::valid;
   }

   template <Packable T>
   validation_t validate_frac16(std::span<const char> data)
   {
      return validate_frac<T, frac_format_16>(data);
   }

   template <typename T>
   struct prevalidated
   {
      template <typename... A>
      explicit constexpr prevalidated(A&&... a) : data(std::forward<A>(a)...)
      {
      }
      T data;
   };
   template <typename T>
   prevalidated(T&) -> prevalidated<T&>;
   template <typename T>
   prevalidated(T*&) -> prevalidated<T*>;
   template <typename T>
   prevalidated(T* const&) -> prevalidated<T*>;
   template <typename T>
   prevalidated(T&&) -> prevalidated<T>;
   template <typename T, typename U>
   prevalidated(T&& t, U&& u)
       -> prevalidated<decltype(std::span{std::forward<T>(t), std::forward<U>(u)})>;
   struct input_stream;
   prevalidated(input_stream) -> prevalidated<std::span<const char>>;

   template <bool Unpack, bool Pointer, typename T, typename Format>
   bool user_validate(T* value, const char* src, std::uint32_t orig_pos)
   {
      if constexpr (Unpack && PackableValidatedObject<T>)
      {
         return psio_validate_packable(*value);
      }
      else if constexpr (!Pointer)
      {
         return psio_validate_packable(frac_validation_view<const T>{prevalidated{src + orig_pos}});
      }
      else
      {
         typename Format::size_type offset;
         std::uint32_t              tmp = orig_pos;
         (void)unpack_numeric<false>(&offset, src, tmp, tmp + Format::size_bytes);
         return psio_validate_packable(
             frac_validation_view<const T>{prevalidated{src + orig_pos + offset}});
      }
   }

   template <Packable T, typename Format = frac_format_32>
   bool from_frac(T& value, std::span<const char> data)
   {
      bool          has_unknown = false;
      bool          known_end;
      std::uint32_t pos = 0;
      if (!is_packable<T, Format>::template unpack<true, true>(&value, has_unknown, known_end,
                                                                data.data(), pos, data.size()))
         return false;
      if (known_end && pos != data.size())
         return false;
      return true;
   }

   template <Packable T, typename Format = frac_format_32>
   T from_frac(std::span<const char> data)
   {
      T result;
      if (!from_frac<T, Format>(result, data))
         abort_error(stream_error::invalid_frac_encoding);
      return result;
   }

   template <Packable T, typename Format = frac_format_32>
   bool from_frac_strict(T& value, std::span<const char> data)
   {
      bool          has_unknown = false;
      bool          known_end;
      std::uint32_t pos = 0;
      if (!is_packable<T, Format>::template unpack<true, true>(&value, has_unknown, known_end,
                                                                data.data(), pos, data.size()))
         return false;
      if (has_unknown)
         return false;
      if (known_end && pos != data.size())
         return false;
      return true;
   }

   template <Packable T, typename Format = frac_format_32>
   T from_frac_strict(std::span<const char> data)
   {
      T result;
      if (!from_frac_strict<T, Format>(result, data))
         abort_error(stream_error::invalid_frac_encoding);
      return result;
   }

   template <Packable T, typename Format = frac_format_32, typename S>
   T from_frac(const prevalidated<S>& data)
   {
      bool                  has_unknown = false;
      bool                  known_end;
      std::uint32_t         pos = 0;
      T                     result;
      std::span<const char> actual(data.data.data(), data.data.size());
      (void)is_packable<T, Format>::template unpack<true, false>(&result, has_unknown, known_end,
                                                                  actual.data(), pos,
                                                                  actual.size());
      return result;
   }

   // frac16 convenience aliases
   template <Packable T>
   bool from_frac16(T& value, std::span<const char> data)
   {
      return from_frac<T, frac_format_16>(value, data);
   }

   template <Packable T>
   T from_frac16(std::span<const char> data)
   {
      return from_frac<T, frac_format_16>(data);
   }

   template <typename T, typename U>
   constexpr bool packable_as_impl = std::is_same_v<T, U>;

   template <typename T, typename U>
   concept PackableAs = std::is_same_v<T, U> || packable_as_impl<std::remove_cvref_t<T>, U>;

   template <typename T, typename U>
   constexpr bool packable_as_impl<std::span<T>, std::vector<U>> = PackableAs<T, U>;

   template <typename... T, typename... U>
      requires(sizeof...(T) == sizeof...(U))
   constexpr bool packable_as_impl<std::tuple<T...>, std::tuple<U...>> = (PackableAs<T, U> && ...);

   template <typename T, typename U>
      requires Reflected<T> && Reflected<U>
   constexpr bool packable_as_impl<T, U> = PackableAs<struct_tuple_t<T>, struct_tuple_t<U>>;

}  // namespace psio1
