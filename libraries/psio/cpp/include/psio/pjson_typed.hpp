#pragma once
//
// psio/pjson_typed.hpp — schema-aware view + canonical fast path.
//
// For a reflected struct T:
//   * pjson_hash_template<T>()   — per-T precomputed hash array.
//   * is_canonical_for<T>(view)  — single memcmp fast-fail check.
//   * typed_pjson_view<T>        — field-by-index access on canonical
//                                   buffers; falls back to find() on
//                                   non-canonical.
//   * to_struct<T>(view)         — materialize T.
//   * from_struct(t)             — encode T → pjson bytes.

#include <psio/format.hpp>
#include <psio/pjson_view.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psio {

   namespace pjson_detail {
      template <typename T>
      inline const std::array<std::uint8_t, reflect<T>::member_count>&
      compute_hash_template() noexcept
      {
         using R = reflect<T>;
         static const auto arr = []() {
            std::array<std::uint8_t, R::member_count> a{};
            [&]<std::size_t... Is>(std::index_sequence<Is...>) {
               ((a[Is] = key_hash8(R::template member_name<Is>)), ...);
            }(std::make_index_sequence<R::member_count>{});
            return a;
         }();
         return arr;
      }
   }

   template <typename T>
   inline const auto& pjson_hash_template() noexcept
   {
      return pjson_detail::compute_hash_template<T>();
   }

   template <typename T>
   inline bool is_canonical_for(pjson_view v) noexcept
   {
      using R = reflect<T>;
      if (v.type() != pjson_view::kind::object) return false;
      if (v.size() < 3) return false;
      // count is the last 2 bytes (u16 LE).
      std::size_t N = static_cast<std::size_t>(v.data()[v.size() - 2]) |
                      (static_cast<std::size_t>(v.data()[v.size() - 1])
                       << 8);
      if (N != R::member_count) return false;
      // hash_table is N bytes, sitting just before slot_table at the
      // tail. slot_table_pos = size - 2 - 4*N; hash_table_pos =
      // slot_table_pos - N.
      std::size_t slot_table_pos = v.size() - 2 - 4 * N;
      std::size_t hash_table_pos = slot_table_pos - N;
      const auto& tmpl = pjson_hash_template<T>();
      return std::memcmp(v.data() + hash_table_pos, tmpl.data(),
                         R::member_count) == 0;
   }

   template <typename T>
   class view<T, pjson_format>
   {
     public:
      using R      = reflect<T>;
      using format = pjson_format;
      static_assert(R::is_reflected,
                    "view<T,pjson_format> requires PSIO_REFLECT(T,...)");

      view() = default;

      static view from_pjson(pjson_view raw) noexcept
      {
         view r;
         r.raw_       = raw;
         r.canonical_ = is_canonical_for<T>(raw);
         return r;
      }

      bool       is_canonical() const noexcept { return canonical_; }
      pjson_view raw() const noexcept { return raw_; }

      template <std::size_t I>
      pjson_view field() const
      {
         if (canonical_) return field_by_index<I>();
         auto v = raw_.find(R::template member_name<I>);
         if (!v)
            throw std::runtime_error("view<T,pjson_format>: field missing");
         return *v;
      }
      template <std::size_t I>
      auto get() const
      {
         using FT = typename R::template member_type<I>;
         return read_as<FT>(field<I>());
      }

      T to_struct() const
      {
         T t{};
         [&]<std::size_t... Is>(std::index_sequence<Is...>) {
            ((t.*R::template member_pointer<Is> = get<Is>()), ...);
         }(std::make_index_sequence<R::member_count>{});
         return t;
      }

     private:
      template <std::size_t I>
      pjson_view field_by_index() const noexcept
      {
         using namespace pjson_detail;
         constexpr std::size_t N = R::member_count;
         std::size_t slot_table_pos   = raw_.size() - 2 - 4 * N;
         std::size_t hash_table_pos   = slot_table_pos - N;
         std::size_t value_data_start = 1;
         std::size_t value_data_size  = hash_table_pos - 1;
         (void)hash_table_pos;
         std::uint32_t s_i =
             read_u32_le(raw_.data() + slot_table_pos + I * 4);
         std::uint32_t off_i = slot_offset(s_i);
         std::uint8_t  ks    = slot_key_size(s_i);
         std::uint32_t off_next;
         if constexpr (I + 1 < N)
            off_next = slot_offset(read_u32_le(
                raw_.data() + slot_table_pos + (I + 1) * 4));
         else
            off_next = static_cast<std::uint32_t>(value_data_size);
         const std::uint8_t* entry      = raw_.data() + value_data_start + off_i;
         std::size_t         entry_size = off_next - off_i;
         std::size_t klen, klen_bytes;
         if (ks != 0xFF) { klen = ks; klen_bytes = 0; }
         else
         {
            std::uint64_t excess;
            klen_bytes = read_varuint62(entry, entry_size, excess);
            klen = 0xFFu + static_cast<std::size_t>(excess);
         }
         std::size_t value_off = klen_bytes + klen;
         return pjson_view{entry + value_off, entry_size - value_off};
      }

      template <typename F>
      struct is_std_vector_ : std::false_type {};
      template <typename E, typename A>
      struct is_std_vector_<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename F>
      struct is_std_optional_ : std::false_type {};
      template <typename U>
      struct is_std_optional_<std::optional<U>> : std::true_type
      {
         using elem = U;
      };

      template <typename F>
      static F read_as(pjson_view v)
      {
         if constexpr (std::is_same_v<F, bool>) return v.as_bool();
         else if constexpr (std::is_integral_v<F>)
            return static_cast<F>(v.as_int64());
         else if constexpr (std::is_floating_point_v<F>)
            return static_cast<F>(v.as_double());
         else if constexpr (std::is_same_v<F, std::string>)
            return std::string(v.as_string());
         else if constexpr (std::is_same_v<F, std::string_view>)
            return v.as_string();
         else if constexpr (is_std_optional_<F>::value)
         {
            // null on the wire → empty optional; otherwise recurse.
            if (v.is_null())
               return F{};
            return F{read_as<typename is_std_optional_<F>::elem>(v)};
         }
         else if constexpr (is_std_vector_<F>::value)
         {
            using E = typename is_std_vector_<F>::elem;
            F out;
            // vector<uint8_t> / vector<std::byte> route through the
            // dedicated t_bytes tag rather than typed-array — bytes
            // are a blob, not a list of small ints; JSON renders
            // base64 rather than [1,2,3,…].
            if constexpr (std::is_same_v<E, std::uint8_t> ||
                          std::is_same_v<E, std::byte>)
            {
               if (v.is_bytes())
               {
                  auto span = v.as_bytes();
                  out.resize(span.size());
                  if (!span.empty())
                     std::memcpy(out.data(), span.data(), span.size());
                  return out;
               }
               // Fall through to array decode for back-compat data
               // emitted before the t_bytes routing landed.
            }
            // Typed-array fast path: bulk-memcpy when the wire form
            // is a typed array whose element matches E.
            if constexpr (
               std::is_same_v<E, std::int8_t>  ||
               std::is_same_v<E, std::int16_t> ||
               std::is_same_v<E, std::int32_t> ||
               std::is_same_v<E, std::int64_t> ||
               std::is_same_v<E, std::uint8_t> ||
               std::is_same_v<E, std::uint16_t> ||
               std::is_same_v<E, std::uint32_t> ||
               std::is_same_v<E, std::uint64_t> ||
               std::is_same_v<E, float>        ||
               std::is_same_v<E, double>)
            {
               if (v.is_typed_array() &&
                   v.typed_array_elem_code() ==
                      pjson_detail::tac_for<E>())
               {
                  std::size_t n = v.count();
                  out.resize(n);
                  if (n)
                     std::memcpy(out.data(), v.data() + 1,
                                 n * sizeof(E));
                  return out;
               }
            }
            //  Fallback: walk a generic array, one element at a time.
            std::size_t n = v.count();
            out.reserve(n);
            for (std::size_t i = 0; i < n; ++i)
               out.push_back(read_as<E>(v.at(i)));
            return out;
         }
         else
            static_assert(sizeof(F) == 0,
                          "view<T,pjson_format>: unsupported field type");
      }

      pjson_view raw_;
      bool       canonical_ = false;
   };

   // Backward-compat alias. New code should prefer view<T, pjson_format>.
   template <typename T>
   using typed_pjson_view = view<T, pjson_format>;

   template <typename F>
   inline std::size_t typed_field_size(const F& v);

   namespace pjson_detail {
      // Local std::vector / std::optional detectors so the typed
      // size/encode dispatch can route them without bouncing through
      // view<T, pjson_format>'s private detectors.
      template <typename F>
      struct typed_is_vector_ : std::false_type {};
      template <typename E, typename A>
      struct typed_is_vector_<std::vector<E, A>> : std::true_type
      {
         using elem = E;
      };

      template <typename F>
      struct typed_is_optional_ : std::false_type {};
      template <typename U>
      struct typed_is_optional_<std::optional<U>> : std::true_type
      {
         using elem = U;
      };

      template <typename E>
      constexpr bool is_typed_array_elem_ =
         std::is_same_v<E, std::int8_t>  ||
         std::is_same_v<E, std::int16_t> ||
         std::is_same_v<E, std::int32_t> ||
         std::is_same_v<E, std::int64_t> ||
         std::is_same_v<E, std::uint8_t> ||
         std::is_same_v<E, std::uint16_t> ||
         std::is_same_v<E, std::uint32_t> ||
         std::is_same_v<E, std::uint64_t> ||
         std::is_same_v<E, float>        ||
         std::is_same_v<E, double>;

      template <typename F>
      inline std::size_t typed_field_size_dispatch(const F& v)
      {
         if constexpr (std::is_same_v<F, bool>) return 1;
         else if constexpr (std::is_integral_v<F>)
            return int_size(static_cast<std::int64_t>(v));
         else if constexpr (std::is_floating_point_v<F>)
         {
            if (!std::isfinite(v)) return 9;
            pjson_number n   = pjson_number::from_double(static_cast<double>(v));
            std::size_t  dec = number_size(n);
            return dec < 9 ? dec : 9;
         }
         else if constexpr (std::is_same_v<F, std::string> ||
                            std::is_same_v<F, std::string_view>)
            return 1u + std::string_view(v).size();
         else if constexpr (typed_is_optional_<F>::value)
         {
            if (!v.has_value())
               return 1;  // null tag
            return typed_field_size_dispatch<
               typename typed_is_optional_<F>::elem>(*v);
         }
         else if constexpr (typed_is_vector_<F>::value)
         {
            using E = typename typed_is_vector_<F>::elem;
            // vector<u8>/vector<std::byte> → t_bytes: 1 tag + N raw bytes.
            if constexpr (std::is_same_v<E, std::uint8_t> ||
                          std::is_same_v<E, std::byte>)
            {
               return 1u + v.size();
            }
            else if constexpr (is_typed_array_elem_<E>)
            {
               // Typed-array body: 1 tag + N*sizeof(E) + 2 count.
               return 1u + v.size() * sizeof(E) + 2u;
            }
            else
            {
               // Generic-array body: tag + Σ(child sizes) + 4*N slot
               // table + 2 count.
               std::size_t total = 1u + 4u * v.size() + 2u;
               for (const auto& x : v)
                  total += typed_field_size_dispatch<E>(x);
               return total;
            }
         }
         else
            static_assert(sizeof(F) == 0,
                          "from_struct: unsupported field type");
      }
   }

   template <typename F>
   inline std::size_t typed_field_size(const F& v)
   {
      return pjson_detail::typed_field_size_dispatch(v);
   }

   namespace pjson_detail {
      template <typename F>
      inline std::size_t typed_field_encode(std::uint8_t* dst,
                                            std::size_t   pos,
                                            const F&      v)
      {
         if constexpr (std::is_same_v<F, bool>)
         {
            dst[pos] = static_cast<std::uint8_t>(
                (t_bool << 4) | (v ? 1u : 0u));
            return 1;
         }
         else if constexpr (std::is_integral_v<F>)
            return encode_int64_at(dst, pos,
                                   static_cast<std::int64_t>(v));
         else if constexpr (std::is_floating_point_v<F>)
            return encode_double_at(dst, pos, static_cast<double>(v));
         else if constexpr (std::is_same_v<F, std::string> ||
                            std::is_same_v<F, std::string_view>)
            return encode_string_at(dst, pos, std::string_view(v));
         else if constexpr (typed_is_optional_<F>::value)
         {
            if (!v.has_value())
            {
               dst[pos] = static_cast<std::uint8_t>(t_null << 4);
               return 1;
            }
            return typed_field_encode<typename typed_is_optional_<F>::elem>(
               dst, pos, *v);
         }
         else if constexpr (typed_is_vector_<F>::value)
         {
            using E = typename typed_is_vector_<F>::elem;
            // vector<u8>/vector<std::byte> → t_bytes (single memcpy,
            // no count tail; consumers see "binary blob" rather than
            // "list of small ints").
            if constexpr (std::is_same_v<E, std::uint8_t> ||
                          std::is_same_v<E, std::byte>)
            {
               dst[pos] = static_cast<std::uint8_t>(t_bytes << 4);
               if (!v.empty())
                  std::memcpy(dst + pos + 1,
                              reinterpret_cast<const std::uint8_t*>(
                                 v.data()),
                              v.size());
               return 1u + v.size();
            }
            else if constexpr (is_typed_array_elem_<E>)
            {
               // Bulk typed-array fast path — single memcpy per
               // vector regardless of length.
               return encode_typed_array_at<E>(
                  dst, pos,
                  std::span<const E>{v.data(), v.size()});
            }
            else
            {
               // Generic-array form: [tag][value_data][slot[N]][count].
               std::size_t start = pos;
               dst[pos++] = static_cast<std::uint8_t>(t_array << 4);
               std::size_t N  = v.size();
               std::size_t vd = 0;
               for (const auto& x : v)
                  vd += typed_field_size_dispatch<E>(x);
               std::size_t value_data_start = pos;
               std::size_t slot_table_pos   = value_data_start + vd;
               std::size_t count_pos        = slot_table_pos + 4 * N;
               for (std::size_t i = 0; i < N; ++i)
               {
                  std::uint32_t off =
                     static_cast<std::uint32_t>(pos - value_data_start);
                  write_u32_le(dst + slot_table_pos + i * 4,
                               pack_slot(off, 0));
                  pos += typed_field_encode<E>(dst, pos, v[i]);
               }
               dst[count_pos]     =
                  static_cast<std::uint8_t>(N & 0xFF);
               dst[count_pos + 1] =
                  static_cast<std::uint8_t>((N >> 8) & 0xFF);
               return count_pos + 2 - start;
            }
         }
      }
   }

   // Compile-time-derivable size of T's pjson encoding for a given t.
   // (Used by the in-place writer to size the buffer up front.)
   template <typename T>
   inline std::size_t pjson_encoded_size(const T& t)
   {
      using R = reflect<T>;
      std::size_t value_data_size = 0;
      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         ((value_data_size +=
              R::template member_name<Is>.size() +
              typed_field_size(t.*R::template member_pointer<Is>)),
          ...);
      }(std::make_index_sequence<R::member_count>{});
      std::size_t N = R::member_count;
      // Tail-indexed: 1 tag + value_data + N hash + 4N slot + 2 count.
      return 1 + value_data_size + N + 4 * N + 2;
   }

   // In-place encoder. Tail-indexed: writes value_data first, then
   // appends hash[N], slot[N], count u16. The hash and slot tables
   // are filled IN PLACE during the field walk (their final positions
   // are known from the pre-computed value_data size).
   template <typename T>
   inline void to_pjson(const T& t, std::vector<std::uint8_t>& out)
   {
      using R = reflect<T>;
      static_assert(R::is_reflected,
                    "to_pjson requires PSIO_REFLECT(T,...)");
      std::size_t total = pjson_encoded_size(t);
      out.resize(total);

      constexpr std::size_t N = R::member_count;
      std::size_t pos = 0;
      out[pos++] = static_cast<std::uint8_t>(pjson_detail::t_object << 4);
      std::size_t value_data_start = pos;
      std::size_t value_data_size  = total - 1 - N - 4 * N - 2;
      std::size_t hash_table_pos   = value_data_start + value_data_size;
      std::size_t slot_table_pos   = hash_table_pos + N;
      std::size_t count_pos        = slot_table_pos + 4 * N;

      const auto& tmpl = pjson_hash_template<T>();
      std::memcpy(out.data() + hash_table_pos, tmpl.data(), N);

      [&]<std::size_t... Is>(std::index_sequence<Is...>) {
         ((pos = [&] {
             constexpr auto name = R::template member_name<Is>;
             std::uint32_t  off  =
                 static_cast<std::uint32_t>(pos - value_data_start);
             std::uint8_t ks =
                 name.size() < 0xFFu
                     ? static_cast<std::uint8_t>(name.size())
                     : static_cast<std::uint8_t>(0xFFu);
             pjson_detail::write_u32_le(
                 out.data() + slot_table_pos + Is * 4,
                 pjson_detail::pack_slot(off, ks));
             std::memcpy(out.data() + pos, name.data(), name.size());
             pos += name.size();
             pos += pjson_detail::typed_field_encode(
                 out.data(), pos,
                 t.*R::template member_pointer<Is>);
             return pos;
          }()),
          ...);
      }(std::make_index_sequence<R::member_count>{});

      out[count_pos]     = static_cast<std::uint8_t>(N & 0xFF);
      out[count_pos + 1] = static_cast<std::uint8_t>((N >> 8) & 0xFF);
   }

   // Returning form. Allocates a fresh vector each call. The in-place
   // `to_pjson(t, out&)` form is preferred for hot loops that can
   // reuse a buffer.
   template <typename T>
   inline std::vector<std::uint8_t> from_struct(const T& t)
   {
      std::vector<std::uint8_t> out;
      to_pjson(t, out);
      return out;
   }

}  // namespace psio
