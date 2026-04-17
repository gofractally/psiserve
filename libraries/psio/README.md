# PSIO

**A serialization library for people who want to own their types.**

PSIO inverts the default assumption of every schema-driven serialization system.
You keep your types. The codegen gives you proofs and a head start — not a
prison.

---

## The idea in one diagram

Every other serialization library fuses three things into one pipeline:

```
  schema  ──→  codegen  ──→  generated types  ──→  wire format
                              (do not edit)
```

PSIO treats them as three orthogonal axes:

```
    native representation          abstract shape              wire format
    ─────────────────────          ──────────────              ───────────
    std::vector<Point>                                         FracPack
    std::list<Point>          ←→   list<Point>          ←→    Cap'n Proto
    boost::small_vector<P>                                    FlatBuffers
    MyArenaList<Point>                                        JSON
```

You pick the native type. You pick the wire format. PSIO derives the abstract
shape from your type via reflection, and validates that it matches whatever
schema you're working against.

## What follows from this

### 1. Duck typing for bytes

A schema doesn't define the bytes on the wire — it defines a *shape* that any
compatible type can inhabit. `std::vector<u8>`, `std::string`, and
`std::array<u8, N>` all satisfy `bytes`. The serializer doesn't care which one
you chose; it only cares about the shape.

This is the same duck typing Python gives you at the function-call level,
applied to the serialization boundary.

### 2. One vocabulary, many surface syntaxes

FracPack, WIT, Cap'n Proto, FlatBuffers, Protobuf, Avro — these are alternate
spellings of the same underlying type algebra (scalars, structs, tuples,
options, variants, lists, maps). PSIO parses all of them into a shared IR and
normalizes away the cosmetic differences. Your team authors in whichever schema
language you prefer; PSIO handles the rest.

### 3. Codegen produces proofs, not code

This is the part that makes the whole thing safe for polyglot production
environments.

Given `schema.wit`, PSIO generates two files:

```
types.hpp           ←  a starting point. Edit freely.
types.contract.hpp  ←  generated, never edited.
                       Embeds the schema + static_asserts that
                       shape(YourType) conforms to the schema.
```

You own `types.hpp`. The contract file is regeneratable from the schema at any
time — CI can regen and diff it. If you change `std::vector<Point>` to
`llvm::SmallVector<Point>`, the static_assert still passes because both have
shape `list<Point>`. If you rename a field or change its type incompatibly, the
compiler fires a `static_assert` pointing exactly at the violation.

**The generated file is a proof, not an implementation.** The implementation is
your own code.

### 4. Codegen bootstraps your initial implementation

`types.hpp` isn't empty — codegen writes a reasonable default
(`std::vector` for lists, `std::optional` for options, `std::variant` for
unions, `std::string` for strings). That's your starting point. You're free
to:

- Leave it alone. Works out of the box.
- Swap `std::vector` → `boost::small_vector` for cache-friendliness.
- Replace a `std::string` with a custom interned-string type.
- Wrap a field in a smart pointer, an arena handle, whatever your codebase
  needs.

As long as the shape survives, the contract passes. You get the ergonomics of
generated code without the straitjacket.

### 5. Schema evolution becomes a `static_assert` diff

Bump the schema → regenerate `types.contract.hpp` → the compiler enumerates
every site that breaks. Code review of the contract file's diff *is* the
migration plan. No runtime `ParseError` surprises in production — the build
fails at the exact line where the type drifted.

### 6. Graded compatibility, not boolean

Given a schema and a binary, PSIO returns a classification:

| Result                     | Meaning                                              |
|----------------------------|------------------------------------------------------|
| **Exact & canonical**      | Bytes match the schema precisely, in canonical form  |
| **Exact**                  | Bytes match the schema                               |
| **Forward compatible**     | Binary has fields the schema doesn't know about      |
| **Backward compatible**    | Schema expects optional fields the binary omits      |
| **Incompatible**           | Structural mismatch; diff included                   |

This is the output shape a database table, RPC ingress, or consensus layer
actually needs — not a bool.

### 7. No converter layer

The original sin of protobuf, FlatBuf, and Cap'n Proto: `PointPb` vs `Point`,
and a conversion function in every file. With PSIO there is one `Point`, in
your namespace, with the types you chose. The schema is satisfied in place.

### 8. Extends past data types to RPC

The same trick applies to service interfaces. Given a WIT interface, the
generated contract header `static_assert`s that your handler signatures
satisfy the interface — no base class, no virtual dispatch, no vtable.
Just a compile-time shape check against your free functions or methods.

### 9. Cross-language, uniformly

Every target language enforces the contract in its own idiom:

- C++ — `static_assert` over reflection-derived shape
- Rust — `const _: () = assert!(…)` or trait-bound checks
- TypeScript — type-level `AssertShape<T, Schema>`
- Python — import-time `assert_shape(T, schema)`

All enforce the same abstract shape against the same schema text. A type change
in one language that breaks the shape fails that language's build without
touching anyone else's toolchain.

---

## Answering the obvious objection

> "But in a polyglot codebase, we *need* codegen to enforce the cross-language
> contract. If anyone changes a type, the schema forces every other language to
> update."

PSIO keeps that discipline. You still have a schema. The schema is still the
source of truth for cross-language compatibility. The compiler still fails the
build if your types drift from the schema.

What you lose is the generated-code prison: the `do not edit` file, the
converter layer, the 4000-line protoc output, the runtime `ParseError` in prod.

What you keep is the contract.

---

## Supported schema languages

| Schema         | Parse | Emit | Notes                                     |
|----------------|-------|------|-------------------------------------------|
| FracPack       | ✓     | ✓    | Self-hosting — schema is itself FracPack  |
| WIT            | ✓     | ✓    | W3C WASI Component Model                  |
| Cap'n Proto    | ✓     | ✓    | `.capnp` IDL                              |
| FlatBuffers    | ✓     | ✓    | `.fbs` IDL                                |
| Protobuf       | —     | ~    | (planned)                                 |
| Avro           | ✓     | ✓    | JSON schema form                          |

## Supported wire formats

| Format         | Encode | Decode | Views | In-place mutate | Canonical |
|----------------|--------|--------|-------|-----------------|-----------|
| FracPack       | ✓      | ✓      | ✓     | ✓               | ✓         |
| Cap'n Proto    | ✓      | ✓      | ✓     | ✓               | ~         |
| FlatBuffers    | ✓      | ✓      | ✓     | unsafe          | —         |
| WIT ABI        | ✓      | ✓      | ✓     | ✓               | ✓         |
| JSON           | ✓      | ✓      | —     | —               | ✓         |
| Bincode, Borsh | ✓      | ✓      | —     | —               | Borsh: ✓  |

See `doc/fracpack-spec.md` for the FracPack wire format specification and
`CompetitionReport.md` for a detailed comparison against every major binary
serialization format.

---

## Status

Early. The C++ core (`cpp/`) is working and has test coverage. Bindings for
Rust, Go, Zig, Moonbit, Python, and JS live alongside (`rust/`, `go/`, etc.)
at varying maturity. The cross-schema validator described above is the
north-star goal documented in `design_todo.md`.

## Building

See the top-level `BUILDING.md`. Short version on macOS:

```bash
export CC=$(brew --prefix llvm)/bin/clang
export CXX=$(brew --prefix llvm)/bin/clang++
cmake -B build -G Ninja -DPSIO_ENABLE_TESTS=ON
cmake --build build
./build/bin/psio_tests
```
