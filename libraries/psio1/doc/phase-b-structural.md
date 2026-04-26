# Phase B — Structural Metadata

Status: design sketch, April 2026. Follows `wit-integration.md` (layered
plan, L2+L3 here) and builds on the attribute machinery delivered in
Phase A (`attributes.hpp`, `Attribute` IR in `schema.hpp`,
`wit_attribute` in `wit_types.hpp`).

Phase A gave types *attributes*. Phase B gives types *a home*: which
package they belong to, which interface groups them, and which worlds
compose them. Without this, `emit_wit` in Phase C has no way to decide
whether `psibase::Block` should appear as a `record block { … }` in the
local package or as `use psibase:kernel.{block}` in a consumer's `.wit`.

## Scope in one paragraph

Author-facing: four namespace-scope macros — `PSIO1_PACKAGE`,
`PSIO1_INTERFACE`, `PSIO1_WORLD`, `PSIO1_USE` — that register structural
metadata into compile-time tag-keyed registries. IR-side: extend
`Schema` from a flat `types` map into a proper envelope with
`package`, `interfaces`, `worlds`, `uses`. `SchemaBuilder` walks from a
world tag to the full envelope. Resource types earn a first-class
`AnyType::Resource` case. Existing consumers (validator, JSON
roundtrip) see no behaviour change for the type-only case.

## The four macros

### `PSIO1_PACKAGE(name, version)`

Per-TU declaration of the authoring package. Only one is allowed per TU;
later declarations are a hard error.

```cpp
PSIO1_PACKAGE(psibase, "0.3.0")
```

Desugars to:

```cpp
namespace psio1::detail {
    template <>
    struct package_info<FixedString{"psibase"}> {
        static constexpr FixedString name    = "psibase";
        static constexpr FixedString version = "0.3.0";
    };
    inline constexpr package_marker<FixedString{"psibase"}> _psio_pkg{};
}
using psio_current_package = psio1::detail::package_info<FixedString{"psibase"}>;
```

`psio_current_package` is a TU-local type alias. Every `PSIO1_INTERFACE` /
`PSIO1_WORLD` / `PSIO1_USE` in the same TU reads it to resolve
"the containing package" without the author re-typing the name. Static
assertion in each macro: `psio_current_package` must be in scope.

### `PSIO1_INTERFACE(name, types(T…), funcs(f…))`

Registers a named group of types and functions against the containing
package.

```cpp
PSIO1_INTERFACE(kernel,
    types(Block, Transaction),
    funcs(submit_tx, query_chain))
```

Desugars to:

```cpp
namespace psio1::detail {
    struct kernel_interface_tag {};

    template <> struct interface_info<kernel_interface_tag> {
        static constexpr FixedString       name    = "kernel";
        using package                              = psio_current_package;
        using types                                = std::tuple<Block, Transaction>;
        static constexpr auto              funcs   = std::tuple{
            &submit_tx, &query_chain };
    };

    inline constexpr interface_marker<kernel_interface_tag> _psio_iface_kernel{};
}

template <> struct psio1::interface_of<Block>       { using type = kernel_interface_tag; };
template <> struct psio1::interface_of<Transaction> { using type = kernel_interface_tag; };
```

Three registries are populated:

- `interface_info<Tag>` — full interface contents, iterable at compile time.
- `interface_of<T>` — reverse lookup: a type back to its interface tag.
- `package_of<T>` — derived transitively:
  `package_of<T>::value == interface_info<interface_of<T>::type>::package::name`.

`funcs(…)` takes free-function identifiers. For member functions on
resources (methods), use the existing `method(name, args…)` grammar in
`PSIO1_REFLECT` on the resource class itself; those flow through the
resource-specific path described in §IR extensions.

### `PSIO1_WORLD(name, imports(…), exports(…))`

Top-level composition: which interfaces this world imports and exports.

```cpp
PSIO1_WORLD(node,
    imports(wasi::clocks::wall_clock, wasi::io::streams),
    exports(kernel, accounts))
```

Desugars to:

```cpp
namespace psio1::detail {
    struct node_world_tag {};

    template <> struct world_info<node_world_tag> {
        static constexpr FixedString name = "node";
        using package                     = psio_current_package;
        using imports = std::tuple<wasi_clocks_wall_clock_tag,
                                    wasi_io_streams_tag>;
        using exports = std::tuple<kernel_interface_tag,
                                    accounts_interface_tag>;
    };

    inline constexpr world_marker<node_world_tag> _psio_world_node{};
}
```

`imports(…)` accepts either a qualified C++ name that resolves to an
interface tag (via the `interface_of` lookup on its tag type — see
`PSIO1_USE` below for how imported-package tag types are introduced), or
a `use_ref<"pkg", "iface">` form for declaring a dep without bringing
its types into scope.

### `PSIO1_USE(package, interface, version)`

Explicit cross-package dependency. Required when a world imports an
interface whose types are defined in another package, so that `emit_wit`
knows to produce a `use` statement instead of attempting a local
redeclaration.

```cpp
PSIO1_USE(wasi, io::streams, "0.2.0")
```

Desugars to:

```cpp
namespace psio1::detail {
    struct wasi_io_streams_tag {};
    template <> struct use_info<wasi_io_streams_tag> {
        static constexpr FixedString package = "wasi";
        static constexpr FixedString interface = "io/streams";
        static constexpr FixedString version = "0.2.0";
    };
    inline constexpr use_marker<wasi_io_streams_tag> _psio_use_wasi_io_streams{};
}
```

When a header defines a cross-package type (e.g. `wasi::io::streams::input_stream`),
that header also carries its own `PSIO1_PACKAGE(wasi, "0.2.0")` +
`PSIO1_INTERFACE(streams, types(input_stream), …)` declarations. Consumers
of that header do not re-declare — they issue `PSIO1_USE` to opt in to
importing from that package. The `PSIO1_USE` and the imported header's
own `PSIO1_INTERFACE` resolve to the same tag type at link time; ODR
enforces they agree.

## IR extensions (L3)

### `Schema` becomes an envelope

```cpp
struct Schema {
    Package                        package;
    std::vector<Interface>         interfaces;
    std::vector<World>             worlds;
    std::vector<Use>               uses;
    std::map<std::string, AnyType> types;   // types not owned by an interface
};

struct Package {
    std::string            name;
    std::string            version;
    std::vector<Attribute> attributes;
};

struct Interface {
    std::string              name;
    std::vector<std::string> type_names;   // keys into Schema::types
    std::vector<Func>        funcs;
    std::vector<Attribute>   attributes;
};

struct World {
    std::string              name;
    std::vector<UseRef>      imports;
    std::vector<std::string> exports;      // interface names in this package
    std::vector<Attribute>   attributes;
};

struct Func {
    std::string                           name;
    std::vector<std::pair<std::string, AnyType>> params;
    std::vector<AnyType>                  results;   // 0 or 1; WIT result<> lifts multi-return
    std::vector<Attribute>                attributes;
};

struct Use {
    std::string package;
    std::string interface;
    std::string version;
    std::vector<std::string> items;   // empty = wildcard
};

struct UseRef {     // how a world's imports refer to external interfaces
    std::string package;
    std::string interface;
};
```

Mirrors the shape of `wit_parser`'s `wit_world` so a parsed `.wit` and
a reflected C++ world produce the same envelope — this is the
commitment in `wit-integration.md:172` ("same IR, origin-agnostic
downstream").

### `AnyType::Resource` as a first-class case

Today `AnyType` is a variant over `Struct | Object | Variant | List |
Custom | …`. Add:

```cpp
struct Resource {
    std::string              name;
    std::vector<Func>        methods;
    std::vector<Attribute>   attributes;
};
```

`SchemaBuilder::insert<T>()` dispatches on `is_wit_resource_v<T>`
(from `wit_resource.hpp`): resources emit `Resource`, everything else
continues through the `Struct`/`Object` path. Reflected `method(…)`
entries on a resource class flow into `Resource::methods`; data members
are ignored (matches the WIT emit contract in `wit_resource.hpp:98`).

`own<T>` and `borrow<T>` do **not** become new `AnyType` cases — they
are serialization wrappers over a `Resource` reference. The IR carries
them as `Custom{.type = ResourceRef{name}, .id = "own"}` or `"borrow"`;
Phase C (`emit_wit`) lowers those to `own<T>` / `borrow<T>` in
WIT text, and Phase D (`gen_cpp`) round-trips them back to the C++
wrappers. Keeping them in `Custom` rather than `AnyType` variants
avoids proliferating cases for what is fundamentally a canonical-ABI
wire annotation on a resource-handle reference.

### Wire-format status

After Phase B:

- `Member::attributes` — serialized (unchanged from A).
- `Resource::{name, methods, attributes}` — serialized.
- `Package`, `Interface`, `World`, `Use`, `Func` — serialized.
- `Object::attributes`, `Struct::attributes`, `Variant::attributes` —
  **still in-memory only** under the existing `clio_unwrap_packable`
  bypass.

Fixing the aggregate-type attribute serialization is pulled out as a
pre-Phase-B mini-change (see open question §Q1 below) because it
interacts with existing `Schema` blobs and wants its own review.

### `SchemaBuilder::insert_world<WorldTag>()`

New entry point, parallels the existing `insert<T>()`:

```cpp
template <typename WorldTag>
void SchemaBuilder::insert_world();
```

Walks `world_info<WorldTag>`:

1. Emits `Schema::package` from `world_info::package`.
2. For each interface in `exports`: emits an `Interface` into
   `Schema::interfaces`; for each type in `interface_info::types`,
   calls the existing `insert<T>()` path (picks up attributes via A).
3. For each interface in `imports`: looks up `use_info<Tag>`, emits a
   `Use` into `Schema::uses`. Does **not** call `insert<T>()` on imported
   types — they live in the foreign package's schema.
4. For each free function in `interface_info::funcs`: emits a `Func`
   into the containing `Interface::funcs`.

A TU that calls `insert_world<node_world_tag>()` gets back a `Schema`
containing exactly what `node.wit` should say, minus formatting.

## Open questions

### Q1 — aggregate-type wire attributes — **decided: drop the unwrap**

`Object`, `Struct`, `Variant` bypass trailing-extension compat via
`clio_unwrap_packable`, so the `attributes` field added in Phase A is
in-memory only. Resolution: **drop the unwraps** and let these types
serialize as normal extensible structs (`{members, attributes}`).
Future field additions to any of them then ride the standard
trailing-extension path.

Concretely deletes these six definitions in `schema.hpp`:

- `clio_unwrap_packable(Object&)`, `const Object&`
- `clio_unwrap_packable(Struct&)`, `const Struct&`
- `clio_unwrap_packable(Variant&)`, `const Variant&`

Migration cost: any existing on-disk `Schema` blob encoded under the
old unwrap (bare `std::vector<Member>`) will fail to parse under the
new reader, which expects the `u16 fixed_size` extensible-struct
header. The break is loud (validator rejects the blob) rather than
silent, so it is containable.

Rejected alternatives: (2) branching on `attributes.empty()` is
asymmetric and ugly; (3) adding a `Schema::schema_version` byte adds a
versioning surface PSIO doesn't need elsewhere. Phase B's own
envelope expansion (new `package` / `interfaces` / `worlds` / `uses`
fields on `Schema`) will necessarily drop the `Schema`-level unwrap
as well; Q1's resolution is the same mechanism one layer down.

Ship as its own pre-Phase-B PR so the break is bisectable.

### Q2 — interface-member ODR

Two TUs that both declare `PSIO1_INTERFACE(kernel, …)` with different
content is undefined at link time. The doc calls for a
`.psio-meta.hpp`-style canonical header (`wit-integration.md:293`).
Concrete mechanism:

```cpp
// kernel_interface.hpp — included exactly once via the usual ODR rules
PSIO1_INTERFACE(kernel,
    types(Block, Transaction),
    funcs(submit_tx, query_chain))
```

`inline constexpr interface_marker<Tag>` is an inline variable; linker
ODR catches divergence as it would for any other inline constexpr. The
pragma-once guard at the header level is what enforces "one declaration
site."

No new mechanism required — just the convention that
`PSIO1_INTERFACE` / `PSIO1_WORLD` declarations live in headers, not in
`.cpp` files, and each interface has one canonical header.

### Q3 — `funcs(…)` taking pointers-to-free-functions

Works for top-level functions; does not work for member functions
(methods on a resource). Current plan: `PSIO1_INTERFACE::funcs(…)`
accepts *only* free functions, and resource methods are reflected via
the existing `method(ident, args…)` grammar inside `PSIO1_REFLECT` on
the resource class. `SchemaBuilder::insert_world` picks them up when it
emits the resource via the Resource IR path.

Alternative considered and rejected: a unified `funcs(&Cls::method,
&free_fn)` grammar. Rejected because free functions have no natural
"self" and methods have no meaning outside a resource; forcing them
into one list would require runtime-ish reflection that the rest of
PSIO avoids.

### Q4 — cross-package type identity

A reachable C++ type is "local" or "imported" based on whether a
`PSIO1_PACKAGE` match is in scope. SchemaBuilder's rule:

```
when visiting type T from world W in package P:
  if package_of<T>::value == P: emit locally
  elif ∃ Use u in P where T ∈ u.items: emit `use`
  else: hard error ("type T is neither local nor imported via PSIO1_USE")
```

This means `PSIO1_USE(wasi, io::streams, "0.2.0")` must list
`items(input_stream, output_stream, …)` explicitly — wildcard imports
are disallowed in this phase. Wildcard is attractive but requires the
builder to enumerate an external interface's types, which entails
loading the other header's `interface_info`. Defer to a later phase.

## Test plan

### Unit tests (psio/cpp/tests)

1. `psio_package_tests.cpp` — `PSIO1_PACKAGE` / `PSIO1_INTERFACE` /
   `PSIO1_WORLD` compile and register; `package_of<T>::value`,
   `interface_of<T>::type`, `world_info<Tag>::exports` return the
   declared values.
2. `psio_schema_envelope_tests.cpp` — `SchemaBuilder::insert_world<T>()`
   produces an envelope with the right package name, interface list,
   exports, and imports; attributes from Phase A flow through
   unchanged.
3. `psio_resource_ir_tests.cpp` — a type inheriting `wit_resource` with
   reflected methods round-trips through `Schema → FracPack → Schema`
   as a `Resource` node, not a `Struct`/`Object`.
4. `psio_cross_package_tests.cpp` — world W in package P imports
   interface I from package Q via `PSIO1_USE`; builder emits `Use`
   entry, does not attempt to insert Q's types into P's schema.

### Integration tests

Deferred to Phase C (`emit_wit`) — a full round-trip
`C++ → SchemaBuilder::insert_world → emit_wit → wit_parser → SchemaBuilder
(from parsed AST) → compare` needs both ends wired up. Phase B tests
verify only the L2 → L3 step.

## What this unblocks

- Phase C (`emit_wit`) becomes mechanical: walk `Schema` envelope,
  print package header, emit interfaces and worlds, emit types, emit
  `use` statements. No shape decisions remain.
- Stage 1 of the host/guest component tests (drafted pre-compaction):
  unblocked — we can reflect a `Database` interface, build a schema,
  and (with Phase B's envelope) verify the world/interface composition
  matches expectations. Actual lift/lower is still a psizam concern.
- psizam's `link()` API (future) can consume the L3 envelope to
  validate world compatibility at link time without touching C++
  reflection directly.

## What this does *not* do

- No WIT text emission (Phase C).
- No WIT → C++ codegen (Phase D).
- No canonical-ABI lifting/lowering (psizam, not PSIO).
- No handle table or resource lifetime semantics (psizam).
- No runtime world linking across components (psizam).
- No wildcard `PSIO1_USE` imports (deferred).

## Estimated surface area

- ~200 lines new in `attributes.hpp` (or a new `structural.hpp`) for
  the tag/marker/info templates.
- ~150 lines of Boost.PP macro work for the four macros. `PSIO1_WORLD`
  is the widest; `PSIO1_PACKAGE` is trivial.
- ~200 lines in `schema.hpp` for the envelope types and
  `insert_world<WorldTag>()`. Aggregate-attribute serialization fix
  (Q1) is ~50 lines, pulled into its own PR.
- ~400 lines of tests across four files.

One PR-sized change total if aggregate-attribute serialization is
handled separately; ~1200 lines otherwise.
