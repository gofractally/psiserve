// frac_ref: zero-copy mutable views over fracpack-encoded data
//
// Capability tiers, deduced from buffer type:
//   span<const char>  → read-only
//   span<char>        → read + fixed-size field overwrites
//   vector<char>      → read + full mutation (splice/realloc)

#pragma once

#include <psio/fracpack.hpp>
#include <psio/view.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <flat_map>
#include <flat_set>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace psio
{
   // ── Buffer traits: deduce capabilities from buffer type ────────────────────

   template <typename Buffer>
   struct buf_traits
   {
     private:
      static constexpr bool check_const()
      {
         if constexpr (requires(Buffer& b) { b.data(); })
            return std::is_const_v<std::remove_pointer_t<decltype(std::declval<Buffer>().data())>>;
         else
            return true;
      }

     public:
      static constexpr bool is_const   = check_const();
      static constexpr bool can_resize = requires(Buffer& b) { b.resize(size_t{}); };

      static constexpr bool can_write_fixed = !is_const;
      static constexpr bool can_splice      = can_write_fixed && can_resize;
   };

   // ── Compile-time layout info for a reflected struct ────────────────────────

   template <typename T, typename Format = frac_format_32>
      requires Reflected<T>
   struct frac_layout
   {
      static constexpr bool     has_header = !reflect<T>::definitionWillNotChange;
      static constexpr uint32_t hdr_size   = has_header ? 2 : 0;

      static consteval auto member_fixed_sizes()
      {
         return apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... members)
             {
                return std::array<uint32_t, sizeof...(members)>{
                    is_packable<std::remove_cvref_t<decltype(std::declval<T>().*members)>,
                                 Format>::fixed_size...};
             });
      }

      static consteval auto member_is_variable()
      {
         return apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... members)
             {
                return std::array<bool, sizeof...(members)>{
                    is_packable<std::remove_cvref_t<decltype(std::declval<T>().*members)>,
                                 Format>::is_variable_size...};
             });
      }

      static constexpr auto   fixed_sizes = member_fixed_sizes();
      static constexpr auto   is_variable = member_is_variable();
      static constexpr size_t num_members = fixed_sizes.size();

      static consteval uint32_t offset_of(size_t index)
      {
         uint32_t off = 0;
         for (size_t i = 0; i < index; ++i)
            off += fixed_sizes[i];
         return off;
      }

      // Precomputed offsets array for use in runtime loops
      static consteval auto compute_offsets()
      {
         std::array<uint32_t, num_members> o{};
         uint32_t                          off = 0;
         for (size_t i = 0; i < num_members; ++i)
         {
            o[i] = off;
            off += fixed_sizes[i];
         }
         return o;
      }
      static constexpr auto offsets = compute_offsets();

      static constexpr uint32_t total_fixed =
          is_packable_reflected<T, true, Format>::members_fixed_size;

      // SIMD fast-path: all members are same-width fixed slots matching the
      // format's size_bytes. At frac32 this is the familiar "all u32" shape.
      static consteval bool all_slot_aligned()
      {
         for (size_t i = 0; i < num_members; ++i)
            if (fixed_sizes[i] != Format::size_bytes)
               return false;
         return true;
      }

      static constexpr bool simd_eligible = all_slot_aligned();
   };

   // ── Type-level path utilities ──────────────────────────────────────────────

   template <typename T, size_t I>
   using nth_field_t = std::tuple_element_t<I, struct_tuple_t<T>>;

   template <typename Root, size_t... Path>
   struct path_parent;

   template <typename Root, size_t Only>
   struct path_parent<Root, Only>
   {
      using type = Root;
   };

   template <typename Root, size_t Head, size_t... Tail>
      requires(sizeof...(Tail) > 0)
   struct path_parent<Root, Head, Tail...>
   {
      using type = typename path_parent<nth_field_t<Root, Head>, Tail...>::type;
   };

   template <typename Root, size_t... Path>
   using path_parent_t = typename path_parent<Root, Path...>::type;

   // Extract the last index from a parameter pack
   template <size_t... Is>
   struct last_index;

   template <size_t Only>
   struct last_index<Only>
   {
      static constexpr size_t value = Only;
   };

   template <size_t Head, size_t... Tail>
      requires(sizeof...(Tail) > 0)
   struct last_index<Head, Tail...> : last_index<Tail...>
   {
   };

   // ── Low-level utilities ────────────────────────────────────────────────────

   namespace frac_detail
   {
      inline uint32_t read_u32(const char* buf, uint32_t pos)
      {
         uint32_t v;
         std::memcpy(&v, buf + pos, 4);
         return v;
      }

      inline uint16_t read_u16(const char* buf, uint32_t pos)
      {
         uint16_t v;
         std::memcpy(&v, buf + pos, 2);
         return v;
      }

      inline void write_u32(char* buf, uint32_t pos, uint32_t val)
      {
         std::memcpy(buf + pos, &val, 4);
      }

      // Format-aware offset/size reader. Returns a uint32_t for consistent
      // arithmetic regardless of whether the stored value is u16 or u32.
      template <typename Format>
      inline uint32_t read_size(const char* buf, uint32_t pos)
      {
         typename Format::size_type v;
         std::memcpy(&v, buf + pos, Format::size_bytes);
         return static_cast<uint32_t>(v);
      }

      template <typename Format>
      inline void write_size(char* buf, uint32_t pos, uint32_t val)
      {
         typename Format::size_type narrowed = static_cast<typename Format::size_type>(val);
         std::memcpy(buf + pos, &narrowed, Format::size_bytes);
      }

      inline int32_t splice_buffer(std::vector<char>& buf,
                                   uint32_t           pos,
                                   uint32_t           old_len,
                                   const char*        new_data,
                                   uint32_t           new_len)
      {
         int32_t delta = static_cast<int32_t>(new_len) - static_cast<int32_t>(old_len);
         if (delta > 0)
         {
            auto grow     = static_cast<size_t>(delta);
            auto old_size = buf.size();
            buf.resize(old_size + grow);
            std::memmove(buf.data() + pos + new_len, buf.data() + pos + old_len,
                         old_size - pos - old_len);
         }
         else if (delta < 0)
         {
            std::memmove(buf.data() + pos + new_len, buf.data() + pos + old_len,
                         buf.size() - pos - old_len);
            buf.resize(buf.size() + static_cast<size_t>(delta));
         }
         if (new_len > 0)
            std::memcpy(buf.data() + pos, new_data, new_len);
         return delta;
      }

      template <typename Format>
      inline void patch_offset(char* buf, uint32_t off_pos, uint32_t after_old, int32_t delta)
      {
         uint32_t val = read_size<Format>(buf, off_pos);
         if (val <= 1)
            return;
         uint32_t abs_target = off_pos + val;
         if (abs_target >= after_old)
            write_size<Format>(buf, off_pos,
                                static_cast<uint32_t>(static_cast<int64_t>(val) + delta));
      }

      // ── packed_size_at: measure packed data size at a buffer position ──

      template <Packable T, typename Format = frac_format_32>
      uint32_t packed_size_at(const char* buf, uint32_t pos)
      {
         if constexpr (!is_packable<T, Format>::is_variable_size)
         {
            return is_packable<T, Format>::fixed_size;
         }
         else if constexpr (std::is_same_v<T, std::string> ||
                            std::is_same_v<T, std::string_view>)
         {
            return Format::size_bytes + read_size<Format>(buf, pos);
         }
         else if constexpr (Reflected<T>)
         {
            // Variable-size struct: walk members to find heap extent
            using layout     = frac_layout<T, Format>;
            uint32_t start   = pos;

            uint16_t fixed_size;
            if constexpr (layout::has_header)
            {
               fixed_size = read_u16(buf, pos);
               pos += 2;
            }
            else
            {
               fixed_size = layout::total_fixed;
            }

            uint32_t fixed_start = pos;
            uint32_t heap_end    = pos + fixed_size;

            uint32_t fpos = fixed_start;
            apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... members)
                {
                   auto process = [&](auto member)
                   {
                      using FT = std::remove_cvref_t<decltype(std::declval<T>().*member)>;
                      if constexpr (is_packable<FT, Format>::is_variable_size)
                      {
                         if (fpos < fixed_start + fixed_size)
                         {
                            uint32_t offset = read_size<Format>(buf, fpos);
                            if (offset > 1)
                            {
                               uint32_t dstart = fpos + offset;
                               uint32_t dend =
                                   dstart + packed_size_at<FT, Format>(buf, dstart);
                               if (dend > heap_end)
                                  heap_end = dend;
                            }
                         }
                      }
                      fpos += is_packable<FT, Format>::fixed_size;
                   };
                   (process(members), ...);
                });

            return heap_end - start;
         }
         else
         {
            // Generic memcpy container: [size_type byte_count][bytes...]
            return Format::size_bytes + read_size<Format>(buf, pos);
         }
      }

      // ── Navigation ──

      struct level_info
      {
         uint32_t fixed_start;
      };

      // Navigate within struct at struct_start to field at FieldIndex.
      // Records this level's fixed_start into levels[depth].
      // Sets leaf_off_pos and leaf_data_pos.

      // Base case: single field index (leaf)
      template <typename T, typename Format, size_t FieldIdx>
      void navigate_recording(const char* buf,
                              uint32_t    struct_start,
                              level_info* levels,
                              size_t      depth,
                              uint32_t&   leaf_off_pos,
                              uint32_t&   leaf_data_pos)
      {
         using layout             = frac_layout<T, Format>;
         uint32_t fixed_start     = struct_start + layout::hdr_size;
         levels[depth].fixed_start = fixed_start;

         constexpr uint32_t rel  = layout::offset_of(FieldIdx);
         leaf_off_pos             = fixed_start + rel;

         if constexpr (layout::is_variable[FieldIdx])
         {
            uint32_t offset = read_size<Format>(buf, leaf_off_pos);
            leaf_data_pos   = leaf_off_pos + offset;
         }
         else
         {
            leaf_data_pos = leaf_off_pos;
         }
      }

      // Recursive case: follow Head, then navigate deeper
      template <typename T, typename Format, size_t HeadIdx, size_t NextIdx, size_t... RestIdx>
      void navigate_recording(const char* buf,
                              uint32_t    struct_start,
                              level_info* levels,
                              size_t      depth,
                              uint32_t&   leaf_off_pos,
                              uint32_t&   leaf_data_pos)
      {
         using layout             = frac_layout<T, Format>;
         uint32_t fixed_start     = struct_start + layout::hdr_size;
         levels[depth].fixed_start = fixed_start;

         constexpr uint32_t rel  = layout::offset_of(HeadIdx);
         uint32_t off_pos        = fixed_start + rel;
         uint32_t offset         = read_size<Format>(buf, off_pos);
         uint32_t nested_start   = off_pos + offset;

         using nested_type = nth_field_t<T, HeadIdx>;
         navigate_recording<nested_type, Format, NextIdx, RestIdx...>(
             buf, nested_start, levels, depth + 1, leaf_off_pos, leaf_data_pos);
      }

      // ── Compile-time patch info ──

      template <typename T, size_t MutatedIndex, typename Format = frac_format_32>
      struct sibling_patches
      {
         static consteval auto compute()
         {
            using layout = frac_layout<T, Format>;
            size_t count = 0;
            for (size_t i = MutatedIndex + 1; i < layout::num_members; ++i)
               if (layout::is_variable[i])
                  ++count;

            std::array<uint32_t, 32> offsets{};
            size_t                   idx = 0;
            for (size_t i = MutatedIndex + 1; i < layout::num_members; ++i)
               if (layout::is_variable[i])
                  offsets[idx++] = layout::offset_of(i);

            return std::pair{offsets, count};
         }

         static constexpr auto result  = compute();
         static constexpr auto offsets = result.first;
         static constexpr auto count   = result.second;
      };

      // Scalar patch: unrolled at compile time
      template <typename T, size_t MutatedIndex, typename Format = frac_format_32>
      void patch_level_scalar(char*    buf,
                              uint32_t fixed_start,
                              uint32_t after_old,
                              int32_t  delta)
      {
         using patches = sibling_patches<T, MutatedIndex, Format>;
         [&]<size_t... I>(std::index_sequence<I...>)
         {
            (patch_offset<Format>(buf, fixed_start + patches::offsets[I], after_old, delta), ...);
         }(std::make_index_sequence<patches::count>{});
      }

      // SIMD-friendly batch patch via compile-time mask
      template <typename T, size_t MutatedIndex, typename Format = frac_format_32>
      struct patch_mask
      {
         static consteval auto compute()
         {
            using layout = frac_layout<T, Format>;
            std::array<uint32_t, layout::num_members> m{};
            for (size_t i = MutatedIndex + 1; i < layout::num_members; ++i)
               if (layout::is_variable[i])
                  m[i] = 1;
            return m;
         }
         static constexpr auto mask = compute();
      };

      template <typename T, size_t MutatedIndex, typename Format = frac_format_32>
      void patch_level_simd(char*    buf,
                            uint32_t fixed_start,
                            int32_t  delta)
      {
         using layout                    = frac_layout<T, Format>;
         using slot_type                 = typename Format::size_type;
         constexpr auto mask             = patch_mask<T, MutatedIndex, Format>::mask;
         slot_type*     slots            = reinterpret_cast<slot_type*>(buf + fixed_start);
         slot_type      udelta           = static_cast<slot_type>(delta);

         // Auto-vectorizable loop with compile-time mask
         for (size_t i = 0; i < layout::num_members; ++i)
         {
            if (mask[i])
            {
               if (slots[i] > 1)
                  slots[i] += udelta;
            }
         }
      }

      template <typename T, size_t MutatedIndex, typename Format = frac_format_32>
      void patch_level_dispatch(char*    buf,
                                uint32_t fixed_start,
                                uint32_t after_old,
                                int32_t  delta)
      {
         if constexpr (frac_layout<T, Format>::simd_eligible)
            patch_level_simd<T, MutatedIndex, Format>(buf, fixed_start, delta);
         else
            patch_level_scalar<T, MutatedIndex, Format>(buf, fixed_start, after_old, delta);
      }

      // ── Multi-level patching: recursive from bottom up ──

      // Base case: single index
      template <size_t Depth, typename T, typename Format, size_t Idx>
      void patch_all(char*            buf,
                     const level_info* levels,
                     uint32_t         after_old,
                     int32_t          delta)
      {
         patch_level_dispatch<T, Idx, Format>(buf, levels[Depth].fixed_start, after_old, delta);
      }

      // Recursive: patch deeper levels first, then this level
      template <size_t Depth, typename T, typename Format, size_t Head, size_t Next, size_t... Rest>
      void patch_all(char*            buf,
                     const level_info* levels,
                     uint32_t         after_old,
                     int32_t          delta)
      {
         using nested = nth_field_t<T, Head>;
         patch_all<Depth + 1, nested, Format, Next, Rest...>(buf, levels, after_old, delta);
         patch_level_dispatch<T, Head, Format>(buf, levels[Depth].fixed_start, after_old, delta);
      }

   }  // namespace frac_detail

   // ── Forward declarations for proxy types ───────────────────────────────────

   template <typename Root, typename Buffer, typename Format, size_t... Path>
   class frac_proxy_obj;

   template <typename Root, typename FieldType, typename Buffer, typename Format, size_t... Path>
   class field_handle;

   // ── field_handle: read/write handle for a leaf field ───────────────────────

   template <typename Root, typename FieldType, typename Buffer, typename Format, size_t... Path>
   class field_handle
   {
      Buffer* buf_;

      static constexpr size_t depth    = sizeof...(Path);
      static constexpr auto   path_arr = std::array<size_t, depth>{Path...};
      static constexpr size_t leaf_idx = last_index<Path...>::value;
      static constexpr bool   leaf_is_var = is_packable<FieldType, Format>::is_variable_size;

      // Parent type (the struct containing the leaf field)
      using parent_type = path_parent_t<Root, Path...>;

      const char* buf_data() const
      {
         if constexpr (requires { buf_->data(); })
            return buf_->data();
         else
            return nullptr;
      }

      char* mut_data()
      {
         if constexpr (buf_traits<Buffer>::can_write_fixed)
            return buf_->data();
         else
            return nullptr;
      }

     public:
      explicit field_handle(Buffer* b) : buf_(b) {}

      // ── Read: always available ──

      FieldType get() const
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Format, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         if constexpr (leaf_is_var)
         {
            uint32_t psize =
                frac_detail::packed_size_at<FieldType, Format>(data, leaf_data_pos);
            FieldType result;
            from_frac<FieldType, Format>(result,
                                          std::span<const char>(data + leaf_data_pos, psize));
            return result;
         }
         else
         {
            FieldType result;
            std::memcpy(&result, data + leaf_data_pos, sizeof(FieldType));
            return result;
         }
      }

      operator FieldType() const { return get(); }

      // ── Zero-copy read for string fields ──

      std::string_view str_view() const
         requires(std::is_same_v<FieldType, std::string>)
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Format, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t len = frac_detail::read_size<Format>(data, leaf_data_pos);
         return {data + leaf_data_pos + Format::size_bytes, len};
      }

      // ── Zero-copy read for bytes fields ──

      std::span<const char> data_span() const
         requires(std::is_same_v<FieldType, std::vector<char>> ||
                  std::is_same_v<FieldType, std::string>)
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Format, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t len = frac_detail::read_size<Format>(data, leaf_data_pos);
         return {data + leaf_data_pos + Format::size_bytes, len};
      }

      // ── Zero-copy check for optional fields ──

      bool has_value() const
         requires(is_packable<FieldType, Format>::is_optional)
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Format, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t offset = frac_detail::read_size<Format>(data, leaf_off_pos);
         return offset >= Format::size_bytes;  // 0/1 → false; >=size_bytes → Some
      }

      // ── Zero-copy byte length for variable-size fields ──

      uint32_t raw_byte_size() const
         requires(is_packable<FieldType, Format>::is_variable_size)
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Format, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t offset = frac_detail::read_size<Format>(data, leaf_off_pos);
         if (offset < Format::size_bytes)
            return 0;
         return frac_detail::packed_size_at<FieldType, Format>(data, leaf_data_pos);
      }

      // ── Write: fixed-size fields (requires mutable buffer) ──

      field_handle& operator=(const FieldType& v)
         requires(!is_packable<FieldType, Format>::is_variable_size &&
                  buf_traits<Buffer>::can_write_fixed)
      {
         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Format, Path...>(
             buf_data(), 0, levels, 0, leaf_off_pos, leaf_data_pos);

         // For fixed-size types, pack value and overwrite in-place
         auto packed = to_frac<FieldType, Format>(v);
         std::memcpy(mut_data() + leaf_data_pos, packed.data(), packed.size());
         return *this;
      }

      // ── Write: variable-size fields (requires splice-capable buffer) ──

      field_handle& operator=(const FieldType& v)
         requires(is_packable<FieldType, Format>::is_variable_size &&
                  buf_traits<Buffer>::can_splice)
      {
         // Pack the new value
         auto packed = to_frac<FieldType, Format>(v);

         // Navigate to find positions
         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Format, Path...>(
             buf_->data(), 0, levels, 0, leaf_off_pos, leaf_data_pos);

         // Read current offset to determine state
         uint32_t offset_val = frac_detail::read_size<Format>(buf_->data(), leaf_off_pos);

         bool new_is_empty = is_packable<FieldType, Format>::is_empty_container(v);

         if (offset_val <= 1)
         {
            // Currently empty (offset 0 or 1)
            if (new_is_empty)
            {
               // Empty → empty: just ensure correct marker
               if constexpr (is_packable<FieldType, Format>::is_optional)
               {
                  if (!v.has_value())
                     frac_detail::write_size<Format>(buf_->data(), leaf_off_pos, 1);
                  else
                     frac_detail::write_size<Format>(buf_->data(), leaf_off_pos, 0);
               }
               return *this;
            }
            // Empty → non-empty: find insertion point and insert
            uint32_t insert_pos = find_insert_pos(levels);
            int32_t  delta = frac_detail::splice_buffer(
                 *buf_, insert_pos, 0, packed.data(), static_cast<uint32_t>(packed.size()));
            uint32_t after_old = insert_pos;
            frac_detail::write_size<Format>(buf_->data(), leaf_off_pos,
                                              insert_pos - leaf_off_pos);
            frac_detail::patch_all<0, Root, Format, Path...>(
                buf_->data(), levels, after_old, delta);
            return *this;
         }

         // Currently non-empty
         if (new_is_empty)
         {
            // Non-empty → empty: remove data and set offset to 0/1
            uint32_t old_size =
                frac_detail::packed_size_at<FieldType, Format>(buf_->data(), leaf_data_pos);
            uint32_t after_old = leaf_data_pos + old_size;
            int32_t  delta     = frac_detail::splice_buffer(
                    *buf_, leaf_data_pos, old_size, nullptr, 0);

            if constexpr (is_packable<FieldType, Format>::is_optional)
               frac_detail::write_size<Format>(buf_->data(), leaf_off_pos, 1);
            else
               frac_detail::write_size<Format>(buf_->data(), leaf_off_pos, 0);

            frac_detail::patch_all<0, Root, Format, Path...>(
                buf_->data(), levels, after_old, delta);
            return *this;
         }

         // Non-empty → non-empty: splice to replace
         uint32_t old_size =
             frac_detail::packed_size_at<FieldType, Format>(buf_->data(), leaf_data_pos);
         uint32_t after_old = leaf_data_pos + old_size;
         int32_t  delta     = frac_detail::splice_buffer(
                 *buf_, leaf_data_pos, old_size,
                 packed.data(), static_cast<uint32_t>(packed.size()));

         // Leaf offset still points correctly (splice was at its target)
         // Patch sibling offsets at all ancestor levels
         frac_detail::patch_all<0, Root, Format, Path...>(
             buf_->data(), levels, after_old, delta);
         return *this;
      }

     private:
      // Find the insertion point when a currently-empty field needs heap data.
      // Scans successor variable-size fields for the first non-empty one;
      // the insertion point is right before its data.  Falls back to the
      // end of the struct's heap region.
      uint32_t find_insert_pos(const frac_detail::level_info* levels) const
      {
         using layout     = frac_layout<parent_type, Format>;
         uint32_t fs      = levels[depth - 1].fixed_start;
         const char* data = buf_->data();

         // Scan successor variable fields for one with data
         for (size_t i = leaf_idx + 1; i < layout::num_members; ++i)
         {
            if (layout::is_variable[i])
            {
               uint32_t off_pos = fs + layout::offsets[i];
               uint32_t offset  = frac_detail::read_size<Format>(data, off_pos);
               if (offset > 1)
                  return off_pos + offset;
            }
         }

         // No successor has data. Insertion point is end of the parent
         // struct's known heap extent.
         // Start from the fixed region end and scan ALL fields for max extent.
         uint32_t heap_end = fs + layout::total_fixed;
         for (size_t i = 0; i < layout::num_members; ++i)
         {
            if (layout::is_variable[i])
            {
               uint32_t off_pos = fs + layout::offsets[i];
               uint32_t offset  = frac_detail::read_size<Format>(data, off_pos);
               if (offset > 1)
               {
                  uint32_t dstart = off_pos + offset;
                  if (dstart > heap_end)
                     heap_end = dstart;
               }
            }
         }
         return heap_end;
      }
   };

   // ── frac_proxy_obj: the ProxyObject for PSIO_REFLECT's proxy<> ─────────────

   template <typename Root, typename Buffer, typename Format, size_t... Path>
   class frac_proxy_obj
   {
      Buffer* buf_;

     public:
      explicit frac_proxy_obj(Buffer* b) : buf_(b) {}

      template <int I, auto MemberPtr>
      decltype(auto) get()
      {
         using field_type = std::remove_cvref_t<decltype(result_of_member(MemberPtr))>;
         constexpr size_t idx = static_cast<size_t>(I);

         if constexpr (Reflected<field_type> &&
                       is_packable<field_type, Format>::is_variable_size)
         {
            // Nested reflected struct → extend path, return proxy
            using inner_obj  = frac_proxy_obj<Root, Buffer, Format, Path..., idx>;
            using proxy_type = typename reflect<field_type>::template proxy<inner_obj>;
            return proxy_type{inner_obj{buf_}};
         }
         else
         {
            // Leaf field → return read/write handle
            return field_handle<Root, field_type, Buffer, Format, Path..., idx>{buf_};
         }
      }

      template <int I, auto MemberPtr>
      decltype(auto) get() const
      {
         return const_cast<frac_proxy_obj*>(this)->template get<I, MemberPtr>();
      }
   };

   // ── frac_ref: top-level typed handle over a fracpack buffer ────────────────

   template <typename T,
             typename Buffer = std::span<const char>,
             typename Format = frac_format_32>
      requires Reflected<T>
   class frac_ref
   {
      Buffer buf_;

      using proxy_obj_t = frac_proxy_obj<T, Buffer, Format>;
      using proxy_t     = typename reflect<T>::template proxy<proxy_obj_t>;

     public:
      explicit frac_ref(Buffer b) : buf_(std::move(b)) {}

      // Field access: returns a proxy with named accessors generated
      // by PSIO_REFLECT.  Each accessor returns either a nested proxy
      // (for reflected struct fields) or a field_handle (for leaf fields).
      proxy_t fields() { return proxy_t{proxy_obj_t{&buf_}}; }

      // Raw data access
      const char*           data() const { return buf_.data(); }
      size_t                size() const { return buf_.size(); }
      std::span<const char> span() const { return {buf_.data(), buf_.size()}; }

      // Access underlying buffer
      Buffer&       buffer() { return buf_; }
      const Buffer& buffer() const { return buf_; }

      // Validate
      validation_t validate() const
      {
         return validate_frac<T, Format>({buf_.data(), buf_.size()});
      }

      // Full deserialization
      T unpack() const
      {
         return from_frac<T, Format>(std::span<const char>{buf_.data(), buf_.size()});
      }
   };

   // frac16 convenience alias
   template <typename T, typename Buffer = std::span<const char>>
      requires Reflected<T>
   using frac16_ref = frac_ref<T, Buffer, frac_format_16>;

   // ── Sorted container helper ─────────────────────────────────────────────────

   namespace frac_detail
   {
      inline uint32_t read_u32(const char* p)
      {
         uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      // Format-aware pointer variant of read_size (no base + pos form).
      template <typename Format>
      inline uint32_t read_size_at(const char* p)
      {
         typename Format::size_type v;
         std::memcpy(&v, p, Format::size_bytes);
         return static_cast<uint32_t>(v);
      }

      // Read a string from a fracpack offset field.
      // `off_pos` points to the relative offset (width = Format::size_bytes).
      // Returns empty view if offset is 0 (elided).
      template <typename Format = frac_format_32>
      inline std::string_view read_frac_string(const char* off_pos)
      {
         uint32_t off = read_size_at<Format>(off_pos);
         if (off == 0)
            return {};
         const char* str = off_pos + off;
         uint32_t    len = read_size_at<Format>(str);
         return {str + Format::size_bytes, len};
      }
   }  // namespace frac_detail

   // ══════════════════════════════════════════════════════════════════════════
   // Unified view<T, frac> — zero-copy read-only view over fracpack data
   //
   //   auto v = psio::view<Order, psio::frac>::from_buffer(packed.data());
   //   v.id()                  // uint64_t
   //   v.customer().name()     // string_view
   //   v.items().size()        // vec_view
   //   v.tags().contains(42)   // set_view
   // ══════════════════════════════════════════════════════════════════════════

   // ── Format tags for fracpack views ────────────────────────────────────────
   //
   // frac_fmt<Format> is the format tag consumed by view<T, Fmt> / vec_view /
   // set_view / map_view. `frac` is the frac_format_32 alias (back-compat);
   // `frac16` is the frac_format_16 alias.

   template <typename Format>
   struct frac_fmt;

   using frac   = frac_fmt<frac_format_32>;
   using frac16 = frac_fmt<frac_format_16>;

   // ── Type detection traits ─────────────────────────────────────────────────

   namespace frac_detail
   {
      template <typename T>
      struct is_frac_set : std::false_type
      {
      };
      template <typename K, typename... Rest>
      struct is_frac_set<std::flat_set<K, Rest...>> : std::true_type
      {
         using key_type = K;
      };
      template <typename K, typename... Rest>
      struct is_frac_set<std::set<K, Rest...>> : std::true_type
      {
         using key_type = K;
      };

      template <typename T>
      struct is_frac_map : std::false_type
      {
      };
      template <typename K, typename V, typename... Rest>
      struct is_frac_map<std::flat_map<K, V, Rest...>> : std::true_type
      {
         using key_type    = K;
         using mapped_type = V;
      };
      template <typename K, typename V, typename... Rest>
      struct is_frac_map<std::map<K, V, Rest...>> : std::true_type
      {
         using key_type    = K;
         using mapped_type = V;
      };

      template <typename T>
      struct is_frac_vector : std::false_type
      {
      };
      template <typename T>
      struct is_frac_vector<std::vector<T>> : std::true_type
      {
         using value_type = T;
      };

      template <typename T>
      struct is_frac_optional : std::false_type
      {
      };
      template <typename T>
      struct is_frac_optional<std::optional<T>> : std::true_type
      {
         using inner_type = T;
      };
   }  // namespace frac_detail

   // ══════════════════════════════════════════════════════════════════════════
   // vec_view<E, frac> — zero-copy vector view (fracpack format)
   //
   // Layout: [u32 byte_count][e0][e1]... for fixed-size elements.
   // ══════════════════════════════════════════════════════════════════════════

   template <typename E, typename Format>
   class vec_view<E, frac_fmt<Format>>
   {
      const char* data_ = nullptr;

     public:
      vec_view() = default;
      explicit vec_view(const char* p) : data_(p) {}
      explicit operator bool() const { return data_ != nullptr; }

      uint32_t size() const
      {
         if (!data_)
            return 0;
         uint32_t byte_count = frac_detail::read_size_at<Format>(data_);
         return byte_count / is_packable<E, Format>::fixed_size;
      }

      bool empty() const { return size() == 0; }

      auto operator[](uint32_t i) const
      {
         const char* entry =
             data_ + Format::size_bytes + i * is_packable<E, Format>::fixed_size;
         if constexpr (std::is_same_v<E, std::string>)
            return frac_detail::read_frac_string<Format>(entry);
         else if constexpr (std::is_same_v<E, bool>)
            return *entry != 0;
         else if constexpr (std::is_enum_v<E>)
         {
            using U = std::underlying_type_t<E>;
            U val;
            std::memcpy(&val, entry, sizeof(U));
            return static_cast<E>(val);
         }
         else if constexpr (std::is_arithmetic_v<E>)
         {
            E val;
            std::memcpy(&val, entry, sizeof(E));
            return val;
         }
         else if constexpr (Reflected<E>)
         {
            if constexpr (is_packable<E, Format>::is_variable_size)
            {
               uint32_t off = frac_detail::read_size_at<Format>(entry);
               return view<E, frac_fmt<Format>>(entry + off);
            }
            else
               return view<E, frac_fmt<Format>>(entry);
         }
         else
         {
            static_assert(!sizeof(E*), "unsupported vec element type for frac view");
         }
      }

      auto at(uint32_t i) const { return operator[](i); }

      struct read_fn
      {
         auto operator()(const vec_view& v, uint32_t i) const { return v[i]; }
      };
      using iterator = index_iterator<vec_view, read_fn>;

      iterator begin() const { return {this, 0}; }
      iterator end() const { return {this, size()}; }
   };

   // ══════════════════════════════════════════════════════════════════════════
   // set_view<K, frac> — zero-copy sorted set view with binary search
   //
   // Inherits sorted_set_algo for contains(), find(), lower_bound().
   // ══════════════════════════════════════════════════════════════════════════

   template <typename K, typename Format>
   class set_view<K, frac_fmt<Format>>
       : public sorted_set_algo<set_view<K, frac_fmt<Format>>>
   {
      friend class sorted_set_algo<set_view<K, frac_fmt<Format>>>;

      const char* data_  = nullptr;
      uint32_t    count_ = 0;

      static constexpr uint32_t entry_sz = is_packable<K, Format>::fixed_size;

      auto read_key(uint32_t i) const
      {
         const char* entry = data_ + Format::size_bytes + i * entry_sz;
         if constexpr (std::is_same_v<K, std::string>)
            return frac_detail::read_frac_string<Format>(entry);
         else
         {
            K val;
            std::memcpy(&val, entry, sizeof(K));
            return val;
         }
      }

     public:
      set_view() = default;
      explicit set_view(const char* data)
          : data_(data),
            count_(data ? frac_detail::read_size_at<Format>(data) / entry_sz : 0)
      {
      }
      explicit set_view(std::span<const char> sp)
          : set_view(sp.empty() ? nullptr : sp.data())
      {
      }
      explicit operator bool() const { return data_ != nullptr; }

      uint32_t size() const { return count_; }
      bool     empty() const { return count_ == 0; }
      // contains(), find(), lower_bound() inherited from sorted_set_algo

      struct read_fn
      {
         auto operator()(const set_view& v, uint32_t i) const { return v.read_key(i); }
      };
      using iterator = index_iterator<set_view, read_fn>;

      iterator begin() const { return {this, 0}; }
      iterator end() const { return {this, size()}; }
   };

   // ══════════════════════════════════════════════════════════════════════════
   // map_view<K, V, frac> — zero-copy sorted map view with O(log n) lookup
   //
   // Inherits sorted_map_algo for find_index(), contains(), lower_bound().
   // Wire format: indirected pairs with u16 header per entry.
   // ══════════════════════════════════════════════════════════════════════════

   template <typename K, typename V, typename Format>
   class map_view<K, V, frac_fmt<Format>>
       : public sorted_map_algo<map_view<K, V, frac_fmt<Format>>>
   {
      friend class sorted_map_algo<map_view<K, V, frac_fmt<Format>>>;

      const char* data_  = nullptr;
      uint32_t    count_ = 0;

      static constexpr uint32_t key_fixed = is_packable<K, Format>::fixed_size;
      static constexpr uint32_t val_fixed = is_packable<V, Format>::fixed_size;

      // Follow offset at index i to get the pair sub-object's fixed region
      // (skips the u16 members_fixed header)
      const char* pair_fixed(uint32_t i) const
      {
         const char* off_pos = data_ + Format::size_bytes + i * Format::size_bytes;
         uint32_t    off     = frac_detail::read_size_at<Format>(off_pos);
         return off_pos + off + 2;  // skip u16 members_fixed header
      }

      auto read_key(uint32_t i) const
      {
         const char* pf = pair_fixed(i);
         if constexpr (std::is_same_v<K, std::string>)
            return frac_detail::read_frac_string<Format>(pf);
         else
         {
            K val;
            std::memcpy(&val, pf, sizeof(K));
            return val;
         }
      }

      auto read_value(uint32_t i) const
      {
         const char* pf = pair_fixed(i) + key_fixed;
         if constexpr (std::is_same_v<V, std::string>)
            return frac_detail::read_frac_string<Format>(pf);
         else if constexpr (std::is_same_v<V, bool>)
            return *pf != 0;
         else if constexpr (std::is_arithmetic_v<V>)
         {
            V val;
            std::memcpy(&val, pf, sizeof(V));
            return val;
         }
         else if constexpr (Reflected<V>)
         {
            if constexpr (is_packable<V, Format>::is_variable_size)
            {
               uint32_t off = frac_detail::read_size_at<Format>(pf);
               return view<V, frac_fmt<Format>>(pf + off);
            }
            else
               return view<V, frac_fmt<Format>>(pf);
         }
         else
         {
            static_assert(!sizeof(V*), "unsupported map value type for frac view");
            return V{};
         }
      }

     public:
      map_view() = default;
      explicit map_view(const char* data)
          : data_(data),
            count_(data ? frac_detail::read_size_at<Format>(data) / Format::size_bytes : 0)
      {
      }
      explicit map_view(std::span<const char> sp)
          : map_view(sp.empty() ? nullptr : sp.data())
      {
      }
      explicit operator bool() const { return data_ != nullptr; }

      uint32_t size() const { return count_; }
      bool     empty() const { return count_ == 0; }

      struct entry_view
      {
         const map_view* map_;
         uint32_t        idx_;
         auto            key() const { return map_->read_key(idx_); }
         auto            value() const { return map_->read_value(idx_); }

         template <size_t I>
         auto get() const
         {
            if constexpr (I == 0)
               return key();
            else
               return value();
         }
      };

      // contains(), find_index(), lower_bound() inherited from sorted_map_algo

      entry_view find(const auto& key) const
      {
         return {this, sorted_map_algo<map_view>::find_index(key)};
      }

      auto value_or(const auto& key, const auto& fallback) const
      {
         uint32_t idx = sorted_map_algo<map_view>::find_index(key);
         if (idx < count_)
            return read_value(idx);
         return decltype(read_value(0))(fallback);
      }

      struct read_fn
      {
         auto operator()(const map_view& m, uint32_t i) const
         {
            return std::pair{m.read_key(i), m.read_value(i)};
         }
      };
      using iterator = index_iterator<map_view, read_fn>;

      iterator begin() const { return {this, 0}; }
      iterator end() const { return {this, size()}; }
   };

   // ── Backward-compatible aliases ───────────────────────────────────────────

   template <typename K>
   using frac_sorted_set = set_view<K, frac>;
   template <typename K, typename V>
   using frac_sorted_map = map_view<K, V, frac>;

   // ── frac_view_field: read field N from fracpack-encoded struct ─────────────

   namespace frac_detail
   {
      template <typename T, size_t N, typename Format = frac_format_32>
      auto frac_view_field(const char* struct_start)
      {
         using F      = nth_field_t<T, N>;
         using layout = frac_layout<T, Format>;

         const char* fixed_start = struct_start + layout::hdr_size;
         const char* field_pos   = fixed_start + layout::offset_of(N);

         if constexpr (std::is_same_v<F, bool>)
            return *field_pos != 0;
         else if constexpr (std::is_enum_v<F>)
         {
            using U = std::underlying_type_t<F>;
            U val;
            std::memcpy(&val, field_pos, sizeof(U));
            return static_cast<F>(val);
         }
         else if constexpr (std::is_arithmetic_v<F>)
         {
            F val;
            std::memcpy(&val, field_pos, sizeof(F));
            return val;
         }
         else if constexpr (std::is_same_v<F, std::string>)
            return read_frac_string<Format>(field_pos);
         else if constexpr (is_frac_optional<F>::value)
         {
            using Inner  = typename is_frac_optional<F>::inner_type;
            uint32_t off = read_size_at<Format>(field_pos);
            if constexpr (std::is_same_v<Inner, bool>)
               return off >= Format::size_bytes
                          ? std::optional<bool>(*(field_pos + off) != 0)
                          : std::optional<bool>{};
            else if constexpr (std::is_enum_v<Inner>)
            {
               using U = std::underlying_type_t<Inner>;
               if (off < Format::size_bytes)
                  return std::optional<Inner>{};
               U val;
               std::memcpy(&val, field_pos + off, sizeof(U));
               return std::optional<Inner>(static_cast<Inner>(val));
            }
            else if constexpr (std::is_arithmetic_v<Inner>)
            {
               if (off < Format::size_bytes)
                  return std::optional<Inner>{};
               Inner val;
               std::memcpy(&val, field_pos + off, sizeof(Inner));
               return std::optional<Inner>(val);
            }
            else if constexpr (std::is_same_v<Inner, std::string>)
            {
               if (off < Format::size_bytes)
                  return std::string_view{};
               const char* str = field_pos + off;
               uint32_t    len = read_size_at<Format>(str);
               return std::string_view{str + Format::size_bytes, len};
            }
            else
               static_assert(!sizeof(Inner*), "unsupported optional inner type for frac view");
         }
         else if constexpr (is_frac_set<F>::value)
         {
            using K      = typename is_frac_set<F>::key_type;
            uint32_t off = read_size_at<Format>(field_pos);
            if (off <= 1)
               return set_view<K, frac_fmt<Format>>{};
            return set_view<K, frac_fmt<Format>>(field_pos + off);
         }
         else if constexpr (is_frac_map<F>::value)
         {
            using K      = typename is_frac_map<F>::key_type;
            using MV     = typename is_frac_map<F>::mapped_type;
            uint32_t off = read_size_at<Format>(field_pos);
            if (off <= 1)
               return map_view<K, MV, frac_fmt<Format>>{};
            return map_view<K, MV, frac_fmt<Format>>(field_pos + off);
         }
         else if constexpr (is_frac_vector<F>::value)
         {
            using E      = typename is_frac_vector<F>::value_type;
            uint32_t off = read_size_at<Format>(field_pos);
            if (off <= 1)
               return vec_view<E, frac_fmt<Format>>{};
            return vec_view<E, frac_fmt<Format>>(field_pos + off);
         }
         else if constexpr (Reflected<F>)
         {
            if constexpr (is_packable<F, Format>::is_variable_size)
            {
               uint32_t off = read_size_at<Format>(field_pos);
               if (off <= 1)
                  return view<F, frac_fmt<Format>>{};
               return view<F, frac_fmt<Format>>(field_pos + off);
            }
            else
               return view<F, frac_fmt<Format>>(field_pos);
         }
         else
            static_assert(!sizeof(F*), "unsupported field type for frac view");
      }
   }  // namespace frac_detail

   // ══════════════════════════════════════════════════════════════════════════
   // frac_fmt<Format> — fracpack format tag for view<T, Fmt>
   //
   // Satisfies the Format concept: ptr_t, root<T>(), field<T,N>().
   // `frac = frac_fmt<frac_format_32>` (back-compat) declared up top.
   // `frac16 = frac_fmt<frac_format_16>` (new).
   // ══════════════════════════════════════════════════════════════════════════

   template <typename Format>
   struct frac_fmt
   {
      using ptr_t = const char*;

      template <typename T>
      static ptr_t root(const void* buf)
      {
         return static_cast<const char*>(buf);
      }

      template <typename T, size_t N>
      static auto field(ptr_t data)
      {
         return frac_detail::frac_view_field<T, N, Format>(data);
      }
   };

   /// frac_view<T> — alias for view<T, frac>
   template <typename T>
   using frac_view = view<T, frac>;

   /// frac16_view<T> — alias for view<T, frac16>
   template <typename T>
   using frac16_view = view<T, frac16>;

}  // namespace psio
