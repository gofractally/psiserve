#pragma once
#include <psio/detail/run_detector.hpp>
#include <psio/reflect.hpp>
#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/detail/layout.hpp>
#include <psio/ext_int.hpp>
#include <psio/stream.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace psio
{
   // Forward declarations used by compute_bin_size below.
   namespace bin_detail
   {
      struct bin_size_cache;
   }
   template <typename T>
   std::uint32_t compute_bin_size(const T& obj, bin_detail::bin_size_cache& cache);
   template <typename S>
   void to_bin(std::string_view sv, S& stream);

   template <typename S>
   void to_bin(const std::string& s, S& stream);

   template <typename T, typename S>
   void to_bin(const std::vector<T>& obj, S& stream);

   template <typename T, typename S>
   void to_bin(const std::optional<T>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_bin(const std::variant<Ts...>& obj, S& stream);

   template <typename... Ts, typename S>
   void to_bin(const std::tuple<Ts...>& obj, S& stream);

   template <typename T, typename S>
   void to_bin(const T& obj, S& stream);

   template <typename S>
   void varuint32_to_bin(uint64_t val, S& stream)
   {
      check(!(val >> 32), stream_error::varuint_too_big);
      do
      {
         uint8_t b = val & 0x7f;
         val >>= 7;
         b |= ((val > 0) << 7);
         stream.write(b);
      } while (val);
   }

   // signed leb128 encoding
   template <typename S>
   void sleb64_to_bin(int64_t val, S& stream)
   {
      bool done = false;
      while (!done)
      {
         uint8_t b = val & 0x7f;
         done      = (val >> 6) == (val >> 7);
         val >>= 7;
         b |= (!done << 7);
         stream.write(b);
      }
   }

   inline void push_varuint32(std::vector<char>& bin, uint32_t v)
   {
      vector_stream st{bin};
      varuint32_to_bin(v, st);
   }

   template <typename S>
   void to_bin(std::string_view sv, S& stream)
   {
      varuint32_to_bin(sv.size(), stream);
      stream.write(sv.data(), sv.size());
   }

   template <typename S>
   void to_bin(const std::string& s, S& stream)
   {
      to_bin(std::string_view{s}, stream);
   }

   template <typename T, typename S>
   void to_bin_range(const T& obj, S& stream)
   {
      varuint32_to_bin(obj.size(), stream);
      for (auto& x : obj)
      {
         to_bin(x, stream);
      }
   }

   template <typename T, std::size_t N, typename S>
   void to_bin(const T (&obj)[N], S& stream)
   {
      varuint32_to_bin(N, stream);
      if constexpr (has_bitwise_serialization<T>())
      {
         stream.write(reinterpret_cast<const char*>(&obj), N * sizeof(T));
      }
      else
      {
         for (auto& x : obj)
         {
            to_bin(x, stream);
         }
      }
   }

   // ── Bit types ─────────────────────────────────────────────────────────────
   //
   // bitvector is auto-handled via has_bitwise_serialization (memcpy path).
   // bitlist needs explicit varuint-prefix encoding.

   template <std::size_t MaxN, typename S>
   void to_bin(const bitlist<MaxN>& v, S& stream)
   {
      varuint32_to_bin(static_cast<std::uint32_t>(v.size()), stream);
      auto data = v.bytes();
      if (!data.empty())
         stream.write(reinterpret_cast<const char*>(data.data()), data.size());
   }

   template <std::size_t N, typename S>
   void to_bin(const std::bitset<N>& bs, S& stream)
   {
      std::uint8_t buf[(N + 7) / 8];
      pack_bitset_bytes(bs, buf);
      stream.write(reinterpret_cast<const char*>(buf), (N + 7) / 8);
   }

   template <typename S>
   void to_bin(const std::vector<bool>& v, S& stream)
   {
      varuint32_to_bin(static_cast<std::uint32_t>(v.size()), stream);
      auto packed = pack_vector_bool(v);
      if (!packed.empty())
         stream.write(reinterpret_cast<const char*>(packed.data()), packed.size());
   }

   // ── Bounded collections (delegate to std::vector/std::string path) ────────

   template <typename T, std::size_t N, typename S>
   void to_bin(const bounded_list<T, N>& val, S& stream)
   {
      to_bin(val.storage(), stream);
   }

   template <std::size_t N, typename S>
   void to_bin(const bounded_string<N>& val, S& stream)
   {
      to_bin(val.storage(), stream);
   }

   template <typename T, typename S>
   void to_bin(const std::vector<T>& obj, S& stream)
   {
      varuint32_to_bin(obj.size(), stream);
      if constexpr (has_bitwise_serialization<T>())
      {
         if (!obj.empty())
            stream.write(reinterpret_cast<const char*>(obj.data()), obj.size() * sizeof(T));
      }
      else
      {
         for (auto& x : obj)
         {
            to_bin(x, stream);
         }
      }
   }

   template <typename... Ts, typename S>
   void to_bin(const std::variant<Ts...>& obj, S& stream)
   {
      varuint32_to_bin(obj.index(), stream);
      std::visit([&](auto& x) { to_bin(x, stream); }, obj);
   }

   template <typename S>
   void to_bin(const input_stream& obj, S& stream)
   {
      varuint32_to_bin(obj.end - obj.pos, stream);
      stream.write(obj.pos, obj.end - obj.pos);
   }

   template <typename First, typename Second, typename S>
   void to_bin(const std::pair<First, Second>& obj, S& stream)
   {
      to_bin(obj.first, stream);
      return to_bin(obj.second, stream);
   }

   template <typename T, typename S>
   void to_bin(const std::optional<T>& obj, S& stream)
   {
      to_bin(obj.has_value(), stream);
      if (obj)
         to_bin(*obj, stream);
   }

   template <int i, typename T, typename S>
   void to_bin_tuple(const T& obj, S& stream)
   {
      if constexpr (i < std::tuple_size_v<T>)
      {
         to_bin(std::get<i>(obj), stream);
         to_bin_tuple<i + 1>(obj, stream);
      }
   }

   template <typename... Ts, typename S>
   void to_bin(const std::tuple<Ts...>& obj, S& stream)
   {
      return to_bin_tuple<0>(obj, stream);
   }

   template <typename T, std::size_t N, typename S>
   void to_bin(const std::array<T, N>& obj, S& stream)
   {
      for (const T& elem : obj)
      {
         to_bin(elem, stream);
      }
   }

#ifndef PSIO_BIN_DETAIL_TRAITS_
#define PSIO_BIN_DETAIL_TRAITS_
   namespace bin_detail
   {
      // Local traits for types not already covered by psio::is_std_vector /
      // psio::is_std_optional (defined in reflect.hpp).

      template <typename T>
      struct is_std_variant : std::false_type
      {
      };
      template <typename... Us>
      struct is_std_variant<std::variant<Us...>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_tuple : std::false_type
      {
      };
      template <typename... Us>
      struct is_std_tuple<std::tuple<Us...>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_array : std::false_type
      {
      };
      template <typename U, std::size_t N>
      struct is_std_array<std::array<U, N>> : std::true_type
      {
      };

      template <typename T>
      struct is_std_pair : std::false_type
      {
      };
      template <typename F, typename S>
      struct is_std_pair<std::pair<F, S>> : std::true_type
      {
      };

      // Byte length of the varuint32 encoding of `v`.
      constexpr std::uint32_t varuint32_encoded_size(std::uint32_t v)
      {
         std::uint32_t n = 1;
         while (v >= 0x80u)
         {
            ++n;
            v >>= 7;
         }
         return n;
      }

      // Pre-computed size cache: one u32 per extensible-struct content size,
      // populated during compute_bin_size and consumed by the write pass.
      struct bin_size_cache
      {
         std::vector<std::uint32_t> slots;
         std::size_t                consumed = 0;

         std::size_t alloc_slot()
         {
            slots.push_back(0);
            return slots.size() - 1;
         }
         void set(std::size_t slot, std::uint32_t v) { slots[slot] = v; }
         std::uint32_t consume() { return slots[consumed++]; }
         void          reset_consume() { consumed = 0; }
      };

      // Stream wrapper that carries a cache pointer. to_bin's extensible-struct
      // branch detects this wrapper via the HasBinSizeCache concept and uses
      // the pre-computed content size instead of re-walking with a size_stream.
      template <typename Inner>
      struct cached_stream
      {
         Inner*          inner;
         bin_size_cache* cache;

         static constexpr bool psio_bin_has_cache = true;

         void write(char ch) { inner->write(ch); }
         void write(const void* src, std::size_t n) { inner->write(src, n); }
         void write(const char* src, std::size_t n) { inner->write(src, n); }
         template <typename T>
         void write_raw(const T& v)
         {
            inner->write_raw(v);
         }
         std::size_t written() const { return inner->written(); }
      };

      template <typename S>
      concept HasBinSizeCache = requires { S::psio_bin_has_cache; };
   }  // namespace bin_detail
#endif

   namespace bin_detail
   {
      // Compile-time: does the LAST declared member of T have type
      // std::optional<...> ? If false, no trimming ever happens, so the
      // num_present scan can be skipped (n == N_members always).
      template <typename T>
      consteval bool last_field_is_optional()
      {
         return psio::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [](auto... member)
             {
                if constexpr (sizeof...(member) == 0)
                   return false;
                else
                {
                   constexpr bool opts[] = {is_std_optional_v<
                       std::remove_cvref_t<decltype(std::declval<T>().*member)>>...};
                   return opts[sizeof...(member) - 1];
                }
             });
      }

      // Compile-time number of reflected data members of T.
      template <typename T>
      consteval int num_data_members()
      {
         return psio::apply_members((typename reflect<T>::data_members*)nullptr,
                                    [](auto... member)
                                    { return static_cast<int>(sizeof...(member)); });
      }

      // Shared struct-layout predicates live in psio::layout_detail; see
      // psio/detail/layout.hpp. Pack_bin gates memcpy on DWNC *plus* the
      // generic layout match — the DWNC flag is specific to pack_bin's
      // extensible-wire semantics and unused by SSZ/bincode/borsh.
      template <typename T>
      consteval bool is_bitwise_dwnc_struct()
      {
         if constexpr (!Reflected<T>)
            return false;
         else if constexpr (!reflect<T>::definitionWillNotChange)
            return false;
         else
            return layout_detail::is_memcpy_layout_struct<T>();
      }


      // For a reflected extensible (non-DWNC) struct: number of fields to
      // serialize — equal to (index of last field that has a value) + 1.
      // Trailing absent std::optional fields are trimmed. When the last
      // field is not optional, returns the compile-time member count with
      // no runtime walk.
      template <typename T>
      inline int pack_bin_num_present(const T& obj)
      {
         if constexpr (!last_field_is_optional<T>())
         {
            (void)obj;
            return num_data_members<T>();
         }
         else
         {
            int last_present = 0;
            int i            = 0;
            psio::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member)
                {
                   auto process = [&](auto m)
                   {
                      ++i;
                      using FT = std::remove_cvref_t<decltype(obj.*m)>;
                      if constexpr (is_std_optional_v<FT>)
                      {
                         if ((obj.*m).has_value())
                            last_present = i;
                      }
                      else
                      {
                         last_present = i;
                      }
                   };
                   (process(member), ...);
                });
            return last_present;
         }
      }

      // Write the first n declared fields of `obj`. If there's any
      // batchable run (>=2 contiguous bitwise fields), use the run walker;
      // otherwise fall back to plain per-field to_bin (avoids walker's
      // compile-time abstraction overhead for types that can't batch).
      template <typename T, typename S>
      inline void pack_bin_write_first_n(const T& obj, int n, S& stream)
      {
         if constexpr (run_detail::has_batchable_run<T>())
         {
            auto op = [&](auto const& val) { to_bin(val, stream); };
            run_detail::walk_with_runs(obj, stream, n, op);
         }
         else
         {
            int i = 0;
            psio::apply_members(
                (typename reflect<T>::data_members*)nullptr,
                [&](auto... member)
                {
                   auto process = [&](auto m)
                   {
                      if (i < n)
                         to_bin(obj.*m, stream);
                      ++i;
                   };
                   (process(member), ...);
                });
         }
      }

      // Write all fields of a DWNC reflected struct (n == N).
      template <typename T, typename S>
      inline void pack_bin_write_all(const T& obj, S& stream)
      {
         if constexpr (run_detail::has_batchable_run<T>())
         {
            auto op = [&](auto const& val) { to_bin(val, stream); };
            run_detail::walk_with_runs(
                obj, stream, static_cast<int>(std::tuple_size_v<struct_tuple_t<T>>), op);
         }
         else
         {
            psio::apply_members((typename reflect<T>::data_members*)nullptr,
                                [&](auto... member) { (to_bin(obj.*member, stream), ...); });
         }
      }
   }  // namespace bin_detail

   template <typename T, typename S>
   void to_bin(const T& obj, S& stream)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         stream.write(reinterpret_cast<const char*>(&obj), sizeof(obj));
      }
      else if constexpr (bin_detail::is_bitwise_dwnc_struct<T>())
      {
         // Compile-time verified: DWNC struct, all members bitwise, zero
         // padding. The in-memory layout matches the wire byte-for-byte,
         // so one stream.write writes the whole struct.
         stream.write(reinterpret_cast<const char*>(&obj), sizeof(obj));
      }
      else if constexpr (reflect<T>::definitionWillNotChange)
      {
         // Fixed schema (DWNC): walk members with compile-time run detection.
         // Contiguous bitwise fields get batched into one stream.write.
         bin_detail::pack_bin_write_all(obj, stream);
      }
      else if constexpr (bin_detail::HasBinSizeCache<S>)
      {
         // Extensible + cached write path: the top-level convert_to_bin
         // already computed our content size in the cache. Consume it to
         // avoid re-walking with a size_stream.
         std::uint32_t content_size = stream.cache->consume();
         varuint32_to_bin(content_size, stream);
         int n = bin_detail::pack_bin_num_present(obj);
         bin_detail::pack_bin_write_first_n(obj, n, stream);
      }
      else
      {
         // Extensible + uncached path (direct to_bin users, unit tests, etc.):
         // compute size locally with a size_stream. Slower but correct.
         int               n = bin_detail::pack_bin_num_present(obj);
         size_stream       ss;
         bin_detail::pack_bin_write_first_n(obj, n, ss);
         varuint32_to_bin(ss.size, stream);
         bin_detail::pack_bin_write_first_n(obj, n, stream);
      }
   }

   // ── compute_bin_size: one walk computes total size + populates cache ────

   template <typename T>
   std::uint32_t compute_bin_size(const T& obj, bin_detail::bin_size_cache& cache)
   {
      if constexpr (has_bitwise_serialization<T>())
      {
         return sizeof(T);
      }
      else if constexpr (std::is_same_v<T, std::string> ||
                         std::is_same_v<T, std::string_view>)
      {
         std::uint32_t len = static_cast<std::uint32_t>(obj.size());
         return bin_detail::varuint32_encoded_size(len) + len;
      }
      else if constexpr (std::is_same_v<T, std::vector<bool>>)
      {
         // Must precede the is_std_vector_v branch — vector<bool> is a packed
         // specialization with distinct wire format.
         std::uint32_t bit_count  = static_cast<std::uint32_t>(obj.size());
         std::uint32_t byte_count = (bit_count + 7) / 8;
         return bin_detail::varuint32_encoded_size(bit_count) + byte_count;
      }
      else if constexpr (is_std_vector_v<T>)
      {
         using E             = typename T::value_type;
         std::uint32_t count = static_cast<std::uint32_t>(obj.size());
         std::uint32_t sz    = bin_detail::varuint32_encoded_size(count);
         if constexpr (has_bitwise_serialization<E>())
         {
            sz += count * sizeof(E);
         }
         else
         {
            for (auto const& e : obj)
               sz += compute_bin_size(e, cache);
         }
         return sz;
      }
      else if constexpr (is_bounded_list_v<T> || is_bounded_string_v<T>)
      {
         // Wire format mirrors std::vector/std::string: varuint count + data.
         return compute_bin_size(obj.storage(), cache);
      }
      else if constexpr (is_bitlist_v<T>)
      {
         std::uint32_t bit_count  = static_cast<std::uint32_t>(obj.size());
         std::uint32_t byte_count = (bit_count + 7) / 8;
         return bin_detail::varuint32_encoded_size(bit_count) + byte_count;
      }
      else if constexpr (is_std_bitset_v<T>)
      {
         return static_cast<std::uint32_t>((T{}.size() + 7) / 8);
      }
      else if constexpr (is_std_optional_v<T>)
      {
         return 1u + (obj.has_value() ? compute_bin_size(*obj, cache) : 0u);
      }
      else if constexpr (bin_detail::is_std_variant<T>::value)
      {
         std::uint32_t idx_sz =
             bin_detail::varuint32_encoded_size(static_cast<std::uint32_t>(obj.index()));
         std::uint32_t inner =
             std::visit([&](auto const& x) { return compute_bin_size(x, cache); }, obj);
         return idx_sz + inner;
      }
      else if constexpr (bin_detail::is_std_tuple<T>::value)
      {
         std::uint32_t sz = 0;
         std::apply(
             [&](auto const&... elems)
             { ((sz += compute_bin_size(elems, cache)), ...); },
             obj);
         return sz;
      }
      else if constexpr (bin_detail::is_std_array<T>::value)
      {
         using E          = typename T::value_type;
         std::uint32_t sz = 0;
         if constexpr (has_bitwise_serialization<E>())
         {
            sz = static_cast<std::uint32_t>(std::tuple_size_v<T> * sizeof(E));
         }
         else
         {
            for (auto const& e : obj)
               sz += compute_bin_size(e, cache);
         }
         return sz;
      }
      else if constexpr (bin_detail::is_std_pair<T>::value)
      {
         return compute_bin_size(obj.first, cache) + compute_bin_size(obj.second, cache);
      }
      else if constexpr (reflect<T>::definitionWillNotChange)
      {
         // DWNC struct: sum of field sizes, no prefix.
         std::uint32_t sz = 0;
         psio::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member) { ((sz += compute_bin_size(obj.*member, cache)), ...); });
         return sz;
      }
      else
      {
         // Extensible reflected struct. Allocate a slot, compute content size
         // across first-n-present fields, store it, and return the total
         // (varuint prefix width + content).
         std::size_t   slot         = cache.alloc_slot();
         int           last_present = 0;
         std::uint32_t content_size = 0;
         std::uint32_t running      = 0;
         int           i            = 0;
         psio::apply_members(
             (typename reflect<T>::data_members*)nullptr,
             [&](auto... member)
             {
                auto process = [&](auto m)
                {
                   using FT = std::remove_cvref_t<decltype(obj.*m)>;
                   std::uint32_t field_sz = compute_bin_size(obj.*m, cache);
                   running += field_sz;
                   ++i;
                   if constexpr (is_std_optional_v<FT>)
                   {
                      if ((obj.*m).has_value())
                      {
                         last_present = i;
                         content_size = running;
                      }
                   }
                   else
                   {
                      last_present = i;
                      content_size = running;
                   }
                };
                (process(member), ...);
             });
         cache.set(slot, content_size);
         return bin_detail::varuint32_encoded_size(content_size) + content_size;
      }
   }

   template <typename T>
   void convert_to_bin(const T& t, std::vector<char>& bin)
   {
      if constexpr (reflect<T>::definitionWillNotChange ||
                    has_bitwise_serialization<T>())
      {
         // DWNC top-level: no extensible content prefixes to cache. Sum sizes
         // directly (size_stream inlines to arithmetic for fixed types).
         size_stream ss;
         to_bin(t, ss);
         auto orig_size = bin.size();
         bin.resize(orig_size + ss.size);
         fixed_buf_stream fbs(bin.data() + orig_size, ss.size);
         to_bin(t, fbs);
         check(fbs.pos == fbs.end, stream_error::underrun);
      }
      else
      {
         // Extensible top-level: pre-compute sizes into a cache, consume on
         // write. Reuse a thread-local cache vector across calls to avoid
         // per-call heap allocation.
         static thread_local bin_detail::bin_size_cache cache;
         cache.slots.clear();
         cache.consumed = 0;
         std::uint32_t total = compute_bin_size(t, cache);

         auto orig_size = bin.size();
         bin.resize(orig_size + total);
         fixed_buf_stream                            fbs(bin.data() + orig_size, total);
         bin_detail::cached_stream<fixed_buf_stream> cs{&fbs, &cache};
         to_bin(t, cs);
         check(fbs.pos == fbs.end, stream_error::underrun);
      }
   }

   template <typename T>
   std::vector<char> convert_to_bin(const T& t)
   {
      std::vector<char> result;
      convert_to_bin(t, result);
      return result;
   }

}  // namespace psio
