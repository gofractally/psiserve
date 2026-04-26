#pragma once

// PSIO1_MODULE — guest-side macro that binds an impl class to one or
// more exported methods via canonical-ABI thunks.
//
// Usage (in a guest .cpp):
//
//   struct greeter_impl
//   {
//      wit::string concat(std::string_view a, std::string_view b)
//      { return wit::string{std::string{a} + std::string{b}}; }
//   };
//
//   PSIO1_MODULE(greeter_impl, concat)
//
// Effects on __wasm__ builds:
//
//   • Defines a static impl singleton (`_psio_module_<impl>_instance`).
//   • For each method name, emits an
//     `extern "C" [[clang::export_name("<method>")]]` thunk that takes
//     up to 16 flat values and delegates to
//     psizam::ComponentProxy<Impl>::call, which lifts the args via
//     canonical_lift_flat, invokes the method, and lowers the return
//     via canonical_lower_flat.
//
// On host builds (non-wasm) the macro expands to nothing: the thunks
// are a guest concern, and a native test of the same impl class simply
// calls its methods directly.
//
// Grammar note: PSIO1_HOST_MODULE uses `interface(Tag, m…)` entries to
// tie methods to reflection tags on the host side. The guest doesn't
// consume that tag at runtime — the canonical-ABI wire format is
// self-describing via the WIT side — so we take a flat method list and
// keep the macro minimal. The shared-header interface declarations
// (via PSIO1_INTERFACE) remain the authoritative contract.
//
// Return-shape caveat (v1): only returns that lower to a single flat
// slot are wired. Methods whose return type lowers to >1 flat slot
// (i.e. strings and records on an export return) need canonical
// return-area threading (an i32 return pointer) which is a follow-up;
// a static_assert inside ComponentProxy catches anything that exceeds
// the supported shape at instantiation time.

#include <psizam/component_proxy.hpp>  // ComponentProxy, flat_val
#include <psio1/wit_constexpr.hpp>      // constexpr WIT generation

#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>

#ifdef __wasm__

// ── PSIO1_WIT_SECTION — embed WIT text as a WASM custom section ──────────────
// Emits a `component-type:NAME` custom section containing the WIT text
// generated at compile time from the PSIO1_INTERFACE reflection. The
// linker reads this section to discover type signatures for module-to-
// module wiring.
//
//   PSIO1_WIT_SECTION(greeter)   // embeds greeter's WIT
//   PSIO1_WIT_SECTION(env)       // embeds env's WIT (imports)

namespace psio1::constexpr_wit {
   template <std::size_t N>
   struct section_blob {
      static constexpr std::size_t size = N;
      char bytes[N];
   };

   template <typename Tag>
   consteval auto make_section_blob() {
      auto arr = wit_array<Tag>();
      section_blob<wit_size<Tag>()> result{};
      for (unsigned i = 0; i < result.size; ++i) result.bytes[i] = arr[i];
      return result;
   }
}

#define PSIO1_WIT_SECTION(IFACE)                                                \
   __attribute__((section(".custom_section.component-type:"                    \
                          BOOST_PP_STRINGIZE(IFACE)),                          \
                  used))                                                       \
   static const auto BOOST_PP_CAT(_psio_wit_sec_, IFACE) =                    \
      ::psio1::constexpr_wit::make_section_blob<IFACE>();

// ── guest thunk emission ─────────────────────────────────────────────────────
// One `extern "C"` function per method, decorated with clang's
// export_name so the linker publishes it under the WIT method name.
// The body routes through ComponentProxy which does lift → invoke →
// lower against canonical ABI flat slots.

#define PSIO1_MODULE_INSTANCE_NAME(IMPL) \
   BOOST_PP_CAT(_psio_module_instance_, IMPL)

// BOOST_PP_STRINGIZE (not `#METHOD`) is required because SEQ_FOR_EACH
// hands the callback a still-deferred expression — plain `#METHOD`
// would stringify the raw token stream (`BOOST_PP_SEQ_HEAD(…)`). The
// STRINGIZE macro forces expansion before the stringify step.
#define PSIO1_MODULE_EMIT_THUNK(r, IMPL, METHOD)                                \
   extern "C" [[clang::export_name(BOOST_PP_STRINGIZE(METHOD))]]               \
   ::psizam::flat_val METHOD(                                                  \
      ::psizam::flat_val a0,  ::psizam::flat_val a1,                           \
      ::psizam::flat_val a2,  ::psizam::flat_val a3,                           \
      ::psizam::flat_val a4,  ::psizam::flat_val a5,                           \
      ::psizam::flat_val a6,  ::psizam::flat_val a7,                           \
      ::psizam::flat_val a8,  ::psizam::flat_val a9,                           \
      ::psizam::flat_val a10, ::psizam::flat_val a11,                          \
      ::psizam::flat_val a12, ::psizam::flat_val a13,                          \
      ::psizam::flat_val a14, ::psizam::flat_val a15)                          \
   {                                                                           \
      return ::psizam::ComponentProxy<IMPL>::template call<&IMPL::METHOD>(     \
         &PSIO1_MODULE_INSTANCE_NAME(IMPL),                                     \
         a0, a1, a2, a3, a4, a5, a6, a7,                                       \
         a8, a9, a10, a11, a12, a13, a14, a15);                                \
   }

#define PSIO1_MODULE(IMPL, ...)                                                 \
   static IMPL PSIO1_MODULE_INSTANCE_NAME(IMPL){};                              \
   BOOST_PP_SEQ_FOR_EACH(PSIO1_MODULE_EMIT_THUNK, IMPL,                         \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

// ── guest import thunk emission ──────────────────────────────────────────────
// Mirror of PSIO1_MODULE but for imports: generates method bodies that
// lower C++ args → 16 flat_vals, call the raw WASM import, and lift
// the return. Data is already in guest linear memory (no alloc/copy).

// `guest_import_lower` lives in psizam/component_proxy.hpp (included
// above). PSIO1_IMPORT_EMIT_CALL_FN below qualifies the call as
// `::psizam::guest_import_lower` and picks up that canonical
// definition. This header previously carried a duplicate copy; the
// two have been merged into component_proxy.hpp with all the prior
// specializations (enum, monostate, tuple, variant, array,
// is_psio_own / is_psio_borrow) preserved.

#define PSIO1_IMPORT_RAW_NAME(IFACE, METHOD) \
   BOOST_PP_CAT(BOOST_PP_CAT(_psio_raw_, BOOST_PP_CAT(IFACE, _)), METHOD)

#define PSIO1_IMPORT_EMIT_THUNK(r, IFACE, METHOD)                               \
   extern "C" [[clang::import_module(BOOST_PP_STRINGIZE(IFACE)),               \
                 clang::import_name(BOOST_PP_STRINGIZE(METHOD))]]              \
   ::psizam::flat_val PSIO1_IMPORT_RAW_NAME(IFACE, METHOD)(                    \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val);

#define PSIO1_IMPORT_EMIT_CALL_FN(r, IFACE, METHOD)                              \
   template <typename... _PsioArgs>                                            \
   inline ::psizam::flat_val                                                   \
   BOOST_PP_CAT(BOOST_PP_CAT(_psio_import_call_, IFACE), BOOST_PP_CAT(_, METHOD)) \
      (_PsioArgs&&... args) {                                                  \
      ::psizam::flat_val _s[16] = {};                                          \
      ::std::size_t _i = 0;                                                    \
      (::psizam::guest_import_lower(_s, _i, args), ...);                       \
      return PSIO1_IMPORT_RAW_NAME(IFACE, METHOD)(                             \
         _s[0],  _s[1],  _s[2],  _s[3],                                       \
         _s[4],  _s[5],  _s[6],  _s[7],                                       \
         _s[8],  _s[9],  _s[10], _s[11],                                      \
         _s[12], _s[13], _s[14], _s[15]);                                      \
   }

#define PSIO1_GUEST_IMPORTS(IFACE, ...)                                         \
   BOOST_PP_SEQ_FOR_EACH(PSIO1_IMPORT_EMIT_THUNK, IFACE,                        \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))                \
   BOOST_PP_SEQ_FOR_EACH(PSIO1_IMPORT_EMIT_CALL_FN, IFACE,                      \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

// ── PSIO1_IMPORT_IMPL — generate full import method bodies ───────────────────
// Uses ImportProxy for automatic lowering/lifting including return-area
// protocol. Usage:
//
//   PSIO1_IMPORT_IMPL(greeter, add, concat, translate, ...)
//
// Generates both raw import declarations AND method bodies for each
// method of the interface. The method signature comes from the static
// declaration in shared.hpp.

#define PSIO1_IMPORT_IMPL_ONE(r, IFACE, METHOD)                                 \
   PSIO1_IMPORT_EMIT_THUNK(r, IFACE, METHOD)

#define PSIO1_IMPORT_IMPL_BODY(IFACE, METHOD, ...)                               \
   {                                                                            \
      return ::psizam::ImportProxy::call_impl<decltype(&IFACE::METHOD)>(        \
         reinterpret_cast<::psizam::raw_import_fn>(                             \
            &PSIO1_IMPORT_RAW_NAME(IFACE, METHOD)),                              \
         __VA_ARGS__);                                                          \
   }

#define PSIO1_IMPORT_IMPL(IFACE, ...)                                            \
   BOOST_PP_SEQ_FOR_EACH(PSIO1_IMPORT_IMPL_ONE, IFACE,                           \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

#else  // !__wasm__

// Host build: no thunks. The impl class is plain C++ code; native
// tests instantiate and call it directly.
#define PSIO1_MODULE(IMPL, ...) /* guest-only */
#define PSIO1_GUEST_IMPORTS(IFACE, ...) /* guest-only */
#define PSIO1_IMPORT_IMPL(IFACE, ...) /* guest-only */

#endif  // __wasm__
