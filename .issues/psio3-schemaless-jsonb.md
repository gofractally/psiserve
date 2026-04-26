# psio3: schemaless canonical binary format

**Status:** design in progress, 2026-04-25. Most layout decisions
settled; open questions called out at the bottom.

## Motivation

psio3 has schema-driven binary formats for schema'd tables (fracpack,
pssz, ssz, bin, borsh, bincode, avro). Each is compact because the
schema lives in the type, not in the bytes — but each requires the
reader to know `T` at compile time.

The companion case — **schemaless tables**, whose rows carry their own
structure — has no format. Existing options:

- **Postgres `jsonb`**: internal-only, no public spec. Reasonable
  reference; we are not bound by its layout.
- **MessagePack / CBOR / BSON**: transit formats. Object lookup is
  linear over key/value pairs; not designed for random key access in
  stored documents.
- **Protocol Buffers**: numeric tags, not self-describing. Cannot be
  read without the `.proto`.

This format fills the gap: a self-describing binary format scoped to
schemaless tables. Lossless JSON round-trip up to key ordering.
Co-exists with the existing schema'd formats.

## Goals

1. **Self-describing.** No external schema needed to read.
2. **Fast random field lookup.** O(log N) by key name within a
   container; constant-time index → byte-offset jump.
3. **Compact.** Minimize overhead vs. JSON text and vs. existing
   binary self-describing formats (target: smaller than JSONB on the
   wire by skipping fixed-width JEntry padding).
4. **Permits in-place modification** when the new value's encoded
   size ≤ the old. Format does not reserve slack and does not use
   indirect pointers; in-place is a property of the same-or-smaller
   overwrite case.
5. **Lossless JSON round-trip up to key order.** Generates canonical
   JSON (JCS-style, RFC 8785-aligned) efficiently. Parses canonical
   JSON in linear time; parses non-canonical JSON at an additional
   O(N log N) per object (sort step).

## Non-goals

- In-place edits for size-changing operations. (Re-encode the doc.)
- Order-preserving round-trip with arbitrary JSON sources. (Sorted
  canonical is the chosen output. Order-sensitive callers use schema'd
  formats, where declaration order is the schema's job.)
- Streaming reads. Whole-doc-fits-in-RAM, container-local offsets.
- Cross-process wire compatibility with Postgres jsonb, BSON, etc.

## Architectural framing

A table picks its mode at creation time:

| | schema'd table | schemaless table |
|---|---|---|
| type info | maintained by DB | per-row, in the bytes |
| row format | pssz / fracpack / ssz / etc. (existing) | this format |
| field access | static offsets | binary search keys + offset table |
| add a field | migration | free, per-row |
| overhead | near-zero | type tags + keys + offset table |

Schema'd is solved by existing formats. This issue is **only** about
the schemaless path. There is no hybrid mode at the row level — a
single value within a schema'd row can be a schemaless blob (the
"`jsonb` column" pattern), but the blob's bytes are still a
self-contained doc in this format.

## Type system

Aligned with JSON's six structural types plus extensions for lossless
numeric round-trip and binary data:

| tag | type | encoding |
|---|---|---|
| 0 | null | tag byte alone |
| 1 | bool_false | tag byte alone |
| 2 | bool_true | tag byte alone |
| 3 | int_inline | low 4 bits of tag = value 0..15 |
| 4 | int_varint | zigzag LEB128 follows |
| 5 | int_fixed64 | 8 raw bytes; in-place-update-friendly |
| 6 | float64 | 8 raw IEEE-754 bytes |
| 7 | decimal | variable; for u128/u256/big-decimal beyond float64 |
| 8 | string_short | low 4 bits of tag = length 0..15; UTF-8 bytes follow |
| 9 | string | varuint length follows; UTF-8 bytes |
| 10 | bytes | varuint length follows; binary blob |
| 11 | array | low 4 bits of tag = layout flags |
| 12 | object | low 4 bits of tag = layout flags |
| 13..15 | reserved | for future tags / extensions |

Tag byte = 4-bit type + 4-bit packed payload (length / value /
layout flags depending on type).

**Numeric strategy.** The writer picks a tag based on the value:

- Small unsigned ints 0..15 → `int_inline` (zero-byte payload)
- Other ints in `int64` range → `int_varint` (compact)
- Counter / mutation-prone fields → `int_fixed64` (always 8 bytes,
  in-place-safe across overwrites)
- Beyond `int64`, or non-double precision required → `decimal`

Float and decimal stay distinct on the wire so the reader does not
silently `double`-coerce a value the writer kept exact.

**Bytes type.** `bytes` is a first-class tag. JSON I/O round-trips it
as a base64-encoded string — the JSON encoder emits base64 for any
`bytes` value; the JSON decoder cannot infer "this string is actually
bytes" without an application-level convention, so a string read from
JSON becomes a `string` in the binary form. This is asymmetric but
matches BSON-style usage. (Settled.)

## Canonical sorted keys

**Decision: object keys sorted by UTF-8 byte sequence on encode.**

Rationale:
- Forced by goal 2 (binary search) and goal 5 (canonical JSON output).
- JSON spec defines objects as unordered, so no semantic loss.
- Content-addressable hashing falls out for free.
- Equality of two docs becomes byte-equality of their canonical
  encodings.
- Order-sensitive use cases belong in schema'd formats, where the
  schema's declaration order is authoritative.

Cost: parsing non-canonical JSON requires an O(N log N) sort per
object at parse time. Parsing canonical JSON is linear (the parser
asserts ordering rather than sorting). The non-canonical writer pays
the cost; the canonical pipeline does not.

## Container layout

**Object container:**

```
[tag_byte:    object | layout_flags]
[count:       varuint]
[keys area:   [varuint key_len, utf-8 bytes] × count, sorted ascending]
[offset_table: uint{1|2|4}[count]   — start offset of each value
                                       within the value payload region]
[value_tags + payloads]
```

**Array container** (no keys area):

```
[tag_byte:    array | layout_flags]
[count:       varuint]
[offset_table: uint{1|2|4}[count]]
[value_tags + payloads]
```

**Adaptive offset width.** The layout-flags nibble of the tag byte
encodes whether the offset table is `u8` / `u16` / `u32`. Most
documents fit `u16` (containers ≤ 64 KB). JSONB is locked at 28-bit
offsets always; we save 2–3 bytes per child on small containers.

**Lookup path for `obj["name"]`:**
1. Binary search the keys area for `"name"` → key index `i`.
2. Read `offset_table[i]` → byte offset within payload region.
3. Read tag byte at that offset; decode value.

Three random-access steps regardless of container size.

## Document header

```
[magic:    1 byte  marker (e.g. 0x4A = 'J')]
[version:  1 byte]
[flags:    1 byte]   bit 0 = dictionary mode, bits 1..7 reserved
[root container]     follows immediately
```

If `dictionary mode` is set, key references in object containers are
varuint key-IDs instead of inline UTF-8 strings. The dictionary
itself lives outside the doc, in table metadata.

## Key-encoding modes

- **Inline mode (default).** Keys live as UTF-8 strings in each
  container. Doc is fully self-contained.
- **Dictionary mode.** Doc carries the dictionary-mode flag; per-
  container key arrays become varuint key-IDs (sorted by ID).

The DB chooses dictionary mode when storing a row in a schemaless
table that maintains a per-table key dictionary. Standalone
payloads (RPC response, file blob, fixture) use inline mode.

**Dictionary scope: per-table.** Each schemaless table owns its
key dictionary. Cross-table sharing was considered and rejected —
maintenance overhead outweighs the rare cross-table key overlap.

## In-place modification semantics

The format does **not** reserve slack space and does **not** use
indirect pointers. Consequently:

**In-place safe** (the DB / caller can rewrite bytes in place):
- Overwriting any value with another whose encoded size ≤ the
  original.
- Bool flips, fixed-int updates, string truncation.
- Caller writes new bytes; if the new value is shorter than the
  original slot, the trailing bytes within the slot become unused
  but harmless padding. (The container's offset table is unchanged
  because the *next* value's offset still points at the next slot.)

**In-place unsafe** (requires full doc re-encode):
- Any size-growing modification: varint integer crossing a
  byte-count boundary, longer string, append to array, insert
  object key.

The DB layer can opt fields into `int_fixed64` to guarantee
in-place safety for counter-style updates.

## Canonical JSON I/O

**Generate canonical JSON from binary:**
- Walk the doc top-down. Containers emit `{` / `[`, walk children
  in storage order (already sorted), emit `}` / `]`.
- Numbers emit shortest round-trippable decimal form.
- Strings emit minimum-escape form per RFC 8259.
- `bytes` emit as base64 strings.
- Output matches RFC 8785 (JCS) modulo number-formatting choices
  to be verified.

**Parse JSON to binary:**
- Tokenize JSON in one pass.
- For each object: collect (key, value) pairs, sort by UTF-8 key
  bytes, emit object container with sorted keys + offset table +
  values.
- Canonical input: the sort step is a no-op assertion; pure linear
  pass.
- Non-canonical input: O(N log N) sort per object on parse. The
  cost is paid by the writer of non-canonical input.

## Open questions

- [ ] **Decimal payload format.** Three candidates:
  - Postgres-numeric on-disk layout (sign/dscale + base-10000 digit
    array). Battle-tested, ~6-byte minimum, complex parser.
  - Stringified-decimal (varuint length + ASCII digits). Simplest;
    trivially round-trips with JSON; slightly larger on the wire.
  - Fixed-width tags for u128/u256/i128/i256 plus a generic decimal
    tag for everything else. Best wire size for the common
    blockchain case; consumes more tag space.
- [ ] **Document size cap.** `u32` payload offsets cap a single
  container at 4 GB. Acceptable, or do we need a "huge doc" mode
  with `u64` offsets?
- [ ] **Validation depth.** Maximum nesting depth before the
  validator refuses (DoS guard)?
- [ ] **Key dictionary entry format and growth semantics.** Length-
  prefixed UTF-8 sorted by ID, append-only. Need to spec the table-
  metadata side for ID assignment, persistence, and concurrent-
  writer behavior — out of scope here, separate psitri-side issue.

## Verification plan (when implementation begins)

End-to-end checks the format must pass before declaring "ready":

- **JSON round-trip corpus.** Run the json.org test corpus + a
  representative real-world set. Canonical inputs round-trip
  byte-equal. Non-canonical inputs round-trip after a single
  canonicalization pass (encode → decode → encode is idempotent
  from the second step).
- **Random-lookup correctness.** Encode a doc with N keys, look up
  each by name; all N succeed with the correct value.
- **In-place mutation harness.** Encode a doc, mutate a field of
  same-or-smaller size, decode, verify result.
- **Fuzz validation.** Arbitrary byte buffers must not crash the
  validator. Every accepted buffer must decode without an
  exception.
- **Cross-format parity.** A value expressible in fracpack / pssz
  must round-trip semantically through this format too. No
  precision loss for `int64`, `float64`, strings; `decimal` for
  big-int.
- **Bench.** Compare `validate`, `decode`, `encode`, and
  random-key-lookup against (a) Postgres `jsonb` reference numbers
  and (b) JSON-text parse-then-lookup. Target: faster than both on
  random lookup; smaller than `jsonb` on the wire for typical docs.

## Critical files (when implementation begins, not in this plan)

- `libraries/psio3/cpp/include/psio3/<format>.hpp` — new format
  tag, encode / decode / validate / validate_or_throw tag_invokes.
  Mirrors the shape of `pssz.hpp`.
- `libraries/psio3/cpp/include/psio3/cpo.hpp` — already has the
  validate / validate_or_throw CPO pair this format will plug
  into. No changes expected.
- `libraries/psio3/cpp/include/psio3/error.hpp` — `codec_status`
  (already `unique_ptr`-based, suitable as-is).
- `libraries/psio3/cpp/include/psio3/json.hpp` — existing JSON
  encoder / decoder. The canonical-JSON I/O paths likely share its
  tokenizer.
- `libraries/psio3/cpp/include/psio3/dynamic_value.hpp` — existing
  runtime-typed value representation. The decoder may produce
  these as the natural in-memory shape for fully-dynamic readers.

## Status

Spec drafted. Open questions above need resolution before
implementation. No code changes accompany this issue.
