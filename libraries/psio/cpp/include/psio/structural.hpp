#pragma once

// Structural-metadata macros for Phase B of WIT integration.
//
//   PSIO_PACKAGE(name, version)              — L2 step 2
//   PSIO_INTERFACE(name, types(…), funcs(…)) — step 3
//   PSIO_WORLD(name, imports(…), exports(…)) — step 4
//   PSIO_USE(package, interface, version)    — step 4
//   PSIO_HOST_MODULE(ImplClass, interface(Tag, m…)…)— step 6 (bind C++ impl
//                                              to one or more interfaces)
//
// Each macro populates a compile-time, tag-keyed registry. Consumers
// (SchemaBuilder::insert_world, emit_wit, Linker::provide) walk those
// registries to assemble the Schema envelope (Package / Interface /
// World / Use) or resolve component-model wiring at instantiation.
//
// Interface tags are user-authored types: the same class that declares
// the interface's static member functions is used directly as the tag.
// No separate `*_interface_tag` helper type exists.

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

// Reverse lookup: ImplClass → tuple of interface tags it implements.
// Specialized by PSIO_HOST_MODULE. The Linker uses this in `provide(impl)` to
// find all slots that a host implementation fills. Undefined for types
// that aren't registered as impls.
template <typename ImplClass>
struct impl_of;

namespace detail
{

// Primary template. A specialization is synthesized by PSIO_PACKAGE.
template <FixedString Name>
struct package_info;

template <FixedString Name>
struct package_marker
{
   static constexpr FixedString name = Name;
};

// Interface registry — specialized by PSIO_INTERFACE. The Tag is the
// user's interface class itself (the one that carries static member
// functions corresponding to the interface's operations).
template <typename Tag>
struct interface_info;

template <typename Tag>
struct interface_marker
{
};

// Cross-package use registry — specialized by PSIO_USE.
template <typename Tag>
struct use_info;

template <typename Tag>
struct use_marker
{
};

// World registry — specialized by PSIO_WORLD.
template <typename Tag>
struct world_info;

template <typename Tag>
struct world_marker
{
};

// Per-(Impl, Tag) registry — specialized by each `interface(...)` entry
// in PSIO_HOST_MODULE. Carries:
//   ::tag     = Tag
//   ::methods = std::tuple<&Impl::m1, &Impl::m2, …>
//   ::names   = std::array<string_view, N>{"m1", "m2", …}
template <typename ImplClass, typename Tag>
struct iface_impl;

// Aggregate per-Impl registry — specialized by PSIO_HOST_MODULE. Carries the
// tuple of iface_impl<Impl, Tag> instantiations that this host fills.
template <typename ImplClass>
struct impl_info;

// Variadic helper so the PSIO_HOST_MODULE macro only has to emit a
// parenthesis-friendly list of tag types. The template-instantiation
// machinery assembles the `iface_impl<Impl, Tag>` tuple for us,
// sidestepping Boost.PP's inability to treat commas inside `< >` as
// part of a single sequence element.
template <typename Impl, typename... Tags>
using iface_impls_tuple = std::tuple<iface_impl<Impl, Tags>...>;

}  // namespace detail

// Convenience: T → package. Resolved transitively through the
// interface_of reverse map.
template <typename T>
struct package_of
{
   using interface_tag = typename interface_of<T>::type;
   using package       = typename detail::interface_info<interface_tag>::package;
   static constexpr auto name = package::name;
};

}  // namespace psio

// ── PSIO_PACKAGE(name, version) ──────────────────────────────────────────

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
// Registers the user-authored class `name` as a WIT interface within
// the current translation unit's package. Example:
//
//   struct greeter {
//      static void run(std::uint64_t count);
//   };
//   PSIO_INTERFACE(greeter, types(), funcs(func(run, count)))
//
// The class itself is both the reflection anchor (we take addresses of
// its static members) and the tag used by downstream registries.
//
// Must appear at global namespace scope. `name` must be an unqualified
// identifier that names a type at global scope (or introduce a `using`
// alias at global scope for namespace-scoped types).

#define PSIO_IFACE_UNWRAP_TYPES(X) BOOST_PP_CAT(PSIO_IFACE_UNWRAP_TYPES_, X)
#define PSIO_IFACE_UNWRAP_TYPES_types(...) __VA_ARGS__

#define PSIO_IFACE_UNWRAP_FUNCS(X) BOOST_PP_CAT(PSIO_IFACE_UNWRAP_FUNCS_, X)
#define PSIO_IFACE_UNWRAP_FUNCS_funcs(...) __VA_ARGS__

// ── funcs(...) entry normalization ────────────────────────────────────────
//
// Each entry in funcs(...) is either a bare identifier or the canonical
// `func(ident, arg1, arg2, …)` form. Both normalize to PSIO_REFLECT's
// `method(ident, args…)` grammar so the shared helpers apply.

#define PSIO_IFACE_MATCH_func(...) ~, 1
#define PSIO_IFACE_ARGS_func(...)  __VA_ARGS__

#define PSIO_IFACE_NORMALIZE_ENTRY(r, _, item)                                       \
   BOOST_PP_IIF(                                                                     \
       BOOST_PP_CHECK_EMPTY(item), ,                                                 \
       BOOST_PP_IIF(PSIO_MATCH(PSIO_IFACE_MATCH_, item),                             \
                    PSIO_IFACE_NORMALIZE_MATCHED,                                    \
                    PSIO_IFACE_NORMALIZE_BARE)(item))

#define PSIO_IFACE_NORMALIZE_MATCHED(item) method(BOOST_PP_CAT(PSIO_IFACE_ARGS_, item))
#define PSIO_IFACE_NORMALIZE_BARE(item)    method(item)

// Reflection only touches the *type* of each interface method, not its
// address: `decltype(&ANCHOR::m)` is unevaluated and does not ODR-use
// the function, so the anchor struct in shared.hpp can be a pure
// declaration (no inline bodies, no stubs). Bodies live where they
// belong — guest.cpp on the guest, Host::m wired by PSIO_HOST_MODULE on the
// host.
#define PSIO_IFACE_FN_TYPE(r, ANCHOR, elem) decltype(&ANCHOR::PSIO_GET_IDENT(elem))
#define PSIO_IFACE_FN_NAME(r, _, elem)      BOOST_PP_STRINGIZE(PSIO_GET_IDENT(elem))
#define PSIO_IFACE_PARAM_NAMES(r, _, elem)                                     \
   ::std::initializer_list<const char*>{PSIO_GET_QUOTED_ARGS(elem)},

#define PSIO_IFACE_PROXY_METHOD(r, ANCHOR, i, elem)                                       \
   template <typename... _PsioProxyArgs>                                                  \
   decltype(auto) PSIO_GET_IDENT(elem)(_PsioProxyArgs&&... args)                          \
   {                                                                                      \
      return _psio_proxy_obj                                                              \
          .template call<i, decltype(&ANCHOR::PSIO_GET_IDENT(elem))>(                     \
              ::std::forward<_PsioProxyArgs>(args)...);                                   \
   }

#define PSIO_IFACE_REV_LOOKUP(s, TAG, T)                             \
   template <>                                                       \
   struct psio::interface_of<T>                                      \
   {                                                                 \
      using type = TAG;                                              \
   };

#define PSIO_INTERFACE(NAME, TYPES_SPEC, FUNCS_SPEC)                         \
   BOOST_PP_IIF(BOOST_PP_CHECK_EMPTY(PSIO_IFACE_UNWRAP_TYPES(TYPES_SPEC)),   \
                PSIO_INTERFACE_NO_TYPES,                                     \
                PSIO_INTERFACE_WITH_TYPES)(NAME, TYPES_SPEC, FUNCS_SPEC)

#define PSIO_INTERFACE_WITH_TYPES(NAME, TYPES_SPEC, FUNCS_SPEC)                     \
   PSIO_INTERFACE_BODY(NAME,                                                        \
                       ::std::tuple<PSIO_IFACE_UNWRAP_TYPES(TYPES_SPEC)>,           \
                       BOOST_PP_SEQ_TRANSFORM(                                      \
                           PSIO_IFACE_NORMALIZE_ENTRY, _,                           \
                           BOOST_PP_VARIADIC_TO_SEQ(PSIO_IFACE_UNWRAP_FUNCS(FUNCS_SPEC)))) \
   BOOST_PP_SEQ_FOR_EACH(PSIO_IFACE_REV_LOOKUP, ::NAME,                             \
                         BOOST_PP_VARIADIC_TO_SEQ(PSIO_IFACE_UNWRAP_TYPES(TYPES_SPEC)))

#define PSIO_INTERFACE_NO_TYPES(NAME, TYPES_SPEC, FUNCS_SPEC) \
   PSIO_INTERFACE_BODY(NAME,                                  \
                       ::std::tuple<>,                        \
                       BOOST_PP_SEQ_TRANSFORM(                \
                           PSIO_IFACE_NORMALIZE_ENTRY, _,     \
                           BOOST_PP_VARIADIC_TO_SEQ(PSIO_IFACE_UNWRAP_FUNCS(FUNCS_SPEC))))

#define PSIO_INTERFACE_BODY(NAME, TYPES_TUPLE, FUNCS_SEQ)                            \
   namespace psio::detail                                                            \
   {                                                                                 \
      template <>                                                                    \
      struct interface_info<::NAME>                                                  \
      {                                                                              \
         static constexpr ::psio::FixedString name = #NAME;                          \
         using package                             = ::psio_current_package;         \
         using types                               = TYPES_TUPLE;                    \
         using func_types = ::std::tuple<PSIO_SEQ_TO_VA_ARGS(                       \
             BOOST_PP_SEQ_TRANSFORM(PSIO_IFACE_FN_TYPE, ::NAME, FUNCS_SEQ))>;        \
         static constexpr ::std::array<::std::string_view,                           \
                                       BOOST_PP_SEQ_SIZE(FUNCS_SEQ)>                 \
             func_names{PSIO_SEQ_TO_VA_ARGS(                                         \
                 BOOST_PP_SEQ_TRANSFORM(PSIO_IFACE_FN_NAME, _, FUNCS_SEQ))};         \
         static constexpr ::std::initializer_list<const char*>                       \
             param_names[BOOST_PP_SEQ_SIZE(FUNCS_SEQ)] = {                           \
                 BOOST_PP_SEQ_FOR_EACH(PSIO_IFACE_PARAM_NAMES, _, FUNCS_SEQ)};       \
                                                                                     \
         template <typename ProxyObject>                                             \
         struct proxy                                                                \
         {                                                                           \
           private:                                                                  \
            ProxyObject _psio_proxy_obj;                                             \
                                                                                     \
           public:                                                                   \
            template <typename... _PsioProxyArgs>                                    \
            explicit proxy(_PsioProxyArgs&&... args)                                 \
                : _psio_proxy_obj(::std::forward<_PsioProxyArgs>(args)...)           \
            {                                                                        \
            }                                                                        \
            auto& psio_get_proxy() { return _psio_proxy_obj; }                       \
            auto& psio_get_proxy() const { return _psio_proxy_obj; }                 \
            BOOST_PP_SEQ_FOR_EACH_I(PSIO_IFACE_PROXY_METHOD, ::NAME, FUNCS_SEQ)      \
         };                                                                          \
      };                                                                             \
      inline constexpr interface_marker<::NAME>                                      \
          BOOST_PP_CAT(_psio_iface_, NAME){};                                        \
   }

// ── PSIO_USE(package, interface, version) ────────────────────────────────

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
// Top-level composition. `imports(…)` lists use tags (created by
// PSIO_USE). `exports(…)` lists interface tags — i.e. the user-authored
// interface class types. The macro expansion places them inside
// `namespace psio::detail`, so bare names are looked up via the chain
// detail → psio → global; unqualified user classes at global scope are
// found through the global fallback.

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

// ── PSIO_HOST_MODULE(ImplClass, interface(Tag, m…)…) ────────────────────────────
//
// Registers ImplClass as the C++ host implementation of one or more
// interfaces. Each entry names the interface tag and the member
// functions on ImplClass that fulfill that interface, in declaration
// order. Example:
//
//   struct Host { void log_u64(std::uint64_t); std::uint64_t now(); };
//   PSIO_HOST_MODULE(Host,
//       interface(env,   log_u64),
//       interface(clock, now))
//
// Effects:
//
//   • psio::detail::iface_impl<Host, env> / <Host, clock> specialized
//     with ::tag, ::methods, ::names.
//   • psio::detail::impl_info<Host> specialized with
//     ::interfaces = std::tuple<iface_impl<Host,env>,
//                               iface_impl<Host,clock>>.
//   • psio::impl_of<Host>::type aliased to the tuple of tags (for
//     Linker::provide discovery).
//
// Must appear at global namespace scope. At least one `interface(…)`
// entry is required.

#define PSIO_HOST_MODULE_ARGS_interface(...) __VA_ARGS__

// Head/tail splitters used to pull the tag off the front of an entry's
// argument list and keep the remaining methods as a comma list.
#define PSIO_HOST_MODULE_HEAD(x, ...) x
#define PSIO_HOST_MODULE_TAIL(x, ...) __VA_ARGS__

#define PSIO_HOST_MODULE_IFACE_TAG(entry)         \
   PSIO_HOST_MODULE_IFACE_TAG_I(BOOST_PP_CAT(PSIO_HOST_MODULE_ARGS_, entry))
#define PSIO_HOST_MODULE_IFACE_TAG_I(...)         PSIO_HOST_MODULE_HEAD(__VA_ARGS__)

#define PSIO_HOST_MODULE_IFACE_METHODS(entry)     \
   PSIO_HOST_MODULE_IFACE_METHODS_I(BOOST_PP_CAT(PSIO_HOST_MODULE_ARGS_, entry))
#define PSIO_HOST_MODULE_IFACE_METHODS_I(...)     PSIO_HOST_MODULE_TAIL(__VA_ARGS__)

#define PSIO_HOST_MODULE_IFACE_METHODS_SEQ(entry) \
   BOOST_PP_VARIADIC_TO_SEQ(PSIO_HOST_MODULE_IFACE_METHODS(entry))

#define PSIO_HOST_MODULE_MEMBER_PTR(s, IMPL, METHOD) &IMPL::METHOD
#define PSIO_HOST_MODULE_METHOD_NAME(s, _, METHOD)   #METHOD

#define PSIO_HOST_MODULE_EMIT_ENTRY(r, IMPL, entry)                                              \
   template <>                                                                            \
   struct psio::detail::iface_impl<IMPL, ::PSIO_HOST_MODULE_IFACE_TAG(entry)>                    \
   {                                                                                      \
      using host = IMPL;                                                                  \
      using tag  = ::PSIO_HOST_MODULE_IFACE_TAG(entry);                                          \
      static constexpr auto methods = ::std::tuple{PSIO_SEQ_TO_VA_ARGS(                   \
          BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_MEMBER_PTR, IMPL,                              \
                                 PSIO_HOST_MODULE_IFACE_METHODS_SEQ(entry)))};                   \
      static constexpr ::std::array<                                                      \
          ::std::string_view,                                                             \
          BOOST_PP_SEQ_SIZE(PSIO_HOST_MODULE_IFACE_METHODS_SEQ(entry))>                          \
          names{PSIO_SEQ_TO_VA_ARGS(                                                      \
              BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_METHOD_NAME, _,                            \
                                     PSIO_HOST_MODULE_IFACE_METHODS_SEQ(entry)))};               \
   };

#define PSIO_HOST_MODULE_IFACE_TAG_TYPE(s, _, entry) \
   ::PSIO_HOST_MODULE_IFACE_TAG(entry)

#define PSIO_HOST_MODULE(IMPL, ...)                                                                  \
   BOOST_PP_SEQ_FOR_EACH(PSIO_HOST_MODULE_EMIT_ENTRY, IMPL, BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__))   \
   template <>                                                                                \
   struct psio::impl_of<IMPL>                                                                 \
   {                                                                                          \
      using type = ::std::tuple<PSIO_SEQ_TO_VA_ARGS(                                          \
          BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_IFACE_TAG_TYPE, _,                                 \
                                 BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))>;                    \
   };                                                                                         \
   template <>                                                                                \
   struct psio::detail::impl_info<IMPL>                                                       \
   {                                                                                          \
      using interfaces = ::psio::detail::iface_impls_tuple<IMPL, PSIO_SEQ_TO_VA_ARGS(         \
          BOOST_PP_SEQ_TRANSFORM(PSIO_HOST_MODULE_IFACE_TAG_TYPE, _,                                 \
                                 BOOST_PP_VARIADIC_TO_SEQ(__VA_ARGS__)))>;                    \
   };
