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

namespace psizam {

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
      else if constexpr (is_std_string_ct<U>::value) {
         uint32_t ptr = p.alloc(1, static_cast<uint32_t>(value.size()));
         p.store_bytes(ptr, value.data(), static_cast<uint32_t>(value.size()));
         p.emit_i32(ptr);
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
      else if constexpr (is_std_string_ct<U>::value) {
         uint32_t ptr = p.next_i32();
         uint32_t len = p.next_i32();
         const char* data = p.load_bytes(ptr, len);
         return std::string(data, len);
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
      else if constexpr (psio::Reflected<U>) {
         U result{};
         psio::apply_members(
            (typename psio::reflect<U>::data_members*)nullptr,
            [&](auto... ptrs) {
               ((result.*ptrs = canonical_lift_flat<
                  std::remove_cvref_t<typename psio::MemberPtrType<decltype(ptrs)>::ValueType>
               >(p)), ...);
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
