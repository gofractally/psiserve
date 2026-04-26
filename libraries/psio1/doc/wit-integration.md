# WIT as PSIO's First-Class Schema

Status: design sketch, April 2026. Companion to `wit-attributes.md` (which
defines the attribute vocabulary this plan transports end-to-end).

PSIO commits to WIT as its preferred authored and exchange schema format.
FracPack remains the wire format; WIT is the lingua franca for *describing*
what's on the wire. Every type, every interface, every world a PSIO user
writes in C++ should be expressible as a `.wit` file, and every `.wit` file
should be expressible as PSIO-reflected C++ code — round-trip, losslessly,
with `@sorted` / `@final` / `@canonical` and the rest riding along.

## Goals

1. **One source of truth per project.** Users who start from C++ never
   hand-author `.wit`; users who start from `.wit` never hand-author the
   matching C++ headers.
2. **Compile-time WIT metadata in C++.** Package identity, interface
   membership, world composition, and attribute tags are all visible to
   `constexpr` traits so users can static-assert, dispatch on interface,
   or drive their own codegen.
3. **Runtime WIT metadata in WASM.** The existing build path that embeds
   `.wit` into `.wasm` continues to work — the `.wit` is derived from the
   reflection, not maintained separately.
4. **Validator parity.** The same dynamic FracPack validator
   (`CompiledValidator`, `fracpack_validate`) works against schemas that
   originate from C++ reflection *or* from a parsed `.wit`. Already true
   at the IR level; stays true as structural metadata is added.
5. **Lossless round-trip.** `C++ → WIT → C++` reproduces equivalent
   types. `WIT → C++ → WIT` reproduces the original `.wit` modulo
   formatting.

## Non-goals

**PSIO does not generate code for non-C++ languages.** WIT is the interop
language; each ecosystem has its own tooling:

- Rust: `wit-bindgen`, `cargo component`
- JavaScript / TypeScript: `jco`, `componentize-js`
- Python: `componentize-py`
- Go: `wit-bindgen-go`

PSIO's job is to make sure the `.wit` it emits is clean and correct so
those tools produce correct bindings for their ecosystems. Going the
other direction, PSIO parses `.wit` authored anywhere and generates C++
that matches — but nothing else. Adding Rust/JS codegen to PSIO would
duplicate mature external tooling and entangle PSIO with language
ecosystems it has no reason to track.

This scope boundary is what makes "first-class WIT support" achievable
for one language: the problem is C++ ↔ WIT, not C++ ↔ every language.

## The round-trip cycle

```
     C++ source                          WIT text
     (authored)                          (authored)
         │                                   │
         │ PSIO1_REFLECT                      │ wit_parser
         │ PSIO1_INTERFACE                    │
         │ PSIO1_PACKAGE                      │
         │ PSIO1_WORLD                        │
         ▼                                   ▼
  ┌──────────────────────────────────────────────┐
  │  Exchange IR                                 │
  │  (Package + Interface + World + AnyType      │
  │   + attributes, FracPack-serializable)       │
  └───────────┬────────────┬─────────────┬───────┘
              │            │             │
              ▼            ▼             ▼
       rapid validator  WIT emit    C++ codegen
       (CompiledType)   (.wit)      (PSIO1_REFLECT'd
              │            │         headers)
              ▼            ▼
        fracpack_validate  embed into
        at ingress         .wasm component
                           (wasm-component-build)
                              │
                              ▼
                       downstream ecosystems:
                       Rust (wit-bindgen),
                       JS (jco),
                       Python (componentize-py),
                       …  — not PSIO's job
```

Every arrow in the PSIO-owned region (everything above and including the
`.wit` emit step) is a first-class supported operation.

## Layered architecture

### L1 — Type attributes

Specified in `wit-attributes.md`. Adds:

- `cpp/include/psio/attributes.hpp` — tag types (`sorted_tag`, `utf8_tag`,
  `canonical_tag`, `final_tag`, `padding_tag`, `number_tag<N>`,
  `since_tag<V>`, …) and two specialization-point registries:
  `type_attrs<T>` (attached to a C++ type) and
  `member_attrs<auto MemberPtr>` (attached to a specific field or method).
- `PSIO1_REFLECT` grammar extension: `ATTRS(field, tag, …)`, sugar macros
  `SORTED(x)` / `UTF8(x)` / `PADDING(x)` / `UNIQUE_KEYS(x)`, and
  type-level flag matchers `canonical()` / `final()` parallel to
  existing `definitionWillNotChange()`.
- Exchange IR: `attributes: vec<Attribute>` fields on `Struct`, `Object`,
  `Variant`, `Member`.
- Built-in specializations: `type_attrs<std::map<K,V>>` → `{sorted,
  unique_keys}`, `type_attrs<std::set<K>>` → `{sorted, unique_keys}`,
  `type_attrs<std::u8string>` → `{utf8}`. Third-party containers add
  one-liner specializations in their own headers.
- Auto-detection: `std::is_final_v<T>` contributes `final_tag` without
  an explicit specialization. The existing `numbered(int, ident)` item
  in PSIO1_REFLECT flows through to `@number(n)` in emitted WIT.

Self-contained; nothing else in L2–L6 depends on PSIO1_REFLECT grammar
beyond what L1 adds.

### L2 — Structural metadata

Declarations at file/namespace scope, parallel to PSIO1_REFLECT:

```cpp
PSIO1_PACKAGE(psibase, "0.3.0")

PSIO1_INTERFACE(kernel,
    types(Block, Transaction),
    funcs(submit_tx, query_chain))

PSIO1_INTERFACE(accounts,
    types(Account),
    funcs(transfer, get_balance))

PSIO1_WORLD(node,
    imports(wasi::clocks::wall_clock, wasi::io::streams),
    exports(kernel, accounts))

PSIO1_USE(wasi, "io", "0.2.0")   // cross-package dep
```

Each macro registers into a compile-time metadata graph (inline
`constexpr` variables keyed by tag types) that `SchemaBuilder` walks
alongside the per-type reflection. Queryable as:

```cpp
psio1::package_of<Block>::value         // "psibase"
psio1::package_version_of<Block>::value // "0.3.0"
psio1::interface_of<Block>::value       // "kernel"
psio1::world<"node">::imports           // tuple of interface tags
```

### L3 — Exchange IR envelope

Today's `Schema` carries a flat `types` vector. Extended to:

```
Schema = {
    package: Package,
    interfaces: vec<Interface>,
    worlds: vec<World>,
    uses: vec<Use>,
    types: vec<AnyType>,      // types not owned by an interface
}
Package   = { name, version, attributes }
Interface = { name, types: vec<TypeRef>, funcs: vec<Func>, attributes }
World     = { name, imports, exports, includes, attributes }
Func      = { name, params, results, attributes }
Use       = { package, interface, version, items }
```

The `wit_*` AST types from `wit_parser` already have this shape — the
Exchange IR adopts the same structure so a parsed WIT file and a
reflected C++ world produce *the same IR*, and the downstream emit /
codegen steps don't have to distinguish origin.

### L4 — WIT emission

`psio1::emit_wit(schema) -> std::string` produces a canonically formatted
`.wit` text from the enriched Exchange IR. Round-trip test:

```
wit_text → wit_parser → IR₁ → emit_wit → wit_text' → wit_parser → IR₂
assert IR₁ == IR₂
```

Output formatting is stable (one line per field, attributes on their own
line above the item they decorate, alphabetical ordering within sections)
so emitted `.wit` files diff cleanly in version control.

### L5 — WIT → C++ codegen

`psio1::gen_cpp(schema) -> std::vector<Header>` produces C++ header(s)
from the Exchange IR. Output includes:

- Per-type `struct` / variant class with the right C++ spellings of WIT
  types.
- `PSIO1_REFLECT(T, ...)` with matching attribute macros (`SORTED(...)`,
  `canonical()`, etc.) so the generated header round-trips back to the
  same IR.
- `PSIO1_PACKAGE` / `PSIO1_INTERFACE` / `PSIO1_WORLD` declarations that
  re-establish the structural metadata.
- Type wrappers where WIT expresses an invariant the C++ type system
  can't (`psio1::sorted<std::vector<T>>` for `@sorted`-tagged plain
  lists).

Stability invariants:

```
C++ → WIT → C++ : types equivalent (same reflect graph)
WIT → C++ → WIT : .wit text equivalent (modulo formatting)
```

Golden tests in both directions.

### L6 — Build tooling

CLI tools:

- `psio-emit-wit <object-files…> -o package.wit` — runs the reflection
  walk, emits `.wit`. Intended to plug into a CMake custom command that
  runs before the wasm-component build step.
- `psio-gen-headers <package.wit> -o include/` — generates C++ headers
  from a `.wit` file. For projects that consume an externally-authored
  schema.

CMake helpers:

```cmake
psio_wit_from_reflection(my_component
    SOURCES src/kernel.cpp src/accounts.cpp
    OUTPUT  build/psibase.wit)

psio_headers_from_wit(wasi_io_bindings
    WIT external/wasi/io.wit
    OUTPUT_DIR generated/)
```

Integration with whatever wasm-component-build path psiserve adopts.

## Phases

| Phase | Scope     | Delivers                                        |
|-------|-----------|-------------------------------------------------|
| A     | L1        | Type attributes, round-tripping at the type level |
| B     | L2 + L3   | Structural C++ metadata + IR envelope to hold it |
| C     | L4        | WIT emission, closes the C++ → WIT arrow         |
| D     | L5 + L6   | WIT → C++ codegen + build tooling                |

Each phase is a self-contained PR-sized chunk that leaves the tree in a
working state. Phase A is the smallest and narrowest; Phase B is the
most design-heavy (structural macros are a once-and-done shape); Phase C
is mostly mechanical given B; Phase D is the biggest raw code volume
but lowest design risk.

## Hard problems that don't vanish

### Cross-package `use`

When a C++ interface references `wasi:io/streams.input-stream`, the
reflection walk sees a C++ type — say `wasi::io::input_stream` — but
must emit a `use wasi:io/streams.{input-stream}` in the WIT, *not*
re-declare the type locally.

Solved by: the imported type's header carries its own
`PSIO1_PACKAGE(wasi, "0.2.0")` + `PSIO1_INTERFACE(streams, types(input_stream, …))`
declarations. When SchemaBuilder sees `wasi::io::input_stream` as a
field type while building the `psibase` package, the package-of query
returns `"wasi"`, not `"psibase"`, and emission generates a `use`
statement instead of a type definition.

Requires: a single canonical registration site per WIT-imported type in
the C++ header graph (ODR-enforced), and `PSIO1_USE` to be explicit about
which external packages this one depends on so cross-package imports
are declared, not inferred.

### Resource types

WIT `resource` is a handle type with methods. C++ side maps to a class
with `resource_drop` / `resource_rep` / constructor patterns. Partial
support exists (`wit_resource_tests.cpp`). Needs integration with
PSIO1_REFLECT so resources appear alongside records in the reflection
graph and round-trip cleanly.

### Parametric types

`list<T>`, `option<T>`, `result<T, E>`, `tuple<T…>` are parametric in
WIT and template-parametric in C++. Emission needs to produce the
parametric WIT reference, not erase through to the instantiated form.
Already mostly handled in the existing schema code; needs verification
the attribute-carrying IR preserves parameter identity.

### Stable identity across TUs

If two translation units both register `PSIO1_INTERFACE(kernel, …)`
with different contents, linking is undefined. Enforced by requiring
one canonical declaration site per interface — a `.psio-meta.hpp`
header that's included once. A link-time assertion catches divergence.

### Version-flow for evolution

When `Block` gains a field tagged `@since(version = 0.4.0)`, the version
string ideally comes from `PSIO1_PACKAGE(psibase, "0.4.0")`, not from
the developer typing it twice. `since(current_package)` as a shorthand
that resolves at compile time to the containing package's version.

### Resource ownership across languages

A WASM component holding a resource handle owned by another component
has ownership semantics that PSIO's C++ side needs to model correctly
(borrow vs own). The WIT type system already expresses this; the C++
codegen needs to pick the right `psio1::borrow<T>` / `psio1::own<T>`
wrapper. Carried in attributes or in type construction — TBD at L5
design time.

## End-user experience

### Author a service (C++ → WIT → wasm component)

```cpp
// service/psibase.cpp — the only authored artifact
#include <psio1/schema.hpp>
#include <wasi/io.hpp>   // carries PSIO1_PACKAGE(wasi,"0.2.0") declarations

PSIO1_PACKAGE(psibase, "0.3.0")

struct Block {
    Hash                     hash;
    Hash                     prev;
    std::vector<Transaction> txs;
};
PSIO1_REFLECT(Block, canonical(), hash, prev, ATTRS(txs, sorted))

struct Transaction { /* … */ };
PSIO1_REFLECT(Transaction, /* … */)

Block submit_tx(Transaction tx);
Block query_chain(Hash at);

PSIO1_INTERFACE(kernel,
    types(Block, Transaction),
    funcs(submit_tx, query_chain))

PSIO1_WORLD(node,
    imports(wasi::clocks::wall_clock),
    exports(kernel))
```

Build flow:

```
cmake → compile psibase.cpp.o
      → psio-emit-wit psibase.cpp.o -o psibase.wit
      → wasm-component-build psibase.cpp.o psibase.wit -o psibase.wasm
```

No `.wit` file is ever hand-authored. The consumer can pull
`psibase.wasm`, extract the embedded WIT, and generate Rust / JS /
Python bindings with their ecosystem's own tooling — none of which PSIO
touches.

### Consume an external WIT (WIT → C++)

```
psio-gen-headers wasi-io.wit -o generated/wasi-io/
```

Produces `generated/wasi-io/streams.hpp` with `input_stream`,
`output_stream`, associated methods, and the `PSIO1_PACKAGE(wasi, …)` /
`PSIO1_INTERFACE(streams, …)` declarations that make subsequent C++ code
referencing these types produce the correct `use` statements in emitted
WIT.

## Open questions

- **Attribute macros: scope of sugar.** `SORTED(x)` / `UTF8(x)` /
  `PADDING(x)` / `UNIQUE_KEYS(x)` vs. only the uniform `ATTRS(x, …)`.
  Sugar is friendlier; uniform is simpler to maintain. Leaning sugar
  for the four common cases + `ATTRS` for anything else.
- **Method attributes: v1 or v2?** Methods only care about `@since` /
  `@unstable` / `@deprecated`. v1-narrow defers method attributes; this
  is clean but means no WIT `@deprecated` round-trip for methods until
  v2. Leaning defer.
- **Custom / pass-through attributes from C++.** The WIT parser passes
  unknown attributes through; the C++ side currently has no way to
  author an attribute PSIO doesn't understand. Options: add
  `PSIO1_CUSTOM_ATTR("name", "key", "value")` for escape, or accept
  that C++-originated WIT carries only the 8 standard attributes and
  authored WIT can carry more.
- **Single-world vs multi-world TUs.** One C++ object file producing
  one world is the simple case. Multiple worlds in one TU is
  expressible but unusual; does the emit tool error or emit multiple
  `.wit` files?
- **Reflected vs imported type ambiguity.** A C++ type reachable from
  reflection might be a PSIO-reflected local type *or* an imported
  WIT type depending on whether a `PSIO1_PACKAGE` matching its package
  is in scope. Needs a concrete rule for how SchemaBuilder resolves
  this without false positives.

## Relationship to existing documents

- `wit-attributes.md` — defines the attribute vocabulary. L1 implements
  it end-to-end.
- `design_todo.md` — the broader PSIO tooling checklist. This document
  is the WIT-specific subset expanded into its full scope.
- `fracpack-spec.md` — defines the wire format. Unchanged by any of
  L1–L6; WIT describes what FracPack carries, FracPack stays as-is.
