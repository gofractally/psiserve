// frac_ref: zero-copy mutable views over fracpack-encoded data
//
// Capability tiers, deduced from buffer type:
//   span<const char>  → read-only
//   span<char>        → read + fixed-size field overwrites
//   vector<char>      → read + full mutation (splice/realloc)

#pragma once

#include <psio/fracpack.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
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

   template <typename T>
      requires Reflected<T>
   struct frac_layout
   {
      static constexpr bool     has_header = !reflect<T>::definitionWillNotChange;
      static constexpr uint32_t hdr_size   = has_header ? 2 : 0;

      static consteval auto member_fixed_sizes()
      {
         return apply_members((typename reflect<T>::data_members*)nullptr,
                              [](auto... members)
                              {
                                 return std::array<uint32_t, sizeof...(members)>{
                                     is_packable<std::remove_cvref_t<
                                         decltype(std::declval<T>().*members)>>::fixed_size...};
                              });
      }

      static consteval auto member_is_variable()
      {
         return apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... members)
             {
                return std::array<bool, sizeof...(members)>{
                    is_packable<std::remove_cvref_t<
                        decltype(std::declval<T>().*members)>>::is_variable_size...};
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
          is_packable_reflected<T, true>::members_fixed_size;

      static consteval bool all_4byte_aligned()
      {
         for (size_t i = 0; i < num_members; ++i)
            if (fixed_sizes[i] != 4)
               return false;
         return true;
      }

      static constexpr bool simd_eligible = all_4byte_aligned();
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

      inline void patch_offset(char* buf, uint32_t off_pos, uint32_t after_old, int32_t delta)
      {
         uint32_t val = read_u32(buf, off_pos);
         if (val <= 1)
            return;
         uint32_t abs_target = off_pos + val;
         if (abs_target >= after_old)
            write_u32(buf, off_pos, static_cast<uint32_t>(static_cast<int64_t>(val) + delta));
      }

      // ── packed_size_at: measure packed data size at a buffer position ──

      template <Packable T>
      uint32_t packed_size_at(const char* buf, uint32_t pos)
      {
         if constexpr (!is_packable<T>::is_variable_size)
         {
            return is_packable<T>::fixed_size;
         }
         else if constexpr (std::is_same_v<T, std::string> ||
                            std::is_same_v<T, std::string_view>)
         {
            return 4 + read_u32(buf, pos);
         }
         else if constexpr (Reflected<T>)
         {
            // Variable-size struct: walk members to find heap extent
            using layout     = frac_layout<T>;
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
                      if constexpr (is_packable<FT>::is_variable_size)
                      {
                         if (fpos < fixed_start + fixed_size)
                         {
                            uint32_t offset = read_u32(buf, fpos);
                            if (offset > 1)
                            {
                               uint32_t dstart = fpos + offset;
                               uint32_t dend   = dstart + packed_size_at<FT>(buf, dstart);
                               if (dend > heap_end)
                                  heap_end = dend;
                            }
                         }
                      }
                      fpos += is_packable<FT>::fixed_size;
                   };
                   (process(members), ...);
                });

            return heap_end - start;
         }
         else
         {
            // Generic memcpy container: [u32 byte_count][bytes...]
            return 4 + read_u32(buf, pos);
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
      template <typename T, size_t FieldIdx>
      void navigate_recording(const char* buf,
                              uint32_t    struct_start,
                              level_info* levels,
                              size_t      depth,
                              uint32_t&   leaf_off_pos,
                              uint32_t&   leaf_data_pos)
      {
         using layout             = frac_layout<T>;
         uint32_t fixed_start     = struct_start + layout::hdr_size;
         levels[depth].fixed_start = fixed_start;

         constexpr uint32_t rel  = layout::offset_of(FieldIdx);
         leaf_off_pos             = fixed_start + rel;

         if constexpr (layout::is_variable[FieldIdx])
         {
            uint32_t offset = read_u32(buf, leaf_off_pos);
            leaf_data_pos   = leaf_off_pos + offset;
         }
         else
         {
            leaf_data_pos = leaf_off_pos;
         }
      }

      // Recursive case: follow Head, then navigate deeper
      template <typename T, size_t HeadIdx, size_t NextIdx, size_t... RestIdx>
      void navigate_recording(const char* buf,
                              uint32_t    struct_start,
                              level_info* levels,
                              size_t      depth,
                              uint32_t&   leaf_off_pos,
                              uint32_t&   leaf_data_pos)
      {
         using layout             = frac_layout<T>;
         uint32_t fixed_start     = struct_start + layout::hdr_size;
         levels[depth].fixed_start = fixed_start;

         constexpr uint32_t rel  = layout::offset_of(HeadIdx);
         uint32_t off_pos        = fixed_start + rel;
         uint32_t offset         = read_u32(buf, off_pos);
         uint32_t nested_start   = off_pos + offset;

         using nested_type = nth_field_t<T, HeadIdx>;
         navigate_recording<nested_type, NextIdx, RestIdx...>(
             buf, nested_start, levels, depth + 1, leaf_off_pos, leaf_data_pos);
      }

      // ── Compile-time patch info ──

      template <typename T, size_t MutatedIndex>
      struct sibling_patches
      {
         static consteval auto compute()
         {
            using layout = frac_layout<T>;
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
      template <typename T, size_t MutatedIndex>
      void patch_level_scalar(char*    buf,
                              uint32_t fixed_start,
                              uint32_t after_old,
                              int32_t  delta)
      {
         using patches = sibling_patches<T, MutatedIndex>;
         [&]<size_t... I>(std::index_sequence<I...>)
         {
            (patch_offset(buf, fixed_start + patches::offsets[I], after_old, delta), ...);
         }(std::make_index_sequence<patches::count>{});
      }

      // SIMD-friendly batch patch via compile-time mask
      template <typename T, size_t MutatedIndex>
      struct patch_mask
      {
         static consteval auto compute()
         {
            using layout = frac_layout<T>;
            std::array<uint32_t, layout::num_members> m{};
            for (size_t i = MutatedIndex + 1; i < layout::num_members; ++i)
               if (layout::is_variable[i])
                  m[i] = 1;
            return m;
         }
         static constexpr auto mask = compute();
      };

      template <typename T, size_t MutatedIndex>
      void patch_level_simd(char*    buf,
                            uint32_t fixed_start,
                            int32_t  delta)
      {
         using layout       = frac_layout<T>;
         constexpr auto mask = patch_mask<T, MutatedIndex>::mask;
         uint32_t*      slots = reinterpret_cast<uint32_t*>(buf + fixed_start);
         uint32_t       udelta = static_cast<uint32_t>(delta);

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

      template <typename T, size_t MutatedIndex>
      void patch_level_dispatch(char*    buf,
                                uint32_t fixed_start,
                                uint32_t after_old,
                                int32_t  delta)
      {
         if constexpr (frac_layout<T>::simd_eligible)
            patch_level_simd<T, MutatedIndex>(buf, fixed_start, delta);
         else
            patch_level_scalar<T, MutatedIndex>(buf, fixed_start, after_old, delta);
      }

      // ── Multi-level patching: recursive from bottom up ──

      // Base case: single index
      template <size_t Depth, typename T, size_t Idx>
      void patch_all(char*            buf,
                     const level_info* levels,
                     uint32_t         after_old,
                     int32_t          delta)
      {
         patch_level_dispatch<T, Idx>(buf, levels[Depth].fixed_start, after_old, delta);
      }

      // Recursive: patch deeper levels first, then this level
      template <size_t Depth, typename T, size_t Head, size_t Next, size_t... Rest>
      void patch_all(char*            buf,
                     const level_info* levels,
                     uint32_t         after_old,
                     int32_t          delta)
      {
         using nested = nth_field_t<T, Head>;
         patch_all<Depth + 1, nested, Next, Rest...>(buf, levels, after_old, delta);
         patch_level_dispatch<T, Head>(buf, levels[Depth].fixed_start, after_old, delta);
      }

   }  // namespace frac_detail

   // ── Forward declarations for proxy types ───────────────────────────────────

   template <typename Root, typename Buffer, size_t... Path>
   class frac_proxy_obj;

   template <typename Root, typename FieldType, typename Buffer, size_t... Path>
   class field_handle;

   // ── field_handle: read/write handle for a leaf field ───────────────────────

   template <typename Root, typename FieldType, typename Buffer, size_t... Path>
   class field_handle
   {
      Buffer* buf_;

      static constexpr size_t depth    = sizeof...(Path);
      static constexpr auto   path_arr = std::array<size_t, depth>{Path...};
      static constexpr size_t leaf_idx = last_index<Path...>::value;
      static constexpr bool   leaf_is_var = is_packable<FieldType>::is_variable_size;

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
         frac_detail::navigate_recording<Root, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         if constexpr (leaf_is_var)
         {
            uint32_t  psize = frac_detail::packed_size_at<FieldType>(data, leaf_data_pos);
            FieldType result;
            from_frac(result, std::span<const char>(data + leaf_data_pos, psize));
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
         frac_detail::navigate_recording<Root, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t len = frac_detail::read_u32(data, leaf_data_pos);
         return {data + leaf_data_pos + 4, len};
      }

      // ── Zero-copy read for bytes fields ──

      std::span<const char> data_span() const
         requires(std::is_same_v<FieldType, std::vector<char>> ||
                  std::is_same_v<FieldType, std::string>)
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t len = frac_detail::read_u32(data, leaf_data_pos);
         return {data + leaf_data_pos + 4, len};
      }

      // ── Zero-copy check for optional fields ──

      bool has_value() const
         requires(is_packable<FieldType>::is_optional)
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t offset = frac_detail::read_u32(data, leaf_off_pos);
         return offset >= 4;  // 0=elided, 1=None → false; >=4=Some → true
      }

      // ── Zero-copy byte length for variable-size fields ──

      uint32_t raw_byte_size() const
         requires(is_packable<FieldType>::is_variable_size)
      {
         const char* data = buf_data();

         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Path...>(
             data, 0, levels, 0, leaf_off_pos, leaf_data_pos);

         uint32_t offset = frac_detail::read_u32(data, leaf_off_pos);
         if (offset < 4)
            return 0;
         return frac_detail::packed_size_at<FieldType>(data, leaf_data_pos);
      }

      // ── Write: fixed-size fields (requires mutable buffer) ──

      field_handle& operator=(const FieldType& v)
         requires(!is_packable<FieldType>::is_variable_size &&
                  buf_traits<Buffer>::can_write_fixed)
      {
         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Path...>(
             buf_data(), 0, levels, 0, leaf_off_pos, leaf_data_pos);

         // For fixed-size types, pack value and overwrite in-place
         auto packed = to_frac(v);
         std::memcpy(mut_data() + leaf_data_pos, packed.data(), packed.size());
         return *this;
      }

      // ── Write: variable-size fields (requires splice-capable buffer) ──

      field_handle& operator=(const FieldType& v)
         requires(is_packable<FieldType>::is_variable_size &&
                  buf_traits<Buffer>::can_splice)
      {
         // Pack the new value
         auto packed = to_frac(v);

         // Navigate to find positions
         frac_detail::level_info levels[depth];
         uint32_t                leaf_off_pos, leaf_data_pos;
         frac_detail::navigate_recording<Root, Path...>(
             buf_->data(), 0, levels, 0, leaf_off_pos, leaf_data_pos);

         // Read current offset to determine state
         uint32_t offset_val = frac_detail::read_u32(buf_->data(), leaf_off_pos);

         bool new_is_empty = is_packable<FieldType>::is_empty_container(v);

         if (offset_val <= 1)
         {
            // Currently empty (offset 0 or 1)
            if (new_is_empty)
            {
               // Empty → empty: just ensure correct marker
               if constexpr (is_packable<FieldType>::is_optional)
               {
                  if (!v.has_value())
                     frac_detail::write_u32(buf_->data(), leaf_off_pos, 1);
                  else
                     frac_detail::write_u32(buf_->data(), leaf_off_pos, 0);
               }
               return *this;
            }
            // Empty → non-empty: find insertion point and insert
            uint32_t insert_pos = find_insert_pos(levels);
            int32_t  delta = frac_detail::splice_buffer(
                 *buf_, insert_pos, 0, packed.data(), static_cast<uint32_t>(packed.size()));
            uint32_t after_old = insert_pos;
            frac_detail::write_u32(buf_->data(), leaf_off_pos, insert_pos - leaf_off_pos);
            frac_detail::patch_all<0, Root, Path...>(
                buf_->data(), levels, after_old, delta);
            return *this;
         }

         // Currently non-empty
         if (new_is_empty)
         {
            // Non-empty → empty: remove data and set offset to 0/1
            uint32_t old_size  = frac_detail::packed_size_at<FieldType>(
                buf_->data(), leaf_data_pos);
            uint32_t after_old = leaf_data_pos + old_size;
            int32_t  delta     = frac_detail::splice_buffer(
                    *buf_, leaf_data_pos, old_size, nullptr, 0);

            if constexpr (is_packable<FieldType>::is_optional)
               frac_detail::write_u32(buf_->data(), leaf_off_pos, 1);
            else
               frac_detail::write_u32(buf_->data(), leaf_off_pos, 0);

            frac_detail::patch_all<0, Root, Path...>(
                buf_->data(), levels, after_old, delta);
            return *this;
         }

         // Non-empty → non-empty: splice to replace
         uint32_t old_size  = frac_detail::packed_size_at<FieldType>(
             buf_->data(), leaf_data_pos);
         uint32_t after_old = leaf_data_pos + old_size;
         int32_t  delta     = frac_detail::splice_buffer(
                 *buf_, leaf_data_pos, old_size,
                 packed.data(), static_cast<uint32_t>(packed.size()));

         // Leaf offset still points correctly (splice was at its target)
         // Patch sibling offsets at all ancestor levels
         frac_detail::patch_all<0, Root, Path...>(
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
         using layout     = frac_layout<parent_type>;
         uint32_t fs      = levels[depth - 1].fixed_start;
         const char* data = buf_->data();

         // Scan successor variable fields for one with data
         for (size_t i = leaf_idx + 1; i < layout::num_members; ++i)
         {
            if (layout::is_variable[i])
            {
               uint32_t off_pos = fs + layout::offsets[i];
               uint32_t offset  = frac_detail::read_u32(data, off_pos);
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
               uint32_t offset  = frac_detail::read_u32(data, off_pos);
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

   template <typename Root, typename Buffer, size_t... Path>
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

         if constexpr (Reflected<field_type> && is_packable<field_type>::is_variable_size)
         {
            // Nested reflected struct → extend path, return proxy
            using inner_obj  = frac_proxy_obj<Root, Buffer, Path..., idx>;
            using proxy_type = typename reflect<field_type>::template proxy<inner_obj>;
            return proxy_type{inner_obj{buf_}};
         }
         else
         {
            // Leaf field → return read/write handle
            return field_handle<Root, field_type, Buffer, Path..., idx>{buf_};
         }
      }

      template <int I, auto MemberPtr>
      decltype(auto) get() const
      {
         return const_cast<frac_proxy_obj*>(this)->template get<I, MemberPtr>();
      }
   };

   // ── frac_ref: top-level typed handle over a fracpack buffer ────────────────

   template <typename T, typename Buffer = std::span<const char>>
      requires Reflected<T>
   class frac_ref
   {
      Buffer buf_;

      using proxy_obj_t = frac_proxy_obj<T, Buffer>;
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
         return fracpack_validate<T>({buf_.data(), buf_.size()});
      }

      // Full deserialization
      T unpack() const
      {
         return from_frac<T>(std::span<const char>{buf_.data(), buf_.size()});
      }
   };

   // ── Sorted container views with binary search ──────────────────────────────
   //
   // Zero-copy O(log n) lookup over fracpack-encoded flat_set / flat_map data.
   //
   // Construction: pass a span/pointer to the packed container bytes,
   // i.e. the [u32 byte_count][entries...] region.
   //
   //   auto packed = psio::to_frac(my_struct);
   //   auto s = psio::frac_sorted_set<int32_t>(packed_container_bytes);
   //   if (s.contains(42)) { ... }

   namespace frac_detail
   {
      inline uint32_t read_u32(const char* p)
      {
         uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      // Read a string from a fracpack offset field.
      // `off_pos` points to the 4-byte relative offset.
      // Returns empty view if offset is 0 (elided).
      inline std::string_view read_frac_string(const char* off_pos)
      {
         uint32_t off = read_u32(off_pos);
         if (off == 0)
            return {};
         const char* str = off_pos + off;
         uint32_t    len = read_u32(str);
         return {str + 4, len};
      }
   }  // namespace frac_detail

   /// Zero-copy sorted view over a fracpack-encoded flat_set<K>.
   /// Provides O(log n) contains/find/lower_bound via binary search.
   template <typename K>
   class frac_sorted_set
   {
      const char* data_;
      uint32_t    count_;

      static constexpr uint32_t entry_sz = is_packable<K>::fixed_size;

      auto read_key(uint32_t i) const
      {
         const char* entry = data_ + 4 + i * entry_sz;
         if constexpr (std::is_same_v<K, std::string>)
            return frac_detail::read_frac_string(entry);
         else
         {
            K val;
            std::memcpy(&val, entry, sizeof(K));
            return val;
         }
      }

     public:
      explicit frac_sorted_set(const char* data = nullptr)
          : data_(data), count_(data ? frac_detail::read_u32(data) / entry_sz : 0)
      {
      }
      explicit frac_sorted_set(std::span<const char> sp)
          : frac_sorted_set(sp.empty() ? nullptr : sp.data())
      {
      }

      explicit operator bool() const { return data_ != nullptr; }
      uint32_t size() const { return count_; }

      auto operator[](uint32_t i) const { return read_key(i); }

      bool contains(const auto& key) const
      {
         uint32_t lo = 0, hi = count_;
         while (lo < hi)
         {
            uint32_t mid = lo + (hi - lo) / 2;
            auto     mk  = read_key(mid);
            if (mk < key)
               lo = mid + 1;
            else if (key < mk)
               hi = mid;
            else
               return true;
         }
         return false;
      }

      uint32_t find(const auto& key) const
      {
         uint32_t lo = 0, hi = count_;
         while (lo < hi)
         {
            uint32_t mid = lo + (hi - lo) / 2;
            auto     mk  = read_key(mid);
            if (mk < key)
               lo = mid + 1;
            else if (key < mk)
               hi = mid;
            else
               return mid;
         }
         return count_;
      }

      uint32_t lower_bound(const auto& key) const
      {
         uint32_t lo = 0, hi = count_;
         while (lo < hi)
         {
            uint32_t mid = lo + (hi - lo) / 2;
            if (read_key(mid) < key)
               lo = mid + 1;
            else
               hi = mid;
         }
         return lo;
      }
   };

   /// Zero-copy sorted view over a fracpack-encoded flat_map<K, V>.
   /// Provides O(log n) lookup by key via binary search.
   ///
   /// Fracpack layout: pair<K,V> is always variable-size (no definitionWillNotChange),
   /// so entries are indirected:
   ///   [u32 total_fixed = count * 4][offset0, offset1, ...]
   ///   [pair0: [u16 members_fixed][K_data][V_data][variable...]]
   ///   [pair1: ...]
   /// The key is the first field of the pair sub-object, after the u16 header.
   template <typename K, typename V>
   class frac_sorted_map
   {
      const char* data_;
      uint32_t    count_;

      static constexpr uint32_t key_fixed = is_packable<K>::fixed_size;
      static constexpr uint32_t val_fixed = is_packable<V>::fixed_size;

      // Follow offset at index i to get the pair sub-object's fixed region
      // (skips the u16 members_fixed header)
      const char* pair_fixed(uint32_t i) const
      {
         const char* off_pos = data_ + 4 + i * 4;
         uint32_t    off     = frac_detail::read_u32(off_pos);
         return off_pos + off + 2;  // skip u16 members_fixed header
      }

      auto read_key(uint32_t i) const
      {
         const char* pf = pair_fixed(i);
         if constexpr (std::is_same_v<K, std::string>)
            return frac_detail::read_frac_string(pf);
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
            return frac_detail::read_frac_string(pf);
         else if constexpr (std::is_same_v<V, bool>)
            return *pf != 0;
         else
         {
            V val;
            std::memcpy(&val, pf, sizeof(V));
            return val;
         }
      }

      uint32_t find_index(const auto& key) const
      {
         uint32_t lo = 0, hi = count_;
         while (lo < hi)
         {
            uint32_t mid = lo + (hi - lo) / 2;
            auto     mk  = read_key(mid);
            if (mk < key)
               lo = mid + 1;
            else if (key < mk)
               hi = mid;
            else
               return mid;
         }
         return count_;
      }

     public:
      explicit frac_sorted_map(const char* data = nullptr)
          : data_(data), count_(data ? frac_detail::read_u32(data) / 4 : 0)
      {
      }
      explicit frac_sorted_map(std::span<const char> sp)
          : frac_sorted_map(sp.empty() ? nullptr : sp.data())
      {
      }

      explicit operator bool() const { return data_ != nullptr; }
      uint32_t size() const { return count_; }

      struct entry_view
      {
         const frac_sorted_map* map_;
         uint32_t               idx_;
         auto                   key() const { return map_->read_key(idx_); }
         auto                   value() const { return map_->read_value(idx_); }
      };

      entry_view operator[](uint32_t i) const { return {this, i}; }

      bool       contains(const auto& key) const { return find_index(key) < count_; }
      entry_view find(const auto& key) const { return {this, find_index(key)}; }

      /// Value lookup — returns default-constructed V if not found.
      auto value_or(const auto& key, const auto& fallback) const
      {
         uint32_t idx = find_index(key);
         if (idx < count_)
            return read_value(idx);
         return decltype(read_value(0))(fallback);
      }
   };

}  // namespace psio
