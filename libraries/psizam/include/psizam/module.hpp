#pragma once

// PSIO_MODULE — guest-side macro that binds an impl class to one or
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
//   PSIO_MODULE(greeter_impl, concat)
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
// Grammar note: PSIO_HOST_MODULE uses `interface(Tag, m…)` entries to
// tie methods to reflection tags on the host side. The guest doesn't
// consume that tag at runtime — the canonical-ABI wire format is
// self-describing via the WIT side — so we take a flat method list and
// keep the macro minimal. The shared-header interface declarations
// (via PSIO_INTERFACE) remain the authoritative contract.
//
// Return-shape caveat (v1): only returns that lower to a single flat
// slot are wired. Methods whose return type lowers to >1 flat slot
// (i.e. strings and records on an export return) need canonical
// return-area threading (an i32 return pointer) which is a follow-up;
// a static_assert inside ComponentProxy catches anything that exceeds
// the supported shape at instantiation time.

#include <psizam/component_proxy.hpp>  // ComponentProxy, flat_val
#include <psio/wit_constexpr.hpp>      // constexpr WIT generation

#include <boost/preprocessor/cat.hpp>
#include <boost/preprocessor/seq/for_each.hpp>
#include <boost/preprocessor/stringize.hpp>
#include <boost/preprocessor/variadic/to_seq.hpp>

#ifdef __wasm__

// ── PSIO_WIT_SECTION — embed WIT text as a WASM custom section ──────────────
// Emits a `component-type:NAME` custom section containing the WIT text
// generated at compile time from the PSIO_INTERFACE reflection. The
// linker reads this section to discover type signatures for module-to-
// module wiring.
//
//   PSIO_WIT_SECTION(greeter)   // embeds greeter's WIT
//   PSIO_WIT_SECTION(env)       // embeds env's WIT (imports)

namespace psio::constexpr_wit {
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

#define PSIO_WIT_SECTION(IFACE)                                                \
   __attribute__((section(".custom_section.component-type:"                    \
                          BOOST_PP_STRINGIZE(IFACE)),                          \
                  used))                                                       \
   static const auto BOOST_PP_CAT(_psio_wit_sec_, IFACE) =                    \
      ::psio::constexpr_wit::make_section_blob<IFACE>();

// ── guest thunk emission ─────────────────────────────────────────────────────
// One `extern "C"` function per method, decorated with clang's
// export_name so the linker publishes it under the WIT method name.
// The body routes through ComponentProxy which does lift → invoke →
// lower against canonical ABI flat slots.

#define PSIO_MODULE_INSTANCE_NAME(IMPL) \
   BOOST_PP_CAT(_psio_module_instance_, IMPL)

// BOOST_PP_STRINGIZE (not `#METHOD`) is required because SEQ_FOR_EACH
// hands the callback a still-deferred expression — plain `#METHOD`
// would stringify the raw token stream (`BOOST_PP_SEQ_HEAD(…)`). The
// STRINGIZE macro forces expansion before the stringify step.
#define PSIO_MODULE_EMIT_THUNK(r, IMPL, METHOD)                                \
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
         &PSIO_MODULE_INSTANCE_NAME(IMPL),                                     \
         a0, a1, a2, a3, a4, a5, a6, a7,                                       \
         a8, a9, a10, a11, a12, a13, a14, a15);                                \
   }

#define PSIO_MODULE(IMPL, ...)                                                 \
   static IMPL PSIO_MODULE_INSTANCE_NAME(IMPL){};                              \
   BOOST_PP_SEQ_FOR_EACH(PSIO_MODULE_EMIT_THUNK, IMPL,                         \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

// ── guest import thunk emission ──────────────────────────────────────────────
// Mirror of PSIO_MODULE but for imports: generates method bodies that
// lower C++ args → 16 flat_vals, call the raw WASM import, and lift
// the return. Data is already in guest linear memory (no alloc/copy).

namespace psizam {

template <typename T>
void guest_import_lower(flat_val* slots, size_t& idx, const T& v) {
   using U = std::remove_cvref_t<T>;
   if constexpr (std::is_same_v<U, bool>)
      slots[idx++] = v ? 1 : 0;
   else if constexpr (std::is_integral_v<U> && sizeof(U) <= 4)
      slots[idx++] = static_cast<flat_val>(static_cast<uint32_t>(v));
   else if constexpr (std::is_integral_v<U> && sizeof(U) == 8)
      slots[idx++] = static_cast<flat_val>(v);
   else if constexpr (std::is_same_v<U, float>) {
      union { float f; int32_t i; } u; u.f = v;
      slots[idx++] = static_cast<flat_val>(u.i);
   }
   else if constexpr (std::is_same_v<U, double>) {
      union { double f; int64_t i; } u; u.f = v;
      slots[idx++] = u.i;
   }
   else if constexpr (std::is_enum_v<U>)
      guest_import_lower(slots, idx, static_cast<std::underlying_type_t<U>>(v));
   else if constexpr (psio::detail::is_psio_own<U>::value)
      slots[idx++] = static_cast<flat_val>(v.handle);
   else if constexpr (psio::detail::is_psio_borrow<U>::value)
      slots[idx++] = static_cast<flat_val>(v.handle);
   else if constexpr (std::is_same_v<U, std::monostate>) {
   }
   else if constexpr (std::is_same_v<U, std::string_view> ||
                      std::is_same_v<U, psio::owned<std::string, psio::wit>>) {
      slots[idx++] = static_cast<flat_val>(reinterpret_cast<uintptr_t>(v.data()));
      slots[idx++] = static_cast<flat_val>(v.size());
   }
   else if constexpr (psio::detail::is_std_vector_ct<U>::value) {
      slots[idx++] = static_cast<flat_val>(reinterpret_cast<uintptr_t>(v.data()));
      slots[idx++] = static_cast<flat_val>(v.size());
   }
   else if constexpr (detail_dispatch::is_wit_vector<U>::value) {
      slots[idx++] = static_cast<flat_val>(reinterpret_cast<uintptr_t>(v.data()));
      slots[idx++] = static_cast<flat_val>(v.size());
   }
   else if constexpr (detail_dispatch::is_std_span<U>::value) {
      slots[idx++] = static_cast<flat_val>(reinterpret_cast<uintptr_t>(v.data()));
      slots[idx++] = static_cast<flat_val>(v.size());
   }
   else if constexpr (psio::is_std_tuple<U>::value) {
      [&]<size_t... Is>(std::index_sequence<Is...>) {
         (guest_import_lower(slots, idx, std::get<Is>(v)), ...);
      }(std::make_index_sequence<std::tuple_size_v<U>>{});
   }
   else if constexpr (psio::detail::is_std_optional_ct<U>::value) {
      if (v.has_value()) {
         slots[idx++] = 1;
         guest_import_lower(slots, idx, *v);
      } else {
         slots[idx++] = 0;
         using E = typename psio::detail::optional_elem_ct<U>::type;
         constexpr size_t payload_count = psio::canonical_flat_count_v<E>;
         for (size_t i = 0; i < payload_count; i++)
            slots[idx++] = 0;
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
      slots[idx++] = static_cast<flat_val>(v.index());
      size_t payload_start = idx;
      std::visit([&](const auto& alt) {
         using A = std::remove_cvref_t<decltype(alt)>;
         if constexpr (!std::is_same_v<A, std::monostate>)
            guest_import_lower(slots, idx, alt);
      }, v);
      size_t emitted = idx - payload_start;
      for (size_t i = emitted; i < max_payload; i++)
         slots[idx++] = 0;
   }
   else if constexpr (std::is_array_v<U>) {
      constexpr uint32_t n = std::extent_v<U>;
      for (uint32_t i = 0; i < n; i++)
         guest_import_lower(slots, idx, v[i]);
   }
   else if constexpr (psio::Reflected<U>) {
      psio::apply_members(
         (typename psio::reflect<U>::data_members*)nullptr,
         [&](auto... ptrs) {
            (guest_import_lower(slots, idx, v.*ptrs), ...);
         }
      );
   }
   else {
      static_assert(sizeof(U) == 0, "guest_import_lower: unsupported type");
   }
}

} // namespace psizam

#define PSIO_IMPORT_RAW_NAME(IFACE, METHOD) \
   BOOST_PP_CAT(BOOST_PP_CAT(_psio_raw_, BOOST_PP_CAT(IFACE, _)), METHOD)

#define PSIO_IMPORT_EMIT_THUNK(r, IFACE, METHOD)                               \
   extern "C" [[clang::import_module(BOOST_PP_STRINGIZE(IFACE)),               \
                 clang::import_name(BOOST_PP_STRINGIZE(METHOD))]]              \
   ::psizam::flat_val PSIO_IMPORT_RAW_NAME(IFACE, METHOD)(                    \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val,                                  \
      ::psizam::flat_val, ::psizam::flat_val);

#define PSIO_IMPORT_EMIT_CALL_FN(r, IFACE, METHOD)                              \
   template <typename... _PsioArgs>                                            \
   inline ::psizam::flat_val                                                   \
   BOOST_PP_CAT(BOOST_PP_CAT(_psio_import_call_, IFACE), BOOST_PP_CAT(_, METHOD)) \
      (_PsioArgs&&... args) {                                                  \
      ::psizam::flat_val _s[16] = {};                                          \
      ::std::size_t _i = 0;                                                    \
      (::psizam::guest_import_lower(_s, _i, args), ...);                       \
      return PSIO_IMPORT_RAW_NAME(IFACE, METHOD)(                             \
         _s[0],  _s[1],  _s[2],  _s[3],                                       \
         _s[4],  _s[5],  _s[6],  _s[7],                                       \
         _s[8],  _s[9],  _s[10], _s[11],                                      \
         _s[12], _s[13], _s[14], _s[15]);                                      \
   }

#define PSIO_GUEST_IMPORTS(IFACE, ...)                                         \
   BOOST_PP_SEQ_FOR_EACH(PSIO_IMPORT_EMIT_THUNK, IFACE,                        \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))                \
   BOOST_PP_SEQ_FOR_EACH(PSIO_IMPORT_EMIT_CALL_FN, IFACE,                      \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

// ── PSIO_IMPORT_IMPL — generate full import method bodies ───────────────────
// Uses ImportProxy for automatic lowering/lifting including return-area
// protocol. Usage:
//
//   PSIO_IMPORT_IMPL(greeter, add, concat, translate, ...)
//
// Generates both raw import declarations AND method bodies for each
// method of the interface. The method signature comes from the static
// declaration in shared.hpp.

#define PSIO_IMPORT_IMPL_ONE(r, IFACE, METHOD)                                 \
   PSIO_IMPORT_EMIT_THUNK(r, IFACE, METHOD)

#define PSIO_IMPORT_IMPL_BODY(IFACE, METHOD, ...)                               \
   {                                                                            \
      return ::psizam::ImportProxy::call_impl<decltype(&IFACE::METHOD)>(        \
         reinterpret_cast<::psizam::raw_import_fn>(                             \
            &PSIO_IMPORT_RAW_NAME(IFACE, METHOD)),                              \
         __VA_ARGS__);                                                          \
   }

#define PSIO_IMPORT_IMPL(IFACE, ...)                                            \
   BOOST_PP_SEQ_FOR_EACH(PSIO_IMPORT_IMPL_ONE, IFACE,                           \
                         BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

#else  // !__wasm__

// Host build: no thunks. The impl class is plain C++ code; native
// tests instantiate and call it directly.
#define PSIO_MODULE(IMPL, ...) /* guest-only */
#define PSIO_GUEST_IMPORTS(IFACE, ...) /* guest-only */
#define PSIO_IMPORT_IMPL(IFACE, ...) /* guest-only */

#endif  // __wasm__
