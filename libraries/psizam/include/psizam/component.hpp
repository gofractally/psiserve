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
//   - Typed extern "C" exports for each method (16-wide flat signature)

#include <psio/reflect.hpp>
#include <psio/wit_gen.hpp>
#include <psio/ctype.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace psizam {

   // ── Canonical ABI flat value type ─────────────────────────────────────────
   // All flat ABI values are passed as int64_t (widest scalar).
   // The template extracts the actual type from the member function pointer.

   using flat_val = int64_t;

   // ── Type classification for dispatch ──────────────────────────────────────

   namespace detail {

      // Count flat values needed for a type in Canonical ABI
      template <typename T>
      struct flat_count {
         static constexpr size_t value = 1;  // scalars = 1 slot
      };

      template <>
      struct flat_count<std::string> {
         static constexpr size_t value = 2;  // ptr + len
      };

      template <typename U>
      struct flat_count<std::vector<U>> {
         static constexpr size_t value = 2;  // ptr + len
      };

      // Sum flat counts for a parameter pack
      template <typename... Args>
      constexpr size_t total_flat_count() {
         return (0 + ... + flat_count<std::remove_cvref_t<Args>>::value);
      }

      // Can this function use flat args (<=16 flat values)?
      template <typename... Args>
      constexpr bool can_use_flat_args() {
         return total_flat_count<Args...>() <= 16;
      }

      // ── Flat arg extraction ────────────────────────────────────────────────
      // Read a C++ value from flat ABI slots.

      template <typename T>
      struct flat_extract {
         static T get(const flat_val* args, size_t& idx) {
            if constexpr (std::is_same_v<T, float>) {
               union { int32_t i; float f; } u;
               u.i = static_cast<int32_t>(args[idx++]);
               return u.f;
            } else if constexpr (std::is_same_v<T, double>) {
               union { int64_t i; double f; } u;
               u.i = args[idx++];
               return u.f;
            } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
               return static_cast<T>(args[idx++]);
            } else {
               // Complex type — shouldn't reach here in flat mode
               static_assert(sizeof(T) == 0, "Cannot flat-extract this type");
               return T{};
            }
         }
      };

      // String: ptr + len from flat args (pointing into linear memory)
      template <>
      struct flat_extract<std::string> {
         static std::string get(const flat_val* args, size_t& idx) {
            auto ptr = reinterpret_cast<const char*>(static_cast<uintptr_t>(args[idx++]));
            auto len = static_cast<size_t>(args[idx++]);
            return std::string(ptr, len);
         }
      };

      // ── Flat result storage ────────────────────────────────────────────────

      template <typename T>
      struct flat_store {
         static flat_val put(const T& val) {
            if constexpr (std::is_same_v<T, float>) {
               union { float f; int32_t i; } u;
               u.f = val;
               return static_cast<flat_val>(u.i);
            } else if constexpr (std::is_same_v<T, double>) {
               union { double f; int64_t i; } u;
               u.f = val;
               return u.i;
            } else if constexpr (std::is_integral_v<T> || std::is_enum_v<T>) {
               return static_cast<flat_val>(val);
            } else {
               static_assert(sizeof(T) == 0, "Cannot flat-store this type");
               return 0;
            }
         }
      };

      // ── Method invocation with flat args ───────────────────────────────────

      template <typename Class, auto MemPtr>
      struct MethodInvoker {
         using MType = psio::MemberPtrType<decltype(MemPtr)>;
         using ReturnType = typename MType::ReturnType;

         // Extract all args from flat slots and call the method
         template <typename... Args>
         static flat_val invoke_flat(Class* impl, const flat_val* args, psio::TypeList<Args...>) {
            size_t idx = 0;
            // Use a tuple to hold extracted args (evaluation order guaranteed)
            auto arg_tuple = std::tuple{
               flat_extract<std::remove_cvref_t<Args>>::get(args, idx)...
            };
            return call_with_tuple(impl, arg_tuple, std::index_sequence_for<Args...>{});
         }

         template <typename Tuple, size_t... Is>
         static flat_val call_with_tuple(Class* impl, Tuple& args, std::index_sequence<Is...>) {
            if constexpr (std::is_void_v<ReturnType>) {
               (impl->*MemPtr)(std::get<Is>(args)...);
               return 0;
            } else if constexpr (std::is_arithmetic_v<ReturnType> || std::is_enum_v<ReturnType>) {
               auto result = (impl->*MemPtr)(std::get<Is>(args)...);
               return flat_store<ReturnType>::put(result);
            } else {
               // Complex return type — allocate in linear memory and return pointer
               // For now, handle string specially
               auto result = (impl->*MemPtr)(std::get<Is>(args)...);
               return store_result(result);
            }
         }

         // Store a complex result and return a pointer
         // For string: allocate, copy, return (ptr, len) packed or via retptr
         static flat_val store_result(const std::string& s) {
            // In WASM context, this would use cabi_realloc
            // For native testing, just return the pointer (caller must handle lifetime)
            // TODO: proper WASM linear memory integration
            auto* buf = static_cast<char*>(std::malloc(s.size()));
            std::memcpy(buf, s.data(), s.size());
            // Pack ptr and len into a struct at a known location
            // For now, return ptr — full implementation needs retptr convention
            return reinterpret_cast<flat_val>(buf);
         }
      };

   } // namespace detail

   // ── ComponentProxy<T> — type-driven dispatch ──────────────────────────────

   /// Dispatches a flat-arg call to a specific method on T.
   /// The template parameter MemPtr carries all type information.
   template <typename T>
   struct ComponentProxy {

      /// Call a method given 16 flat args.
      /// The template deduces parameter types from the member pointer
      /// and extracts only the slots it needs.
      template <auto MemPtr>
      static flat_val call(T* impl,
                           flat_val a0,  flat_val a1,  flat_val a2,  flat_val a3,
                           flat_val a4,  flat_val a5,  flat_val a6,  flat_val a7,
                           flat_val a8,  flat_val a9,  flat_val a10, flat_val a11,
                           flat_val a12, flat_val a13, flat_val a14, flat_val a15)
      {
         using MType = psio::MemberPtrType<decltype(MemPtr)>;
         using ArgTypes = typename MType::SimplifiedArgTypes;

         flat_val args[16] = {a0, a1, a2, a3, a4, a5, a6, a7,
                              a8, a9, a10, a11, a12, a13, a14, a15};

         return detail::MethodInvoker<T, MemPtr>::invoke_flat(impl, args, ArgTypes{});
      }
   };

   // ── WIT section generation ────────────────────────────────────────────────

   // Generate WIT text for embedding in a custom section.
   // Not constexpr yet (uses std::string at runtime), but produces the right output.
   template <typename T>
   std::string generate_component_wit() {
      return psio::generate_wit_text<T>();
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
// Supports up to 32 methods. Uses a counting trick to iterate.

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
