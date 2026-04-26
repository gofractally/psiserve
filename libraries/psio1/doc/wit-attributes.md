# WIT Attributes in PSIO

Status: draft v1. Implemented in `wit_parser.hpp` / `wit_types.hpp` as of
April 2026. Recognized at parse time; pass-through for unknown attributes.

PSIO accepts WIT as a human-authored schema format. The base WIT language
describes *structure* (records, variants, enums, flags, functions,
interfaces, worlds) but carries only a thin vocabulary for *evolution* and
*semantic invariants*. This document catalogs the attribute vocabulary PSIO
understands on top of base WIT — both the three attributes endorsed by
the W3C Component Model spec (`@since`, `@unstable`, `@deprecated`) and the
PSIO-specific extensions (`@final`, `@sorted`, `@unique-keys`, `@canonical`,
`@utf8`).

## Summary table

| Attribute        | Source       | Arg form                     | Purpose                                                  |
|------------------|--------------|------------------------------|----------------------------------------------------------|
| `@since`         | W3C spec     | `version = X.Y.Z`            | Item added at this schema version                        |
| `@unstable`      | W3C spec     | `feature = name`             | Item gated behind a feature flag                         |
| `@deprecated`    | W3C spec     | `version = X.Y.Z`            | Item deprecated as of this schema version                |
| `@final`         | PSIO         | —                            | Closed type — no future fields or cases may be added     |
| `@sorted`        | PSIO         | —                            | Elements appear in ascending key order on the wire       |
| `@unique-keys`   | PSIO         | —                            | No duplicate keys (for `list<tuple<K,V>>` as map)        |
| `@canonical`     | PSIO         | —                            | Bytes are in canonical form; non-canonical is invalid    |
| `@utf8`          | PSIO         | —                            | `list<u8>` bytes are valid UTF-8                         |
| `@padding`       | PSIO         | —                            | Field exists for alignment; value is not interpreted     |
| `@number`        | PSIO         | `n` (bare integer)           | Stable field/case identity for Protobuf/Capnp interop    |

## Grammar

```
attribute      ::= '@' name ('(' arg ')')?
name           ::= ident                 // kebab-case identifier
arg            ::= (key '=')? value      // key optional — bare value allowed
key            ::= ident
value          ::= ident                 // e.g. 'foo'
                 | semver                // e.g. 0.2.0
                 | integer               // e.g. 5
semver         ::= number '.' number '.' number
integer        ::= [0-9]+
```

Multiple attributes stack as a sequence before the item they decorate:

```wit
@since(version = 0.3.0)
@final
record point { x: f64, y: f64, z: f64 }
```

Attributes are attached to the *following* item. A field/case/label
consumes any attributes that appear immediately before it; a type
definition consumes any attributes before the `record` / `variant` /
`enum` / `flags` / `type` keyword.

## Attribute reference

### `@since(version = X.Y.Z)` — W3C spec

Marks the item as present from schema version `X.Y.Z` onward. Used by
dynamic validators to reason about blob compatibility across versions:
a blob tagged with schema version `V` must satisfy every item whose
`@since` is `<= V`.

Attaches to: records, variants, enums, flags, fields, cases, enum labels,
flag labels, type aliases, functions, interfaces, worlds.

### `@unstable(feature = name)` — W3C spec

The item is only present when the consumer opts into the named feature.
In FracPack, unstable items that a consumer has not enabled are treated
the same as unknown fields (forward-compat, preserved but not interpreted).

Attaches to: same positions as `@since`.

### `@deprecated(version = X.Y.Z)` — W3C spec

The item has been deprecated as of `X.Y.Z`. Consumers SHOULD migrate.
FracPack still accepts deprecated items unchanged; this attribute is
advisory.

Attaches to: same positions as `@since`.

### `@final` — PSIO

The record or variant is closed. No future fields or cases may be added
in any schema version. A receiver may reject blobs that contain unknown
trailing data against a `@final` type (the "extended" compat state is
not admissible; extra bytes mean invalid).

`@final` is distinct from the existing `definitionWillNotChange()`
PSIO1_REFLECT flag. `definitionWillNotChange()` is a *wire-format*
optimization — it selects the `Struct` variant of `AnyType` (immutable,
fixed size, no extension table) over `Object` (extensible). `@final` is
a *semantic contract* — the schema is closed, receivers reject trailing
unknowns. They often co-occur but mean different things and are
captured independently.

On the C++ side, `@final` is auto-detected from `std::is_final_v<T>` or
an explicit `type_attrs<T>` specialization containing `final_tag`.

Attaches to: records, variants.

### `@sorted` — PSIO

The elements appear in ascending order by key on the wire. For
`list<tuple<K,V>>` used as a map representation, this is the invariant
that enables `O(log N)` binary search directly on packed FracPack bytes
via the view API. For `list<T>` where T has a natural ordering, it
enables sorted-merge operations without deserialization.

Validators MUST verify sort order during semantic validation. A blob
declared `@sorted` but carrying unsorted data is invalid.

Attaches to: fields, type aliases, list types.

### `@unique-keys` — PSIO

For `list<tuple<K,V>>` used as a map representation, no duplicate keys.
Pairs with `@sorted` (sorted + unique = canonical map representation).
A validator MUST verify uniqueness during semantic validation.

Attaches to: fields, type aliases, list types.

### `@canonical` — PSIO

The bytes MUST be in FracPack canonical form — no trailing extension
tables beyond the last populated field, no alternative encodings of the
same logical value. Non-canonical bytes are invalid against this
attribute.

This is stricter than `@final`: `@final` says "no future fields may be
added to the schema," `@canonical` says "the wire form has exactly one
admissible representation." Often they appear together on database key
types, consensus-layer messages, and cryptographic digest inputs.

`@canonical` is type-scope only — a record or variant is canonical or
not. Individual fields don't independently have a wire form (their
types do), so `@canonical` does not attach to fields. If a specific
field needs canonical wire form, declare its *type* canonical.

Attaches to: records, variants.

### `@utf8` — PSIO

The `list<u8>` bytes are valid UTF-8. WIT's built-in `string` type is
already UTF-8 by spec; this attribute is for fields declared as
`list<u8>` (raw byte buffers) that carry textual content. A validator
MUST run UTF-8 validation during semantic validation.

Attaches to: fields of type `list<u8>`.

### `@padding` — PSIO

The field exists for alignment only — its bytes have no semantic
meaning and the validator does not interpret the value. FracPack
normally packs fields tightly with no inter-field padding; `@padding`
is the opt-in marker when an author inserts an explicit field to align
subsequent data (e.g., for SIMD-friendly layout or fixed-width record
embedding).

Attaches to: fields.

Maps to the PSIO1_REFLECT field type at codegen time (a fixed-size byte
array sized to carry the pad).

### `@number(n)` — PSIO

Assigns an explicit, stable numeric identity to a field or variant
case. FracPack identifies fields positionally and does not encode
numbers on the wire, so for FracPack this attribute is advisory and
round-trip-only. It matters for interop:

- **Protobuf** — field tags on the wire. `@number(5)` on a WIT field
  preserves the Protobuf tag through translation in either direction.
- **Cap'n Proto** — `@0`, `@1` ordinals on fields. `@number(n)` stores
  the ordinal so a WIT → Capnp emission produces stable ordinals.

If a field has no `@number`, emission chooses a number based on
declaration order (starting from 0 for Capnp-style or 1 for
Protobuf-style, format-dependent). Authors who need stable wire
identity across schema evolutions should assign `@number` explicitly.

The C++ reflection side already captures this via the existing
`numbered(int, ident)` PSIO1_REFLECT item (`reflect.hpp:612`). This
attribute is the WIT surface for that same concept.

Attaches to: fields, variant cases.

## Attachment matrix

| Position              | `@since` | `@unstable` | `@deprecated` | `@final` | `@sorted` | `@unique-keys` | `@canonical` | `@utf8` | `@padding` | `@number` |
|-----------------------|:--------:|:-----------:|:-------------:|:--------:|:---------:|:--------------:|:------------:|:-------:|:----------:|:---------:|
| record                | yes      | yes         | yes           | yes      | no        | no             | yes          | no      | no         | no        |
| variant               | yes      | yes         | yes           | yes      | no        | no             | yes          | no      | no         | no        |
| enum                  | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | no        |
| flags                 | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | no        |
| type alias            | yes      | yes         | yes           | no       | yes       | yes            | yes          | yes     | no         | no        |
| field                 | yes      | yes         | yes           | no       | yes       | yes            | no           | yes     | yes        | yes       |
| variant case          | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | yes       |
| enum label            | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | yes       |
| flags label           | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | yes       |
| function              | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | no        |
| interface             | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | no        |
| world                 | yes      | yes         | yes           | no       | no        | no             | no           | no      | no         | no        |

The parser accepts any attribute in any position (pass-through behavior,
see below); this matrix is the *semantic* contract for what each
attribute means. Applying `@sorted` to an enum is not rejected by the
parser, but has no defined meaning and should not be emitted by codegen.

## AST representation

Each attribute is parsed into a `wit_attribute`:

```cpp
struct wit_attribute {
    std::string name;       // "since", "final", "unique-keys", ...
    std::string arg_key;    // "version", "feature", "" for bare attrs
    std::string arg_value;  // "0.2.0", "experimental-api", ""
};
```

`wit_attribute` is `PSIO1_REFLECT`'d, so a parsed WIT AST containing
attributes is itself FracPack-serializable — attributes survive the
`WIT text → wit_world AST → FracPack bytes → wit_world AST` roundtrip.

Attribute vectors are attached to:

- `wit_type_def::attributes` — records, variants, enums, flags, aliases
- `wit_named_type::attributes` — fields, cases, labels, function params/results
- `wit_func::attributes` — functions
- `wit_interface::attributes` — interfaces
- `wit_world::attributes` — worlds

## Unknown-attribute policy

The parser accepts any `@name` or `@name(key = value)` form and stores it
verbatim in the `attributes` vector. Unknown attributes are *not*
rejected. This serves three goals:

1. **Forward compat.** A schema carrying attributes introduced in a
   later PSIO version still parses cleanly today.
2. **Roundtrip fidelity.** Importing a WIT file and exporting it back
   preserves user-authored attributes that PSIO does not understand.
3. **User extensibility.** Teams can carry project-specific attributes
   (e.g. `@owner(team = platform)`, `@pii`, `@audit-log`) without
   waiting for PSIO to standardize them.

Validators ignore attributes they don't recognize. Codegen targets ignore
attributes they have no lowering for, but MAY emit them as comments in
generated code.

## Round-trip story

The complete roundtrip that PSIO aims to support:

```
C++ types (PSIO1_REFLECT)
   ↓ SchemaBuilder::insert<T>()
Schema / AnyType (IR, FracPack-serializable)
   ↓ export to WIT
WIT text (human-readable, with attributes)
   ↓ wit_parse
wit_world AST (with attributes)
   ↓ import to IR
Schema / AnyType (round-tripped)
   ↓ codegen
C++ types (equivalent to original)
```

The attribute machinery in `wit_parser`/`wit_types` covers the WIT text ↔
wit_world step. The Schema/IR ↔ WIT hop and the reflection ↔ Schema/IR
hop still need work to thread attributes end-to-end. The current gap on
the C++ reflection side:

| Need at codegen          | Reflection captures today          | Gap                                   |
|--------------------------|------------------------------------|---------------------------------------|
| `@final` on a record     | `std::is_final_v<T>` (native C++)  | surface through `type_attrs` to IR    |
| `@canonical` on a record | —                                  | add `canonical()` flag to PSIO1_REFLECT |
| `@sorted` on a field     | via `type_attrs<std::map<…>>`      | ship stdlib specializations; wire to IR |
| `@unique-keys` on field  | via `type_attrs<std::map<…>>`      | same as above                         |
| `@utf8` on list<u8>      | via `type_attrs<std::u8string>`    | ship spec; field escape via `member_attrs` |
| `@padding` on field      | —                                  | field-level via `ATTRS(x, padding)` or wrapper |
| `@number` on field       | `numbered(int, ident)` (existing)  | flow reflected number into IR and emit |
| `@since` / `@unstable`   | —                                  | `member_attrs` specialization with string arg |

Type-level flags already exist in `PSIO1_REFLECT` (see
`PSIO1_MATCH_ITEMSdefinitionWillNotChange` in `reflect.hpp`) — adding new
boolean type-level flags is mechanical. Field-level markers don't exist
today; fields are currently just identifiers. Adding them requires
either (a) a new `PSIO1_REFLECT` grammar (e.g. `attr(field_name,
"sorted")`) or (b) a type-system convention where field types themselves
carry attributes (e.g. a `psio1::sorted<std::vector<T>>` wrapper).

The IR side also needs an `attributes` vector on `Struct`, `Object`,
`Variant`, and `Member` in `schema.hpp` to carry attributes from the
reflection walk through to WIT export. `Custom{type, id}` wrapping is
not sufficient — `id` is a single string, attributes are a list with
keyed args.

## Migration and compat

None of the attributes change FracPack wire bytes. A blob that was valid
before this document is still valid. Existing schemas that do not use
any attributes parse and round-trip unchanged.

The `wit_attribute` struct, `wit_named_type::attributes`, and the other
`attributes` fields are appended to the end of their reflect lists, so
older FracPack-serialized `wit_world` AST blobs remain readable under
the trailing-extension-field compat rule.

Attributes are an *authored-schema* concept. FracPack itself — being a
reflection export, not an authored form — does not need to encode them,
but it may surface them if the reflection walk captures them (see
roundtrip story above).
