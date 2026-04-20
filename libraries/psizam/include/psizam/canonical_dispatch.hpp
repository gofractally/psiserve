#pragma once

// Canonical ABI flat calling convention — lower/lift for WASM function dispatch.
//
// Extends psio's wire-format serialization with flat-value emit/consume
// for the Component Model calling convention. The type walk is the same;
// the difference is: psio writes to memory (StorePolicy), psizam also
// emits/consumes flat i32/i64/f32/f64 values (LowerPolicy/LiftPolicy).
//
// Functions:
//   canonical_lower_flat(value, policy)  — emit flat values (scalars, ptr+len)
//   canonical_lift_flat<T>(policy)       — consume flat values → T
//   canonical_lower(value, policy)       — auto: flat if ≤16, else spill to memory
//   canonical_lift<T>(policy)            — auto: flat if ≤16, else from memory ptr
//
// Policies:
//   LowerPolicy concept = StorePolicy + emit_i32/i64/f32/f64
//   LiftPolicy  concept = LoadPolicy  + next_i32/i64/f32/f64
//   buffer_lower_policy  — standalone testing (bump alloc + flat_values vector)
//   buffer_lift_policy   — standalone testing (buffer + flat_values array)

#include <psio/canonical_abi.hpp>
#include <psio/wit_owned.hpp>

#include <span>

namespace psizam {

   namespace detail_dispatch {
      template <typename T>
      struct is_std_span : std::false_type {};
      template <typename T, std::size_t N>
      struct is_std_span<std::span<T, N>> : std::true_type {
         using element_type = T;
      };

      template <typename T>
      struct is_wit_vector : std::false_type {};
      template <typename E>
      struct is_wit_vector<psio::owned<std::vector<E>, psio::wit>> : std::true_type {
         using element_type = E;
      };
   }

   // ── flat_count helper ──────────────────────────────────────────────────────
   // Both `wit::string` and `std::string_view` lower to 2 flat slots
   // (i32 ptr, i32 len), but psio::canonical_flat_count_v refuses them —
   // wit::string isn't PSIO_REFLECT'd, and string_view isn't recognized
   // as a canonical "string" by psio (which only sees std::basic_string).
   // Special-case both here; everything else delegates to psio. The
   // partial specialization form short-circuits — a plain ternary over
   // variable templates would instantiate the false arm eagerly and trip
   // psio's static_assert for the string_view case.

   namespace detail {
      template <typename T, typename = void>
      struct flat_count_impl {
         static constexpr size_t value = psio::canonical_flat_count_v<T>;
      };

      // Any type with wasm_type_traits maps to 1 flat slot
      template <typename T>
      struct flat_count_impl<T, std::enable_if_t<
         psizam::wasm_type_traits<std::decay_t<T>, void>::is_wasm_type &&
         !std::is_integral_v<T> && !std::is_floating_point_v<T>>> {
         static constexpr size_t value = 1;
      };

      template <>
      struct flat_count_impl<std::monostate> {
         static constexpr size_t value = 0;
      };

      template <typename... Ts>
      struct flat_count_impl<std::tuple<Ts...>> {
         static constexpr size_t value = (flat_count_impl<Ts>::value + ...);
      };

      template <>
      struct flat_count_impl<psio::owned<std::string, psio::wit>> {
         static constexpr size_t value = 2;
      };

      template <>
      struct flat_count_impl<std::string_view> {
         static constexpr size_t value = 2;
      };

      template <typename T, std::size_t N>
      struct flat_count_impl<std::span<T, N>> {
         static constexpr size_t value = 2;
      };

      template <typename E>
      struct flat_count_impl<psio::owned<std::vector<E>, psio::wit>> {
         static constexpr size_t value = 2;
      };

      template <>
      struct flat_count_impl<std::string> {
         static constexpr size_t value = 2;
      };

      template <typename E>
      struct flat_count_impl<std::vector<E>> {
         static constexpr size_t value = 2;
      };

      template <typename T>
      struct flat_count_impl<std::optional<T>> {
         static constexpr size_t value = 1 + psio::canonical_flat_count_v<T>;
      };

      template <typename T, typename E>
      struct flat_count_impl<std::expected<T, E>> {
         static constexpr size_t vc = [] {
            if constexpr (std::is_void_v<T>) return size_t{0};
            else return psio::canonical_flat_count_v<T>;
         }();
         static constexpr size_t value = 1 + std::max(vc, psio::canonical_flat_count_v<E>);
      };
   }

   template <typename T>
   inline constexpr size_t flat_count_v =
      detail::flat_count_impl<std::remove_cvref_t<T>>::value;

   // =========================================================================
   // Policy concepts — calling convention (extends psio wire-format policies)
   // =========================================================================

   /// LowerPolicy — StorePolicy + flat value emission (for WASM dispatch)
   template <typename P>
   concept LowerPolicy = psio::StorePolicy<P> && requires(P& p) {
      { p.emit_i32(uint32_t{}) };
      { p.emit_i64(uint64_t{}) };
      { p.emit_f32(float{}) };
      { p.emit_f64(double{}) };
   };

   /// LiftPolicy — LoadPolicy + flat value consumption (for WASM dispatch)
   template <typename P>
   concept LiftPolicy = psio::LoadPolicy<P> && requires(P& p) {
      { p.next_i32() } -> std::same_as<uint32_t>;
      { p.next_i64() } -> std::same_as<uint64_t>;
      { p.next_f32() } -> std::same_as<float>;
      { p.next_f64() } -> std::same_as<double>;
   };

   // =========================================================================
   // canonical_lower_flat — emit flat values directly (for ≤16 params)
   // =========================================================================

   template <typename T, LowerPolicy Policy>
   void canonical_lower_flat(const T& value, Policy& p) {
      using U = std::remove_cvref_t<T>;
      using namespace psio::detail;
      if constexpr (std::is_same_v<U, bool>)
         p.emit_i32(value ? 1 : 0);
      else if constexpr (std::is_same_v<U, uint8_t> || std::is_same_v<U, int8_t> ||
                         std::is_same_v<U, uint16_t> || std::is_same_v<U, int16_t> ||
                         std::is_same_v<U, uint32_t> || std::is_same_v<U, int32_t>)
         p.emit_i32(static_cast<uint32_t>(value));
      else if constexpr (std::is_same_v<U, uint64_t> || std::is_same_v<U, int64_t>)
         p.emit_i64(static_cast<uint64_t>(value));
      else if constexpr (std::is_same_v<U, float>)
         p.emit_f32(value);
      else if constexpr (std::is_same_v<U, double>)
         p.emit_f64(value);
      else if constexpr (std::is_enum_v<U>)
         canonical_lower_flat(static_cast<std::underlying_type_t<U>>(value), p);
      else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
         p.emit_i32(value.handle);
      else if constexpr (is_std_string_ct<U>::value ||
                         std::is_same_v<U, std::string_view>) {
         uint32_t ptr = p.alloc(1, static_cast<uint32_t>(value.size()));
         p.store_bytes(ptr, value.data(), static_cast<uint32_t>(value.size()));
         p.emit_i32(ptr);
         p.emit_i32(static_cast<uint32_t>(value.size()));
      }
      else if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>) {
         // The wit::string's buffer already lives in the owning allocator's
         // address space (guest linear memory on __wasm__, host heap otherwise).
         // Emit the existing (ptr, len) pair directly — no alloc, no copy.
         // Ownership is surrendered to the wire: the caller must ensure this
         // is the last use of `value` before it goes out of scope. ComponentProxy
         // does so by release()-ing the result immediately after lowering.
         p.emit_i32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value.data())));
         p.emit_i32(static_cast<uint32_t>(value.size()));
      }
      else if constexpr (detail_dispatch::is_wit_vector<U>::value) {
         // Same surrender-to-the-wire semantics as wit::string; the
         // backing buffer is already laid out canonically.
         p.emit_i32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value.data())));
         p.emit_i32(static_cast<uint32_t>(value.size()));
      }
      else if constexpr (is_std_vector_ct<U>::value) {
         using E = typename vector_elem_ct<U>::type;
         constexpr uint32_t es = psio::canonical_size_v<E>;
         constexpr uint32_t ea = psio::canonical_align_v<E>;
         uint32_t count = static_cast<uint32_t>(value.size());
         uint32_t arr = p.alloc(ea, count * es);
         for (uint32_t i = 0; i < count; i++)
            psio::detail_canonical::store_field(value[i], p, arr + i * es);
         p.emit_i32(arr);
         p.emit_i32(count);
      }
      else if constexpr (psio::detail::is_psio_own<U>::value)
         p.emit_i32(value.handle);
      else if constexpr (psio::detail::is_psio_borrow<U>::value)
         p.emit_i32(value.handle);
      else if constexpr (std::is_same_v<U, std::monostate>) {
      }
      else if constexpr (psio::is_std_tuple<U>::value) {
         [&]<size_t... Is>(std::index_sequence<Is...>) {
            (canonical_lower_flat(std::get<Is>(value), p), ...);
         }(std::make_index_sequence<std::tuple_size_v<U>>{});
      }
      else if constexpr (is_std_optional_ct<U>::value) {
         using E = typename optional_elem_ct<U>::type;
         if (value.has_value()) {
            p.emit_i32(1);
            canonical_lower_flat(*value, p);
         } else {
            p.emit_i32(0);
            constexpr size_t payload_count = psio::canonical_flat_count_v<E>;
            for (size_t i = 0; i < payload_count; i++)
               p.emit_i64(0);
         }
      }
      else if constexpr (psio::is_std_variant_v<U>) {
         constexpr size_t N = std::variant_size_v<U>;
         constexpr size_t max_payload = []<size_t... Is>(std::index_sequence<Is...>) {
            size_t m = 0;
            ((m = std::max(m, psio::detail_canonical::canonical_flat_count_impl<
               std::variant_alternative_t<Is, U>>())), ...);
            return m;
         }(std::make_index_sequence<N>{});
         p.emit_i32(static_cast<uint32_t>(value.index()));
         size_t emitted = 0;
         std::visit([&](const auto& v) {
            using A = std::remove_cvref_t<decltype(v)>;
            if constexpr (!std::is_same_v<A, std::monostate>) {
               constexpr size_t fc = psio::detail_canonical::canonical_flat_count_impl<A>();
               canonical_lower_flat(v, p);
               emitted = fc;
            }
         }, value);
         for (size_t i = emitted; i < max_payload; i++)
            p.emit_i64(0);
      }
      else if constexpr (std::is_array_v<U>) {
         constexpr uint32_t n = std::extent_v<U>;
         for (uint32_t i = 0; i < n; i++)
            canonical_lower_flat(value[i], p);
      }
      else if constexpr (is_std_expected_ct<U>::value) {
         using V = typename expected_value_ct<U>::type;
         using Err = typename expected_error_ct<U>::type;
         constexpr size_t vc = std::is_void_v<V> ? 0 : psio::canonical_flat_count_v<V>;
         constexpr size_t ec = psio::canonical_flat_count_v<Err>;
         constexpr size_t max_payload = std::max(vc, ec);
         if (value.has_value()) {
            p.emit_i32(0);
            if constexpr (!std::is_void_v<V>)
               canonical_lower_flat(*value, p);
            for (size_t i = vc; i < max_payload; i++)
               p.emit_i64(0);
         } else {
            p.emit_i32(1);
            canonical_lower_flat(value.error(), p);
            for (size_t i = ec; i < max_payload; i++)
               p.emit_i64(0);
         }
      }
      else if constexpr (psio::Reflected<U>) {
         psio::apply_members(
            (typename psio::reflect<U>::data_members*)nullptr,
            [&](auto... ptrs) {
               (canonical_lower_flat(value.*ptrs, p), ...);
            }
         );
      }
      else {
         static_assert(sizeof(U) == 0, "canonical_lower_flat: unsupported type");
      }
   }

   // =========================================================================
   // canonical_lower — top-level: flat if ≤16, else spill to memory
   // =========================================================================

   template <typename T, LowerPolicy Policy>
   void canonical_lower(const T& value, Policy& p) {
      constexpr size_t flat = psio::canonical_flat_count_v<T>;
      if constexpr (flat <= psio::MAX_FLAT_PARAMS) {
         canonical_lower_flat(value, p);
      } else {
         uint32_t ptr = p.alloc(psio::canonical_align_v<T>, psio::canonical_size_v<T>);
         psio::canonical_lower_fields(value, p, ptr);
         p.emit_i32(ptr);
      }
   }

   // =========================================================================
   // canonical_lift_flat — read T from flat values
   // =========================================================================

   template <typename T, LiftPolicy Policy>
   T canonical_lift_flat(Policy& p) {
      using U = std::remove_cvref_t<T>;
      using namespace psio::detail;
      if constexpr (std::is_same_v<U, bool>)
         return p.next_i32() != 0;
      else if constexpr (std::is_same_v<U, uint8_t> || std::is_same_v<U, int8_t> ||
                         std::is_same_v<U, uint16_t> || std::is_same_v<U, int16_t> ||
                         std::is_same_v<U, uint32_t> || std::is_same_v<U, int32_t>)
         return static_cast<U>(p.next_i32());
      else if constexpr (std::is_same_v<U, uint64_t> || std::is_same_v<U, int64_t>)
         return static_cast<U>(p.next_i64());
      else if constexpr (std::is_same_v<U, float>)
         return p.next_f32();
      else if constexpr (std::is_same_v<U, double>)
         return p.next_f64();
      else if constexpr (std::is_enum_v<U>)
         return static_cast<U>(canonical_lift_flat<std::underlying_type_t<U>>(p));
      else if constexpr (is_own_ct<U>::value || is_borrow_ct<U>::value)
         return U{p.next_i32()};
      else if constexpr (is_std_string_ct<U>::value) {
         uint32_t ptr = p.next_i32();
         uint32_t len = p.next_i32();
         const char* data = p.load_bytes(ptr, len);
         return std::string(data, len);
      }
      else if constexpr (std::is_same_v<U, std::string_view>) {
         // Borrowed view over memory the caller owns for the call's duration.
         uint32_t ptr = p.next_i32();
         uint32_t len = p.next_i32();
         return std::string_view(p.load_bytes(ptr, len), len);
      }
      else if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>) {
         // Adopt the buffer the caller allocated via the local cabi_realloc.
         // Ownership transfers here; the returned wit::string will free it.
         uint32_t ptr = p.next_i32();
         uint32_t len = p.next_i32();
         return psio::owned<std::string, psio::wit>::adopt(
            const_cast<char*>(p.load_bytes(ptr, len)), len);
      }
      else if constexpr (detail_dispatch::is_std_span<U>::value) {
         // Borrowed list view. No allocation — just resolve (ptr, len) into
         // a span over the caller's linear-memory buffer. The element type
         // must match the canonical layout (trivially-copyable, fixed size).
         using E = typename detail_dispatch::is_std_span<U>::element_type;
         uint32_t ptr = p.next_i32();
         uint32_t len = p.next_i32();
         auto*    base = reinterpret_cast<E*>(
            const_cast<char*>(p.load_bytes(ptr, len * sizeof(E))));
         return std::span<E>(base, len);
      }
      else if constexpr (detail_dispatch::is_wit_vector<U>::value) {
         // Adopt the (ptr, len) pair into an owning wit::vector. Ownership
         // transfers to the callee — on the guest side, the returned vector's
         // destructor will cabi_realloc(0) the buffer when it goes out of
         // scope, matching the contract of the canonical ABI.
         using E = typename detail_dispatch::is_wit_vector<U>::element_type;
         uint32_t ptr = p.next_i32();
         uint32_t len = p.next_i32();
         auto*    base = reinterpret_cast<E*>(
            const_cast<char*>(p.load_bytes(ptr, len * sizeof(E))));
         return psio::owned<std::vector<E>, psio::wit>::adopt(base, len);
      }
      else if constexpr (is_std_vector_ct<U>::value) {
         using E = typename vector_elem_ct<U>::type;
         constexpr uint32_t es = psio::canonical_size_v<E>;
         uint32_t ptr = p.next_i32();
         uint32_t len = p.next_i32();
         std::vector<E> result;
         result.reserve(len);
         for (uint32_t i = 0; i < len; i++)
            result.push_back(psio::detail_canonical::load_field<E>(p, ptr + i * es));
         return result;
      }
      else if constexpr (psio::detail::is_psio_own<U>::value)
         return U{p.next_i32()};
      else if constexpr (psio::detail::is_psio_borrow<U>::value)
         return U{p.next_i32()};
      else if constexpr (std::is_same_v<U, std::monostate>)
         return std::monostate{};
      else if constexpr (psio::is_std_tuple<U>::value) {
         return [&]<size_t... Is>(std::index_sequence<Is...>) {
            return U{canonical_lift_flat<std::tuple_element_t<Is, U>>(p)...};
         }(std::make_index_sequence<std::tuple_size_v<U>>{});
      }
      else if constexpr (is_std_optional_ct<U>::value) {
         using E = typename optional_elem_ct<U>::type;
         uint32_t disc = p.next_i32();
         if (disc)
            return std::optional<E>(canonical_lift_flat<E>(p));
         else {
            constexpr size_t payload_count = psio::canonical_flat_count_v<E>;
            for (size_t i = 0; i < payload_count; i++)
               (void)p.next_i64();
            return std::optional<E>(std::nullopt);
         }
      }
      else if constexpr (psio::is_std_variant_v<U>) {
         constexpr size_t N = std::variant_size_v<U>;
         constexpr size_t max_payload = []<size_t... Is>(std::index_sequence<Is...>) {
            size_t m = 0;
            ((m = std::max(m, psio::detail_canonical::canonical_flat_count_impl<
               std::variant_alternative_t<Is, U>>())), ...);
            return m;
         }(std::make_index_sequence<N>{});
         uint32_t disc = p.next_i32();
         std::optional<U> result;
         size_t consumed = 0;
         [&]<size_t... Is>(std::index_sequence<Is...>) {
            auto try_alt = [&]<size_t I>() {
               if (disc == I) {
                  using Alt = std::variant_alternative_t<I, U>;
                  if constexpr (std::is_same_v<Alt, std::monostate>)
                     result.emplace(std::in_place_index<I>);
                  else {
                     result.emplace(std::in_place_index<I>, canonical_lift_flat<Alt>(p));
                     consumed = psio::detail_canonical::canonical_flat_count_impl<Alt>();
                  }
               }
            };
            (try_alt.template operator()<Is>(), ...);
         }(std::make_index_sequence<N>{});
         for (size_t i = consumed; i < max_payload; i++)
            (void)p.next_i64();
         return std::move(*result);
      }
      else if constexpr (std::is_array_v<U>) {
         constexpr uint32_t n = std::extent_v<U>;
         using E = std::remove_extent_t<U>;
         U arr;
         for (uint32_t i = 0; i < n; i++)
            arr[i] = canonical_lift_flat<E>(p);
         return arr;
      }
      else if constexpr (is_std_expected_ct<U>::value) {
         using V = typename expected_value_ct<U>::type;
         using Err = typename expected_error_ct<U>::type;
         constexpr size_t vc = std::is_void_v<V> ? 0 : psio::canonical_flat_count_v<V>;
         constexpr size_t ec = psio::canonical_flat_count_v<Err>;
         constexpr size_t max_payload = std::max(vc, ec);
         uint32_t disc = p.next_i32();
         if (disc == 0) {
            std::expected<V, Err> result;
            if constexpr (!std::is_void_v<V>)
               result = canonical_lift_flat<V>(p);
            for (size_t i = vc; i < max_payload; i++)
               (void)p.next_i64();
            return result;
         } else {
            auto err = canonical_lift_flat<Err>(p);
            for (size_t i = ec; i < max_payload; i++)
               (void)p.next_i64();
            return std::expected<V, Err>{std::unexpected(err)};
         }
      }
      else if constexpr (psio::Reflected<U>) {
         U result{};
         psio::apply_members(
            (typename psio::reflect<U>::data_members*)nullptr,
            [&](auto... ptrs) {
               auto lift_member = [&]<typename Ptr>(Ptr ptr) {
                  using VT = std::remove_cvref_t<typename psio::MemberPtrType<Ptr>::ValueType>;
                  if constexpr (std::is_array_v<VT>) {
                     using E = std::remove_extent_t<VT>;
                     constexpr uint32_t n = std::extent_v<VT>;
                     for (uint32_t j = 0; j < n; j++)
                        (result.*ptr)[j] = canonical_lift_flat<E>(p);
                  } else {
                     result.*ptr = canonical_lift_flat<VT>(p);
                  }
               };
               (lift_member(ptrs), ...);
            }
         );
         return result;
      }
      else {
         static_assert(sizeof(U) == 0, "canonical_lift_flat: unsupported type");
         return {};
      }
   }

   // =========================================================================
   // canonical_lift — top-level: flat if ≤16, else from memory pointer
   // =========================================================================

   template <typename T, LiftPolicy Policy>
   T canonical_lift(Policy& p) {
      constexpr size_t flat = psio::canonical_flat_count_v<T>;
      if constexpr (flat <= psio::MAX_FLAT_PARAMS) {
         return canonical_lift_flat<T>(p);
      } else {
         uint32_t ptr = p.next_i32();
         return psio::canonical_lift_fields<T>(p, ptr);
      }
   }

   // =========================================================================
   // Concrete policies — extend psio buffer policies with flat dispatch
   // =========================================================================

   // ── buffer_lower_policy — bump allocator + flat value emission ────────────

   struct buffer_lower_policy : psio::buffer_store_policy {
      std::vector<psio::native_value> flat_values;

      buffer_lower_policy(uint32_t base_offset = 0) : buffer_store_policy(base_offset) {}

      void emit_i32(uint32_t v) { psio::native_value nv; nv.i64 = 0; nv.i32 = v; flat_values.push_back(nv); }
      void emit_i64(uint64_t v) { psio::native_value nv; nv.i64 = v; flat_values.push_back(nv); }
      void emit_f32(float v)    { psio::native_value nv; nv.i64 = 0; nv.f32 = v; flat_values.push_back(nv); }
      void emit_f64(double v)   { psio::native_value nv; nv.f64 = v; flat_values.push_back(nv); }
   };

   // ── buffer_lift_policy — buffer + flat value consumption ─────────────────

   struct buffer_lift_policy : psio::buffer_load_policy {
      const psio::native_value* flat_values;
      size_t flat_idx = 0;

      buffer_lift_policy(const uint8_t* b, uint32_t size, const psio::native_value* fv)
         : buffer_load_policy(b, size), flat_values(fv) {}

      uint32_t next_i32() { return flat_values[flat_idx++].i32; }
      uint64_t next_i64() { return flat_values[flat_idx++].i64; }
      float    next_f32() { return flat_values[flat_idx++].f32; }
      double   next_f64() { return flat_values[flat_idx++].f64; }
   };

} // namespace psizam
