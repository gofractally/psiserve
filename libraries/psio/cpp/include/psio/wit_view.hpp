#pragma once

// wit_view.hpp — Canonical ABI (WIT) format tag for the unified view<T, Fmt> framework
//
// Provides a complete serialization format alongside fracpack and flatbuf:
//
//   Pack:         auto buf = psio::wit::pack(person);
//   Unpack:       auto p   = psio::wit::unpack<Person>(buf);
//   View:         auto v   = psio::view<Person, psio::wit>::from_buffer(buf);
//                 v.name()   // → std::string_view  (zero-copy)
//                 v.age()    // → uint32_t           (read from buffer)
//   Validate:     bool ok  = psio::wit::validate<Person>(buf);
//   Modify:       auto m   = psio::wit_mut<Person>(buf);
//                 m.age() = 31;  // in-place scalar write
//   Canonicalize: psio::wit::canonicalize<Person>(buf);
//
// Wire format is the Component Model canonical ABI:
//   - Fixed-layout records (fields at compile-time offsets)
//   - Strings/lists as (i32 ptr, i32 len) pointing into the same buffer
//   - Deterministic: same value always produces the same bytes

#include <psio/view.hpp>
#include <psio/wit_gen.hpp>
#include <psio/canonical_abi.hpp>

#include <cstring>
#include <optional>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace psio {

   // ── wit_ptr — navigates canonical ABI buffers ────────────────────────────
   //
   // Carries the buffer base (for resolving absolute string/list offsets)
   // and a pointer to the current record within that buffer.

   struct wit_ptr {
      const uint8_t* base = nullptr;  // buffer start
      const uint8_t* rec  = nullptr;  // record data within buffer

      explicit operator bool() const { return rec != nullptr; }
   };

   // ── wit_mut_ptr — mutable version for in-place modification ──────────────

   struct wit_mut_ptr {
      uint8_t* base = nullptr;
      uint8_t* rec  = nullptr;

      explicit operator bool() const { return rec != nullptr; }
   };

   // ── wit_mut_ref — assignable proxy reference for in-buffer scalars ───────

   template <typename T>
   class wit_mut_ref {
      uint8_t* ptr_;

   public:
      explicit wit_mut_ref(uint8_t* p) : ptr_(p) {}

      operator T() const {
         T v;
         std::memcpy(&v, ptr_, sizeof(T));
         return v;
      }

      wit_mut_ref& operator=(T v) {
         std::memcpy(ptr_, &v, sizeof(T));
         return *this;
      }
   };

   template <>
   class wit_mut_ref<bool> {
      uint8_t* ptr_;

   public:
      explicit wit_mut_ref(uint8_t* p) : ptr_(p) {}
      operator bool() const { return *ptr_ != 0; }
      wit_mut_ref& operator=(bool v) {
         *ptr_ = static_cast<uint8_t>(v);
         return *this;
      }
   };

   namespace detail_wit {

      template <typename T>
      T read_scalar(const uint8_t* p) {
         T v;
         std::memcpy(&v, p, sizeof(T));
         return v;
      }

   } // namespace detail_wit

   // ══════════════════════════════════════════════════════════════════════════
   // struct wit — Canonical ABI format tag for view<T, wit>
   //
   // Satisfies the Format concept: ptr_t, root<T>(), field<T,N>().
   // Canonical ABI buffers are flat, fixed-layout: field N of type T
   // is at offset canonical_field_offset_v<T, N> from the record start.
   // ══════════════════════════════════════════════════════════════════════════

   struct wit {
      using ptr_t = wit_ptr;

      template <typename T>
      static ptr_t root(const void* buf) {
         auto* p = static_cast<const uint8_t*>(buf);
         return {p, p};
      }

      template <typename T, size_t N>
      static auto field(ptr_t p) {
         using F = std::tuple_element_t<N, psio::struct_tuple_t<T>>;
         constexpr uint32_t off = canonical_field_offset_v<T, N>;
         const uint8_t* fp = p.rec + off;

         if constexpr (std::is_same_v<F, bool>)
            return *fp != 0;
         else if constexpr (std::is_enum_v<F>) {
            using U = std::underlying_type_t<F>;
            return static_cast<F>(detail_wit::read_scalar<U>(fp));
         }
         else if constexpr (std::is_arithmetic_v<F>)
            return detail_wit::read_scalar<F>(fp);
         else if constexpr (psio::detail::is_std_string_ct<F>::value) {
            uint32_t str_off = detail_wit::read_scalar<uint32_t>(fp);
            uint32_t str_len = detail_wit::read_scalar<uint32_t>(fp + 4);
            return std::string_view(
               reinterpret_cast<const char*>(p.base + str_off), str_len);
         }
         else if constexpr (psio::detail::is_std_vector_ct<F>::value) {
            using E = typename psio::detail::vector_elem_ct<F>::type;
            uint32_t arr_off = detail_wit::read_scalar<uint32_t>(fp);
            uint32_t count   = detail_wit::read_scalar<uint32_t>(fp + 4);
            return psio::vec_view<E, wit>(p.base, arr_off, count);
         }
         else if constexpr (psio::detail::is_std_optional_ct<F>::value) {
            using E = typename psio::detail::optional_elem_ct<F>::type;
            constexpr uint32_t ea = canonical_align_v<E>;
            constexpr uint32_t payload_off = (1 + ea - 1) & ~(ea - 1);
            uint8_t disc = *fp;
            if (!disc) {
               if constexpr (std::is_same_v<E, bool>)
                  return std::optional<bool>{};
               else if constexpr (std::is_enum_v<E>)
                  return std::optional<E>{};
               else if constexpr (std::is_arithmetic_v<E>)
                  return std::optional<E>{};
               else if constexpr (psio::detail::is_std_string_ct<E>::value)
                  return std::optional<std::string_view>{};
               else if constexpr (psio::Reflected<E>)
                  return std::optional<psio::view<E, wit>>{};
               else
                  static_assert(sizeof(E) == 0, "wit::field: unsupported optional payload");
            }
            const uint8_t* payload = fp + payload_off;
            if constexpr (std::is_same_v<E, bool>)
               return std::optional<bool>(*payload != 0);
            else if constexpr (std::is_enum_v<E>)
               return std::optional<E>(static_cast<E>(
                  detail_wit::read_scalar<std::underlying_type_t<E>>(payload)));
            else if constexpr (std::is_arithmetic_v<E>)
               return std::optional<E>(detail_wit::read_scalar<E>(payload));
            else if constexpr (psio::detail::is_std_string_ct<E>::value) {
               uint32_t so = detail_wit::read_scalar<uint32_t>(payload);
               uint32_t sl = detail_wit::read_scalar<uint32_t>(payload + 4);
               return std::optional<std::string_view>(
                  std::string_view(reinterpret_cast<const char*>(p.base + so), sl));
            }
            else if constexpr (psio::Reflected<E>) {
               return std::optional<psio::view<E, wit>>(
                  psio::view<E, wit>(wit_ptr{p.base, payload}));
            }
            else {
               static_assert(sizeof(E) == 0, "wit::field: unsupported optional payload");
            }
         }
         else if constexpr (psio::is_std_variant_v<F>) {
            constexpr size_t VN = std::variant_size_v<F>;
            constexpr uint32_t disc_size = VN <= 256 ? 1 : VN <= 65536 ? 2 : 4;
            constexpr uint32_t max_pa = []<size_t... Js>(std::index_sequence<Js...>) {
               uint32_t m = 1;
               ((m = std::max(m, canonical_align_v<std::variant_alternative_t<Js, F>>)), ...);
               return m;
            }(std::make_index_sequence<VN>{});
            constexpr uint32_t payload_off = (disc_size + max_pa - 1) & ~(max_pa - 1);

            uint32_t disc;
            if constexpr (disc_size == 1)
               disc = *fp;
            else if constexpr (disc_size == 2)
               disc = detail_wit::read_scalar<uint16_t>(fp);
            else
               disc = detail_wit::read_scalar<uint32_t>(fp);

            const uint8_t* payload = fp + payload_off;
            F result;
            [&]<size_t... Js>(std::index_sequence<Js...>) {
               auto try_alt = [&]<size_t J>() {
                  if (disc != J) return;
                  using Alt = std::variant_alternative_t<J, F>;
                  if constexpr (std::is_same_v<Alt, std::monostate>)
                     result = F(std::in_place_index<J>);
                  else if constexpr (std::is_same_v<Alt, bool>)
                     result = F(std::in_place_index<J>, *payload != 0);
                  else if constexpr (std::is_enum_v<Alt>)
                     result = F(std::in_place_index<J>, static_cast<Alt>(
                        detail_wit::read_scalar<std::underlying_type_t<Alt>>(payload)));
                  else if constexpr (std::is_arithmetic_v<Alt>)
                     result = F(std::in_place_index<J>, detail_wit::read_scalar<Alt>(payload));
                  else if constexpr (psio::detail::is_std_string_ct<Alt>::value) {
                     uint32_t so = detail_wit::read_scalar<uint32_t>(payload);
                     uint32_t sl = detail_wit::read_scalar<uint32_t>(payload + 4);
                     result = F(std::in_place_index<J>,
                        std::string(reinterpret_cast<const char*>(p.base + so), sl));
                  }
                  else {
                     psio::buffer_load_policy lp(p.base, UINT32_MAX);
                     result = F(std::in_place_index<J>,
                        psio::detail_canonical::load_field<Alt>(lp,
                           static_cast<uint32_t>(payload - p.base)));
                  }
               };
               (try_alt.template operator()<Js>(), ...);
            }(std::make_index_sequence<VN>{});
            return result;
         }
         else if constexpr (psio::Reflected<F>) {
            return psio::view<F, wit>(wit_ptr{p.base, fp});
         }
         else {
            static_assert(sizeof(F) == 0, "wit::field: unsupported type");
         }
      }

      // ── Pack: C++ value → canonical ABI bytes ─────────────────────────────
      //
      // Two-pass: compute total size, allocate once, write into fixed buffer.

      template <typename T>
      static std::vector<uint8_t> pack(const T& value) {
         uint32_t total = canonical_packed_size(value);
         std::vector<uint8_t> buf(total);
         fixed_store_policy fp(buf.data());
         uint32_t root = fp.alloc(canonical_align_v<T>, canonical_size_v<T>);
         canonical_lower_fields(value, fp, root);
         return buf;
      }

      // ── Unpack: canonical ABI bytes → C++ value ───────────────────────────

      template <typename T>
      static T unpack(std::span<const uint8_t> buf) {
         buffer_load_policy lp(buf.data(), static_cast<uint32_t>(buf.size()));
         return canonical_lift_fields<T>(lp, 0);
      }

      template <typename T>
      static T unpack(const std::vector<uint8_t>& buf) {
         return unpack<T>(std::span<const uint8_t>(buf));
      }

      // ── Validate: check buffer integrity ──────────────────────────────────

      template <typename T>
      static bool validate(std::span<const uint8_t> buf) {
         return canonical_validate<T>(buf.data(),
            static_cast<uint32_t>(buf.size()), 0);
      }

      template <typename T>
      static bool validate(const std::vector<uint8_t>& buf) {
         return validate<T>(std::span<const uint8_t>(buf));
      }

      // ── Canonicalize: ensure deterministic canonical form ─────────────────
      //
      // For canonical ABI, pack() already produces canonical bytes (the layout
      // is deterministic). This function re-packs a buffer to guarantee
      // canonical form — useful if the buffer was modified or received
      // from an untrusted source.

      template <typename T>
      static std::vector<uint8_t> canonicalize(std::span<const uint8_t> buf) {
         T value = unpack<T>(buf);
         return pack(value);
      }

      template <typename T>
      static std::vector<uint8_t> canonicalize(const std::vector<uint8_t>& buf) {
         return canonicalize<T>(std::span<const uint8_t>(buf));
      }

      // ── Schema: WIT text generation ───────────────────────────────────────

      template <typename T>
      static std::string schema(const std::string& package = "local:component") {
         return psio::generate_wit_text<T>(package);
      }
   };

   /// wit_view<T> — alias for view<T, wit>
   template <typename T>
   using wit_view = psio::view<T, wit>;

   // ══════════════════════════════════════════════════════════════════════════
   // wit_mut_proxy — mutable ProxyObject for in-place scalar writes
   //
   // Returns wit_mut_ref<T> for scalar fields (assignable), read-only views
   // for strings/vectors, and nested wit_mut for reflected sub-structs.
   // ══════════════════════════════════════════════════════════════════════════

   template <typename T>
   class wit_mut_proxy {
      wit_mut_ptr ptr_;

   public:
      explicit wit_mut_proxy(wit_mut_ptr p = {}) : ptr_(p) {}
      wit_mut_ptr ptr() const { return ptr_; }

      template <int I, auto MemberPtr>
      auto get() const {
         using F = std::tuple_element_t<static_cast<size_t>(I), psio::struct_tuple_t<T>>;
         constexpr uint32_t off = canonical_field_offset_v<T, static_cast<size_t>(I)>;
         uint8_t* fp = ptr_.rec + off;

         if constexpr (std::is_same_v<F, bool>)
            return wit_mut_ref<bool>(fp);
         else if constexpr (std::is_enum_v<F>)
            return wit_mut_ref<F>(fp);
         else if constexpr (std::is_arithmetic_v<F>)
            return wit_mut_ref<F>(fp);
         else if constexpr (psio::detail::is_std_string_ct<F>::value) {
            // Read-only: return string_view (strings can't be resized in-place)
            uint32_t str_off = detail_wit::read_scalar<uint32_t>(fp);
            uint32_t str_len = detail_wit::read_scalar<uint32_t>(fp + 4);
            return std::string_view(
               reinterpret_cast<const char*>(ptr_.base + str_off), str_len);
         }
         else if constexpr (psio::detail::is_std_vector_ct<F>::value) {
            // Read-only: return vec_view
            using E = typename psio::detail::vector_elem_ct<F>::type;
            uint32_t arr_off = detail_wit::read_scalar<uint32_t>(fp);
            uint32_t count   = detail_wit::read_scalar<uint32_t>(fp + 4);
            return psio::vec_view<E, wit>(ptr_.base, arr_off, count);
         }
         else if constexpr (psio::is_std_variant_v<F>) {
            // Read-only: return the variant value (variants can't be modified in-place generically)
            constexpr size_t VN = std::variant_size_v<F>;
            constexpr uint32_t disc_size = VN <= 256 ? 1 : VN <= 65536 ? 2 : 4;
            constexpr uint32_t max_pa = []<size_t... Js>(std::index_sequence<Js...>) {
               uint32_t m = 1;
               ((m = std::max(m, psio::canonical_align_v<std::variant_alternative_t<Js, F>>)), ...);
               return m;
            }(std::make_index_sequence<VN>{});
            constexpr uint32_t payload_off = (disc_size + max_pa - 1) & ~(max_pa - 1);

            uint32_t disc;
            if constexpr (disc_size == 1)
               disc = *fp;
            else if constexpr (disc_size == 2)
               disc = detail_wit::read_scalar<uint16_t>(fp);
            else
               disc = detail_wit::read_scalar<uint32_t>(fp);

            const uint8_t* payload = fp + payload_off;
            F result;
            [&]<size_t... Js>(std::index_sequence<Js...>) {
               auto try_alt = [&]<size_t J>() {
                  if (disc != J) return;
                  using Alt = std::variant_alternative_t<J, F>;
                  if constexpr (std::is_same_v<Alt, std::monostate>)
                     result = F(std::in_place_index<J>);
                  else {
                     psio::buffer_load_policy lp(reinterpret_cast<const uint8_t*>(ptr_.base), UINT32_MAX);
                     result = F(std::in_place_index<J>,
                        psio::detail_canonical::load_field<Alt>(lp,
                           static_cast<uint32_t>(payload - reinterpret_cast<const uint8_t*>(ptr_.base))));
                  }
               };
               (try_alt.template operator()<Js>(), ...);
            }(std::make_index_sequence<VN>{});
            return result;
         }
         else if constexpr (psio::Reflected<F>) {
            // Nested mutable view
            using mut_t = typename psio::reflect<F>::template proxy<wit_mut_proxy<F>>;
            return mut_t(wit_mut_proxy<F>(wit_mut_ptr{ptr_.base, fp}));
         }
         else {
            static_assert(sizeof(F) == 0, "wit_mut: unsupported type");
         }
      }
   };

   /// wit_mut<T> — mutable view over a canonical ABI buffer
   template <typename T>
   class wit_mut : public psio::reflect<T>::template proxy<wit_mut_proxy<T>> {
      using base = typename psio::reflect<T>::template proxy<wit_mut_proxy<T>>;

   public:
      wit_mut() : base(wit_mut_proxy<T>{}) {}
      explicit wit_mut(wit_mut_ptr p) : base(wit_mut_proxy<T>(p)) {}

      explicit operator bool() const { return this->psio_get_proxy().ptr().rec != nullptr; }

      static wit_mut from_buffer(void* buf) {
         auto* p = static_cast<uint8_t*>(buf);
         return wit_mut(wit_mut_ptr{p, p});
      }

      static wit_mut from_buffer(std::vector<uint8_t>& buf) {
         return from_buffer(buf.data());
      }
   };

// ── Container view specializations ──────────────────────────────────────────

   // ══════════════════════════════════════════════════════════════════════════
   // vec_view<E, wit> — zero-copy vector view over canonical ABI list
   //
   // Canonical ABI lists are contiguous arrays of fixed-stride elements.
   // Each element occupies canonical_size_v<E> bytes.
   // ══════════════════════════════════════════════════════════════════════════

   template <typename E>
   class vec_view<E, wit> {
      const uint8_t* base_    = nullptr;
      uint32_t       arr_off_ = 0;
      uint32_t       count_   = 0;

   public:
      vec_view() = default;
      vec_view(const uint8_t* base, uint32_t arr_off, uint32_t count)
         : base_(base), arr_off_(arr_off), count_(count) {}
      explicit operator bool() const { return base_ != nullptr; }

      uint32_t size() const { return count_; }
      bool     empty() const { return count_ == 0; }

      auto operator[](uint32_t i) const {
         constexpr uint32_t es = canonical_size_v<E>;
         const uint8_t* elem = base_ + arr_off_ + i * es;

         if constexpr (std::is_same_v<E, bool>)
            return *elem != 0;
         else if constexpr (std::is_enum_v<E>) {
            using U = std::underlying_type_t<E>;
            return static_cast<E>(detail_wit::read_scalar<U>(elem));
         }
         else if constexpr (std::is_arithmetic_v<E>)
            return detail_wit::read_scalar<E>(elem);
         else if constexpr (detail::is_std_string_ct<E>::value) {
            uint32_t str_off = detail_wit::read_scalar<uint32_t>(elem);
            uint32_t str_len = detail_wit::read_scalar<uint32_t>(elem + 4);
            return std::string_view(
               reinterpret_cast<const char*>(base_ + str_off), str_len);
         }
         else if constexpr (Reflected<E>) {
            return view<E, wit>(wit_ptr{base_, elem});
         }
         else {
            static_assert(sizeof(E*) == 0, "vec_view<E, wit>: unsupported element type");
         }
      }

      auto at(uint32_t i) const { return operator[](i); }

      // ── Iterator ──────────────────────────────────────────────────────────
      struct iterator {
         const vec_view* vec_;
         uint32_t        idx_;

         auto      operator*() const { return (*vec_)[idx_]; }
         iterator& operator++() { ++idx_; return *this; }
         iterator  operator++(int) { auto tmp = *this; ++idx_; return tmp; }
         bool operator==(const iterator& o) const { return idx_ == o.idx_; }
         bool operator!=(const iterator& o) const { return idx_ != o.idx_; }
      };

      iterator begin() const { return {this, 0}; }
      iterator end() const { return {this, count_}; }
   };

   // ══════════════════════════════════════════════════════════════════════════
   // set_view<K, wit> — sorted vector with binary search
   // ══════════════════════════════════════════════════════════════════════════

   template <typename K>
   class set_view<K, wit> : public sorted_set_algo<set_view<K, wit>> {
      friend class sorted_set_algo<set_view<K, wit>>;

      const uint8_t* base_    = nullptr;
      uint32_t       arr_off_ = 0;
      uint32_t       count_   = 0;

      auto read_key(uint32_t i) const {
         constexpr uint32_t es = canonical_size_v<K>;
         const uint8_t* elem = base_ + arr_off_ + i * es;
         if constexpr (std::is_same_v<K, std::string>) {
            uint32_t str_off = detail_wit::read_scalar<uint32_t>(elem);
            uint32_t str_len = detail_wit::read_scalar<uint32_t>(elem + 4);
            return std::string_view(
               reinterpret_cast<const char*>(base_ + str_off), str_len);
         }
         else if constexpr (std::is_enum_v<K>) {
            using U = std::underlying_type_t<K>;
            return static_cast<K>(detail_wit::read_scalar<U>(elem));
         }
         else {
            static_assert(std::is_arithmetic_v<K>);
            return detail_wit::read_scalar<K>(elem);
         }
      }

   public:
      set_view() = default;
      set_view(const uint8_t* base, uint32_t arr_off, uint32_t count)
         : base_(base), arr_off_(arr_off), count_(count) {}
      explicit operator bool() const { return base_ != nullptr; }

      uint32_t size() const { return count_; }
      bool     empty() const { return count_ == 0; }
   };

}  // namespace psio
