# psio: schemaless self-describing binary format

**Status:** design draft, 2026-04-26.

## Motivation

psio has schema-driven binary formats for schema'd tables (fracpack, pssz,
ssz, bin, borsh, bincode, avro). Each is compact because the schema lives
in the type, not in the bytes — but each requires the reader to know `T`
at compile time.

The companion case — **schemaless tables**, whose rows carry their own
structure — has no format. Postgres `jsonb` is the obvious reference for
"binary JSON suitable for database queries," but is internal-only with
no public spec. MessagePack / CBOR / BSON are transit formats with weak
random-key lookup. Protocol Buffers carry numeric tags and are not
self-describing without `.proto`.

This issue specifies a new format scoped to schemaless tables. It
co-exists with the existing schema'd formats; it does not replace them.

## Guiding principle

Reflection on structs is psio's data model. The JSON and binary forms of
this format must preserve the properties of those structs across
languages — and must not be able to express anything that doesn't map
cleanly to a struct (in particular, no duplicate keys, no anonymous
unions, no positional arguments).

## JSON is the canonical form

**The JSON form is the canonical exchange representation; the binary
form is an indexed cache of it.** This is the most important framing
decision. Concretely:

- The round-trip guarantee is *at the JSON layer*: `parse_json →
  encode_binary → decode_binary → emit_json` produces the same JSON
  (modulo whitespace and the writer's choice of equivalent encodings —
  see below).
- Two binary documents are equal iff their JSON forms are equal.
  Content-addressed hashing happens at the JSON layer, not the binary
  layer.
- The writer is free to make storage-encoding choices in the binary
  form that do *not* affect the JSON form. For example, the value
  `42` may be stored as `int_inline` (zero-byte payload, value packed
  in the tag) or as `int` with `byte_count=1` (single byte payload).
  Both decode to the JSON token `42`.
- `binary → JSON → binary` is therefore *not* required to be byte-equal
  on the binary side. Semantic equivalence is the contract.

This frees the binary form to optimize aggressively — small ints get
packed into the tag byte, the writer chooses the smallest byte-count
that holds the value, etc. — without breaking any "round-trip"
promise.

## Goals

1. **Self-describing.** No external schema needed to read.
2. **Lossless JSON round-trip** for everything the format can
   represent. `parse_json → encode → decode → emit_json` is identity.
3. **Lossless round-trip with schema'd formats via JSON.** The same
   JSON conventions used by this format are also the canonical
   exchange form for psio's schema-driven formats (pssz, fracpack,
   ssz, ...). Consequently any pssz value can round-trip through
   this format (pssz → JSON → schemaless → JSON → pssz) without
   loss, and vice versa. JSON is the single canonical waypoint.
4. **Fast random field lookup.** Sub-microsecond lookup by key name
   within a container, regardless of the field count.
5. **Compact.** Per-key overhead in the binary form should be at or
   below JSON's own per-key punctuation overhead (≈ 6 bytes), with
   adaptive offset width sizing per container.
6. **Permits in-place modification** when the new value's encoded
   size ≤ the old. Format does not reserve slack and does not use
   indirect pointers.

## Non-goals

- Order-canonical encoding. Field order in the binary form is the order
  the writer chose, mirroring the developer's struct declaration. There
  is no "sorted internal layout"; canonicalization belongs at a higher
  layer.
- In-place edits for size-changing operations. Re-encode the document.
- Streaming reads. Whole-doc-fits-in-RAM, container-local offsets.
- Cross-process wire compatibility with Postgres jsonb, BSON, etc.

## Architectural framing

A table picks its mode at creation time:

| | schema'd table | schemaless table |
|---|---|---|
| type info | maintained by DB | per-row, in the bytes |
| row format | pssz / fracpack / ssz / etc. (existing) | this format |
| field access | static offsets | hash-prefiltered key lookup |
| add a field | migration | free, per-row |
| overhead | near-zero | type tags + keys + index |

Schema'd is solved. This issue is **only** about the schemaless path.

## Size limits

A document is defined by a single root container. Limits:

| limit | value | why |
|---|---|---|
| max container size | 16 MB (24-bit `key_pos` in large mode) | well above practical needs |
| max key length | 255 bytes (8-bit `key_len`) | covers all reasonable identifiers |
| max field count per container | 65535 (16-bit `count`) | TBD; could go larger if needed |
| max nesting depth | 64 (validator-enforced) | DoS guard |

The container's offset width is **adaptive per container**, not fixed
across the document. Each container's tag byte's low nibble selects
one of three offset widths:

| mode | offset | per-key index cost | container payload cap |
|---|---|---|---|
| small | u8  | 1B hash + 1B offset + 1B key_len = 3 bytes | 256 B |
| mid   | u16 | 1B hash + 2B offset + 1B key_len = 4 bytes | 64 KB |
| large | u24 | 1B hash + 3B offset + 1B key_len = 5 bytes | 16 MB |

Small documents pay 3 bytes per key in their index; large documents
4 or 5. PG jsonb is locked at a single 28-bit offset width — every
container, large or small, pays the same 4-byte JEntry cost. Our
per-container width selection saves bytes on the small-container case
that dominates real workloads.

The 16 MB ceiling is intentional: PG's MVCC model already biases
real-world jsonb values toward sub-MB sizes (every UPDATE rewrites
the entire value, so larger-than-MB jsonb in heavily-updated rows
becomes prohibitively expensive). 16 MB is well above any
practical-and-sustainable container size; documents larger than that
should split into multiple rows. If a future workload genuinely
demands more, a u32 mode can be added behind a flag without breaking
existing readers.

## JSON ↔ binary round-trip rules

### Field order

**Decision: preserve the order in which fields appear.** The binary
form stores fields in encounter order; lookups use the hash + offset
index (below), so order is independent of access cost.

Rationale:
- Preserves the C++ struct field declaration order across the
  round-trip, which matches developer mental model.
- A higher-level canonicalization layer can sort if and when it needs
  to (e.g. for content-addressed hashing). It's not the storage
  format's job.

Rejected: sorted-on-disk. Forces a layout the user did not choose, and
breaks tools that depend on order.

### Duplicate JSON keys

**Decision: throw.** A duplicate key is malformed input. The parser
detects duplicates during ingest and reports the offending key.

Rationale: matches struct semantics — a C++ struct can't have two
fields of the same name. The other candidates fail the round-trip
guarantee or introduce surprising side effects:

- *Last wins*: arbitrary, silent.
- *Convert to array*: changes the document's type shape; later updates
  see an array where they expected a scalar.
- *Auto-rename* (e.g. `name`, `name1`, `name2`): cascading collisions
  if the real keys already use that pattern.

### Integer representation

**JSON form:** any JSON Number whose lexical form has no decimal point
and no exponent is an integer.

**Range:** any signed integer that fits in 128 bits (i.e.
−2¹²⁷ … 2¹²⁸−1, covering both `i128` and `u128` ranges). Larger
integers cause a **parse error** — the format does not silently
truncate or lose precision.

**Binary form:** the writer stores each integer in the smallest
representation that holds the value:

- `0..15` (small unsigned) → `int_inline` (zero-byte payload, value
  packed in the tag's low nibble)
- otherwise → `int` with the byte-count nibble set to the minimum
  number of bytes needed to hold the zigzag-encoded value
  (1, 2, 3, … up to 16)

**Width information is not preserved.** A C++ `u64` field with value
`42` and a C++ `i32` field with value `42` produce the same JSON
(`42`) and the same binary (`int_inline` with payload 42). On decode
into a typed C++ struct, the *target* field type tells the decoder
how to interpret the value; if the value doesn't fit, the reader
errors.

The round-trip contract is at the **value** level: any integer that
parses successfully will round-trip to the same JSON Number. Width
identity is not part of the contract — it doesn't need to be,
because reflection on the target struct re-establishes the C++
width on decode.

**Implication for the JSON parser.** Standard JSON parsers
materialize numbers as `double`, silently truncating beyond 2⁵³.
The format's parser must be precision-preserving — keeping the
literal digit string until it knows to materialize as integer (up
to 128 bits) or float. This is a standard capability of competent
libraries (RapidJSON lookahead, simdjson on-demand, etc.).

### Float / decimal representation

**JSON form:** any JSON Number whose lexical form contains a `.` or
an exponent (`1.0`, `1e10`, `3.14e-7`) is a decimal.

**Binary form:** fixed-point — a signed mantissa plus a signed
decimal scale, representing the value `mantissa × 10^scale`. This
captures every JSON Number a decimal-aware parser can produce, with
no IEEE rounding loss.

The writer normalizes to canonical form (mantissa not divisible by
10) on encode, so two semantically equal numbers produce
byte-identical binaries.

The mantissa is a zigzag varint up to 16 bytes (covering 128 bits
of significand). The scale is a single signed byte (−128..127,
covering any practical decimal exponent including the full IEEE
double range of ~10⁻³²⁴..10³⁰⁸).

**Range:** any decimal `M × 10^S` with `M` in 128-bit signed range
and `S` in 8-bit signed range. This strictly contains the IEEE
double range of finite normal values, plus subnormals down to
~10⁻³²⁴.

**Why fixed-point instead of IEEE.** JSON Numbers are
decimal-shaped strings. Storing them as IEEE binary64 introduces
rounding (`0.1 + 0.2 ≠ 0.3` etc.); storing as fixed-point preserves
the exact decimal the JSON producer wrote. Fixed-point is also
strictly smaller than IEEE for typical decimal-shaped values
(`3.14` packs into 4 bytes instead of 9), comparable in cost on
compute-light DB queries (range comparisons reduce to mantissa
comparison after normalization), and removes the entire class of
NaN / ±Inf / signed-zero edge cases that don't survive a JSON
round-trip anyway. PG's `numeric` is the same idea in a less
compact (BCD-digit-array) wrapper.

**Trade-off acknowledged.** Hardware-speed transcendental math
(`sqrt`, `log`, `sin`) is not directly supported by fixed-point;
applications needing those convert to IEEE on demand at the query
or struct boundary, just as they would when reading from a column
of PG `numeric`. The format is a JSON-equivalent storage layer, not
a numeric kernel.

**Width information is not preserved.** Typed-struct decoding picks
up the target field type from reflection, just as for integers.

**Special values.** `NaN`, `±Inf` are not representable as JSON
Numbers and are not representable in this format. JSON-driven inputs
cannot produce them; reflection-driven inputs from C++ `double`
fields containing such values are a write-time error.

### Key-suffix encoding hints

The format defines a small registry of recognized key suffixes, each
introduced by a `.` separator: `keyname.tag`. A recognized suffix is
an *encoding hint* that lets the binary form store the value more
compactly than its JSON appearance would suggest.

**Recognized suffixes are an optimization, not a part of the data
model.** Round-trip is preserved either way:

- *Recognized* — e.g. `"payload.b64": "SGVsbG8="`. The JSON parser
  decodes the base64 and stores the value with the binary `bytes`
  tag. The binary form's per-key index stores `payload` (without the
  suffix); the value's tag records the encoding. On JSON emit, the
  encoder reattaches `.b64` to the key and base64-encodes the bytes
  back. Net effect: smaller binary representation for the same JSON.
- *Unrecognized or absent* — e.g. `"foo.bar": "..."` or
  `"hostname": "example.com"`. The parser treats the entire string
  as the key name and stores the value as a normal `string`. On
  emit, the same key comes back unchanged. The dot is not special;
  the format never rejects a key for containing one.

The suffix is purely a hint to the binary encoder. There is no
reserved character, no escape mechanism, no incompatibility with
arbitrary JSON.

**Why a `.` separator.** It's not legal in C++ identifiers, so a
reflection-driven encoder writing `mydata.b64` from a struct field
named `mydata` cannot collide with another struct field literally
named `mydata.b64`. The reflected encoder always knows the suffix
is its own work, and the JSON ingest direction is unambiguous: a
recognized suffix triggers the optimization; anything else is a
plain key.

### Suffix vocabulary follows psio3's presentation tags

Suffix names are not invented for this format — they mirror psio3's
existing `as<Tag>` presentation-tag vocabulary defined in
`annotate.hpp`. A field annotated `as<hex_tag>` round-trips through
JSON with `.hex` on its key. A field annotated `as<rfc3339_tag>`
gets `.rfc3339`. The format and the reflection layer share one
vocabulary.

This means the schema-aware encoder works directly: walk the struct,
read each field's `as<Tag>` annotation, attach the tag's name as the
suffix. No mapping table, no parallel registry.

### Restated principle

Schema → JSON → schema is lossless **because the schema knows the
type**. Anything beyond the raw value in the JSON form is
pretty-printing for human readers, schemaless consumers, or tools
that need to interpret without access to the schema. The suffix is
the carrier for that pretty-printing.

### Default (no-suffix) types

C++ types whose JSON form is unambiguous; encoder emits them without
any suffix:

| C++ type | JSON form | binary tag |
|---|---|---|
| `bool` | `true` / `false` | `bool_true` / `bool_false` |
| signed/unsigned integers up to 64-bit | JSON Number (integer form) | `int_inline` or `int` |
| `int128`, `uint128` | JSON Number, long-digit form | `int` |
| `float`, `double` | JSON Number with `.` or exponent | `decimal` |
| `std::string`, `std::string_view` | JSON string | `string` |
| `std::optional<T>` | T or `null` | (T's tag) or `null` |
| `std::vector<T>` (T not byte) | JSON array | `array` |
| `std::array<T, N>` (T not byte) | JSON array | `array` |
| reflected struct | JSON object | `object` |
| enum | JSON string (the enum name) | `string` |

These cover the bulk of typical struct fields. No `as<Tag>`
annotation, no suffix on the JSON key.

### v1 suffix registry — psio3 presentation tags

| suffix | psio3 tag (annotate.hpp) | meaning | typical JSON value | binary tag |
|---|---|---|---|---|
| `.hex` | `hex_tag` | lowercase hex string | `"deadbeef"` | `bytes` (or `string` if writer prefers) |
| `.base58` | `base58_tag` | base58 string | `"3MNQE1Y9JaXKi8..."` | `bytes` |
| `.base64` | `base64_tag` *(new — to add)* | base64 string, RFC 4648 §4 | `"3q2+7w=="` | `bytes` |
| `.decimal` | `decimal_tag` | decimal-digit string for arbitrary-precision integer | `"123456789012345678901234"` | `int` (when in range) or `string` |
| `.rfc3339` | `rfc3339_tag` | RFC 3339 / ISO 8601 timestamp or date-only string | `"2026-04-26T14:30:00Z"`, `"2026-04-26"` | `int` (ns since epoch) |
| `.unix_us` | `unix_us_tag` | microseconds since Unix epoch | `1745683800000000` | `int` |
| `.unix_ms` | `unix_ms_tag` | milliseconds since Unix epoch | `1745683800000` | `int` |

**`.base64` is the only suffix that requires an addition to psio3.**
Currently psio3 has `hex_tag`, `base58_tag`, `decimal_tag`,
`rfc3339_tag`, `unix_us_tag`, `unix_ms_tag`. Adding `base64_tag`
keeps the JSON convention and the `as<Tag>` reflection
vocabulary in sync.

UUIDs, calendar dates, and durations are *not* separate suffixes.
They live within the existing tags:

- A UUID field annotated `as<hex_tag>` emits as
  `{"id.hex": "550e8400e29b41d4a716446655440000"}` (32 hex chars,
  no hyphens). If the schema author wants the hyphenated form, they
  define a `uuid_tag` and a corresponding `adapter<T, uuid_tag>` —
  the format absorbs it automatically.
- Calendar dates use `as<rfc3339_tag>`; RFC 3339 allows date-only
  form `"2026-04-26"`.
- Durations don't have a tag yet; they'd be added the same way.

The set is open. New `*_tag` types added to psio3 — by the library
or by user code — automatically extend the JSON suffix vocabulary
without spec amendments. The format simply stores the suffix as part
of the key; recognized vs unrecognized is a reader-side concern.

### Custom presentation tags

Users defining their own `psio3::adapter<T, MyTag>` specialization
gain a custom `.my_tag` suffix automatically. Example: a `Hash32`
type with `adapter<Hash32, hash32_hex_tag>` emits as
`{"checksum.hash32_hex": "..."}` in JSON, with the value being
whatever string form the adapter produces.

This is the principal extensibility point: type authors register
their type's text representations once, in one place, and the JSON
convention picks them up.

### Storage of the suffix in the binary form

**The full JSON key, including any suffix, is stored verbatim in the
binary container's key area.**

For `.b64` / `.hex` the suffix is technically redundant with the
`bytes` binary tag — but the alternative (strip suffix on encode,
re-attach on emit) fails for the ambiguous case where two suffixes
map to the same binary tag (`.b64` and `.hex` both → `bytes`; `.uuid`
also → `bytes`(16); a generic 16-byte binary also → `bytes`(16)).
Keeping the suffix in the key disambiguates without spending tag
slots, and costs only a few bytes per non-default field.

The result: round-trip is byte-equal on the JSON key (suffix included),
schema-aware decoders strip recognized suffixes when matching to C++
field names, and the format stays open to new suffixes without
needing new binary tags.

**Round-trip example.**

A reflected pssz struct with several `as<Tag>` annotations:

```cpp
struct Order {
    Hash32                          id;             // adapter<Hash32, hex_tag>
    std::string                     customer_name;
    PSIO3_AS(decimal_tag)
    psio3::uint128                  total_cents;    // big-int as decimal string
    PSIO3_AS(rfc3339_tag)
    std::uint64_t                   created_at;     // ns since epoch, displayed RFC 3339
    PSIO3_AS(base64_tag)
    std::vector<std::uint8_t>       signature;
};
```

JSON form (emitted by reflection-driven encoder):

```json
{
  "id.hex": "0123456789abcdef0123456789abcdef01234567",
  "customer_name": "Alice",
  "total_cents.decimal": "123456789012345678901234",
  "created_at.rfc3339": "2026-04-26T14:30:00Z",
  "signature.base64": "MEQCIH..."
}
```

Schemaless binary form stores keys verbatim (`"id.hex"`,
`"total_cents.decimal"`, etc.). Values use the binary tag dictated
by the suffix:

| key | binary tag | rationale |
|---|---|---|
| `id.hex` | `bytes` (raw 20) | hex-string is base16 binary; raw bytes are smaller |
| `customer_name` | `string` | default |
| `total_cents.decimal` | `int` (16-byte mantissa) | within 128-bit range, store as raw int |
| `created_at.rfc3339` | `int` (8-byte) | ns-since-epoch as int; suffix recovers display form |
| `signature.base64` | `bytes` | base64-string is binary; raw bytes are smaller |

### I/O direction: source streams, sink looks up

In any pairwise conversion, the source is iterated sequentially and
the sink is filled (or emitted into) via random-access lookup. This
matters most for JSON, which is fundamentally streaming — random
access into JSON requires building a tree first or repeatedly
re-parsing.

| direction | source (sequentially walked) | sink (indexed) |
|---|---|---|
| pssz → JSON | struct (reflection in declaration order) | JSON output stream |
| JSON → pssz | JSON parser stream | struct (compile-time field-name index) |
| schemaless → JSON | binary container walk | JSON output stream |
| JSON → schemaless | JSON parser stream | container builder (hash + offset arrays grown incrementally) |
| pssz → schemaless | struct walk | container builder |
| schemaless → pssz | binary container walk | struct (compile-time index) |

For JSON-as-source specifically: the parser is single-pass / SAX-
style. Each emitted key event triggers a lookup into the target.

The reverse pattern — walking the struct first and looking up keys
in the JSON — would require either a full parse-to-tree (memory +
time) or N re-scans of the JSON (quadratic). JSON-driven streaming
with fast lookup on the target is the only design that scales.

### Canonical-first parse: lock-step fast path

For schema-driven targets (pssz / fracpack / ssz), **canonical JSON
ordering is the C++ struct's declaration order**. Any JSON produced
by a psio encoder is canonical-for-its-struct by construction, so
round-tripped JSON lands on a fast path that avoids the index lookup
entirely.

The decoder runs in two stages:

1. **Lock-step compare (fast).** Walk the JSON keys and the struct
   field iterator in parallel. For each JSON key, compare against
   the next field's expected JSON name (with any `.<tag>` suffix
   already attached). On match, decode into the field, advance both
   pointers. One byte-compare per field — no hash, no index.
2. **Index fallback (sticky).** On the first mismatch, latch into
   the index-driven path: each subsequent JSON key is looked up in
   the compile-time field-name index. Once latched, never retry
   lock-step.

Sketch:

```cpp
template <typename T>
T parse_json_to_pssz(std::string_view json) {
    T out{};
    sax_parser p(json);
    p.expect_object_open();

    auto field_it = reflect<T>::fields_begin();
    bool canonical = true;

    while (auto key = p.next_key()) {
        if (canonical && field_it != reflect<T>::fields_end()
            && key.value == field_it->json_name()) {
            // Lock-step hit: one string compare per field.
            field_it->decode(p.value_stream(), out);
            ++field_it;
        } else {
            canonical = false;  // sticky
            if (auto* f = reflect<T>::lookup_field(key.value))
                f->decode(p.value_stream(), out);
            else
                p.skip_value();
        }
    }
    p.expect_object_close();
    return out;
}
```

Why this is a clean optimization:

- Encoders always emit declaration-order, so the canonical case is
  the common case for any pssz↔JSON↔pssz round-trip.
- One byte-compare beats a hash lookup, even though both are O(1).
  Compile-time field-name strings sit in rodata adjacent to the
  iterator state; the prefetcher loves it.
- For non-canonical inputs (browser-emitted JSON, hand-edited
  fixtures, foreign serializers) we pay one wasted string compare
  on the first miss, then proceed at full index speed. Tiny ceiling
  on the cost of not being canonical.

Forward-compat falls out:

- JSON has fewer keys than struct (older writer, newer schema):
  lock-step matches present keys; missing fields stay
  default-initialized.
- JSON has extra keys not in struct (newer writer, older schema):
  fallback's lookup fails for extras; they're skipped.
- JSON keys reordered: first reorder triggers fallback; rest of the
  parse uses the index.

### The compile-time field-name index

The fallback's lookup is a compile-time perfect hash (or sorted
binary search, or trie — implementation choice) over the reflected
field names. The index entries include both `field_name` and
`field_name.<tag>` variants, so a JSON key with a known suffix lands
directly on the right field without a strip-and-retry step.

For schemaless targets, the corresponding "lookup" is the
container builder appending to its own incrementally-built hash +
offset arrays — no canonical fast path applies (the schemaless
binary preserves whatever order arrived).

### Compile-time constants enable single-pass encoding

Reflected struct field names — including any `.<tag>` suffix from
`as<Tag>` annotations — are `constexpr` strings, addressable in
rodata. From this every per-field encoding constant follows:

- Full JSON key string (concatenated `field_name + ".tag"`):
  constexpr `string_view`.
- Length of each key: constexpr `size_t`.
- 8-bit hash of each key (used in the JSONB container index):
  constexpr `uint8_t`.
- Punctuation byte count for a JSON object of N fields: constexpr.
- Sum-of-key-lengths: constexpr.

The encode-side packsize calculation reduces to a constexpr fold
plus a runtime sum over variable-sized values. Same shape as
psio3's existing `size_of_v` walkers in pssz / frac / ssz:

```cpp
template <typename T>
constexpr size_t json_packsize_fixed = /* folded sum of
    "<key>":,  punctuation per field
    + outer { } */;

size_t json_packsize(const T& v) {
    size_t total = json_packsize_fixed<T>;  // compile-time constant
    // Add runtime variable contribution per field
    reflect<T>::for_each_field([&](auto& f) {
        total += variable_value_size(v.*f.ptr);
    });
    return total;
}
```

The same fold applies to schemaless-binary packsize: the per-key
index contribution (hash byte + offset byte + key_len byte) is
constexpr, the key bytes themselves are constexpr, only the value
payloads need runtime sizing.

`pssz → JSON` and `pssz → schemaless-binary` therefore both run as:

1. Compute total size = `fixed_constant + sum_variable_runtime`.
2. Resize output buffer to total size (single allocation).
3. Walk struct in declaration order, memcpy each key from rodata,
   encode each value via its `as<Tag>` adapter.

No realloc, no produce-then-measure, no per-call string
construction. The 8-bit hashes for the schemaless-binary container
index are written via one memcpy from rodata of all hashes
concatenated.

For the JSON-driven decode path, the lock-step fast path's
`key.value == field_it->json_name()` is a comparison against a
constexpr string of known length — the compiler emits unrolled
byte compares (or a single 8/16-byte aligned load + compare for
short keys) rather than a runtime strcmp.

### Round-trips established by this convention

| from | to | mechanism |
|---|---|---|
| pssz | JSON | walk struct in declaration order; for each field, read `as<Tag>`, attach `.tag` suffix, emit value via `adapter<T, Tag>` text encoder |
| JSON | pssz | stream JSON, look up each key in the struct's field-name-index (with-or-without-suffix), decode the value per the matched field's `as<Tag>` |
| schemaless | JSON | walk binary container; emit each key verbatim, render each value per its binary tag |
| JSON | schemaless | stream JSON, append each key (with suffix) and value to the container builder; recognized suffix picks the compact binary tag |
| pssz | schemaless | via JSON |
| schemaless | pssz | via JSON |

The JSON form is the canonical waypoint. Any pair of formats that
can round-trip to and from JSON can round-trip through each other,
because the suffix vocabulary is shared.

### Binary blobs

JSON has no "binary" type. Everything appears as a string. The binary
form distinguishes `string` from `bytes` via the `.b64` (or `.hex`)
key-suffix hint described above:

- A JSON key without a recognized binary suffix → binary form uses
  the `string` tag.
- A JSON key with `.b64` (or `.hex`) → binary form uses the `bytes`
  tag, value is base64-decoded (or hex-decoded) at parse time.

Both round-trip correctly to the same JSON. The suffix exists so the
binary form can avoid storing the base64 expansion of binary data
when the encoder knows the value is genuinely binary — typically from
reflection over a C++ field of `std::vector<uint8_t>` or similar.

When a JSON document arrives without any binary suffix on its keys,
all string values are stored as `string`. The encoder never guesses
whether a value "looks like base64"; intent is always carried by an
explicit suffix on the key, or absent entirely.

## Lookup acceleration: hash + offset index

For each container we encode a per-field index designed for SIMD-aided
random lookup. The order of fields in the index matches the order of
fields in the payload (preserving JSON insertion order). The index's
offset width comes from the container's tag byte (small / mid / large
mode):

```c
struct container_index {                        // OffT = u8 | u16 | u32
    uint16_t count;                              // number of fields
    uint8_t  key_hash[count];                    // 8-bit hash of each key
    OffT     key_pos[count + 1];                 // start of key i within payload
    uint8_t  key_len[count];                     // key length in bytes (≤ 255)
};
```

`key_pos[i]` is the byte offset of key `i`'s string within the
container's payload region. The sentinel `key_pos[count]` points one
byte past the last value, giving the final value's length.

Per-key index cost (1B hash + sizeof(OffT) for the `key_pos` slot +
1B key_len): 3 / 4 / 5 bytes in small / mid / large mode respectively.
The sentinel adds one more `OffT` per container (1 / 2 / 3 bytes).

The value bytes for key `i` follow the key bytes immediately:

```
value_pos = key_pos[i] + key_len[i]
value_len = key_pos[i+1] - key_pos[i] - key_len[i]
```

Lookup of name `K`:

1. Compute the 8-bit hash of `K`.
2. SIMD-scan `key_hash[]` for matching bytes (one `_mm_cmpeq_epi8`
   covers 16 entries; AVX2 covers 32; AVX-512 covers 64).
3. For each candidate index `i`, verify the actual key bytes by
   comparing `K` to `payload[key_pos[i] .. key_pos[i] + key_len[i]]`.
4. On a match, value bytes start at `key_pos[i] + key_len[i]`, with
   length `key_pos[i+1] - key_pos[i] - key_len[i]`.

For typical containers (5–30 fields), this is faster than binary search
and works on unsorted keys. The 8-bit hash gives ~1/256 false-positive
rate per scan — collisions trigger a string compare, no correctness
issue.

Sort-order recovery (when needed for canonicalization at a higher
layer) is a permutation derived by sorting `key_offset[i]` entries by
their key bytes — done once, externally. The on-disk format never
stores a sorted permutation.

## Wire format

### Document header

```
[magic:    1 byte  e.g. 0x4A]
[version:  1 byte]
[flags:    1 byte]   bit 0 = dictionary mode; bits 1..7 reserved
[root container]    follows immediately
```

If `dictionary mode` is set, key strings in container indexes are
replaced by `varuint` key IDs referencing an external (table-level) key
dictionary. Standalone payloads (RPC, fixture, file blob) use inline
keys.

### Type tags

13 types fit in 4 bits:

| tag | type | payload |
|---|---|---|
| 0 | null | none |
| 1 | bool_false | none |
| 2 | bool_true | none |
| 3 | int_inline | low 4 bits of tag = unsigned value 0..15 |
| 4 | int | low 4 bits of tag = byte count 1..16; zigzag bytes follow |
| 5 | decimal | low 4 bits = mantissa byte count 1..16; zigzag mantissa + 1 signed scale byte |
| 6 | reserved | future `ieee_float` (for bit-exact double round-trip when needed) |
| 7 | reserved | future `bigint` (for >128-bit integers) or other |
| 8 | string_short | low 4 bits of tag = length 0..15; UTF-8 bytes follow |
| 9 | string | varuint length follows; UTF-8 bytes |
| 10 | bytes | varuint length follows; binary blob |
| 11 | array | low 4 bits = layout flags |
| 12 | object | low 4 bits = layout flags |
| 13..15 | reserved | future tags / extensions |

The 4-bit type field plus the 4-bit packed payload nibble yields a
1-byte tag for null / small int / short string / bool, and a 2+ byte
tag for larger values. The integer-byte-count nibble of `int` covers
1..16 bytes — i.e. all integer values up to 128 bits — without
preserving any width identity beyond "this many bytes are needed to
hold the value." The same nibble on `decimal` covers up to a 128-bit
mantissa.

### Per-field overhead

Per-key index cost depends on the container's offset-width mode:

| element | small (u8) | mid (u16) | large (u24) |
|---|---|---|---|
| key_hash entry | 1 | 1 | 1 |
| key_pos entry | 1 | 2 | 3 |
| key_len entry | 1 | 1 | 1 |
| value tag (1 byte; payload separate) | 1 | 1 | 1 |
| **total per field** | **4** | **5** | **6** |

Compared to JSON: `"key":"value",` has ≈ 6 bytes of punctuation per
key (`""`, `:`, `""`, `,`). Our binary form beats JSON's punctuation
in small and mid modes (4–5 bytes vs 6) and matches it in large mode
(6 vs 6) — and our bytes are doing real work (hash filter, random
access) instead of being syntactic noise.

PG jsonb has ~11 bytes of structural overhead per pair: two 4-byte
JEntries plus average ~3 bytes of 4-byte-alignment padding. Our
adaptive width beats that in every mode by 5–7 bytes per pair, and
PG also can't avoid the universal `numeric` overhead (~6–8 bytes for
even small integers) that we save with `int_inline`.

### Root container

A 1-byte container tag (`{` or `[`, plus low-nibble offset-width mode)
introduces the root. Array containers omit `key_hash` and `key_len` —
entries are positional. The array offset table is a single
`OffT[count+1]` of payload positions; element length =
`offset[i+1] - offset[i]`.

## Zero-copy reader

The format is laid out so a reader can answer queries directly
against a `std::span<const std::byte>` view of the buffer — no
allocation, no copying, no intermediate parse-to-tree.

Two reader modes share the same buffer layout:

- **Schema-aware** (`jsonb_view<T>` on a buffer that has been
  validated against `T`) — the reader knows the C++ struct type
  via reflection. Field access reduces to pure offset arithmetic;
  the hash array and key strings are not read.
- **Schemaless** — the reader only knows it's a self-describing
  document and looks up fields by string key. The hash + offset
  index is essential here.

What makes both work:

- **Strings** stored verbatim as UTF-8. A `string_view` pointing
  into the buffer *is* the value — no JSON unescaping at the storage
  layer.
- **Bytes** stored raw, never base64-expanded. A
  `span<const byte>` pointing into the buffer is the value.
- **Integers** are zigzag varint or `int_inline`-packed; reading is
  a few-byte decode into a local. Endian-neutral by encoding.
- **Decimals** are mantissa varint + signed scale byte, decoded in
  place. A `decimal_view{mantissa_ptr, mantissa_len, scale}` holds
  pointers into the buffer.
- **Sub-containers** addressable at known byte offsets. Constructing
  a child view is pointer arithmetic — no rounding, no padding (no
  INTALIGN as in PG).

### Schema-aware reader: pure offset arithmetic

When the reader knows `T` and the buffer has been validated against
`T`, almost everything in the container header is redundant:

- The **count** matches `reflect<T>::field_count` — validated, never
  re-read.
- The **hash array** is for string-keyed lookup. Schema-aware access
  knows the field index directly; hash array isn't read.
- The **key_len array** matches each field's compile-time name length —
  validated, never re-read.
- The **key bytes** match the compile-time name strings (with any
  `.<tag>` suffix) — validated, never re-read.

What remains as runtime data:

- The **offset_width nibble** in the tag byte (varies per container).
- The **offset_table[i]** entry for the field we want (varies per
  document).
- The **value tag and payload** at the resulting position.

Field access is three buffer reads — no view-construction or
index-parse step. `PSIO3_REFLECT` generates a per-field accessor
method on `jsonb_view<T>` named after each field, so the call site
reads naturally:

```cpp
template <typename T>
class jsonb_view {
    std::span<const std::byte> bytes;       // that's it — no cached pointers
public:
    // Field accessors are generated by PSIO3_REFLECT.
    // For PSIO3_REFLECT(Order, id, customer, balance), the macro emits:
    //   auto id()       const noexcept;     // returns jsonb_view<Hash32> or value
    //   auto customer() const noexcept;     // returns jsonb_view<Customer>
    //   auto balance()  const noexcept;     // returns std::uint64_t
    //
    // Each accessor expands to:
    //   constexpr size_t i = /* field index, compile-time */;
    //   const uint8_t  mode  = uint8_t(bytes[0]) & 0xF;        // 1 byte read
    //   const size_t   off_w = mode_to_offset_width(mode);
    //   constexpr size_t hdr  = container_header_size_for<T>;  // constexpr
    //   const size_t   pos   = read_unaligned_offset(           // 1–3 byte read
    //                              bytes.data() + hdr + i * off_w, off_w);
    //   return decode_value<field_type<T, i>>(bytes, pos);
};
```

Three memory reads (mode byte, offset entry, value tag) per field,
inlined into the accessor. No SIMD, no hash, no string compare.
Sub-objects recurse identically — `view.customer().address().city()`
is six reads + three subspans, reaching the `string_view` for the
city.

This is the moral equivalent of `pssz_view<T>` for fully-fixed
records: validate once, then every field access collapses to known
offsets. The schemaless container's offset_table is the
"where does field i live" map; the schema fills in everything else.

### Schemaless reader shape

```cpp
class jsonb_object_view {
    std::span<const std::byte> bytes;
    // Parsed once on construction:
    uint16_t                   count;
    uint8_t                    off_w;     // 1, 2, or 3
    const uint8_t*             key_hash;  // pointer into bytes
    const std::byte*           key_pos;   // stride = off_w
    const uint8_t*             key_len;
    const std::byte*           payload;

public:
    // O(1) lookup: SIMD prefilter + string-compare verify + pointer arithmetic.
    std::optional<jsonb_value_view> operator[](std::string_view key) const;

    // Iteration yields sub-views, also zero-copy.
    iterator begin() const;
    iterator end()   const;

    size_t size() const noexcept { return count; }
};

class jsonb_value_view {
    std::byte                  tag;
    std::span<const std::byte> payload;

public:
    bool is_string() const;
    std::string_view           as_string()  const;  // slice into the buffer
    std::span<const std::byte> as_bytes()   const;  // slice into the buffer
    int128                     as_int()     const;  // small decode, no alloc
    decimal_view               as_decimal() const;  // pointer + scale
    jsonb_object_view          as_object()  const;  // sub-view
    jsonb_array_view           as_array()   const;
};
```

### End-to-end lookup

`view["user"]["address"]["city"]` against a deeply-nested object,
where `city` is a string:

1. **Hash the literal** at compile time (or once at view-construction
   time) → 8-bit target.
2. **SIMD prefilter** the top container's `key_hash[]` for the target
   byte. One register comparison covers 16–64 entries.
3. **String-compare verify** on the candidate index:
   `memcmp(payload + key_pos[i], "user", 4) == 0`.
4. **Construct child view** at `payload + key_pos[i] + key_len[i]`.
5. Recurse for `"address"`, then `"city"`.
6. **Final value**: `string_view(payload + value_pos, value_len)` —
   direct slice into the source buffer.

Three hash probes, three string-compare verifies, three pointer
constructions. No allocations. No copies. The returned `string_view`
is valid for the lifetime of the source buffer.

### What this enables

- **Schema-aware storage-layer queries** (the common path):

  ```cpp
  auto row = txn.get(key);                       // span<const byte>
  jsonb_view<Order> view{row};
  return view.balance();                         // 3 reads, no alloc, no parse
  ```

- **Schemaless storage-layer queries** by string path:

  ```cpp
  auto row = txn.get(key);
  jsonb_object_view doc{row};
  if (auto v = doc["user"]["balance"]; v && v->is_int())
      return v->as_int();                        // 2 hash probes + 2 verifies
  ```

- **Path-based extracted-column indexes.** Walking a view to extract
  a single field's bytes for index population is allocation-free.
- **JSON streaming output.** Walk the view, emit JSON tokens; the
  output side needs quoting / base64-encoding work, but the source
  side is zero-copy.

### What's not zero-copy

- **JSON re-emission output buffer.** Output strings need quoting
  and bytes need base64-encoding; that's freshly composed. Walking
  the source for emission is zero-copy.
- **Decoding into a typed C++ struct via reflection.** Materializes
  the C++ value, including string / vector copies. A
  `pssz_view<T, jsonb_buffer>` typed-view-over-schemaless option
  could provide zero-copy at this layer too — out of scope for v1.
- **Mutation.** Edits beyond same-size overwrites require
  re-encoding into a new buffer. The view is read-only.

### Path-keyed access

`PSIO3_REFLECT` generates one method per struct field on the view,
named after the field. To keep that name space pristine, any
library-level operation that *isn't* a field accessor is a **free
function**, not a member. A struct field named `get` or `at` or
`size` must continue to dispatch to the user's reflected field —
not get shadowed by a library helper.

The path-keyed getter is therefore a free function:

```cpp
// Schema-aware — return type derived from the schema's leaf type
jsonb_view<Account> acct{buffer};
auto city    = psio3::at<"/user/addresses/2/city">(acct);  // std::string_view
auto balance = psio3::at<"/user/balance">(acct);            // std::uint64_t

// Schemaless — same call, dynamic result
bjson_view obj{buffer};
auto v = psio3::at<"/user/addresses/2/city">(obj);          // bjson_value_view
```

**Path syntax: JSON Pointer (RFC 6901).** `/` separates segments;
numeric segments index into arrays; `~1` and `~0` escape literal
`/` and `~` in keys. Optional `[N]` shorthand for array indices is
accepted (`/member/submember[5]` ≡ `/member/submember/5`).

**Compile-time parsed.** The path is a string-literal NTTP. A
`consteval` parser splits it once at compile time. For
schema-aware access, each segment is validated against
`reflect<T>` — typoed paths fail to compile with the offending
segment named in the diagnostic. The result type is derived: walk
the schema to figure out what `T` lives at the leaf path.

**Generated walker is identical to the hand-chained version.**
For `psio3::at<"/user/addresses/2/city">(acct)` against the
schema-aware view, the walker the compiler emits is:

```cpp
acct.user().addresses()[2].city()
```

— four accessor calls (each a `PSIO3_REFLECT`-generated method),
twelve-ish buffer reads, no allocation, no runtime parsing. The
compile-time path resolution is a pure desugaring; same
instructions either way.

For schemaless access, each segment becomes a hash-prefilter
lookup whose target hash is precomputed at compile time
(constexpr from the literal path string). Numeric segments become
direct offset_table index reads.

**Bulk extraction.** Multiple paths at once compile to one walker
that can share sub-walks where paths overlap:

```cpp
auto [city, country, balance] = psio3::at<
    "/user/addresses/2/city",
    "/user/addresses/2/country",
    "/user/balance"
>(acct);
```

The first two paths share the prefix `/user/addresses/2`; the
walker descends to the shared point once and reads both leaves.

**Use cases.** Path-keyed access is the natural API when:

- The path comes from configuration / a GraphQL field path /
  a JSONPath query.
- Code wants to extract a single field deep in a document
  without writing a chain of method calls.
- Schemaless code wants a uniform syntax that works regardless
  of nesting depth.

### Convention: free functions vs member methods

To preserve the field-namespace invariant, the format and view
APIs follow this rule:

- **Member methods on `jsonb_view<T>` and `bjson_view`** are
  *only* the per-field accessors generated by `PSIO3_REFLECT` —
  one method per field, named exactly after the field. (Plus
  reserved-prefix internals like `_psio3_*` if needed.)
- **Library-level operations** — path-getter, encode, decode,
  validate, size_of, iterate — are free functions in the
  `psio3::` namespace, taking a view as an argument.

This means a struct field named `at`, `get`, `size`, `data`, or
any other common-library identifier still resolves to the user's
field, not a library helper. The trade-off: slightly more verbose
call sites (`psio3::at(view, ...)` instead of `view.at(...)`),
in exchange for unambiguous reflection.

## In-place modification semantics

The format does **not** reserve slack space and does **not** use
indirect pointers. Consequently:

**In-place safe** (the DB / caller can rewrite bytes in place):

- Overwriting any value with another whose encoded size ≤ the original.
- Bool flips, fixed-int updates, string truncation.

**In-place unsafe** (requires a full doc re-encode):

- Any size-growing modification: integer crossing a byte-count
  boundary, longer string, append to array, insert object key.

A field can be opted into the largest-width int tag (e.g. `int` with
byte_count = 8) at encode time to guarantee in-place safety for
counter-style updates.

## Compiled queries

The `key_hash` index supports a "compiled query" optimization: pre-hash
the query's literal key strings at compile time, dispatch each lookup
directly through the SIMD scan. The query's compiled form is a sequence
of `(key_hash, key_string)` pairs; key_string is retained for collision
verification.

Conversion of compiled queries back to source form requires a
key-name dictionary built from the schema or schemaless dict. Same
mechanism as the per-table dictionary above.

## Open questions

- Whether the 16 MB large-mode cap proves too tight in practice.
  Real-world PG jsonb is bounded by MVCC update costs to roughly
  the same range, so 16 MB should be ample. If we ever want larger,
  a u32 mode can be added behind a flag without breaking existing
  readers.
- Maximum field count per container. 65535 from the proposed `uint16_t
  count` is plenty for normal use; larger should fall back to
  splitting the doc.
- Hash function for `key_hash`. Need a stable, fast 8-bit hash with
  good distribution over typical identifiers. xxh3 truncated to 8
  bits, or a simpler FNV-1a / Murmur variant?
- Key-dictionary entry format and growth semantics — separate
  psitri-side issue (the DB layer manages dictionary state).
- Beyond-128-bit integers. The current rule rejects integers
  outside `[−2¹²⁷, 2¹²⁸)` at parse time. If a use case demands
  arbitrary-precision integers (e.g. cryptographic primitives that
  go beyond u256), we'd need to add a `bigint` tag with variable-
  length payload. Not in v1.
- `NaN` / `±Inf` from C++ `double` fields. The format does not
  represent them (decimal can't, JSON Number can't). For v1 these
  are write-time errors; the encoder rejects the value. If a future
  use case needs them, an `ieee_float` tag (reserved at slot 6)
  could store the bit pattern and round-trip through a `.tag` like
  `.f64`.
- Initial `.tag` registry. v1 must specify the canonical lowercase
  identifiers and their semantics (`b64`, `hex`, `uuid`,
  `iso8601`, ...). The base set should be small; the namespace is
  open for future extensions but each addition needs a spec
  amendment.

## Verification plan

End-to-end checks the format must pass before declaring "ready":

- **JSON round-trip corpus.** Run the json.org test corpus + a real-
  world set. For inputs that the format can represent: `parse →
  encode → decode → emit_json` produces the same JSON tokens
  (modulo whitespace).
- **Random-lookup correctness.** Encode a doc with N keys, look up
  each by name; all N succeed with the correct value.
- **In-place mutation harness.** Encode a doc, mutate a field of
  same-or-smaller size, decode, verify result.
- **Fuzz validation.** Arbitrary byte buffers must not crash the
  validator. Every accepted buffer must decode without an exception.
- **Cross-format parity.** A value expressible in fracpack / pssz
  must round-trip semantically through this format. No precision
  loss for any integer in `[−2¹²⁷, 2¹²⁸)`, no precision loss for
  any decimal `M × 10^S` with `M` in 128-bit signed range and `S`
  in 8-bit signed range (this strictly contains all `float64`
  decimal-text representations), no string mangling. Width identity
  is not preserved (by design) — typed-struct decode re-establishes
  the C++ width via reflection.
- **Bench.** Compare validate / decode / encode / random-key-lookup
  against (a) Postgres `jsonb` reference numbers and (b) JSON-text
  parse-then-lookup. Target: faster than both on random lookup;
  smaller than `jsonb` on the wire for typical docs.

## Implementation notes (for follow-up)

When implementation begins, the relevant entry points are:

- `libraries/psio3/cpp/include/psio3/<format>.hpp` — new format tag,
  encode / decode / validate / validate_or_throw `tag_invoke`s.
  Mirrors the shape of `pssz.hpp`.
- `libraries/psio3/cpp/include/psio3/cpo.hpp` — already has the
  validate / validate_or_throw CPO pair this format will plug into.
- `libraries/psio3/cpp/include/psio3/error.hpp` — `codec_status`
  (`unique_ptr`-based, suitable as-is).
- `libraries/psio3/cpp/include/psio3/json.hpp` — existing JSON
  encoder / decoder. The canonical-JSON I/O paths likely share its
  tokenizer.
- `libraries/psio3/cpp/include/psio3/dynamic_value.hpp` — existing
  runtime-typed value representation. The decoder may produce these
  as the natural in-memory shape for fully-dynamic readers.
