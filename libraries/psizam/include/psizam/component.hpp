#pragma once

// PZAM_COMPONENT — single macro that generates a complete WASM Component.
//
// Usage:
//   struct calculator {
//       int32_t add(int32_t a, int32_t b) { return a + b; }
//       std::string greet(std::string name) { return "Hello, " + name; }
//   };
//
//   PZAM_COMPONENT(calculator,
//       method(add, a, b),
//       method(greet, name)
//   )
//
// Produces:
//   - PSIO_REFLECT for the class
//   - WIT embedded in component-type custom section
//   - cabi_realloc export
//   - Typed extern "C" exports for each method
//   - Canonical ABI lift/lower for all parameter and return types

#include <psio/reflect.hpp>
#include <psio/wit_gen.hpp>
#include <psio/wit_encode.hpp>
#include <psio/wview.hpp>
#include <psizam/canonical_dispatch.hpp>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psizam {

   // ── Flat ABI slot type ────────────────────────────────────────────────────
   // All flat ABI values are passed as int64_t (widest scalar) in our convention.
   // The canonical ABI specifies i32/i64/f32/f64, but since we control both
   // host and guest, we use int64_t as a universal envelope.
   using flat_val = int64_t;

   // ── Lift policy for component export args ─────────────────────────────────
   // Reads canonical ABI flat values from an int64_t array (the 16 export params).
   // Satisfies LiftPolicy. For memory access (strings, vectors), resolves offsets
   // through a configurable base pointer:
   //   - mem_base != nullptr: offsets are relative to mem_base (native testing)
   //   - mem_base == nullptr: offsets treated as native pointers (WASM context)

   struct export_lift_policy {
      const int64_t* slots;
      size_t         idx = 0;
      const uint8_t* mem_base;

      explicit export_lift_policy(const int64_t* s, const uint8_t* mem = nullptr)
         : slots(s), mem_base(mem) {}

      uint32_t next_i32() { return static_cast<uint32_t>(slots[idx++]); }
      uint64_t next_i64() { return static_cast<uint64_t>(slots[idx++]); }

      float next_f32() {
         union { int32_t i; float f; } u;
         u.i = static_cast<int32_t>(slots[idx++]);
         return u.f;
      }

      double next_f64() {
         union { int64_t i; double f; } u;
         u.i = slots[idx++];
         return u.f;
      }

      const uint8_t* resolve(uint32_t off) const {
         return mem_base ? (mem_base + off)
                         : reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(off));
      }

      uint8_t  load_u8(uint32_t off)  { return resolve(off)[0]; }
      uint16_t load_u16(uint32_t off) { uint16_t v; std::memcpy(&v, resolve(off), 2); return v; }
      uint32_t load_u32(uint32_t off) { uint32_t v; std::memcpy(&v, resolve(off), 4); return v; }
      uint64_t load_u64(uint32_t off) { uint64_t v; std::memcpy(&v, resolve(off), 8); return v; }
      float    load_f32(uint32_t off) { float v; std::memcpy(&v, resolve(off), 4); return v; }
      double   load_f64(uint32_t off) { double v; std::memcpy(&v, resolve(off), 8); return v; }
      const char* load_bytes(uint32_t off, uint32_t) {
         return reinterpret_cast<const char*>(resolve(off));
      }
   };

   // ── Lower policy for component export results ─────────────────────────────
   // Writes canonical ABI flat values to a psio::native_value array.
   // Satisfies LowerPolicy. For memory allocation (string/vector data in results),
   // uses a bump allocator into an internal buffer.

   struct export_lower_policy {
      psio::native_value results[16] = {};
      size_t       result_count = 0;

      // Bump allocator for result data (string/vector contents)
      std::vector<uint8_t> result_buf;
      uint32_t             bump = 0;

      uint32_t alloc(uint32_t align, uint32_t size) {
         bump = (bump + align - 1) & ~(align - 1);
         uint32_t ptr = bump;
         bump += size;
         if (result_buf.size() < bump)
            result_buf.resize(bump);
         return ptr;
      }

      void store_u8(uint32_t off, uint8_t v)   { result_buf[off] = v; }
      void store_u16(uint32_t off, uint16_t v)  { std::memcpy(&result_buf[off], &v, 2); }
      void store_u32(uint32_t off, uint32_t v)  { std::memcpy(&result_buf[off], &v, 4); }
      void store_u64(uint32_t off, uint64_t v)  { std::memcpy(&result_buf[off], &v, 8); }
      void store_f32(uint32_t off, float v)     { std::memcpy(&result_buf[off], &v, 4); }
      void store_f64(uint32_t off, double v)    { std::memcpy(&result_buf[off], &v, 8); }
      void store_bytes(uint32_t off, const char* data, uint32_t len) {
         if (len > 0) std::memcpy(&result_buf[off], data, len);
      }

      void emit_i32(uint32_t v) { psio::native_value nv; nv.i64 = 0; nv.i32 = v; results[result_count++] = nv; }
      void emit_i64(uint64_t v) { psio::native_value nv; nv.i64 = v; results[result_count++] = nv; }
      void emit_f32(float v)    { psio::native_value nv; nv.i64 = 0; nv.f32 = v; results[result_count++] = nv; }
      void emit_f64(double v)   { psio::native_value nv; nv.f64 = v; results[result_count++] = nv; }
   };

   // ── Method flat count computation ─────────────────────────────────────────

   namespace detail_component {

      template <typename... Args>
      constexpr size_t param_flat_count(psio::TypeList<Args...>) {
         return (0 + ... + psio::canonical_flat_count_v<std::remove_cvref_t<Args>>);
      }

      template <typename T>
      constexpr size_t result_flat_count() {
         if constexpr (std::is_void_v<T>)
            return 0;
         else
            return psio::canonical_flat_count_v<T>;
      }

   } // namespace detail_component

   // ── ComponentProxy<T> — canonical ABI dispatch ────────────────────────────
   //
   // Dispatches a flat-arg call to a specific method on T using canonical
   // lift/lower. The template parameter MemPtr carries all type information.

   template <typename T>
   struct ComponentProxy {

      /// Call a method given 16 flat args (no memory context — scalar methods only).
      template <auto MemPtr>
      static flat_val call(T* impl,
                           flat_val a0,  flat_val a1,  flat_val a2,  flat_val a3,
                           flat_val a4,  flat_val a5,  flat_val a6,  flat_val a7,
                           flat_val a8,  flat_val a9,  flat_val a10, flat_val a11,
                           flat_val a12, flat_val a13, flat_val a14, flat_val a15)
      {
         flat_val slots[16] = {a0, a1, a2, a3, a4, a5, a6, a7,
                               a8, a9, a10, a11, a12, a13, a14, a15};
         return call_with_memory<MemPtr>(impl, slots, nullptr);
      }

      /// Call with explicit memory base (for dispatching methods with complex types).
      /// The memory parameter provides the base for resolving i32 offsets to string/
      /// vector data (as produced by buffer_lower_policy).
      template <auto MemPtr>
      static flat_val call_with_memory(T* impl, const flat_val* slots, const uint8_t* memory) {
         using MType    = psio::MemberPtrType<decltype(MemPtr)>;
         using ArgTypes = typename MType::SimplifiedArgTypes;

         constexpr size_t pcnt = detail_component::param_flat_count(ArgTypes{});
         static_assert(pcnt <= psio::MAX_FLAT_PARAMS,
            "Method exceeds psio::MAX_FLAT_PARAMS (16). Spilled args not yet supported.");

         // Lift all args from flat values using canonical ABI rules
         export_lift_policy lift_p(slots, memory);
         auto arg_tuple = lift_args(lift_p, ArgTypes{});

         // Call method and lower the result
         return invoke_and_lower<MemPtr>(impl, arg_tuple,
            std::make_index_sequence<std::tuple_size_v<decltype(arg_tuple)>>{});
      }

   private:
      template <psizam::LiftPolicy Policy, typename... Args>
      static auto lift_args(Policy& p, psio::TypeList<Args...>) {
         return std::tuple{psizam::canonical_lift_flat<std::remove_cvref_t<Args>>(p)...};
      }

      template <auto MemPtr, typename Tuple, size_t... Is>
      static flat_val invoke_and_lower(T* impl, Tuple& args, std::index_sequence<Is...>) {
         using MType      = psio::MemberPtrType<decltype(MemPtr)>;
         using ReturnType = typename MType::ReturnType;

         if constexpr (std::is_void_v<ReturnType>) {
            (impl->*MemPtr)(std::get<Is>(args)...);
            return 0;
         } else {
            auto result = (impl->*MemPtr)(std::get<Is>(args)...);
            export_lower_policy lower_p;
            psizam::canonical_lower_flat(result, lower_p);
            return static_cast<flat_val>(lower_p.results[0].i64);
         }
      }
   };

   // ── WIT section generation ────────────────────────────────────────────────

   template <typename T>
   std::string generate_component_wit(const std::string& package) {
      return psio::generate_wit_text<T>(package);
   }

   // Generate Component Model binary for embedding as a component-type custom section.
   template <typename T>
   std::vector<uint8_t> generate_component_wit_binary(const std::string& package) {
      return psio::generate_wit_binary<T>(package);
   }

} // namespace psizam

// ── PZAM_COMPONENT macro ─────────────────────────────────────────────────────
//
// Generates: PSIO_REFLECT + static instance + extern "C" exports
//
// The preprocessor iteration uses a simple X-macro pattern to handle
// each method(...) entry in the variadic args.

// Helper: extract method name from method(name, ...) via token pasting.
// _PZAM_GET_NAME(method(add, a, b)) → add
// Step 1: ## pastes _PZAM_UNWRAP_ with 'method' → _PZAM_UNWRAP_method
// Step 2: _PZAM_UNWRAP_method(add, a, b) → add
#define _PZAM_UNWRAP_method(name, ...) name
#define _PZAM_GET_NAME(spec) _PZAM_GET_NAME_I(spec)
#define _PZAM_GET_NAME_I(spec) _PZAM_UNWRAP_##spec

// Helper: generate one extern "C" export for a method
#define _PZAM_EXPORT_ONE(Class, method_spec) \
   extern "C" psizam::flat_val _PZAM_GET_NAME(method_spec)( \
      psizam::flat_val a0,  psizam::flat_val a1,  psizam::flat_val a2,  psizam::flat_val a3, \
      psizam::flat_val a4,  psizam::flat_val a5,  psizam::flat_val a6,  psizam::flat_val a7, \
      psizam::flat_val a8,  psizam::flat_val a9,  psizam::flat_val a10, psizam::flat_val a11, \
      psizam::flat_val a12, psizam::flat_val a13, psizam::flat_val a14, psizam::flat_val a15) \
   { \
      return psizam::ComponentProxy<Class>::call<&Class::_PZAM_GET_NAME(method_spec)>( \
         &_pzam_impl, a0, a1, a2, a3, a4, a5, a6, a7, \
         a8, a9, a10, a11, a12, a13, a14, a15); \
   }

// ── Preprocessor iteration over variadic method specs ────────────────────────
// Supports up to 16 methods. Uses a counting trick to iterate.

#define _PZAM_FOREACH_1(Class, m)       _PZAM_EXPORT_ONE(Class, m)
#define _PZAM_FOREACH_2(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_1(Class, __VA_ARGS__)
#define _PZAM_FOREACH_3(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_2(Class, __VA_ARGS__)
#define _PZAM_FOREACH_4(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_3(Class, __VA_ARGS__)
#define _PZAM_FOREACH_5(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_4(Class, __VA_ARGS__)
#define _PZAM_FOREACH_6(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_5(Class, __VA_ARGS__)
#define _PZAM_FOREACH_7(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_6(Class, __VA_ARGS__)
#define _PZAM_FOREACH_8(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_7(Class, __VA_ARGS__)
#define _PZAM_FOREACH_9(Class, m, ...)  _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_8(Class, __VA_ARGS__)
#define _PZAM_FOREACH_10(Class, m, ...) _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_9(Class, __VA_ARGS__)
#define _PZAM_FOREACH_11(Class, m, ...) _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_10(Class, __VA_ARGS__)
#define _PZAM_FOREACH_12(Class, m, ...) _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_11(Class, __VA_ARGS__)
#define _PZAM_FOREACH_13(Class, m, ...) _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_12(Class, __VA_ARGS__)
#define _PZAM_FOREACH_14(Class, m, ...) _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_13(Class, __VA_ARGS__)
#define _PZAM_FOREACH_15(Class, m, ...) _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_14(Class, __VA_ARGS__)
#define _PZAM_FOREACH_16(Class, m, ...) _PZAM_EXPORT_ONE(Class, m) _PZAM_FOREACH_15(Class, __VA_ARGS__)

// Count variadic args (up to 16)
#define _PZAM_COUNT_ARGS(...) _PZAM_COUNT_ARGS_IMPL(__VA_ARGS__, \
   16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define _PZAM_COUNT_ARGS_IMPL( \
   _1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N

// Dispatch to _PZAM_FOREACH_N
#define _PZAM_CAT(a, b) a##b
#define _PZAM_FOREACH_(N, Class, ...) _PZAM_CAT(_PZAM_FOREACH_, N)(Class, __VA_ARGS__)
#define _PZAM_FOREACH(Class, ...) _PZAM_FOREACH_(_PZAM_COUNT_ARGS(__VA_ARGS__), Class, __VA_ARGS__)

// ── The main macro ───────────────────────────────────────────────────────────

#define PZAM_COMPONENT(Class, ...) \
   PSIO_REFLECT(Class, __VA_ARGS__) \
   static Class _pzam_impl; \
   _PZAM_FOREACH(Class, __VA_ARGS__)
