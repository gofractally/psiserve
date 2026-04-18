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
#include <psizam/component_proxy.hpp>  // flat_val, export_*_policy, ComponentProxy

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

namespace psizam {

   // flat_val, export_lift_policy, export_lower_policy, ComponentProxy
   // are re-exported from <psizam/component_proxy.hpp>.

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
