# psio — Multi-language Design Principles & Checklist

**Status:** specification, 2026-04-24. Cross-language counterpart to
`psio-v2-design.md` (which is C++-specific).

This doc captures the principles every language port must honor and
the per-language checklist showing how each principle is realized.

---

## 0. The founding philosophy — *schema is contract, code is yours*

This is the one design commitment psio makes that no other major
serialization library makes. It shapes every other principle.

Every existing schema-driven codec (protobuf, capnproto, flatbuffers,
Avro, Thrift) treats generated code as sacred:

- The codegen tool emits a class / struct / trait.
- The top of the file says `// DO NOT EDIT. Regenerate from schema.`
- Users can't add methods, can't rename fields, can't refactor layout
  for their language's idioms. They write wrapper objects around the
  generated type.
- Schema evolves → regenerate → manual re-merge of any local tweaks.
  Often impossible in practice; users commit to never customizing.

psio inverts this:

- **Schema is the contract.** Parse it; reflect against it at build
  time; enforce equivalence via a single check
  (`static_assert(reflect<T>::schema() == parse_schema("x.wit"))`).
- **Generated code is a one-time starting point.** The developer
  owns it from the moment it's generated. They can rename things,
  add methods, reshape the class for their language's conventions,
  add validators in their types, integrate with their DI framework,
  whatever.
- **Reflection detects drift.** If the developer's edits change the
  wire contract, the build fails with a precise diagnostic pointing
  at the mismatched field. If their edits don't change the contract,
  the build passes silently.
- **Schema evolution is a human/AI refactor.** When `payment.wit`
  gains a field, the build fails until the dev adds the field. The
  dev adds it however best fits their code (new member in the right
  place, maybe with a default, maybe with annotations). The library
  doesn't generate anything new; it just re-checks the contract.

### Concrete consequences

**For the developer:**

- One-time codegen: `psio schema-import payment.wit > payment.hpp`
  (or `.rs` / `.ts` / `.py` / ...).
- Open the file; edit it like any other source file. Rename fields
  to match the team's style, add helper methods, change struct
  layout, add validators, add doc comments.
- Commit it to source control as normal first-party code.
- Evolve over time: when the schema changes, the build fails; patch
  by hand or with AI assistance; never regenerate.

**For the library:**

- Every language port ships both an importer (`schema-import`) and a
  live contract checker (build-time reflect-vs-schema comparison).
- Error messages from the contract checker name the mismatched
  field, wire position, and what-was-expected-vs-what-was-found. Good
  diagnostics are non-negotiable.
- The importer is an **initial-code generator**, not a
  source-of-truth. It never runs a second time against existing
  code. Users can regenerate into a temp directory if they want to
  eyeball the current canonical form, but they never overwrite
  their live source.

**For AI tooling:**

- When schema changes, build fails at a specific line with a
  specific expectation.
- Patching that line is a well-scoped, local edit an AI can do
  reliably. (Add a field, change a type, move an annotation.)
- No need to merge "generated vs edited" trees — the edit is just
  source code.

### Why every other library doesn't do this

Reflection-back-to-schema with byte-for-byte equivalence is *hard*.
Protobuf doesn't do it because the original protoc model predates
anyone expecting it. Capnproto's philosophy is "schema drives code,
code drives nothing." Flatbuffers is similar. For psio, the
reflection system is sophisticated enough to close the loop — and
the loop-closing is what makes schema-is-contract possible.

### Per-language requirement from this philosophy

Every tier-1 language must provide:

1. A **`schema-import <file.ext>` CLI** that emits initial code in
   the target language.
2. A **`reflect<T>::schema() == parse_schema(path)`** build-time
   check (or equivalent — `static_assert` in C++, `const_assert!`
   in Rust, proc-macro check in TS via type-level schemas, ...).
3. A **diagnostic** on mismatch that names the field and the
   expected-vs-actual difference.
4. **Survival of user edits.** The import tool never needs to run
   against an existing file; it only emits new files.
5. A **schema evolution story documented in the language's README**
   ("when the schema changes, here's how you patch your code").

This flips an API design constraint into a DX promise: *you do not
need to choose between clean code and schema-driven safety.*

---

## 1. Core principles (language-agnostic)

Applies to every language implementation of psio:

1. **Native reflection / code generation drives encoders and decoders.**
   Use whatever the language provides natively — C++26 `std::meta::` +
   `template for`, Rust proc macros, Zig comptime, TS decorators
   (stage 3) or schema-driven codegen, Python type hints + `inspect`,
   Go `reflect` + `go generate`. Never invent a parallel type
   registry.

2. **Views mimic natural type access in the target language.** If
   reading a decoded value is `obj.field` in language L, the view
   spelling is also `v.field` (or as close as L's type system
   permits). Views are not second-class citizens.

3. **Performance is the primary goal; developer experience is king.**
   Both matter; DX wins ties. Zero-cost abstraction where the
   language allows; measurable bench parity with best-of-class
   libraries per format; but never at the cost of a cryptic API.

4. **Reflection implies the schema; schema implies the wire format.**
   Three orthogonal layers:
   1. **User type** (what the programmer wrote)
   2. **Type structure** (the reflection graph: names, types,
      annotations, ordinals)
   3. **Wire format** (how the structure is laid out in bytes)

   Going from (1) → (2) is the reflection system. Going from (2) →
   (3) is the codec. Schemas export (2) to any number of declarative
   representations (WIT, capnp, fbs, proto, JSON schema).

5. **Per-format performance matches or beats the best-of-class
   library.** The gate: on representative workloads (BeaconState-
   size for SSZ/frac, Order-book for capnp/flatbuffers, etc.), psio
   meets or exceeds throughput of the leading library in that format
   for that language. No "we're almost as fast" acceptance.

6. **Schema definitions round-trip in each format.** Every format
   provides parse-schema (text → `psio::schema`) and
   emit-schema (`psio::schema` → text). A schema loaded from WIT,
   emitted back to WIT, re-parsed, re-emitted must byte-equal the
   original modulo whitespace/comments.

---

## 2. Target languages

Tier-1: shipping simultaneously with v2. Tier-2: ports landing after
tier-1 stabilizes. Tier-3: opt-in / community-driven.

| Language | Tier | Native impl? | WASM-psio bridge? | Primary use |
|---|---|---|---|---|
| **C++** | 1 | ✅ | — | server engine, psizam host, WASM host |
| **Rust** | 1 | ✅ | — | alternative server, safer subsystems, Rust-native apps |
| **TypeScript** | 1 | ✅ (via WASM + TS bindings) | ✅ primary path | web clients, dev tooling, schemas UI |
| **Python** | 2 | partial (native primitives + WASM for hot paths) | ✅ | scripts, data tooling, fuzz harness drivers |
| **Go** | 2 | ✅ codegen-driven | optional | infra integration, existing Go services |
| **Zig** | 3 | ✅ (comptime-native) | — | experimental; great fit for Zig's comptime |
| **Swift** | 3 | ✅ codegen-driven | ✅ | macOS/iOS clients |
| **Kotlin** | 3 | ✅ codegen-driven | ✅ | Android, JVM integration |

---

## 3. Per-language checklist

For each tier-1 or tier-2 language, all six principles must be
addressed. The table below shows what each principle looks like per
language and where status currently stands.

### 3.1 C++ (tier 1)

| # | Principle | Realization | Status |
|---|---|---|---|
| 0 | Schema-is-contract | `psio schema-import x.wit > x.hpp` emits struct + PSIO_REFLECT; `static_assert(reflect<T>::schema() == parse_schema(path))` enforces contract; user-owned file | CLI not built; static_assert machinery is implied by § 5.2.6 |
| 1 | Native reflection / codegen | C++26 `std::meta::` + `template for` when available; PSIO_REFLECT macro today with identical surface via `psio::reflect<T>` | design spec'd, step 1 pending |
| 2 | Views mimic native access | `v.field()` direct; storage ops are free functions (`psio::bytes(v)`). No `operator->`. Rich wrapper types opt-in | spec'd, see `psio-v2-design.md` § 4.1, § 5.5 |
| 3 | Perf + DX | Zero-cost templates; memcpy fast paths for POD layouts; single API surface via `psio::encode<Fmt>(v)` | partial: v1 has the perf; v2 unifies the surface |
| 4 | Reflection → schema | `reflect<T>::schema()` consteval → `psio::schema`; schema emitters consteval → text | spec'd |
| 5 | Best-of-class perf | Bench parity vs sszpp (C++), capnp (C++), flatbuffers C++, protobuf C++ | v1 ahead of sszpp; others TBD |
| 6 | Schema round-trip | `parse_wit`, `parse_capnp`, `parse_fbs` → `psio::schema` → `emit_*` → text. Byte-equal modulo whitespace | parsers exist (v1); unified via schema value in v2 |

### 3.2 Rust (tier 1)

| # | Principle | Realization | Status |
|---|---|---|---|
| 0 | Schema-is-contract | `psio schema-import x.wit > x.rs` emits struct + derives; `const_assert!(<T as Reflect>::schema() == parse_schema!(path))` (or proc-macro-based) enforces contract | not built |
| 1 | Native reflection / codegen | proc macros via `syn` + `quote` (today); future reflection RFCs when stabilized | derive macros exist |
| 2 | Views mimic native access | Method-call field access on `View<'a, T, Fmt>`; storage ops are free fns in the format module. Follows Rust conventions (no ADL tricks; explicit `use`) | exists for ssz/pssz |
| 3 | Perf + DX | Monomorphized generics; `#[repr(C, packed)]` for memcpy-safe structs; `Box<[T; N]>` direct-alloc path | v2 ssz/pssz path matches C++ throughput on BeaconState |
| 4 | Reflection → schema | `<T as Reflect>::schema()` const fn or `Schema::of::<T>()`; schema emitters as `to_*_schema::<T>()` free fns | exists for wit/capnp/fbs |
| 5 | Best-of-class perf | Bench parity vs `ethereum_ssz` (Lighthouse), `ssz_rs`, `capnproto-rust`, `flatbuffers-rust`, `prost` (protobuf), `rkyv`, `bincode`, `borsh` | ssz beats ethereum_ssz ~45×, ssz_rs ~51× on BeaconState |
| 6 | Schema round-trip | `parse_wit`, `parse_capnp`, `parse_fbs` → `Schema` → emitters → text | exists |

### 3.3 TypeScript (tier 1)

| # | Principle | Realization | Status |
|---|---|---|---|
| 0 | Schema-is-contract | `psio schema-import x.wit > x.ts` emits class + decorators; a build step (`psio-check` Vite/esbuild plugin or TS transformer) asserts `schemaOf(T) === parseSchema(path)` at build time | not built |
| 1 | Native reflection / codegen | Stage-3 decorators + TS type-level metadata for annotations; schema-driven codegen for typed accessors | not started |
| 2 | Views mimic native access | ES Proxy over the buffer — `view.field` reads from bytes on demand; no parenthesized method calls | design TBD |
| 3 | Perf + DX | Hot loops delegated to a WASM build of psio C++; TS glue wraps view creation, field access, RPC dispatch. Zero-copy via `SharedArrayBuffer` where available | WASM strategy decided, binding code TBD |
| 4 | Reflection → schema | `schemaOf<T>()` reads the decorator metadata; emitters convert to JSON / WIT / capnp text | not started |
| 5 | Best-of-class perf | Bench parity vs `protobufjs`, `@capnp-js`, `flatbuffers-js`, `@msgpack/msgpack` | not started |
| 6 | Schema round-trip | Parsers for WIT, capnp, fbs, proto → TS `Schema` object → emitters | not started |

### 3.4 Python (tier 2)

| # | Principle | Realization | Status |
|---|---|---|---|
| 0 | Schema-is-contract | `psio schema-import x.wit > x.py` emits dataclass + annotations; `psio.assert_schema(T, path)` called once at module import, raises on mismatch. Optional CI hook for build-time equivalent | not built |
| 1 | Native reflection / codegen | Dataclasses + `typing` annotations + `inspect` module; custom `__class_getitem__` for generic containers | not started |
| 2 | Views mimic native access | `__getattr__` on view classes — `v.field` returns a typed leaf or nested view. No Python-special boilerplate for users | design TBD |
| 3 | Perf + DX | Hot paths via C extension (wrapping C++ psio or pure-C ffi); pure-Python path for portability | design TBD |
| 4 | Reflection → schema | `psio.schema_of(T)` walks annotations → `psio.Schema` → emitters | not started |
| 5 | Best-of-class perf | Bench parity vs `protobuf` python, `pycapnp`, `flatbuffers` python, `msgpack`, `cbor2` | not started |
| 6 | Schema round-trip | Same parser/emitter set | not started |

### 3.5 Go (tier 2)

| # | Principle | Realization | Status |
|---|---|---|---|
| 0 | Schema-is-contract | `psio schema-import x.wit > x.go` emits struct with tags; a `go generate` comment + `psio-check` tool emits an `init()` test or build-time check asserting schema equivalence | not built |
| 1 | Native reflection / codegen | Struct tags + `reflect` package + `go generate` for accelerated codegen | not started |
| 2 | Views mimic native access | Generated view types with field methods; Go lacks operator overloading, so methods are the only path. Use short names (`v.Foo()`) matching exported field convention | design TBD |
| 3 | Perf + DX | Avoid allocations; use `unsafe` for zero-copy byte access; respect Go escape analysis | design TBD |
| 4 | Reflection → schema | `psio.SchemaOf[T]()` (generics); emitters as functions | not started |
| 5 | Best-of-class perf | Bench parity vs `protobuf-go`, `capnp-go`, `flatbuffers-go`, `msgpack-go` | not started |
| 6 | Schema round-trip | Same parser/emitter set, pure Go | not started |

### 3.6 Zig (tier 3)

| # | Principle | Realization | Status |
|---|---|---|---|
| 1 | Native reflection / codegen | `@typeInfo(T)` + `inline for` — comptime-native, the cleanest fit of any language | not started |
| 2 | Views mimic native access | Struct with member access; Zig's comptime generates field accessors trivially | straightforward |
| 3 | Perf + DX | Comptime resolves everything; runtime is hand-picked inlined loops | expected win |
| 4 | Reflection → schema | `comptime schemaOf(T)` → `Schema` value, emittable at comptime | straightforward |
| 5 | Best-of-class perf | Fewer competitors in Zig; probably psio becomes best-of-class by default | low bar |
| 6 | Schema round-trip | Same parser/emitter set, Zig-native | not started |

---

## 4. Cross-language invariants

Independent of which language a buffer was encoded in, these hold:

1. **Byte-identical wire output for the same logical value and
   format.** Encoding `{name: "alice", amount: 100}` as frac32 in
   C++, Rust, TS, Python, Go, and Zig must produce identical bytes.
   The conformance harness (see § 5) enforces this with
   cross-language fixtures.

2. **Schema files are language-agnostic.** A WIT schema written in
   C++ with `emit_wit_schema` parses in Rust, Python, Go, etc. —
   and the parsed `Schema` there is equivalent (same fields, same
   ordinals, same annotations) to the one the emitter produced.

3. **Third-party annotations round-trip by canonical name.** A spec
   type defined in Rust with `spec_kind = "myapp.v1.retry"` can be
   read back in C++ via a C++ spec type that declares the same
   `kind_name`, or passed through opaquely by code that doesn't
   know the kind.

4. **Reflection is a superset of the schema.** `reflect<T>` in C++
   / `Reflect<T>` in Rust / equivalents elsewhere carry enough
   information to fully reconstruct the schema and thus the wire.
   If you can reflect it, you can emit it; if you can parse a
   schema, you can decode bytes.

5. **Views and owning types are wire-identical.** Reading a field
   from a view equals reading the field from a decoded value
   (deeply, recursively, for every format).

---

## 5. Cross-language conformance harness

A single test corpus drives every language port:

- **Fixtures** — a set of canonical values (Validator, BeaconState,
  Order, Message, Config, Enum, Variant, Optional, Sorted, …) with
  known wire bytes per format.
- **Conformance checks per (language, format, fixture)**:
  - Encode fixture in the language → matches canonical bytes.
  - Decode canonical bytes in the language → fixture value equal.
  - Validate canonical bytes → `ok`.
  - View access in the language → leaf values equal fixture.
  - Schema of the fixture type → matches canonical schema file.
  - Transcode canonical bytes from format A to format B → matches
    canonical bytes in format B.
- **Cross-language fixtures** — bytes produced by language L₁'s
  encoder, decoded by language L₂'s decoder, must round-trip.
  Exercises every pair (C++↔Rust, C++↔TS, Rust↔Python, …).

Per-language CI runs this harness against every format the language
supports. A new language port is "done" when all relevant matrix
cells pass.

---

## 6. Per-language bench checklist

For principle 5 (on par or better than best-of-class), the bench
matrix is:

| Language | SSZ competitor(s) | Frac competitor(s) | Capnp | FlatBuf | Protobuf | Borsh/bincode | Avro | JSON |
|---|---|---|---|---|---|---|---|---|
| C++ | sszpp, Lighthouse (via C++26 interop) | — | capnproto-c++ | flatbuffers-c++ | protobuf-c++ | bincode-cpp? | apache-avro-c++ | rapidjson, simdjson |
| Rust | ethereum_ssz, ssz_rs | — | capnproto-rust, recapn | flatbuffers-rust | prost, rust-protobuf | bincode, borsh | apache-avro | serde_json, simd-json |
| TS | js-ssz (if exists) | — | capnp-ts | flatbuffers-js | protobufjs | — | avro-js | native `JSON.parse` |
| Python | py-ssz (eth2spec) | — | pycapnp | flatbuffers-python | protobuf | — | avro | stdlib json, orjson |
| Go | go-ssz (prysm) | — | go-capnp | flatbuffers-go | protobuf-go | — | goavro | encoding/json |
| Zig | — (be the first) | — | — | — | — | — | — | stdlib |

Each cell gets a bench harness + a committed expected throughput
range. Regressions past the range are CI failures; improvements
update the range.

---

## 7. Checklist for starting a new language port

When adding a new language port:

- [ ] **Philosophy (§ 0):** ship the `schema-import` CLI that emits
      initial, editable code in this language. Ship the build-time
      `reflect<T>::schema() == parse_schema(path)` contract check.
      Document the evolution workflow ("schema changed, here's how
      you patch").
- [ ] Principle 1: decide reflection mechanism (native vs codegen vs hybrid).
- [ ] Principle 2: decide view access spelling; validate it feels native.
- [ ] Principle 3: decide "hot path" implementation strategy (native code vs WASM-psio delegation).
- [ ] Principle 4: implement `reflect::schema` bridge.
- [ ] Principle 5: pick top-3 best-of-class competitors per format and benchmark before shipping.
- [ ] Principle 6: ship parsers + emitters for at least WIT, capnp, fbs, proto, JSON-schema.
- [ ] Conformance harness integration — new language becomes another row in § 5's matrix.
- [ ] Documentation — at least the 4 DX code patterns (end user, type author, format author, bench) in the language-specific README, PLUS the schema-evolution workflow required by § 0.

A port is "tier-1 ready" when every item on this checklist has a
green CI row, **including** the § 0 philosophy items — `schema-import`
works, contract check works, diagnostics on mismatch are legible,
the evolution walkthrough exists. Tier-2 is "works, some checks
pending." Tier-3 is "community-maintained, best-effort."

---

## 8. Pointer back

When a language port updates status, update the table cell in § 3.X.
When a new language is added, copy the § 3.X template and add a
column to § 6. When a principle is revised, update § 1 and audit
every language's implementation of it.
