# psio: type-level caps (`maxFields`, `maxDynamicData`)

**Status:** implemented across foundation + all format consumers + schema
emitters as of 2026-04-29. Cross-format integration test still pending
(see "Open work").

## Motivation

`PSIO_REFLECT` already supports `definitionWillNotChange()` — a
type-level assertion that the schema author treats the layout as
frozen. It is consumed by every binary format (`pssz`, `frac`, `bin`,
`borsh`, `bincode`) to drop per-record extension headers and pick
fast-path memcpy paths.

`maxFields(N)` and `maxDynamicData(N)` extend the same channel:

| annotation | what it bounds | who consumes it |
|---|---|---|
| `definitionWillNotChange()` | layout stability (no future field additions) | every binary format |
| `maxFields(N)` | declared field count | `pssz`/`frac`/`psch` slot-index width; future schema-emit `maxProperties` |
| `maxDynamicData(N)` | total encoded payload size in bytes | every format's encode + validate path |

The three preconditions for narrowing offset width in `pssz`/`frac`/`psch`:

1. `is_dwnc_v<T>` — no extension header.
2. `all_dwnc_v<T>` — all transitively reachable types are also DWNC.
3. compile-time-known max ≤ chosen width's range, where the cap can
   tighten the bound below what `max_encoded_size<T>` infers from
   per-field `length_bound`s.

The third is the load-bearing one. With these annotations, the
"compile-time-knowable max" becomes the schema author's claim, not the
format's deduction. Validators verify the claim at decode time.

## Surface

### Annotation specs (`psio/annotate.hpp`)

```cpp
struct max_fields_spec        { std::size_t value{}; ... };
struct max_dynamic_data_spec  { std::size_t value{}; ... };
```

### Macro keywords (`psio/reflect.hpp`)

```cpp
PSIO_REFLECT(BeaconBlock,
   slot, proposer_index, parent_root, state_root, body,
   attr(body, max<8192>),               // field-level (existing)
   definitionWillNotChange(),           // type-level (existing)
   maxFields(8),                        // type-level (NEW)
   maxDynamicData(4096))                // type-level (NEW)
```

Both new keywords use **paren form** `maxFields(N)` rather than
`maxFields<N>()` because the macro dispatch uses identifier-cat to
detect keywords and `<` isn't valid in macro names.

### Format-author accessors

```cpp
psio::is_dwnc_v<T>                  // bool                    — same as before
psio::all_dwnc_v<T>                 // bool                    — transitive
psio::max_fields_v<T>               // optional<size_t>        — explicit cap
psio::max_dynamic_data_v<T>         // optional<size_t>        — explicit cap
psio::effective_max_fields_v<T>     // optional<size_t>        — min(cap, member_count)
psio::effective_max_dynamic_v<T>    // optional<size_t>        — min(cap, max_encoded_size<T>)
```

### Shared cap helpers

```cpp
template <typename T>
inline void enforce_max_dynamic_cap(std::size_t total,
                                     std::string_view format_name);
//   throws codec_exception when cap is set and total > cap.

template <typename T>
inline codec_status check_max_dynamic_cap(std::size_t total,
                                           std::string_view format_name) noexcept;
//   noexcept counterpart for validators that return codec_status.
```

## Decision: `tuple_cat` aggregator

The original `EMIT_ANN_typeattr_dwnc` emitted a *separate*
`annotate<type<T>{}>` specialization per type-level keyword. With three
possible keywords (dwnc + maxFields + maxDynamicData), naïvely emitting
one specialization per keyword causes redefinition errors.

**Resolution:** all type-level keywords from one `PSIO_REFLECT` are
combined into a single `annotate<type<T>{}>` specialization via
`std::tuple_cat`. An empty-guard via `BOOST_PP_CHECK_EMPTY` skips
emission when no type-level keywords are present, leaving room for an
external `PSIO_TYPE_ATTRS` to attach the type-level annotation
separately (the `macro_attrs` pattern in `annotate_tests.cpp` continues
to work).

## Decision: cap wins downward only

`effective_max_dynamic_v<T>` returns `min(explicit_cap, inferred_bound)`.
Override-upward would defeat the purpose: an explicit cap looser than
the inferred bound silently weakens the per-field annotations.

The same rule applies to `effective_max_fields_v<T>` vs
`reflect<T>::member_count`.

## Decision: encode-time throw, no silent widening

When the encoded payload size exceeds the cap, encoders throw
`psio::codec_exception`. Silently widening offset width would defeat
the predictable-wire-size contract that motivated the cap. Honest
failure is the contract.

Today's implementation enforces cap as a ceiling on **total encoded
size** (fixed + dynamic). The spec describes it as a dynamic-region
cap, but for the formats whose fixed-region overhead is small (pssz
without DWNC, all DWNC formats), total-vs-cap is the conservative
correct check. A strictly-dynamic split is a refinement available when
fixed-region overhead becomes material (e.g., capnp with deeply nested
struct headers).

## Decision: validator hard-rejects oversized input

Every format's `validate<T>(bytes)` calls `check_max_dynamic_cap<T>`
**before** any structural decode. This protects against DoS via
oversized payloads that would otherwise consume parser memory before
the structural error surfaces.

Formats with their own bound machinery (e.g., SSZ's canonical
sum-of-bounds) layer the cap on top: reject if size exceeds
`min(format-canonical-bound, type-level-cap)`.

## Format consumer matrix

| format | offset-width consumer | encode throw | validate reject | notes |
|---|:-:|:-:|:-:|---|
| pssz       | ✓ | ✓ | ✓ | full vertical via `auto_pssz_width` + `enforce_max_dynamic` |
| frac       | n/a (explicit width) | ✓ | ✓ | frac32/16 are explicit aliases, no auto-selector |
| psch       | n/a (schema format) | n/a | n/a | psch encodes schemas; cap surfacing handled in `emit_pssz`/Schema IR |
| pjson      | n/a (runtime-pick) | ✓ | ✓ (via `from_pjson_checked`) | adaptive widths picked from actual content; cap is the ceiling |
| bin/borsh/bincode | — | — | ✓ | no offsets to narrow; bytes-size reject only |
| ssz        | — | — | ✓ | layered atop SSZ canonical bounds |
| wit        | — | — | ✓ | reject before lift |
| avro/msgpack/protobuf | — | — | ✓ | DoS guard before decode |
| capnp/flatbuf | — | — | ✓ | reject before offset-following |
| json/bson  | — | — | ✓ | document-size ceiling |

## Schema emission

| target IDL | surface mechanism |
|---|---|
| pSSZ schema (psch) | Attribute name/value pair on `Object`/`Struct` IR node — round-trips through `emit_pssz` / `parse_pssz` |
| WIT | native `@maxFields(N)` / `@maxDynamicData(N)` attributes (forward-compat: unknown WIT attrs are spec-permitted) |
| capnp `.capnp` | preceding-line comment `# @maxFields(N)` |
| protobuf `.proto` | preceding-line comment `// @maxFields(N)` (proto3 has no native attribute syntax) |
| flatbuffers `.fbs` | preceding-line comment `// @maxFields(N)` |
| Avro JSON schema | not yet — no standalone `emit_avro` exists; future emitter would use vendor-namespaced metadata key |
| JSON Schema | not yet — would map maxFields → `maxProperties`, maxDynamicData → vendor extension `x-psio-max-dynamic-data` |

## Open work

- **Cross-format integration test** (#130). One test per format that
  encodes T at cap (must succeed), encodes T over cap (must throw with
  consistent error class), validates a wire-mutated oversized buffer
  (must reject). Ensures the contract is uniform across formats.
- **Strictly-dynamic-region cap enforcement** when fixed-region overhead
  becomes material. Requires per-format `dynamic_size<T>(v)` accessor.
- **Avro / JSON Schema** standalone emitters when consumers materialize.

## Files touched

- `libraries/psio/cpp/include/psio/annotate.hpp` — specs, accessors,
  `enforce_max_dynamic_cap` / `check_max_dynamic_cap` helpers.
- `libraries/psio/cpp/include/psio/reflect.hpp` — keyword macros, KIND
  constants, `tuple_cat` aggregator with empty-guard.
- `libraries/psio/cpp/include/psio/max_size.hpp` —
  `effective_max_dynamic_v<T>`.
- `libraries/psio/cpp/include/psio/{pssz,frac,bin,borsh,bincode,ssz,wit,avro,msgpack,protobuf,capnp,flatbuf,json,bson,pjson_typed}.hpp`
  — encode-throw and/or validate-reject hooks.
- `libraries/psio/cpp/include/psio/schema_builder.hpp` — surface caps as
  Attribute pairs on `Object`/`Struct` IR.
- `libraries/psio/cpp/include/psio/{emit_capnp,emit_fbs,emit_protobuf,emit_wit}.hpp`
  — emit cap attributes per IDL conventions.
- `libraries/psio/cpp/tests/annotate_tests.cpp` — declaration +
  composability tests; foundation trait tests.
- `libraries/psio/cpp/tests/pssz_tests.cpp` — encode-throw +
  validate-reject tests.
- `libraries/psio/doc/pjson-spec.md` — §7 type-level caps section.
