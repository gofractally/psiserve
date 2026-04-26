# Schema Expressiveness Survey

A comparative survey of the type-system expressiveness of the schema formats
PSIO targets (or will target). The goal is **not** to treat any one format —
including PSIO's current `AnyType` IR — as the gold standard. The goal is
to enumerate every shape any schema in this set expresses, so that PSIO's
unified IR can be designed against the **union of expressiveness**, not the
intersection, and so that deliberate decisions can be made about what to
preserve and what to lower.

## Why outliers matter

Every schema has a handful of shapes that are uniquely its own, or nearly so.
These outliers are the most informative data we have for IR design:

- A shape that exists in *every* format is a safe bet for first-class IR.
- A shape that exists in *one* format is a decision: promote it to IR, lower
  it to something else, or explicitly refuse to preserve it. Silence is the
  worst answer — that's where information quietly disappears at the boundary.

The tables below flag every outlier explicitly.

---

## Expressiveness matrices

These schemas speak at three different layers at once. A single matrix
conflates them. We split them here by concern:

1. **Wire format** — what *values* the format can encode. The type system
   for data.
2. **Runtime** — schema-level features that affect evolution, metadata,
   canonicalization, and how the wire is interpreted — but not the types
   of values themselves.
3. **RPC / interfaces** — cross-boundary behavior: handles, methods,
   ownership, streaming.

Cells show what the format *natively* expresses as a first-class feature,
not what you can fake with a workaround.

Legend: 🟢 native, 🟡 partial / conventional / layered, 🔴 not expressible,
`n/a` not applicable to this class of format, `WKT` = expressed via
Well-Known Types.

---

### 1. Wire format — data types and shapes

What values can the format encode?

| Shape / concept              | FracPack | WIT  | Cap'n | FlatBuf | Proto | Avro  | JSON | MsgPack | CBOR  | Bincode | Borsh |
|------------------------------|----------|------|-------|---------|-------|-------|------|---------|-------|---------|-------|
| `bool`                       | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟢    | 🟢   | 🟢      | 🟢    | 🟢      | 🟢    |
| Sized ints (i8..i64/u8..u64) | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟡    | 🔴   | 🟢      | 🟢    | 🟢      | 🟢    |
| `f32` / `f64`                | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟢    | 🟡   | 🟢      | 🟢    | 🟢      | 🟢    |
| `f16` half-precision         | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🟢    | 🔴      | 🔴    |
| Arbitrary-precision int      | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🟢 (tag 2/3) | 🔴 | 🔴 |
| `char` (unicode scalar)      | 🔴       | 🟢   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🔴    | 🟢      | 🔴    |
| `string` (UTF-8)             | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟢    | 🟢   | 🟢      | 🟢    | 🟢      | 🟢    |
| `bytes` (opaque)             | 🟢       | 🔴   | 🟢    | 🟢      | 🟢    | 🟢    | 🟡   | 🟢      | 🟢    | 🟢      | 🟢    |
| Fixed-length bytes (`fixed`) | 🟢       | 🔴   | 🔴    | 🟡 arr  | 🔴    | 🟢    | 🔴   | 🔴      | 🔴    | 🟢      | 🟢    |
| `list<T>`                    | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟢    | 🟢   | 🟢      | 🟢    | 🟢      | 🟢    |
| `array<T, N>` (fixed)        | 🟢       | 🔴   | 🔴    | 🟢      | 🔴    | 🟢    | 🔴   | 🔴      | 🔴    | 🟢      | 🟢    |
| `option<T>` / nullable       | 🟢       | 🟢   | 🟡 union | 🟡 field | 🟡 opt | 🟡 union | 🟡 null | 🟡 nil | 🟢 | 🟢  | 🟢    |
| `result<ok, err>`            | 🔴       | 🟢   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🔴    | 🟢      | 🔴    |
| `tuple<...>`                 | 🟢       | 🟢   | 🔴    | 🔴      | 🔴    | 🟡    | 🟡   | 🟢      | 🟢    | 🟢      | 🟢    |
| `record` / `struct`          | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟢    | 🟢   | 🟢      | 🟢    | 🟢      | 🟢    |
| `variant` / `oneof`          | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟢    | 🔴   | 🔴      | 🔴    | 🟢      | 🟢    |
| `enum` (closed)              | 🟢       | 🟢   | 🟢    | 🟢      | 🟢    | 🟢    | 🔴   | 🔴      | 🔴    | 🟢      | 🟢    |
| `flags` / bitfield           | 🔴       | 🟢   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🔴    | 🔴      | 🔴    |
| `map<K,V>`                   | 🔴       | 🔴   | 🔴    | 🔴      | 🟢    | 🟢    | 🟡 str keys | 🟢 | 🟢    | 🔴     | 🔴    |
| `timestamp`                  | 🔴       | 🔴   | 🔴    | 🔴      | 🟡 WKT | 🟡 logical | 🔴 | 🔴   | 🟡 tag 1 | 🔴   | 🔴    |
| `duration`                   | 🔴       | 🔴   | 🔴    | 🔴      | 🟡 WKT | 🟡 logical | 🔴 | 🔴   | 🔴    | 🔴      | 🔴    |
| `date` / `time-of-day`       | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🟡 logical | 🔴 | 🔴   | 🔴    | 🔴      | 🔴    |
| `uuid`                       | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🟡 logical | 🔴 | 🔴   | 🔴    | 🔴      | 🔴    |
| `decimal` (arbitrary prec.)  | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🟡 logical | 🔴 | 🔴   | 🟡 tag 4 | 🔴   | 🔴    |
| Bignum / bigfloat            | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🟡 tag | 🔴     | 🔴    |
| URI / MIME / regex (tagged)  | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🟡 tag | 🔴     | 🔴    |

---

### 2. Runtime — schema features, evolution, wire properties

Features that affect how the schema is processed, evolved, or interpreted —
independent of the values themselves.

| Feature                            | FracPack | WIT  | Cap'n | FlatBuf | Proto | Avro  | JSON | MsgPack | CBOR  | Bincode | Borsh |
|------------------------------------|----------|------|-------|---------|-------|-------|------|---------|-------|---------|-------|
| Parameterized user-defined generics| 🔴       | 🔴   | 🟢    | 🔴      | 🔴    | 🔴    | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Field annotations / metadata       | 🔴       | 🔴   | 🟢    | 🟢 attr | 🟢 opt | 🔴   | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Field default values               | 🔴       | 🔴   | 🟢    | 🟢      | 🟡    | 🟢    | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Aliases (rename evolution)         | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🟢    | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Constants in schema                | 🔴       | 🔴   | 🟢    | 🔴      | 🔴    | 🔴    | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Field numbering / wire tags        | 🔴       | 🔴   | 🟢 ord | 🟢 id  | 🟢 num | 🔴   | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Reserved field numbers / names     | 🔴       | 🔴   | 🔴    | 🔴      | 🟢    | 🔴    | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Per-type extensibility opt-in      | 🟢       | 🔴   | 🔴    | 🟡 table/struct | 🔴 | 🔴  | n/a  | n/a     | n/a   | 🔴      | 🔴    |
| Canonical form specified           | 🟢       | 🟢   | 🟡    | 🔴      | 🔴    | 🟡    | 🔴   | 🔴      | 🟡    | 🔴      | 🟢    |
| Self-describing (schema in data)   | 🔴       | 🔴   | 🟡 pkd | 🔴     | 🔴    | 🟢 OCF | 🟢  | 🟢      | 🟢    | 🔴      | 🔴    |
| User-defined extension tags        | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🟢 ext  | 🟢 tag | 🔴     | 🔴    |
| Indefinite-length encoding         | 🔴       | 🔴   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🟢    | 🔴      | 🔴    |
| Zero-copy views                    | 🟢       | 🟢   | 🟢    | 🟢      | 🔴    | 🔴    | 🔴   | 🔴      | 🔴    | 🔴      | 🔴    |
| Safe in-place mutation             | 🟢       | 🟢   | 🟢    | 🟡 unsafe | 🔴   | 🔴    | 🔴   | 🔴      | 🔴    | 🔴      | 🔴    |
| Forward compat (old code, new data)| 🟢       | 🔴   | 🟢    | 🟢      | 🟢    | 🟢    | 🟢   | 🟢      | 🟢    | 🔴      | 🔴    |
| Backward compat (new code, old data)| 🟢      | 🔴   | 🟢    | 🟢      | 🟢    | 🟢    | 🟢   | 🟢      | 🟢    | 🟢      | 🟢    |

---

### 3. RPC / interfaces — cross-boundary behavior

Methods, handles, and ownership semantics. This is where the schema
describes *code*, not just data.

| Feature                           | FracPack | WIT  | Cap'n | FlatBuf | Proto | Avro  | JSON | MsgPack | CBOR | Bincode | Borsh |
|-----------------------------------|----------|------|-------|---------|-------|-------|------|---------|------|---------|-------|
| Interface / method declarations   | 🔴       | 🟢   | 🟢    | 🟡 rpc ext | 🟢 service | 🟡 protocol | 🔴 | 🔴 | 🔴 | 🔴 | 🔴 |
| Resource / handle type            | 🔴       | 🟢   | 🟢 intf | 🔴    | 🔴    | 🔴    | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |
| `own<T>` / `borrow<T>` ownership  | 🔴       | 🟢   | 🔴    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |
| Unary RPC                         | 🔴       | 🟢   | 🟢    | 🔴      | 🟢    | 🟡    | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |
| Server streaming                  | 🔴       | 🔴   | 🟢    | 🔴      | 🟢    | 🔴    | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |
| Client streaming                  | 🔴       | 🔴   | 🟢    | 🔴      | 🟢    | 🔴    | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |
| Bidirectional streaming           | 🔴       | 🔴   | 🟢    | 🔴      | 🟢    | 🔴    | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |
| Promise pipelining                | 🔴       | 🔴   | 🟢    | 🔴      | 🔴    | 🔴    | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |
| Exception / fallible return       | 🔴       | 🟢 ᵃ | 🟢    | 🔴      | 🟢 status | 🟢 | 🔴   | 🔴      | 🔴   | 🔴      | 🔴    |

ᵃ WIT expresses fallibility via `result<ok, err>` on function returns, not
as a separate exception channel.

---

## Outliers by format

The features below are *unique or near-unique* to one schema. Each is a
decision point for PSIO's IR: preserve, lower, or refuse.

### FracPack — the outliers

1. **Per-type extensibility choice.** Every struct is *opt-in* extensible
   (fracpack-extensible) or fixed (fracpack-fixed). No other format offers
   this granularity; most commit globally.
2. **Trailing optional elision.** Optionals at the end of a struct can be
   omitted entirely from the wire, distinct from being present-but-absent.
3. **Self-hosting schema.** FracPack's schema type (`AnyType` in
   `schema.hpp:420`) is itself FracPack-encodable. Schemas and data share a
   transport.
4. **Forward-only self-relative offsets.** A wire-format property, but one
   with schema-level consequence: traversal is guaranteed bounded.
5. **Fast mode vs canonical mode for in-place mutation.** The same bytes
   can be written in two layouts; only canonical is used for hashing.

### WIT — the outliers

1. **`result<ok, err>` as a first-class type.** No other schema in this set
   expresses fallibility structurally. Rust's `Result<T, E>` is its direct
   analogue; every other schema forces `variant { ok; err; }` or similar.
2. **`flags` as a distinct type.** A bitfield over named labels, semantically
   richer than "record of bools" even if isomorphic. Unique to WIT in this
   set.
3. **`char` (unicode scalar value).** Only WIT and Bincode have it as a
   type. Most formats force you to use `u32` or a one-character `string`.
4. **`resource` + `own<T>` + `borrow<T>` handle model.** Distinguishes
   owning handles from borrowed handles at the type level. Cap'n Proto has
   interfaces but no own/borrow distinction in the type system.
5. **No `bytes` primitive.** Forces `list<u8>`, losing the
   "this is an opaque blob, not a list of numbers" intent. A notable *gap*
   in WIT relative to every other binary schema here.
6. **No field defaults, no maps, no timestamps, no field ordinals.**
   Evolution story differs from Protobuf/Capnp.

### Cap'n Proto — the outliers

1. **Parameterized generics.** `List(T)` and user-defined generics are
   first-class (e.g. `struct Box(T) { value @0 :T; }`). No other schema in
   this set supports generic *user-defined* types; only built-in
   `List<T>` / `Option<T>` parameterization elsewhere.
2. **Constants in the schema.** Schemas can declare typed constants
   (`const pi :Float64 = 3.14159;`) that participate in the schema namespace.
3. **Groups.** Named collections of fields sharing an enclosing namespace
   without forming a separate struct. No structural equivalent elsewhere.
4. **Annotations with user-defined annotation types.** Open-ended typed
   metadata attachable to any declaration. FlatBuf and Protobuf have
   attributes/options but they are fixed or free-form; Capnp annotations
   are *typed*.
5. **AnyPointer / AnyStruct.** Fully-dynamic pointer slots with runtime
   type resolution.
6. **Field ordinals decoupled from source order.** `@0`, `@1` are explicit
   and independent of declaration order. Protobuf and FlatBuf have field
   numbers too, but Capnp is the only one that lets you reorder source
   without breaking wire layout as aggressively as it does.
7. **Bidirectional and pipelined RPC semantics** baked into the schema.

### FlatBuffers — the outliers

1. **Table vs Struct at the schema level.** Explicit per-type choice between
   a forward-evolvable vtable layout (`table`) and a fixed-layout packed
   struct (`struct`). FracPack's extensibility opt-in is close but
   layout-wise different.
2. **File identifiers.** Four-byte magic strings declared in schema, used
   for runtime format identification.
3. **Scalar field defaults encoded into the vtable.** Absent-in-wire means
   "use schema default," which is wire-format-semantic, not just encoder
   convention.
4. **Fixed-size arrays inside structs.** One of the few formats that allows
   `[T:N]` inline within a packed struct layout.

### Protobuf — the outliers

1. **Field numbers as the fundamental identity.** Names are for source;
   numbers are the wire contract. Cap'n Proto has ordinals, but Protobuf's
   `1-15 vs 16+` varint cost split has shaped a generation of schema
   design.
2. **`Any` type with URL type identifier.** A fully-dynamic message type
   whose payload is self-describing via a `type.googleapis.com/...` URL.
   Capnp's `AnyPointer` is close but less self-describing.
3. **Well-Known Types (WKTs).** `Timestamp`, `Duration`, `Empty`, `Struct`,
   `Value`, `ListValue`, `FieldMask` — a de-facto logical-type layer
   shipped in-band with the format.
4. **Services with streaming semantics in four flavors.** Unary,
   server-streaming, client-streaming, and bidirectional — declared in
   schema. Capnp has pipelining; Protobuf has streaming; the two are
   different primitives.
5. **Reserved field numbers and names.** Schema can declare removed fields
   as off-limits for future reuse. No analogue elsewhere.
6. **Proto2 extensions.** Third-party extension of a message's field space
   without modifying the original schema. Obsolete in proto3 but still in
   widespread use.
7. **Canonical JSON mapping specified.** Every scalar and message has a
   normative JSON form. No other binary schema in this set specifies
   JSON mapping normatively.

### Avro — the outliers

1. **Logical types layer.** A formalized convention for adding semantic
   meaning atop primitives: `decimal`, `uuid`, `date`, `time-millis`,
   `time-micros`, `timestamp-millis`, `timestamp-micros`, `local-timestamp-*`,
   `duration`. The most sophisticated semantic-type story in the set, and
   the one PSIO should steal directly.
2. **Aliases for field renames.** Evolution story includes explicit
   per-field aliases — a field can be renamed without breaking readers
   that know the old name.
3. **Schema resolution rules, fully specified.** Reader/writer schema
   differences have normative resolution rules (promotion, default
   backfill, alias substitution). No other format spec'd this tightly.
4. **Object Container Files.** A self-describing file format that wraps
   Avro data with its schema, compression, and sync markers.
5. **Default values are *in the schema*, not the encoder.** Required for
   resolution when a reader expects a field the writer didn't send.

### JSON — the outliers

1. **No integer/float distinction** at the type level. `number` is a
   superset. This is a *negative* outlier — an expressiveness reduction —
   but one every other format has to deal with when emitting JSON.
2. **Self-describing by default, no schema required.** Not expressible as
   a shape, but relevant: JSON is the only format in this set that
   routinely travels without a schema.
3. **Object keys are always strings.** Affects `map<K,V>` round-tripping.

### MessagePack — the outliers

1. **Distinct `bin` and `str` types on the wire.** Unlike JSON, MsgPack
   separates these at the bit level, even though it's otherwise
   schema-less.
2. **Extension types.** User-defined type codes (`ext 0..127`) for custom
   tagged values. A minimal version of CBOR tags.

### CBOR — the outliers

1. **The tag system.** An IANA registry of hundreds of typed tags (dates,
   bignums, decimals, URIs, MIME, regex, base64, rational, self-describe,
   …). The richest *extensible* semantic-type mechanism in the set.
2. **Half-precision floats (f16).** Only format here with native f16.
3. **Arbitrary-precision integers as primitive** (via tag 2/3).
4. **Indefinite-length encoding.** Streaming arrays/maps/strings/bytes
   whose length is unknown at the start of encoding — closed by a break
   marker.

### Bincode / Borsh — the outliers

1. **Rust-faithful sum types.** `Option<T>`, `Result<T, E>`, and user
   enums are first-class in ways that match Rust's memory model directly.
   Bincode is essentially "Rust types on the wire."
2. **Borsh: deterministic canonical form with floats excluded.** Borsh's
   design decision to refuse float encoding in consensus contexts is
   unique in the set.
3. **No named fields on the wire.** Field order is the contract. Evolution
   is by explicit versioning, not field identity.

---

## Dimensions of variation worth a table of their own

These axes cut across the matrix above and deserve calling out:

### UTF-8 enforcement

| Format       | Wire-level UTF-8 required? |
|--------------|----------------------------|
| FracPack     | Yes for `string`           |
| WIT          | Yes (Canonical ABI)        |
| Cap'n Proto  | Yes for `Text`             |
| FlatBuf      | Yes for `string`           |
| Protobuf     | Yes for `string` (proto3)  |
| Avro         | Yes for `string`           |
| JSON         | Yes                        |
| MsgPack      | `str` yes; `bin` no        |
| CBOR         | `text` yes; `bytes` no     |
| Bincode      | Yes (Rust `String`)        |
| Borsh        | Yes (Rust `String`)        |

The `string`/`bytes` split is universal in binary formats; only schema-less
formats (JSON) blur it. UTF-8 enforcement at the wire is nearly universal
for `string`.

### Language enforcement of the same distinction

| Language | `string` carries UTF-8? | Notes                                                |
|----------|-------------------------|------------------------------------------------------|
| Rust     | Hard enforcement        | `String` / `&str` are invariant-carrying types        |
| Python   | Hard enforcement        | `str` is Unicode; `bytes` is distinct                 |
| JS / TS  | Hard (UTF-16 internal)  | `string` is Unicode codepoints; `Uint8Array` is bin   |
| Java     | Hard (UTF-16 internal)  | `String` vs `byte[]`                                  |
| Go       | **By convention only**  | `string` and `[]byte` are layout-compatible           |
| Zig      | None                    | `[]const u8` used for both; convention is all you get |
| C++      | None                    | `std::string` is 8-bit bytes; `std::u8string` adds a typing hint without enforcement |

**Key observation:** the language's type system is a *hint* about intent
even when it's not a guarantee. Go code that says `string` is declaring
"this is intended text" even though the compiler won't enforce UTF-8.
PSIO's contract system should respect the dev's type choice as a
declaration, and leave UTF-8 validity to the wire-level validator.

### Evolution model

| Format       | Rename fields | Remove fields | Add fields | Reorder fields |
|--------------|---------------|---------------|------------|----------------|
| FracPack     | No (name-keyed via schema) | Yes (trailing optionals) | Yes (trailing) | No |
| WIT          | No            | No (strict)   | No (strict)| No             |
| Cap'n Proto  | Yes (ord-keyed)| Yes (obsolete)| Yes (new ord)| Yes (by ord) |
| FlatBuf      | Yes (id-keyed)| Yes (deprecated)| Yes (new id)| Yes         |
| Protobuf     | Yes (num-keyed)| Yes (reserved)| Yes       | Yes            |
| Avro         | Yes (alias)   | With default  | With default| N/A (by name) |
| Bincode      | Yes (name n/a)| No            | No (appending is informal) | No |
| Borsh        | Yes           | No            | No          | No             |

WIT has the strictest evolution model of any first-tier schema — this is
a real ergonomic problem when WIT is the source-of-truth in a PSIO pipeline.

### Semantic type layering

| Format       | How are timestamps / uuids / decimals expressed?     |
|--------------|------------------------------------------------------|
| FracPack     | `Custom` escape hatch (machine name, no registry)    |
| WIT          | Not expressible; use `resource` or `u64` convention  |
| Cap'n Proto  | `@annotation` or convention                          |
| FlatBuf      | Attribute annotation or convention                   |
| Protobuf     | Well-Known Types (Timestamp, Duration, ...)          |
| Avro         | First-class logical types with registry              |
| CBOR         | Tag registry (IANA)                                  |
| MsgPack      | Extension type codes (0-127 user-defined)            |

This row is the most important for PSIO. **Avro's logical types and CBOR's
tag registry are the two best-engineered semantic-type layers in the set.**
A PSIO IR that wants to faithfully carry timestamps, UUIDs, decimals,
durations, and IP addresses across all target formats needs an equivalent
registry-based mechanism. Avro's is the better starting point because
tags are named, not numbered.

---

## Proposed PSIO IR coverage targets

Not a final design — a list of shapes the IR must be able to carry to
claim "any-schema ↔ any-binary." Organized by decision type:

### Must be first-class in the IR

These are either universal or near-universal; not having them in the IR
forces every parser into `Custom` workarounds:

- `bool` (currently encoded as `Int` width 1 — should be its own case)
- `string` distinct from `bytes`
- `enum` (closed set of named labels) distinct from `Int`
- `map<K,V>` distinct from `list<tuple<K,V>>`
- `result<ok, err>` distinct from `variant { ok; err; }` (for WIT fidelity)

### Must be representable, possibly via a semantic-type registry

- `timestamp` (with encoding resolution: millis, micros, nanos)
- `duration`
- `date`, `time-of-day`
- `uuid`
- `decimal` (with scale and precision)
- `ipv4_addr`, `ipv6_addr`
- `uri`, `email`, `mime_type`

These should live in a registry modeled after Avro logical types: a named
tag plus a validated underlying encoding. `Custom` today is the hook, but
it needs a registry and well-known names.

### Must be representable, possibly with lowering

- `flags` (WIT) — preserve as distinct IR case or lower to record-of-bools
  with a flag marker. Decide.
- `char` (WIT, Bincode) — preserve as distinct case or lower to `u32` with
  a unicode-scalar tag. Preferring "preserve" because the intent is
  load-bearing for WIT interop.
- `fixed<N>` bytes — distinct from `array<u8, N>`? Probably yes.
- `array<T, N>` — already in IR as `Array`.
- Parameterized user-defined generics (Capnp) — may be out of scope;
  document the decision.

### Deliberately not preserved

These are decisions to refuse, not gaps:

- Field ordinals / numbers (Capnp / FlatBuf / Protobuf) — the IR is
  name-keyed. Numbers are a serialization detail of their respective
  encoders.
- File identifiers (FlatBuf) — wire-envelope concern, not shape.
- Indefinite-length encoding (CBOR) — wire-format concern, not shape.
- Well-Known Type URLs (Protobuf `Any`) — expressed via the semantic-type
  registry instead.
- Bidirectional streaming RPC semantics (Capnp, Protobuf) — interface
  shapes carry method signatures; streaming semantics are a separate
  dimension, possibly handled at the psiber layer, not in the type IR.
- Constants and annotations in schema (Capnp) — schema-attached metadata
  is an out-of-band concern.

### Open questions

- How to carry extensibility opt-in (FracPack) through to the IR without
  forcing every other format's structs into a binary extensible/fixed
  choice.
- How to represent `resource` / `own` / `borrow` when the target format
  has no handle concept. Probably: the IR preserves it, but only WIT and
  Capnp encoders know how to emit it; other encoders error out or opaque
  it.
- Whether to adopt a named annotation mechanism (Capnp-style) as an IR
  metadata channel, separate from the type IR itself.

---

## Takeaway

The current `AnyType` IR is *structurally adequate* for the intersection
of shapes common to most formats, but *semantically lossy* for the union.
The gap is in three places:

1. Primitive types that got lowered too aggressively (`string`, `bool`,
   `enum`, `map`, `result`) — the IR is parsimonious where it should be
   explicit.
2. Semantic types have no home except `Custom`, which is a name-only
   escape hatch with no registry, no validation rules, and no cross-format
   equivalence.
3. A handful of genuine outliers (`flags`, `char`, Capnp generics,
   FracPack extensibility-per-type) force per-parser decisions that should
   be unified.

The right next step is a proposed `AnyType` v2 that promotes the
near-universals to first-class cases and establishes a semantic-type
registry. Every parser is then rewritten against v2, and the cross-format
conversion story becomes a pure IR→IR fold for the first time.
