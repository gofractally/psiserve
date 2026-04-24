#pragma once
//
// psio3/flatbuf.hpp — native (zero-dep) FlatBuffer format tag.
//
// Produces a wire-compatible FlatBuffer buffer without depending on
// the Google FlatBuffers runtime. Intended to be byte-identical to
// psio3/flatbuf_lib.hpp on the shapes they both support — this is
// what lets a consumer freely swap between native and library paths
// during migration.
//
// MVP scope (same as flatbuf_lib):
//   - primitives (arithmetic + bool)
//   - std::string
//   - std::vector of arithmetic / std::string / nested tables
//   - std::optional<arithmetic | bool | string>
//   - reflected records (root or nested tables)
//
// Wire reference: FlatBuffers binary format spec
// (https://flatbuffers.dev/internals.html). Field layout is encoded
// via PSIO3_REFLECT order — field I maps to vtable voffset 4 + 2*I.

#include <psio3/cpo.hpp>
#include <psio3/error.hpp>
#include <psio3/format_tag_base.hpp>
#include <psio3/adapter.hpp>
#include <psio3/reflect.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <vector>

namespace psio3 {

   struct flatbuf;

   namespace detail::flatbuf_impl {

      template <typename T>
      concept Record = ::psio3::Reflected<T>;

      template <typename T>
      struct is_optional : std::false_type {};
      template <typename T>
      struct is_optional<std::optional<T>> : std::true_type {};

      template <typename T>
      struct is_vector : std::false_type {};
      template <typename T, typename A>
      struct is_vector<std::vector<T, A>> : std::true_type {};

      template <typename T, typename = void>
      struct is_table : std::false_type {};
      template <typename T>
      struct is_table<T, std::void_t<decltype(::psio3::reflect<T>::member_count)>>
         : std::bool_constant<!std::is_arithmetic_v<T> &&
                              !std::is_same_v<T, std::string>> {};

      // Back-to-front buffer: writes grow toward the front, so finished
      // offsets count *backwards* from the buffer's "end" (its capacity).
      class builder
      {
         std::vector<std::uint8_t> buf_;  // head at buf_[head_]
         std::size_t               head_;
         std::size_t               min_align_ = 1;

         struct field_loc
         {
            std::uint32_t off;
            std::uint16_t vt;
         };
         static constexpr std::size_t kMaxFields = 64;
         field_loc                    fields_[kMaxFields]{};
         std::size_t                  nfields_   = 0;
         std::uint32_t                tbl_start_ = 0;

         std::size_t sz() const { return buf_.size() - head_; }

         void grow(std::size_t needed)
         {
            const std::size_t tail = sz();
            const std::size_t new_cap =
               std::max(buf_.size() * 2, buf_.size() + needed);
            std::vector<std::uint8_t> nb(new_cap);
            std::memcpy(nb.data() + new_cap - tail,
                        buf_.data() + head_, tail);
            buf_  = std::move(nb);
            head_ = new_cap - tail;
         }

         std::uint8_t* alloc(std::size_t n)
         {
            if (n > head_)
               grow(n);
            head_ -= n;
            return buf_.data() + head_;
         }

         void zero_pad(std::size_t n) { std::memset(alloc(n), 0, n); }

         void track(std::size_t a)
         {
            if (a > min_align_)
               min_align_ = a;
         }

         void align(std::size_t a)
         {
            const std::size_t p = (~sz() + 1) & (a - 1);
            if (p)
               zero_pad(p);
            track(a);
         }

         void pre_align(std::size_t len, std::size_t a)
         {
            if (!len)
               return;
            const std::size_t p = (~(sz() + len) + 1) & (a - 1);
            if (p)
               zero_pad(p);
            track(a);
         }

         template <typename T>
         void push(T v)
         {
            auto* p = alloc(sizeof(T));
            std::memcpy(p, &v, sizeof(T));
         }

        public:
         builder() : buf_(256), head_(256) {}

         std::uint32_t create_string(const char* s, std::size_t len)
         {
            pre_align(len + 1, sizeof(std::uint32_t));
            *alloc(1) = 0;  // trailing null
            if (len)
               std::memcpy(alloc(len), s, len);
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(len));
            return static_cast<std::uint32_t>(sz());
         }

         template <typename T>
         std::uint32_t create_vec_scalar(const T* data, std::size_t count)
         {
            const std::size_t body = count * sizeof(T);
            pre_align(body, sizeof(std::uint32_t));
            pre_align(body, sizeof(T));
            if (body)
               std::memcpy(alloc(body), data, body);
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(count));
            return static_cast<std::uint32_t>(sz());
         }

         std::uint32_t create_vec_offsets(const std::uint32_t* offs,
                                          std::size_t          count)
         {
            pre_align(count * sizeof(std::uint32_t),
                      sizeof(std::uint32_t));
            for (std::size_t i = count; i > 0;)
            {
               --i;
               align(sizeof(std::uint32_t));
               push(static_cast<std::uint32_t>(sz()) - offs[i] +
                    std::uint32_t(4));
            }
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(count));
            return static_cast<std::uint32_t>(sz());
         }

         void start_table()
         {
            nfields_   = 0;
            tbl_start_ = static_cast<std::uint32_t>(sz());
         }

         template <typename T>
         void add_scalar(std::uint16_t vt, T val, T def)
         {
            if (val == def)
               return;
            align(sizeof(T));
            push(val);
            fields_[nfields_++] = {static_cast<std::uint32_t>(sz()), vt};
         }

         template <typename T>
         void add_scalar_force(std::uint16_t vt, T val)
         {
            align(sizeof(T));
            push(val);
            fields_[nfields_++] = {static_cast<std::uint32_t>(sz()), vt};
         }

         void add_offset_field(std::uint16_t vt, std::uint32_t off)
         {
            if (!off)
               return;
            align(sizeof(std::uint32_t));
            // uoffset_t stored = target_addr - cell_addr. Push evaluates
            // the value before alloc, so sz() here is the pre-push sz;
            // the cell occupies the next 4 bytes, making its sz_at_addr
            // sz() + 4. target sz = off. value = (sz+4) - off.
            push(static_cast<std::uint32_t>(sz()) - off +
                 std::uint32_t(4));
            fields_[nfields_++] = {static_cast<std::uint32_t>(sz()), vt};
         }

         // Write N bytes at an absolute sz() position (back-to-front).
         void store_at(std::uint32_t at_sz, const void* src,
                       std::size_t n)
         {
            // buf_[cap - at_sz..cap - at_sz + n] = src
            std::memcpy(buf_.data() + buf_.size() - at_sz, src, n);
         }

         std::uint32_t end_table()
         {
            // Reserve the 4-byte soffset_t back-pointer (patched later).
            align(sizeof(std::int32_t));
            push(std::int32_t{0});
            const std::uint32_t tbl_off = static_cast<std::uint32_t>(sz());

            // Vtable size = 4 (own size + table size) + 2 per slot up to
            // max declared voffset.
            std::uint16_t max_vt = 0;
            for (std::size_t i = 0; i < nfields_; ++i)
               if (fields_[i].vt > max_vt)
                  max_vt = fields_[i].vt;
            const std::uint16_t vt_size =
               static_cast<std::uint16_t>(std::max<int>(max_vt + 2, 4));
            const std::uint16_t tbl_obj_sz =
               static_cast<std::uint16_t>(tbl_off - tbl_start_);

            // Allocate vtable bytes and lay them out forward.
            std::uint8_t* vt = alloc(vt_size);
            std::memset(vt, 0, vt_size);
            std::memcpy(vt + 0, &vt_size, 2);
            std::memcpy(vt + 2, &tbl_obj_sz, 2);
            for (std::size_t i = 0; i < nfields_; ++i)
            {
               const std::uint16_t fo = static_cast<std::uint16_t>(
                  tbl_off - fields_[i].off);
               std::memcpy(vt + fields_[i].vt, &fo, 2);
            }

            const std::uint32_t vt_off = static_cast<std::uint32_t>(sz());

            // Patch the soffset_t in the table (at byte address
            // buf_[cap - tbl_off]).
            const std::int32_t soff =
               static_cast<std::int32_t>(vt_off) -
               static_cast<std::int32_t>(tbl_off);
            store_at(tbl_off, &soff, 4);

            return tbl_off;
         }

         std::vector<char> finish(std::uint32_t root)
         {
            pre_align(sizeof(std::uint32_t), min_align_);
            align(sizeof(std::uint32_t));
            push(static_cast<std::uint32_t>(sz()) - root +
                 std::uint32_t(4));
            const std::size_t out_size = sz();
            std::vector<char> out(out_size);
            std::memcpy(out.data(), buf_.data() + head_, out_size);
            return out;
         }
      };

      // Pre-create a record's child offsets, then emit its vtable +
      // table. Returns the table's offset (which the caller either
      // records in a parent table's field slot, or finishes as the
      // root).
      template <typename T>
      std::uint32_t pre_create(builder& b, const T& val);

      template <typename V>
      std::uint32_t pre_create_child(builder& b, const V& val)
      {
         if constexpr (std::is_arithmetic_v<V>)
            return 0;
         else if constexpr (std::is_same_v<V, std::string>)
            return b.create_string(val.data(), val.size());
         else if constexpr (is_optional<V>::value)
         {
            if (val.has_value())
               return pre_create_child<typename V::value_type>(b, *val);
            return 0;
         }
         else if constexpr (is_vector<V>::value)
         {
            using E = typename V::value_type;
            if (val.empty())
               return 0;
            if constexpr (std::is_arithmetic_v<E>)
               return b.create_vec_scalar(val.data(), val.size());
            else if constexpr (std::is_same_v<E, std::string>)
            {
               std::vector<std::uint32_t> offs(val.size());
               for (std::size_t i = 0; i < val.size(); ++i)
                  offs[i] =
                     b.create_string(val[i].data(), val[i].size());
               return b.create_vec_offsets(offs.data(), offs.size());
            }
            else if constexpr (is_table<E>::value)
            {
               std::vector<std::uint32_t> offs(val.size());
               for (std::size_t i = 0; i < val.size(); ++i)
                  offs[i] = pre_create(b, val[i]);
               return b.create_vec_offsets(offs.data(), offs.size());
            }
            else
            {
               static_assert(sizeof(E) == 0,
                             "psio3::flatbuf: unsupported vector element");
            }
         }
         else if constexpr (is_table<V>::value)
         {
            return pre_create(b, val);
         }
         else
         {
            static_assert(sizeof(V) == 0,
                          "psio3::flatbuf: unsupported type");
         }
      }

      template <typename T>
      std::uint32_t pre_create(builder& b, const T& val)
      {
         using R                 = ::psio3::reflect<T>;
         constexpr std::size_t N = R::member_count;
         std::uint32_t offsets[N > 0 ? N : 1]{};
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            ((offsets[Is] =
                 pre_create_child<typename R::template member_type<Is>>(
                    b, val.*(R::template member_pointer<Is>))),
             ...);
         }(std::make_index_sequence<N>{});

         b.start_table();
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (([&]
              {
                 constexpr std::size_t i = N - 1 - Is;
                 using F = typename R::template member_type<i>;
                 const F& fref = val.*(R::template member_pointer<i>);
                 const std::uint16_t vt =
                    static_cast<std::uint16_t>(4 + 2 * i);
                 if constexpr (std::is_same_v<F, bool>)
                    b.add_scalar<std::uint8_t>(
                       vt, fref ? 1 : 0, 0);
                 else if constexpr (std::is_arithmetic_v<F>)
                    b.add_scalar<F>(vt, fref, F{});
                 else if constexpr (is_optional<F>::value)
                 {
                    using Inner = typename F::value_type;
                    if (fref.has_value())
                    {
                       if constexpr (std::is_same_v<Inner, bool>)
                          b.add_scalar_force<std::uint8_t>(
                             vt, *fref ? 1 : 0);
                       else if constexpr (std::is_arithmetic_v<Inner>)
                          b.add_scalar_force<Inner>(vt, *fref);
                       else if (offsets[i])
                          b.add_offset_field(vt, offsets[i]);
                    }
                 }
                 else
                 {
                    if (offsets[i])
                       b.add_offset_field(vt, offsets[i]);
                 }
              }()),
             ...);
         }(std::make_index_sequence<N>{});
         return b.end_table();
      }

      // ── Decoder ───────────────────────────────────────────────────────
      //
      // Buffer layout: [u32 root_offset at byte 0]. Root offset counts
      // forward from byte 0 to the start of the root table. A table
      // starts with an int32 back-pointer to its vtable.

      inline std::uint32_t read_u32(const std::uint8_t* p)
      {
         std::uint32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      inline std::int32_t read_i32(const std::uint8_t* p)
      {
         std::int32_t v;
         std::memcpy(&v, p, 4);
         return v;
      }

      inline std::uint16_t read_u16(const std::uint8_t* p)
      {
         std::uint16_t v;
         std::memcpy(&v, p, 2);
         return v;
      }

      struct table_ptr
      {
         const std::uint8_t* buf;
         std::uint32_t       tbl_pos;  // offset of table in `buf`

         const std::uint8_t* vtable() const
         {
            return buf + tbl_pos - read_i32(buf + tbl_pos);
         }

         // Returns the field's offset relative to tbl_pos (or 0 if
         // not present in the vtable).
         std::uint16_t field_off(std::uint16_t voffset) const
         {
            const std::uint8_t* vt     = vtable();
            const std::uint16_t vtsize = read_u16(vt);
            if (voffset + 2 > vtsize)
               return 0;
            return read_u16(vt + voffset);
         }

         template <typename T>
         T scalar(std::uint16_t voff, T def) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return def;
            T v;
            std::memcpy(&v, buf + tbl_pos + fo, sizeof(T));
            return v;
         }

         std::string string_at(std::uint16_t voff) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return {};
            // Field cell holds a forward offset to the string header.
            const std::uint32_t abs =
               tbl_pos + fo + read_u32(buf + tbl_pos + fo);
            const std::uint32_t len = read_u32(buf + abs);
            return std::string(reinterpret_cast<const char*>(buf + abs + 4),
                               len);
         }

         table_ptr nested_at(std::uint16_t voff) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return {buf, 0};
            const std::uint32_t abs =
               tbl_pos + fo + read_u32(buf + tbl_pos + fo);
            return {buf, abs};
         }

         std::uint32_t vector_pos(std::uint16_t voff) const
         {
            const std::uint16_t fo = field_off(voff);
            if (!fo)
               return 0;
            return tbl_pos + fo + read_u32(buf + tbl_pos + fo);
         }
      };

      template <typename T>
      void unpack_table(table_ptr t, T& out);

      template <typename V>
      void unpack_field(table_ptr t, int idx, V& out)
      {
         const std::uint16_t vt =
            static_cast<std::uint16_t>(4 + 2 * idx);
         if constexpr (std::is_same_v<V, bool>)
            out = t.scalar<std::uint8_t>(vt, 0) != 0;
         else if constexpr (std::is_arithmetic_v<V>)
            out = t.scalar<V>(vt, V{});
         else if constexpr (std::is_same_v<V, std::string>)
            out = t.string_at(vt);
         else if constexpr (is_optional<V>::value)
         {
            using Inner = typename V::value_type;
            if (!t.field_off(vt))
               return;
            if constexpr (std::is_same_v<Inner, bool>)
               out = t.scalar<std::uint8_t>(vt, 0) != 0;
            else if constexpr (std::is_arithmetic_v<Inner>)
               out = t.scalar<Inner>(vt, Inner{});
            else if constexpr (std::is_same_v<Inner, std::string>)
               out = t.string_at(vt);
         }
         else if constexpr (is_vector<V>::value)
         {
            using E = typename V::value_type;
            const std::uint32_t vec_pos = t.vector_pos(vt);
            if (!vec_pos)
               return;
            const std::uint32_t n = read_u32(t.buf + vec_pos);
            out.resize(n);
            if constexpr (std::is_arithmetic_v<E>)
            {
               if (n)
                  std::memcpy(out.data(), t.buf + vec_pos + 4,
                              n * sizeof(E));
            }
            else if constexpr (std::is_same_v<E, std::string>)
            {
               for (std::uint32_t i = 0; i < n; ++i)
               {
                  const std::uint32_t cell = vec_pos + 4 + i * 4;
                  const std::uint32_t sp =
                     cell + read_u32(t.buf + cell);
                  const std::uint32_t slen = read_u32(t.buf + sp);
                  out[i] = std::string(
                     reinterpret_cast<const char*>(t.buf + sp + 4),
                     slen);
               }
            }
            else if constexpr (is_table<E>::value)
            {
               for (std::uint32_t i = 0; i < n; ++i)
               {
                  const std::uint32_t cell = vec_pos + 4 + i * 4;
                  const std::uint32_t sub =
                     cell + read_u32(t.buf + cell);
                  unpack_table<E>(table_ptr{t.buf, sub}, out[i]);
               }
            }
         }
         else if constexpr (is_table<V>::value)
         {
            const table_ptr nested = t.nested_at(vt);
            if (nested.tbl_pos)
               unpack_table<V>(nested, out);
         }
      }

      template <typename T>
      void unpack_table(table_ptr t, T& out)
      {
         using R = ::psio3::reflect<T>;
         [&]<std::size_t... Is>(std::index_sequence<Is...>)
         {
            (unpack_field(t, static_cast<int>(Is),
                          out.*(R::template member_pointer<Is>)),
             ...);
         }(std::make_index_sequence<R::member_count>{});
      }

   }  // namespace detail::flatbuf_impl

   struct flatbuf : format_tag_base<flatbuf>
   {
      using preferred_presentation_category = ::psio3::binary_category;

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend std::vector<char> tag_invoke(decltype(::psio3::encode),
                                          flatbuf, const T& v)
      {
         detail::flatbuf_impl::builder b;
         const auto root = detail::flatbuf_impl::pre_create(b, v);
         return b.finish(root);
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend T tag_invoke(decltype(::psio3::decode<T>), flatbuf, T*,
                          std::span<const char> bytes)
      {
         const auto* buf =
            reinterpret_cast<const std::uint8_t*>(bytes.data());
         const std::uint32_t root_off =
            detail::flatbuf_impl::read_u32(buf);
         T out{};
         detail::flatbuf_impl::unpack_table<T>(
            detail::flatbuf_impl::table_ptr{buf, root_off}, out);
         return out;
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend std::size_t tag_invoke(decltype(::psio3::size_of), flatbuf,
                                    const T& v)
      {
         // One-shot: build the buffer, return its size. No cheaper
         // estimate is available without replicating the pack math.
         const auto out = tag_invoke(::psio3::encode, flatbuf{}, v);
         return out.size();
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend codec_status tag_invoke(decltype(::psio3::validate<T>),
                                     flatbuf, T*,
                                     std::span<const char> bytes) noexcept
      {
         if (bytes.size() < 4)
            return codec_fail("flatbuf: buffer too small", 0, "flatbuf");
         return codec_ok();
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend codec_status
      tag_invoke(decltype(::psio3::validate_strict<T>), flatbuf, T*,
                 std::span<const char> bytes) noexcept
      {
         return tag_invoke(::psio3::validate<T>, flatbuf{}, (T*)nullptr,
                           bytes);
      }

      template <typename T>
         requires detail::flatbuf_impl::is_table<T>::value
      friend std::unique_ptr<T>
      tag_invoke(decltype(::psio3::make_boxed<T>), flatbuf, T*,
                 std::span<const char> bytes) noexcept
      {
         auto v = std::make_unique<T>();
         const auto* buf =
            reinterpret_cast<const std::uint8_t*>(bytes.data());
         const std::uint32_t root_off =
            detail::flatbuf_impl::read_u32(buf);
         detail::flatbuf_impl::unpack_table<T>(
            detail::flatbuf_impl::table_ptr{buf, root_off}, *v);
         return v;
      }
   };

}  // namespace psio3
