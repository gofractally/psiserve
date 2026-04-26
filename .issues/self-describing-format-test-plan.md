# self-describing-format: test & benchmark plan

**Status:** plan, 2026-04-26. Companion to `self-describing-format.md`.
Tests defined here will be implemented alongside the format itself.

## Scope

Validate that every JSON ↔ JSONB ↔ pssz path is correct, fast, and
forward-compatible. The format earns its keep at runtime; tests must
prove it does.

Three concentric goals:

1. **Correctness.** Every claim in the spec is verified by at least
   one test. Wire-format compliance is established by golden vectors
   and round-trip identities.
2. **Robustness.** Adversarial input doesn't crash the validator,
   doesn't produce undefined behavior in the decoder, and doesn't
   leak resources.
3. **Performance.** Encode, decode, and lookup beat reference
   formats (simdjson + rapidjson for JSON, libbson for BSON, our
   pssz/fracpack for binary) on the workloads schemaless tables
   actually see.

## Test file layout

Mirrors the existing psio3 conventions in `libraries/psio3/cpp/tests/`.

| file | concern |
|---|---|
| `jsonb_format_tests.cpp` | wire-format compliance: tag inventory, container layout, offset modes, varuint semantics, header byte |
| `jsonb_roundtrip_tests.cpp` | JSON → JSONB → JSON identity; canonical and non-canonical inputs; type coverage |
| `jsonb_suffix_tests.cpp` | every `.tag` in the v1 registry: encode, decode, round-trip; unrecognized suffixes round-trip as plain keys |
| `jsonb_schema_tests.cpp` | pssz reflection ↔ JSONB; fast-path lock-step parse; index fallback on non-canonical input; missing/extra fields |
| `jsonb_view_tests.cpp` | zero-copy reader correctness: `jsonb_view<T>` and `jsonb_object_view`; sub-views; iteration; lifetime |
| `jsonb_validation_tests.cpp` | every "must error" case from the spec: truncated buffers, out-of-range offsets, non-monotonic offsets, invalid varints, malformed UTF-8, count mismatch |
| `jsonb_inplace_tests.cpp` | same-or-smaller overwrites land in place; size-growing overwrites refuse cleanly |
| `jsonb_fuzz_tests.cpp` | extends the existing `validate_fuzz_tests.cpp` pattern: arbitrary bytes never crash validate; valid bytes round-trip stable |
| `jsonb_corpus_tests.cpp` | runs the test corpora (json.org suite + real-world set) end-to-end |

Plus benchmarks:

| file | concern |
|---|---|
| `libraries/psio3/cpp/benchmarks/jsonb_perf_report.cpp` | head-to-head: jsonb vs JSON-text vs BSON vs PG-jsonb-modeled vs pssz, across shape tiers |
| reuses `harness.hpp` | auto-tuning iters, warmup, 7-trial min/median/stddev, CV reporting (already built) |

## Test corpora

| corpus | source | size | purpose |
|---|---|---|---|
| `json_org_suite` | json.org's standard JSON test suite | ~30 files | spec compliance for parser-side |
| `nativejson_benchmark` | github.com/miloyip/nativejson-benchmark | a few MB | parser performance reference shapes |
| `psitri_real_world` | sampled from psitri DB workloads (anonymized) | TBD | actual production-shape documents |
| `pssz_struct_corpus` | reflected structs from psio3's existing shape library (`shapes.hpp`) — Point, NameRecord, FlatRecord, Record, Validator, Order, ValidatorList | 7 tiers | parity with existing bench shapes |
| `adversarial` | hand-crafted malformed JSONB buffers | a few hundred | validator robustness |

Corpora live in `libraries/psio3/cpp/tests/jsonb_corpus/` as files plus
expected golden outputs.

## Wire-format compliance — `jsonb_format_tests.cpp`

Per-tag-inventory tests:

- `null` encodes as 1 byte (tag = 0); decodes correctly.
- `bool_false` / `bool_true` encode as 1-byte tags 1/2.
- `int_inline` packs values 0..15 in tag low nibble; tag byte 0x3F = inline 15.
- `int` with byte_count=N encodes value as N-byte zigzag; round-trip for boundary values (0, 1, -1, ±2⁷, ±2¹⁵, ±2³¹, ±2⁶³, ±2¹²⁷).
- `decimal` encodes mantissa varint + scale byte; round-trip for representative values including subnormal-scale, large-scale, normalized form.
- `string_short` packs length 0..15 in tag low nibble; round-trip empty string, length-1, length-15.
- `string` uses varuint length prefix; round-trip for various lengths.
- `bytes` uses varuint length prefix.
- `array` / `object` carry layout-flag nibble in tag.

Container-layout tests:

- Object with N=0 fields: header + count varuint + empty arrays + empty payload.
- Object with N=1: index correctness for the single entry.
- Object with N=8 (small mode): u8 offsets correctly placed.
- Object with N=8 forced into mid mode (when sum of values > 256B): u16 offsets.
- Object with N=8 forced into large mode (sum > 64KB): u24 offsets.
- Sentinel offset entry (`offset_table[count]` one past end) correctly identifies the last value's length.

Specific byte-layout tests:

- Hex-dump golden vector for `{"a": 1}` matches the spec exactly.
- Hex-dump golden vector for `{"a": "b"}` matches.
- Hex-dump golden vector for `{"a": [1,2,3]}` matches.
- Each tested in all three offset modes if the size triggers it.

## Round-trip — `jsonb_roundtrip_tests.cpp`

Identity tests for every type:

- Null, true, false → JSON → JSONB → JSON.
- Integer at every boundary (0, 1, -1, 2⁷-1, 2⁷, 2³¹-1, 2³¹, 2⁶³-1, 2⁶³, 2¹²⁷-1, 2¹²⁷).
- Reject 2¹²⁸ and beyond; reject −2¹²⁸ - 1 and beyond.
- Decimal: 0.1, 0.0001, 1e10, 1e-10, π-truncated-to-15-digits, max f64-text, min subnormal-scale.
- String: empty, ASCII, UTF-8 multi-byte (Greek, CJK, emoji ZWJ sequences, RTL with combining marks).
- Bytes (via `.base64`): empty, 1 byte, 16 bytes (UUID-shaped), 1KB, large.
- Array: empty, 1 element of each tag, mixed types, 100 elements, deeply nested.
- Object: empty, 1 field, all-default-types, all-suffixed-types, large.

Canonical vs non-canonical:

- Canonical input → JSONB → JSON: byte-identical to input.
- Non-canonical input (sorted lex, reversed, random) → JSONB → JSON: produces canonical-for-shape output (i.e. the writer's natural order, which for parsed-JSON = original input order).
- A pssz value → JSON → JSONB → JSON → pssz: byte-equal pssz on both ends.

Every `.tag` suffix:

- `.hex` round-trips at all integer widths and binary lengths.
- `.base58` round-trips with known good vectors.
- `.base64` round-trips, padded and unpadded.
- `.decimal` round-trips for big-int values.
- `.rfc3339` round-trips for date-only and full-timestamp forms.
- `.unix_ms` / `.unix_us` round-trip across the full int64 range.
- Unrecognized suffix `.foo` round-trips as a plain string-keyed pair (forward compatibility).

## Schema integration — `jsonb_schema_tests.cpp`

Cross-format identities (pssz ↔ JSONB ↔ JSON):

- Every shape tier from `shapes.hpp`: encode through pssz → JSONB → JSON → JSONB → pssz; assert pssz value equality.
- Cross-direction: encode through JSON → JSONB → pssz → JSONB → JSON; assert canonical JSON.

Lock-step fast path:

- Canonical-order JSON input reaches the fast path (instrument decoder to count fast-path hits vs fallback hits; assert all hits are fast).
- Non-canonical-order input latches into fallback after first miss.
- Mixed canonical-prefix-then-shuffled input: fast path runs through prefix, fallback handles tail.

Forward / backward compatibility:

- Older schema (fewer fields), newer JSON (extra fields): extras are skipped; missing fields default-init.
- Newer schema (extra fields), older JSON (missing fields): missing C++ fields default-init.
- Reordered keys: round-trip succeeds, fallback engaged.

Reflection-driven encoder tests:

- Field with `as<hex_tag>` emits `.hex` suffix.
- Field with `as<rfc3339_tag>` emits `.rfc3339`.
- Field with `as<base64_tag>` emits `.base64`.
- Custom user-defined `MyTag` with registered `adapter<T, MyTag>` emits `.my_tag`.

## Zero-copy view — `jsonb_view_tests.cpp`

Correctness:

- `jsonb_view<T>::get<&T::field>()` returns the same value the encoder put in.
- Sub-objects: `view.inner().field()` (chain of `PSIO3_REFLECT`-generated accessors) returns the right value.
- Iteration: walking a `jsonb_object_view` yields all (key, value) pairs in storage order; iterating a `jsonb_array_view` yields elements in order.
- `string_view` returned from `as_string()` points into the source buffer (verify by pointer-arithmetic check).
- `span<const byte>` from `as_bytes()` points into the source buffer.

Allocation discipline:

- Custom allocator instrumented to count allocations; assert zero allocations across a typical query path:
  - Construct `jsonb_view`
  - Get 3 fields, two of them strings, one a sub-object
  - Walk the sub-object's array
  - Read each array element

Lifetime:

- View is invalid after the source buffer is freed (covered by ASan when the test runs under `-fsanitize=address`).

Schema-aware fast path (instrumented):

- Read field i of a struct: count exactly 3 buffer reads (mode byte, offset entry, value tag). Verify via instrumented buffer wrapper.

## Validation — `jsonb_validation_tests.cpp`

Each error case from the spec gets a test:

- Truncated header (1 byte): `validate` returns error.
- Truncated count varuint.
- Truncated key_hash array (count claims more than buffer holds).
- Truncated key_pos array.
- Out-of-range offset (offset > container payload size).
- Non-monotonic offset (offset[i+1] < offset[i] for sequential variable layout). For our shape, offsets are independent so this doesn't strictly apply; but verify the offset-table sentinel is consistent.
- Invalid varint (continuation bit on last byte).
- Invalid UTF-8 in a `string` field.
- Invalid UTF-8 in a key.
- Mismatched count: declared 5, actual buffer holds 3.
- Recursion depth exceeded (> 64): rejected.

For every error: validator returns `codec_status` with appropriate offset and message; throwing variant raises `codec_exception`.

## Adversarial fuzzing — `jsonb_fuzz_tests.cpp`

Builds on `validate_fuzz_tests.cpp` pattern:

- Truncate every well-formed buffer at every byte position; validate must not crash.
- Flip every byte position of every well-formed buffer; validate must not crash.
- Fully random bytes (deterministic PRNG, fixed seed); validate must not crash.
- Crash-on-validation-success failure mode: for each randomly-passed-validate buffer, attempt decode; decode must not crash either.

Run under ASan and UBSan in CI.

Optionally: libfuzzer harness for continuous fuzzing.

## In-place mutation — `jsonb_inplace_tests.cpp`

- Encode `{"counter": 42}` with explicit `int` byte_count = 8; mutate in place to `{"counter": 100}`; verify success without re-encoding.
- Encode `{"name": "alice"}`; in-place overwrite to `{"name": "bob"}`: same length, success.
- Encode `{"name": "alice"}`; in-place overwrite to `{"name": "al"}`: smaller, success (gap remains).
- Encode `{"name": "alice"}`; in-place overwrite to `{"name": "alexander"}`: larger, fails; library returns "needs re-encode" status.
- Verify that the round-trip after in-place edit produces the new value (no gap leakage).

## Performance benchmarks — `jsonb_perf_report.cpp`

Mirrors `bench_perf_report.cpp` shape: head-to-head across shapes,
formats, libraries.

### Shapes

Reuse `shapes.hpp` (Point, NameRecord, FlatRecord, Record, Validator,
Order, ValidatorList) — already established in the existing bench.
Add jsonb-specific shapes:

- `WideObject` — 100 string keys with int values (stresses key lookup)
- `DeepObject` — 10 levels of nested structs (stresses recursive decode)
- `BigArray` — 10000-element array of small structs (stresses sequential walk)
- `MixedTypes` — every type tag exercised (stresses dispatch)

### Operations measured

| op | direction | rationale |
|---|---|---|
| `encode_pssz_to_jsonb(v)` | encode | hot path: in-process serialization |
| `encode_pssz_to_json(v)` | encode | reflection-driven JSON output |
| `decode_jsonb_to_pssz(buf)` | decode | hot path: fast-path lock-step |
| `decode_jsonb_to_pssz(shuffled)` | decode | fallback path: index lookup |
| `decode_json_to_pssz(text)` | decode | streaming parse |
| `decode_json_to_jsonb(text)` | format conversion | DB ingest path |
| `lookup_typed<&T::field>(view)` | lookup | schema-aware random access |
| `lookup_schemaless(view, "name")` | lookup | schemaless random access |
| `validate(buf)` | validate | both throwing and status modes |
| `size_of(v)` | packsize | constexpr-folded fast path |
| `iterate_all_fields(view)` | iteration | full walk |

### Reference libraries

| library | role | how |
|---|---|---|
| simdjson | JSON parse reference | `simdjson::ondemand` for lookup, `dom` for full parse |
| rapidjson | JSON parse + DOM reference | mixed-mode |
| libbson | BSON reference | direct API |
| pssz (psio3) | binary baseline | already in the bench |
| fracpack (psio3) | binary baseline | already in the bench |

### Reporting

Reuse `harness.hpp` (already supports auto-tuning, warmup, CV
reporting). Output format: same markdown table style as
`PERF_V1_V3_FULL.md`.

Specific output: `libraries/psio3/JSONB_PERF.md` with one row per
(shape, format, library) tuple, columns for encode / decode / lookup /
validate / size, plus CV. Plus a CSV mirror.

### Target numbers (success criteria)

- **Encode pssz → JSONB:** within 1.5× of pssz → fracpack on the
  same shape. We pay for self-describing tags + index; not free.
- **Decode JSONB → pssz canonical:** within 1.2× of fracpack →
  pssz on the same shape. Lock-step fast path means the per-field
  cost is ~one string compare; small overhead vs format-driven
  decode.
- **Decode JSON → pssz:** within 2× of simdjson DOM → pssz. We pay
  for SAX-style streaming + per-key index lookup.
- **Lookup schema-aware:** under 10 ns per `get<&T::field>()` for
  small structs. Three buffer reads + value decode.
- **Lookup schemaless:** under 30 ns per `view["name"]` for objects
  with up to 30 keys. SIMD prefilter + verify + value decode.
- **Wire size:** strictly ≤ JSON text on every shape, strictly <
  BSON on most shapes (per analysis in spec).

### Anti-DCE harness

Reuse the `[[gnu::noinline]]` shim pattern from
`bench_perf_report.cpp`. Both throwing and non-throwing validate
variants need shims. Lookup paths need their results escaped via
`asm volatile` so the compiler can't elide.

## CI integration

- Tests run under `ctest -j$(nproc)` as part of every CMake build.
- ASan + UBSan enabled in a separate CI matrix entry.
- Fuzz tests run with a 30-second budget per case in CI; longer
  runs done locally.
- Benchmark not run in CI by default (too noisy on shared
  runners); developers run locally and commit the `JSONB_PERF.md`
  snapshot.

## Open questions

- Should we publish the test vector corpus (golden inputs and
  outputs) as a separate package so other implementations of the
  format can verify? Currently scoped to internal psio3 use.
- libbson dependency: pulls in mongoc; might want a lighter
  reference (custom BSON walker covering the subset we benchmark).
- Real-world corpus collection: needs psitri team coordination.

## Relationship to the format spec

This plan does not change the format spec. It only verifies what
the spec already says. New requirements found during testing get
fed back to `self-describing-format.md` as spec amendments before
the corresponding test is added.
