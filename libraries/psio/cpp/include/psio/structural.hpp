#pragma once

// Structural-metadata macros for Phase B of WIT integration.
//
//   PSIO_PACKAGE(name, version)              — L2 step 2
//   PSIO_INTERFACE(name, types(…), funcs(…)) — step 3
//   PSIO_WORLD(name, imports(…), exports(…)) — step 4
//   PSIO_USE(package, interface, version)    — step 4
//   PSIO_IMPL(ImplClass, Tag, methods…)      — step 6 (bind C++ impl
//                                              to an interface/use tag)
//
// Each macro populates a compile-time, tag-keyed registry. Consumers
// (SchemaBuilder::insert_world, emit_wit, Linker::provide) walk those
// registries to assemble the Schema envelope (Package / Interface /
// World / Use) or resolve component-model wiring at instantiation.
//
// See doc/phase-b-structural.md for the full design.

#include <psio/reflect.hpp>  // for psio::FixedString

#include <array>
#include <string_view>
#include <tuple>

namespace psio
{

// Reverse lookup: T → interface tag. Specialized by PSIO_INTERFACE for
// every type listed in its `types(…)` argument. Undefined for
// interface-less types, which is what PSIO_WORLD / SchemaBuilder use
// to detect "this type is a raw reflected value, not interface-owned."
template <typename T>
struct interface_of;

// Reverse lookup: ImplClass → interface/use tag. Specialized by
// PSIO_IMPL. The Linker uses this in `provide(impl)` to find the slot
// that a host implementation fills, without the caller naming the tag
// explicitly. Undefined for types that aren't registered as impls —
// `requires` / SFINAE on this serves as the compile-time opt-in check
// that something is indeed a host impl.
template <typename ImplClass>
struct impl_of;

namespace detail
{

// Primary template. A specialization is synthesized by PSIO_PACKAGE.
// Calling sites that read `psio_current_package::name` before any
// PSIO_PACKAGE has been declared will fail with a concrete compile
// error ("incomplete type") rather than a silent empty value.
template <FixedString Name>
struct package_info;

// ODR-level marker: giving the *variable* a single fixed name means
// declaring two different packages in the same translation unit
// produces a redefinition error with mismatched types. Declaring the
// same package twice (same name/version) is harmless because
// `inline constexpr` permits repeated definitions across includes.
template <FixedString Name>
struct package_marker
{
   static constexpr FixedString name = Name;
};

// Interface registry — specialized by PSIO_INTERFACE. Each
// specialization carries:
//   ::name        — FixedString, matches the macro's NAME argument
//   ::package     — type alias to the containing package_info
//   ::types       — std::tuple<…> of reflected type names in this iface
//   ::funcs       — std::tuple of &free_fn pointers for this iface
template <typename Tag>
struct interface_info;

// Per-interface ODR marker. The variable name embeds the tag type so
// two `PSIO_INTERFACE(same_name, …)` in different TUs collapse cleanly
// while distinct interfaces never collide.
template <typename Tag>
struct interface_marker
{
};

// Cross-package use registry — specialized by PSIO_USE. Each
// specialization carries:
//   ::package         — FixedString, the imported package name
//   ::interface_name  — FixedString, the interface within that package
//   ::version         — FixedString, the version requirement
template <typename Tag>
struct use_info;

template <typename Tag>
struct use_marker
{
};

// World registry — specialized by PSIO_WORLD. Each specialization
// carries:
//   ::name     — FixedString, the world name
//   ::package  — type alias to the containing package_info
//   ::imports  — std::tuple<…> of use tags (from PSIO_USE)
//   ::exports  — std::tuple<…> of interface tags (from PSIO_INTERFACE)
template <typename Tag>
struct world_info;

template <typename Tag>
struct world_marker
{
};

// Per-impl metadata — specialized by PSIO_IMPL. Each specialization
// carries:
//   ::tag     — the interface/use tag this impl fills
//   ::methods — std::tuple of member-function pointers bound by the
//               macro, in declaration order
//   ::names   — std::array<string_view, N> of corresponding method
//               names (parallel to ::methods), used by the linker to
//               match WASM import symbols to C++ members
//
// Both this and psio::impl_of<ImplClass> act as ODR anchors — two
// PSIO_IMPL declarations for the same impl class in different TUs
// with mismatched argument lists produce a template-redefinition
// error. No separate marker variable is needed.
template <typename ImplClass>
struct impl_info;

}  // namespace detail

// Convenience: T → package. Resolved transitively through the
// interface_of reverse map, so a type that belongs to interface kernel
// which lives in package psibase satisfies
//   package_of<T>::name == "psibase".
template <typename T>
struct package_of
{
   using interface_tag = typename interface_of<T>::type;
   using package       = typename detail::interface_info<interface_tag>::package;
   static constexpr auto name = package::name;
};

}  // namespace psio

// ── PSIO_PACKAGE(name, version) ──────────────────────────────────────────
//
// Declares the authoring package for the current translation unit.
//
//   PSIO_PACKAGE(psibase, "0.3.0")
//
// Constraints:
//
//   • Must appear at the global namespace scope. The expansion re-opens
//     ::psio::detail, which is only reachable from the global scope.
//     Using the macro inside another namespace will produce a nested
//     psio::detail relative to that namespace and the package_info
//     specialization will fail to find the primary template.
//
//   • Only one per translation unit. A second PSIO_PACKAGE with a
//     different name triggers a redefinition error on _psio_pkg (whose
//     type differs between the two expansions).
//
//   • Headers that declare a package (e.g. `wasi_io_streams.hpp`) must
//     be included at most once per TU; the `inline constexpr` marker
//     handles cross-TU ODR automatically.
//
// After expansion:
//
//   psio::detail::package_info<FixedString{"psibase"}>::name    == "psibase"
//   psio::detail::package_info<FixedString{"psibase"}>::version == "0.3.0"
//   psio_current_package is a TU-local alias to that specialization
//   (consumed by PSIO_INTERFACE / PSIO_WORLD / PSIO_USE in step 3+).

#define PSIO_PACKAGE(NAME, VERSION)                                             \
   namespace psio::detail                                                       \
   {                                                                            \
      template <>                                                               \
      struct package_info<::psio::FixedString{#NAME}>                           \
      {                                                                         \
         static constexpr ::psio::FixedString name    = #NAME;                  \
         static constexpr ::psio::FixedString version = VERSION;                \
      };                                                                        \
      inline constexpr package_marker<::psio::FixedString{#NAME}> _psio_pkg{};  \
   }                                                                            \
   using psio_current_package =                                                 \
       ::psio::detail::package_info<::psio::FixedString{#NAME}>

// ── PSIO_INTERFACE(name, types(…), funcs(…)) ─────────────────────────────
//
// Registers a named group of reflected types and free functions against
// the current translation unit's package (see PSIO_PACKAGE). A call like
//
//   PSIO_INTERFACE(kernel,
//       types(psibase::Block, psibase::Transaction),
//       funcs(psibase::submit_tx, psibase::query_chain))
//
// populates three registries:
//
//   • psio::detail::interface_info<kernel_interface_tag> — name, package,
//     type tuple, free-function pointer tuple
//   • psio::detail::_psio_iface_kernel  — inline constexpr ODR marker
//   • psio::interface_of<T>::type       — reverse lookup for each T in
//     the types(…) list
//
// Constraints:
//
//   • Must be at global namespace scope (same reason as PSIO_PACKAGE).
//   • A PSIO_PACKAGE must precede it in the same TU; psio_current_package
//     is referenced and an undeclared alias produces a clear error.
//   • Type and function identifiers must be fully-qualified (or
//     otherwise visible) at global scope.
//   • Member functions (resource methods) go through PSIO_REFLECT's
//     `method(…)` grammar on the resource class, not funcs(…) here.
//
// Sub-argument grammar: `types(…)` and `funcs(…)` look like function
// calls in source. They are parsed by token-prefix match (same pattern
// as PSIO_REFLECT's method(…)): the preprocessor concatenates
// `PSIO_IFACE_UNWRAP_TYPES_` with the literal token `types`, yielding a
// macro that expands to the parenthesized argument list.

#define PSIO_IFACE_UNWRAP_TYPES(X) BOOST_PP_CAT(PSIO_IFACE_UNWRAP_TYPES_, X)
#define PSIO_IFACE_UNWRAP_TYPES_types(...) __VA_ARGS__

#define PSIO_IFACE_UNWRAP_FUNCS(X) BOOST_PP_CAT(PSIO_IFACE_UNWRAP_FUNCS_, X)
#define PSIO_IFACE_UNWRAP_FUNCS_funcs(...) __VA_ARGS__

#define PSIO_IFACE_ADDR_OF(s, _, fn) &fn
#define PSIO_IFACE_FN_NAME(s, _, fn) #fn

#define PSIO_IFACE_REV_LOOKUP(s, TAG, T)                             \
   template <>                                                       \
   struct ::psio::interface_of<T>                                    \
   {                                                                 \
      using type = TAG;                                              \
   };

#define PSIO_INTERFACE(NAME, TYPES_SPEC, FUNCS_SPEC)                               \
   PSIO_INTERFACE_I(NAME,                                                          \
                    BOOST_PP_VARIADIC_TO_SEQ(PSIO_IFACE_UNWRAP_TYPES(TYPES_SPEC)), \
                    BOOST_PP_VARIADIC_TO_SEQ(PSIO_IFACE_UNWRAP_FUNCS(FUNCS_SPEC)))

#define PSIO_INTERFACE_I(NAME, TYPES_SEQ, FUNCS_SEQ)                                 \
   namespace psio::detail                                                            \
   {                                                                                 \
      struct BOOST_PP_CAT(NAME, _interface_tag)                                      \
      {                                                                              \
      };                                                                             \
      template <>                                                                    \
      struct interface_info<BOOST_PP_CAT(NAME, _interface_tag)>                      \
      {                                                                              \
         static constexpr ::psio::FixedString name = #NAME;                          \
         using package                             = ::psio_current_package;         \
         using types = ::std::tuple<PSIO_SEQ_TO_VA_ARGS(TYPES_SEQ)>;                 \
         static constexpr auto funcs = ::std::tuple{PSIO_SEQ_TO_VA_ARGS(             \
             BOOST_PP_SEQ_TRANSFORM(PSIO_IFACE_ADDR_OF, _, FUNCS_SEQ))};             \
         static constexpr ::std::array<::std::string_view,                           \
                                       BOOST_PP_SEQ_SIZE(FUNCS_SEQ)>                 \
             func_names{PSIO_SEQ_TO_VA_ARGS(                                         \
                 BOOST_PP_SEQ_TRANSFORM(PSIO_IFACE_FN_NAME, _, FUNCS_SEQ))};         \
      };                                                                             \
      inline constexpr interface_marker<BOOST_PP_CAT(NAME, _interface_tag)>          \
          BOOST_PP_CAT(_psio_iface_, NAME){};                                        \
   }                                                                                 \
   BOOST_PP_SEQ_FOR_EACH(PSIO_IFACE_REV_LOOKUP,                                      \
                         ::psio::detail::BOOST_PP_CAT(NAME, _interface_tag),         \
                         TYPES_SEQ)

// ── PSIO_USE(package, interface, version) ────────────────────────────────
//
// Declares a cross-package dependency. A world that imports an
// interface from a different package needs one of these so that
// emit_wit (Phase C) can produce a `use` statement instead of
// attempting a local redeclaration.
//
//   PSIO_USE(wasi, streams, "0.2.0")
//
// Both `package` and `interface` must be plain C++ identifiers. The
// generated tag type is `<package>_<interface>_use_tag` inside
// psio::detail. That tag is referenced unqualified inside PSIO_WORLD's
// imports(…) list.
//
// Interfaces with dotted/slashed WIT paths (e.g. `wasi:io/streams`)
// should pick a representative identifier for the interface part
// (e.g. `streams`); the full string form lands on the IR via
// `use_info::interface_name`. Identifier-level ambiguity across
// packages is resolved by the package prefix in the tag name.
//
// Must appear at global scope, same as PSIO_PACKAGE / PSIO_INTERFACE.
// Multiple PSIO_USE declarations per TU are allowed — they populate
// distinct tag types.

#define PSIO_USE(PACKAGE, INTERFACE, VERSION)                                       \
   namespace psio::detail                                                           \
   {                                                                                \
      struct BOOST_PP_CAT(BOOST_PP_CAT(PACKAGE, _),                                 \
                          BOOST_PP_CAT(INTERFACE, _use_tag))                        \
      {                                                                             \
      };                                                                            \
      template <>                                                                   \
      struct use_info<BOOST_PP_CAT(BOOST_PP_CAT(PACKAGE, _),                        \
                                   BOOST_PP_CAT(INTERFACE, _use_tag))>              \
      {                                                                             \
         static constexpr ::psio::FixedString package        = #PACKAGE;            \
         static constexpr ::psio::FixedString interface_name = #INTERFACE;          \
         static constexpr ::psio::FixedString version        = VERSION;             \
      };                                                                            \
      inline constexpr use_marker<BOOST_PP_CAT(BOOST_PP_CAT(PACKAGE, _),            \
                                               BOOST_PP_CAT(INTERFACE, _use_tag))>  \
          BOOST_PP_CAT(BOOST_PP_CAT(_psio_use_, PACKAGE),                           \
                       BOOST_PP_CAT(_, INTERFACE)){};                               \
   }

// ── PSIO_WORLD(name, imports(…), exports(…)) ─────────────────────────────
//
// Top-level composition: which interfaces this world imports and which
// it exports.
//
//   PSIO_WORLD(node,
//       imports(wasi_streams_use_tag, wasi_clocks_use_tag),
//       exports(kernel_interface_tag, accounts_interface_tag))
//
// Both `imports(…)` and `exports(…)` accept unqualified tag identifiers.
// The macro expansion places them inside `namespace psio::detail`, which
// is where PSIO_INTERFACE and PSIO_USE registered the tags, so
// unqualified lookup finds them without the user re-typing the
// `::psio::detail::` prefix.
//
// Must be at global scope and follow PSIO_PACKAGE in the same TU.
// (`psio_current_package` is consumed.)

#define PSIO_WORLD_UNWRAP_IMPORTS(X) BOOST_PP_CAT(PSIO_WORLD_UNWRAP_IMPORTS_, X)
#define PSIO_WORLD_UNWRAP_IMPORTS_imports(...) __VA_ARGS__

#define PSIO_WORLD_UNWRAP_EXPORTS(X) BOOST_PP_CAT(PSIO_WORLD_UNWRAP_EXPORTS_, X)
#define PSIO_WORLD_UNWRAP_EXPORTS_exports(...) __VA_ARGS__

#define PSIO_WORLD(NAME, IMPORTS_SPEC, EXPORTS_SPEC) \
   PSIO_WORLD_I(NAME, PSIO_WORLD_UNWRAP_IMPORTS(IMPORTS_SPEC),  \
                PSIO_WORLD_UNWRAP_EXPORTS(EXPORTS_SPEC))

#define PSIO_WORLD_I(NAME, IMPORTS_VA, EXPORTS_VA)                             \
   namespace psio::detail                                                      \
   {                                                                           \
      struct BOOST_PP_CAT(NAME, _world_tag)                                    \
      {                                                                        \
      };                                                                       \
      template <>                                                              \
      struct world_info<BOOST_PP_CAT(NAME, _world_tag)>                        \
      {                                                                        \
         static constexpr ::psio::FixedString name = #NAME;                    \
         using package                             = ::psio_current_package;   \
         using imports                             = ::std::tuple<IMPORTS_VA>; \
         using exports                             = ::std::tuple<EXPORTS_VA>; \
      };                                                                       \
      inline constexpr world_marker<BOOST_PP_CAT(NAME, _world_tag)>            \
          BOOST_PP_CAT(_psio_world_, NAME){};                                  \
   }

// ── PSIO_IMPL(ImplClass, Tag, methods…) ──────────────────────────────────
//
// Registers ImplClass as the C++ host implementation of the interface
// or use identified by Tag. A world-building linker uses this to
// resolve `linker.provide(impl)` back to the slot that `impl` fills —
// the caller never names the tag.
//
//   // Assume: PSIO_INTERFACE(kernel, types(...), funcs(...))
//   struct KernelImpl { int submit_tx(int x); int query_chain(); };
//   PSIO_IMPL(KernelImpl, kernel_interface_tag, submit_tx, query_chain)
//
// Effects:
//
//   • psio::impl_of<KernelImpl>::type aliased to kernel_interface_tag
//     (the reverse lookup `provide(impl)` consumes).
//   • psio::detail::impl_info<KernelImpl> specialized with:
//       ::tag     = kernel_interface_tag
//       ::methods = std::tuple{&KernelImpl::submit_tx,
//                              &KernelImpl::query_chain}
//       ::names   = std::array{"submit_tx", "query_chain"}
//
// Constraints:
//
//   • Must appear at global namespace scope. The expansion re-opens
//     ::psio::detail, which is only reachable from the global scope.
//   • Tag must be a type already declared in ::psio::detail (typically
//     by PSIO_INTERFACE or PSIO_USE). Written unqualified here because
//     unqualified lookup inside psio::detail finds it; the impl_of
//     specialization outside the namespace uses the `::psio::detail::`
//     prefix.
//   • ImplClass must be complete and visible from global scope (or
//     reachable via a fully-qualified name).
//   • Each method identifier must be a member function of ImplClass.
//   • One PSIO_IMPL per ImplClass across all TUs (redefinition error on
//     impl_info<ImplClass> otherwise). Multiple impls of the same tag
//     by different impl classes are allowed — that's the whole point
//     of being able to swap a mock for a production implementation.
//
// Method-signature checking is intentionally NOT performed here. The
// Linker validates signatures at `provide(impl)` time against the
// interface's declared funcs, because that's when it has access to
// both sides and can emit a composed error pointing at the mismatched
// method.

#define PSIO_IMPL_MEMBER_PTR(s, IMPL, METHOD)  &IMPL::METHOD
#define PSIO_IMPL_METHOD_NAME(s, _, METHOD)    #METHOD

#define PSIO_IMPL(IMPL, TAG, ...)                                                \
   PSIO_IMPL_I(IMPL, TAG, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))

#define PSIO_IMPL_I(IMPL, TAG, METHODS_SEQ)                                      \
   template <>                                                                   \
   struct ::psio::impl_of<IMPL>                                                  \
   {                                                                             \
      using type = ::psio::detail::TAG;                                          \
   };                                                                            \
   namespace psio::detail                                                        \
   {                                                                             \
      template <>                                                                \
      struct impl_info<IMPL>                                                     \
      {                                                                          \
         using tag = TAG;                                                        \
         static constexpr auto methods = ::std::tuple{PSIO_SEQ_TO_VA_ARGS(       \
             BOOST_PP_SEQ_TRANSFORM(PSIO_IMPL_MEMBER_PTR, IMPL, METHODS_SEQ))};  \
         static constexpr ::std::array<::std::string_view,                       \
                                       BOOST_PP_SEQ_SIZE(METHODS_SEQ)>           \
             names{PSIO_SEQ_TO_VA_ARGS(                                          \
                 BOOST_PP_SEQ_TRANSFORM(PSIO_IMPL_METHOD_NAME, _, METHODS_SEQ))};\
      };                                                                         \
   }
