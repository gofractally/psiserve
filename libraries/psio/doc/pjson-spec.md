# pjson — Schemaless Self-Describing Binary Format

**Status:** v1 (stable). Reference implementation in
`libraries/psio/cpp/include/psio/pjson*.hpp`. This document is the
normative specification.

**Audience:** anyone implementing a pjson parser, encoder, or
interoperable consumer in another language.

---

## 1. Purpose and Position

pjson is a binary serialization format for **schemaless data** —
documents whose structure is not known to the consumer at compile
time. It is the binary peer of JSON: it can losslessly carry any value
JSON's grammar can express, and it can be converted to JSON text and
back. The wire form is approximately the same size as compact JSON
text (~102% on representative API payloads).

The format is designed for:

* **Efficient random-access reads** by name or by index — sub-20 ns
  lookups on cached buffers in practice (see §11 for the algorithm and
  why a hash-prefilter scan outperforms binary-search-of-strings at
  realistic JSON field counts).
* **Single-allocation, single-pass encode** — no DOM tape, no
  intermediate value tree. The encoder appends children forward and
  writes the index at the tail.
* **Persistence and transit** — pjson is a single self-contained byte
  string, suitable for database rows, file storage, RPC payloads, and
  content-addressed hashing.

Compared with:

* **JSON text** — pjson supports random access; JSON text requires a
  re-parse on every read.
* **Postgres `jsonb`** — pjson is publicly specified; `jsonb` is a
  Postgres-internal layout with no third-party readers.
* **MessagePack / CBOR / BSON** — pjson supports indexed lookup by
  hash, where these formats require a linear scan per object.
* **simdjson DOM** — pjson is a persistent byte form that does not
  reference the original JSON text. simdjson DOM cannot exist
  separated from its source buffer.

---

## 2. Conventions

* All multi-byte integers are **little-endian**.
* `u8`, `u16`, `u32`, `u64`, `i8`, `i32`, `i64` denote their usual
  fixed-width unsigned/signed integer types.
* `i128` is a 128-bit signed integer in two's complement.
* Bit positions are MSB = bit 7 within a byte unless otherwise stated.
* Byte ranges are written `[start, end)` (start inclusive, end
  exclusive).
* "Caller" means whatever code provides the buffer to the parser. The
  caller is always responsible for telling the parser the buffer's
  total size; pjson values do not store their own length except where
  noted.

A pjson **value** is a contiguous byte sequence whose total length is
known to the consumer. The first byte is always the **tag**.

---

## 3. Tag Byte

```
bit 7 6 5 4 3 2 1 0
    └─type─┘ └─low─┘
```

The high nibble (`bits 7..4`) is the **type code**. The low nibble
(`bits 3..0`) is **payload-specific** — for some types it carries
encoded data; for others it is reserved.

| code | type          | low nibble use                                 | raw bits after tag |
|------|---------------|------------------------------------------------|--------------------|
| 0    | `null`        | reserved (must be 0)                           | 0 bytes |
| 1    | `bool`        | the boolean value: 0 = false, 1 = true (others reserved) | 0 bytes |
| 2    | reserved      |                                                | — |
| 3    | `uint_inline` | unsigned value 0..15 (the integer)             | 0 bytes |
| 4    | `int`         | mantissa byte count − 1 (range 1..16)          | `bc` bytes (zigzag-LE) |
| 5    | `decimal`     | mantissa byte count − 1 (range 1..16)          | `bc` mantissa bytes + varscale (1..4 bytes) |
| 6    | `ieee_float`  | reserved (must be 0)                           | 8 bytes (raw IEEE-754 binary64, LE) |
| 7    | reserved      |                                                | — |
| 8    | `string`      | encoding flag (see §4.7); 0..2 valid, others reserved | (size − 1) bytes (length implicit from `size`) |
| 9    | reserved      |                                                | — |
| 10 (A) | reserved   |                                                | — |
| 11 (B) | `array`     | reserved (must be 0)                           | container body — see §5 |
| 12 (C) | `object`    | reserved (must be 0)                           | container body — see §5 |
| 13–15  | reserved   |                                                | — |

Implementations must reject (return error) on any reserved tag code or
non-zero low-nibble bits in tags that mark them reserved.

---

## 4. Scalar Encodings

### 4.1 `null` (code 0)

Tag byte `0x00` alone. Container `size` is exactly 1 byte.

### 4.2 `bool` (code 1)

Single byte. The low nibble carries the boolean value:

```
0x10  →  false
0x11  →  true
```

Low-nibble values 2..15 are reserved and must cause a parse error.
Container `size` is exactly 1 byte.

### 4.3 `uint_inline` (code 3)

Tag byte alone. The value is the **unsigned** integer encoded in the
low nibble (range `0..15`). Container `size` is exactly 1 byte.

The name reflects the encoding: only non-negative values 0..15 fit
this form. Negative integers and values ≥ 16 use the `int` tag (code
4).

### 4.4 `int` (code 4)

```
tag (1 B): high = 4, low = bc − 1   where bc ∈ {1..16}
mantissa (bc B): zigzag-encoded signed integer, little-endian
```

**Zigzag encoding** maps signed integers to unsigned for compact
representation:

```
zz_encode(v) = (v << 1) ^ (v >> 63)         // for i64
zz_decode(z) = (z >> 1) ^ −(z & 1)          // arithmetic shift
```

For `bc ≤ 8` the value fits `i64`. For `bc ∈ 9..16` the value uses the
full 128-bit range.

Encoders should choose the **smallest `bc`** that exactly represents
the value. Decoders must accept any `bc ∈ 1..16`. There is no
canonical-encoding requirement; both `bc = 2` and `bc = 8` are valid
encodings of the value 200, but encoders should pick `bc = 1`.

The `uint_inline` form (code 3) is preferred over `int` for values
0..15.

### 4.5 `decimal` (code 5)

```
tag (1 B):       high = 5, low = bc − 1   where bc ∈ {1..16}
mantissa (bc B): zigzag-encoded mantissa, little-endian
varscale (1..4 B): scale, see §4.4.1
```

Represents `mantissa × 10^scale` as an exact decimal value.

#### 4.5.1 Varscale — 2-bit-prefix variable-length signed integer

The scale uses a compact variable-length encoding. The first byte's
top 2 bits give the total byte count of the varscale; the remaining
bits hold zigzag-encoded scale data, little-endian.

> **Note: this is NOT the QUIC variable-length integer encoding.**
> QUIC (RFC 9000) uses a 1/2/4/8-byte length ladder (length codes
> map to byte counts 2⁰/2¹/2²/2³). Our encoding uses a 1/2/3/4-byte
> linear ladder. Both share the "top-2-bits-encode-length" idea
> in the first byte, but the length values and per-form capacities
> differ. We chose the linear ladder because pjson's typical values
> (decimal scale, long-key excess) cluster in the 1-3-byte range
> where the linear ladder is more byte-efficient than QUIC's
> power-of-2 jumps.

```
First byte:
   bits 7..6: total byte count − 1   (0 → 1 byte, 1 → 2, 2 → 3, 3 → 4)
   bits 5..0: low 6 bits of zigzag(scale)

Subsequent bytes (if any): bits 6..(6 + 8·(n-1)) of zigzag(scale).
```

Capacities:

| total bytes | zigzag bits | scale range          |
|-------------|-------------|----------------------|
| 1           | 6           | −32..31              |
| 2           | 14          | −8 192..8 191        |
| 3           | 22          | −2 097 152..2 097 151 |
| 4           | 30          | −536 870 912..536 870 911 |

Encoders must use the smallest byte count that fits the value.

#### 4.5.2 Decimal vs IEEE float

A floating-point value may be encoded either as `decimal`
(mantissa × 10^scale) or `ieee_float` (raw IEEE-754 binary64). When
both encodings can represent the value exactly:

* If the **decimal form is shorter** in bytes, encoders **should**
  choose `decimal`.
* Otherwise encoders **should** choose `ieee_float`.

Decoders must accept either form for any numeric value.

Round-tripping a JSON number through pjson preserves the **value**,
not the **textual form**. Trailing zeros (`100.00`) and scientific
notation (`1.5e10`) are normalized away.

### 4.6 `ieee_float` (code 6)

```
tag (1 B): high = 6, low = 0
bits (8 B): IEEE-754 binary64, little-endian
```

The 8 bytes are the raw memory representation of the `double`. NaN and
Infinity are technically representable but are forbidden in JSON; pjson
encoders reading from JSON will not produce NaN or Infinity values.
pjson encoders sourced from non-JSON inputs may emit them.

### 4.7 `string` (code 8)

```
tag (1 B): high = 8, low = encoding flag
content (size − 1 B): bytes; interpretation per the flag
```

The low nibble carries an **encoding flag** that tells the JSON
emitter what to do with the stored bytes. This collapses what would
otherwise need two distinct types (text and binary) into one tag with
a sub-flag.

| flag | name          | meaning                                                       |
|------|---------------|---------------------------------------------------------------|
| 0    | `raw_text`    | UTF-8 text NOT in JSON-escape form. JSON emit must run a per-character escape pass (handle `"`, `\`, control chars, etc.). |
| 1    | `escape_form` | Text already in JSON-escape form (`\"`, `\n`, `\uXXXX` etc are LITERAL bytes). JSON emit just wraps in surrounding quotes — no per-byte work. |
| 2    | `binary`      | Raw binary bytes. JSON emit must base64-encode them. The encoded JSON typically uses a `.b64` key-suffix convention (see §7) so consumers know to base64-decode on input. |
| 3..15 | reserved     | Implementations must reject.                                  |

The string length is always `size − 1` (no length prefix).

**Encoder choices.** A producer should set the flag according to its
source:

* JSON parser → `escape_form` (the bytes between quotes are taken
  verbatim from the source JSON).
* Typed value of `std::string` (or equivalent) where the producer
  doesn't know whether escape characters are present → `raw_text`.
* Typed value that the producer has already validated to contain no
  characters needing escape → `escape_form` (cheaper emit later).
* Raw binary blob (image, hash digest, etc.) → `binary`.

**Decoder behavior.** On `flag = binary` the value is conceptually
binary data, not text. Implementations may expose it via a separate
"bytes" accessor (returning a span of bytes) versus a "string"
accessor (returning a string view). The reference C++ implementation
uses both `view::as_string()` and `view::as_bytes()`, with the kind
discriminated by the flag.

---

## 5. Containers

Containers (`array`, `object`) use a **tail-indexed** layout: child
values are written first, the index is appended at the tail. The
caller-provided `size` of the container is required to locate the
index by walking backward from the end.

### 5.1 Array layout (code 11)

```
[tag: u8 = 0xB0]
[value_data: variable]
   for each i in 0..N:
      [child value: tag + raw bits]
[slot[N]: N × u32 LE]            packed { offset:24, unused:8 }
[count: u16 LE]                  N — number of children
```

`count` is the **last 2 bytes** of the container.

`slot[i]` packs:

```
bits 23..0  : offset (within value_data, i.e. relative to byte 1 of the container)
bits 31..24 : unused for arrays — must be 0
```

Each child's byte range within the container is:

```
child_i_start = container_start + 1 + slot[i].offset
child_i_end   = container_start + 1 + (i + 1 < N
                                       ? slot[i+1].offset
                                       : value_data_size)
child_i_size  = child_i_end − child_i_start
```

where `value_data_size = container_size − 2 − 4·N − 1`.

### 5.2 Object layout (code 12)

```
[tag: u8 = 0xC0]
[value_data: variable]
   for each i in 0..N (in encounter order):
      if slot[i].key_size <  0xFF:
         [key bytes (slot[i].key_size B)][child value]
      else (long-key escape):
         [key_excess: 2-bit-prefix varuint]
         [key bytes (0xFF + key_excess B)][child value]
[hash[N]: N × u8]
[slot[N]: N × u32 LE]            packed { offset:24, key_size:8 }
[count: u16 LE]                  N — number of fields
```

`count` is the **last 2 bytes** of the container.

`slot[i]` packs:

```
bits 23..0  : offset (within value_data, relative to byte 1 of the container)
bits 31..24 : key_size — 0..0xFE for the key length in bytes;
                          0xFF marks the long-key escape (see §5.4)
```

`hash[i]` is the 8-bit prefilter hash for field `i`'s key (see §5.3).

Each entry's byte range:

```
entry_i_start = container_start + 1 + slot[i].offset
entry_i_end   = container_start + 1 + (i + 1 < N
                                       ? slot[i+1].offset
                                       : value_data_size)
```

where `value_data_size = container_size − 2 − 4·N − N − 1`.

Within an entry:

```
if slot[i].key_size != 0xFF:
   key_bytes        = entry[0 ..  slot[i].key_size)
   child_value_starts at entry[slot[i].key_size]

else (long-key):
   varuint excess at entry[0]
   varuint_size = 1..4 (per §4.4.1, but encoding 'unsigned' — see §5.4)
   key_size = 0xFF + excess
   key_bytes = entry[varuint_size .. varuint_size + key_size)
   child_value_starts at entry[varuint_size + key_size]
```

Field encounter order is preserved (no canonical sort).

### 5.3 Key prefilter hash

Each object's `hash[i]` byte is the **low 8 bits of XXH3-64** computed
over the field's key with any **trailing `.tag` suffix stripped**.

```
key_hash8(k):
   strip k from the last '.' onward (if any)   // "amount.decimal" → "amount"
   return XXH3_64(stripped) & 0xFF
```

Suffix stripping enables presentation-tag-suffix matching: a stored key
`"amount.decimal"` and a query key `"amount"` hash to the same byte,
allowing a fast prefilter to find candidates regardless of suffix
attachment. A full byte-equality compare on the matching slot
distinguishes them.

Decoders **must** verify the stored hash byte matches `key_hash8` of
the stored key bytes; mismatch is an error.

#### Collision behavior

The hash is 8 bits — there are 256 possible values. For an object of
N fields with random keys, the expected number of hash collisions per
query is ≈ N/256. Typical JSON objects have N ≤ 30, so the average
lookup hits one hash candidate, runs one byte-equal-string-compare,
and returns. For very wide objects (N ≈ 256) you'd expect ~1 false
positive per query — still cheap.

The lookup algorithm (§11) handles collisions correctly: on a hash
hit that fails the byte-equal key check, it advances and re-scans
the remainder. So worst-case N collisions degrade lookup to O(N) of
**string compares** rather than just O(N) of byte compares. For
N ≤ 1000 this remains acceptable (microseconds, not milliseconds).

**Adversarial / hash-flooding inputs.** An attacker who controls the
JSON keys can construct N keys that all hash to the same byte (only
~256 attempts to find one collision; finding N colliders by birthday
attack is `O(N · √256) = O(16N)` work for the attacker — trivial
preprocessing). This degrades a single lookup to N string compares.
For pjson's intended use cases (database storage of API request
payloads, internal RPC, configuration files) JSON keys are typically
**schema-defined by the application**, not attacker-controlled, so
this attack vector is rare.

If keys ARE attacker-controlled in your deployment (e.g., a generic
key-value store storing user dictionaries), an implementation may:

* Switch to a **seeded** hash. XXH3-64 takes a 64-bit seed; pick a
  random seed at process startup and use it for every `key_hash8`
  computation. Attackers can no longer precompute colliders.
  Producers and consumers in the same process share the seed; if
  pjson values are persisted across processes (database, file)
  the seed must also be persisted alongside the value or recomputed
  per-doc.
* Cap the max fields per container at the application layer below
  pjson's 65535 hard limit, bounding worst-case lookup work.
* Skip hash verification on read (see §9 errors) and rely solely on
  byte-equal key compare. Drops to O(N) per lookup unconditionally
  but eliminates the hash-flood asymmetry; ~3-5× slower for typical
  workloads.

The reference C++ implementation does **not** seed the hash —
`key_hash8` is deterministic, which makes content-addressable
hashing (two pjson buffers from the same logical input hash to the
same content-id) work cleanly. This is the right tradeoff when
keys are not attacker-controlled. Deployments that need both
deterministic hashing AND attacker-resistance should canonicalize
inputs at a higher layer (e.g., reject objects with > K fields, or
hash the key set externally).

### 5.4 Long-key escape

If a key's byte length is ≥ 255, the slot's `key_size` byte is set to
`0xFF` and the actual key length is encoded inline in the entry's
value_data region as a 2-bit-prefix **unsigned** varuint (same
encoding as §4.4.1 minus the zigzag step) for the **excess**:

```
actual_key_length = 0xFF + varuint_excess_value
```

The varuint is read forward starting at the entry's first byte. After
it, the key bytes start, then the value bytes.

Capacities (varuint of `excess`):

| varuint bytes | excess range | actual key length |
|---|---|---|
| 1 | 0..63 | 255..318 |
| 2 | 0..16 383 | 255..16 638 |
| 3 | 0..2²² − 1 | up to ≈ 4 M + 255 |
| 4 | 0..2³⁰ − 1 | up to ≈ 1 G + 255 |

In practice, JSON keys are short identifiers; the long-key escape is
defensive. Encoders should assume keys < 255 chars in the common path.

### 5.5 `count` width

The count field is a fixed `u16 LE` (2 bytes), supporting up to 65 535
fields per container. Implementations **must** reject containers with
counts beyond this on the wire. (For deeper limits, see §8 — pjson is
not designed for million-field containers.)

### 5.6 Offset width and container size limit

The `offset` field within each `slot` is **24 bits**, bounding any
single container's `value_data` region to **16 MiB**. Documents larger
than this must split into multiple top-level containers.

---

## 6. Document — top-level

A pjson document **is** a single value. There is no envelope, magic
number, or version marker. The format defines no document framing of
its own; consumers know they are reading pjson because their
application protocol said so (HTTP `Content-Type`, file extension,
database column type, etc.).

A consumer parses a pjson document by being given a `(ptr, size)` pair
of bytes. The first byte is the tag, and the value occupies all `size`
bytes.

If multi-document framing is needed at the application layer, a simple
prefix scheme works:

```
[size: 2-bit-prefix varuint][document bytes]    repeated
```

This framing is application-defined and not part of pjson.

---

## 7. JSON Round-trip

### 7.1 Value-preserving, not byte-preserving

JSON ↔ pjson round-trips preserve **values**, not source-text
byte-for-byte. Specifically:

* Whitespace and indentation are not preserved (compact emit only).
* Trailing zeros in decimals are normalized (`100.00` → `100`).
* Scientific notation may be normalized (`1.5e10` → `15000000000`).
* Field encounter order **is** preserved.

For applications requiring byte-identical round-trip (e.g., signature
verification on the exact source JSON), pjson is insufficient — use a
canonical-JSON form on both sides, or keep the original text alongside
the pjson.

### 7.2 Type mappings

| JSON | pjson tag |
|---|---|
| `null` | 0 (null) |
| `false` | 1 (bool, low nibble = 0) |
| `true` | 1 (bool, low nibble = 1) |
| integer fitting i64 | 3 (int_inline) for 0..15, else 4 (int) |
| integer beyond i64 | 4 (int) with bc 9..16 |
| fractional / exponent | 5 (decimal) when shortest, else 6 (ieee_float) |
| `"..."` | 8 (string) — bytes between quotes, escape-form preserved |
| `[ ... ]` | 11 (array) |
| `{ ... }` | 12 (object) |

There is no JSON literal for `bytes`. JSON encoders may emit them as
base64-encoded strings under a key-suffix convention:

```
// JSON form
{"avatar.b64": "iVBORw0KGgo..."}

// pjson form
object with key "avatar" (suffix stripped on lookup hash) whose value
is a `bytes` tag holding the base64-decoded raw bytes.
```

This convention is established at the application layer; pjson stores
the bytes raw regardless of how they were textually represented.

### 7.3 Unrepresentable JSON values

JSON forbids `NaN`, `+Infinity`, `−Infinity`. pjson encoders sourcing
from JSON will not emit these. Decoders converting pjson to JSON must
reject them or signal an error.

### 7.4 Suffix vocabulary (recommended)

| suffix | meaning | typical pjson tag |
|---|---|---|
| `.b64` | base64-encoded binary | bytes (10) |
| `.hex` | lowercase hex string | bytes (10) |
| `.base58` | base58 string | bytes (10) |
| `.decimal` | decimal-digit string | int (4) or decimal (5) |
| `.rfc3339` | RFC 3339 timestamp | int (4) — nanoseconds since epoch |
| `.unix_ms` | milliseconds since Unix epoch | int (4) |
| `.unix_us` | microseconds since Unix epoch | int (4) |

The suffix is purely a JSON-side hint: pjson stores the binary form;
the consumer reattaches the suffix when emitting JSON. The hash byte
strips the suffix so `view["amount"]` and a stored key
`"amount.decimal"` land in the same hash bucket.

---

## 8. Limits

| limit | value |
|---|---|
| max fields per container | 65 535 (`u16` count) |
| max value_data bytes per container | 16 777 215 (24-bit offset) |
| max key length | unbounded via long-key escape (§5.4); short-key path covers 0..254 bytes |
| max integer mantissa | 128-bit signed (16-byte zigzag) |
| max decimal scale | ±536 870 911 (4-byte varscale) |
| max nesting depth | implementation-defined; suggested cap: 256 |

---

## 9. Errors

A parser must detect and reject:

* Tag with reserved type code (7, 9, 13–15).
* Tag with non-zero low-nibble bits where the spec marks them
  reserved.
* `int` or `decimal` with bc < 1 or bc > 16.
* Container with stated count yielding `slot_table_pos < 1` or
  `value_data_size > size − overhead`.
* `slot[i].offset` ≥ `value_data_size`, or `slot[i].offset` ≥
  `slot[i+1].offset` (children must be ordered, no overlap).
* `hash[i] != key_hash8(stored_key_bytes_for_i)`.
* `varscale` or long-key varuint with claimed length exceeding the
  remaining buffer.
* Invalid UTF-8 in a string (parser may opt to reject; the spec does
  not mandate UTF-8 validation, but consumers needing it should
  validate).

---

## 10. Reference Algorithm — Read

```
parse_value(ptr, size) -> Value:
   tag  = ptr[0]
   type = tag >> 4
   low  = tag & 0x0F

   case type of
   0: return Null()                                       // tag 0x00
   1: assert low ≤ 1                                      // 0x10 = false, 0x11 = true
      return Bool(low == 1)
   3: return Int(low)                                     // 0..15
   4: bc = low + 1
      assert 1 + bc == size
      zz = read_le_uint(ptr + 1, bc)
      return Int(zz_decode(zz))
   5: bc = low + 1
      mantissa_zz = read_le_uint(ptr + 1, bc)
      (scale, scale_bytes) = read_varscale(ptr + 1 + bc, size - 1 - bc)
      assert 1 + bc + scale_bytes == size
      return Decimal(zz_decode(mantissa_zz), scale)
   6: assert size == 9
      return Double(read_le_double(ptr + 1))
   8: return String(ptr[1 .. size))
   10: return Bytes(ptr[1 .. size))
   11: return parse_array(ptr, size)
   12: return parse_object(ptr, size)
   else: error

parse_array(ptr, size):
   N = read_u16_le(ptr + size − 2)
   slot_table = ptr + size − 2 − 4·N
   value_data_start = ptr + 1
   value_data_size = (slot_table − ptr) − 1

   for i in 0..N:
      off_i    = slot_offset(read_u32_le(slot_table + i·4))
      off_next = (i+1 < N)
                  ? slot_offset(read_u32_le(slot_table + (i+1)·4))
                  : value_data_size
      child = parse_value(value_data_start + off_i, off_next − off_i)
      append child

parse_object(ptr, size):
   N = read_u16_le(ptr + size − 2)
   slot_table = ptr + size − 2 − 4·N
   hash_table = slot_table − N
   value_data_start = ptr + 1
   value_data_size = (hash_table − ptr) − 1

   for i in 0..N:
      slot     = read_u32_le(slot_table + i·4)
      off_i    = slot & 0x00FFFFFF
      key_size_byte = slot >> 24
      off_next = (i+1 < N)
                  ? slot_offset(read_u32_le(slot_table + (i+1)·4))
                  : value_data_size
      entry      = value_data_start + off_i
      entry_size = off_next − off_i

      if key_size_byte != 0xFF:
         klen, klen_bytes = key_size_byte, 0
      else:
         excess, klen_bytes = read_varuint(entry, entry_size)
         klen = 0xFF + excess

      key   = entry[klen_bytes .. klen_bytes + klen)
      assert hash_table[i] == key_hash8(key)
      child = parse_value(entry + klen_bytes + klen,
                          entry_size − klen_bytes − klen)
      append (key, child)
```

## 11. Reference Algorithm — Object Lookup by Key

```
find(ptr, size, query_key) -> Value or NotFound:
   if (ptr[0] >> 4) != 12: return NotFound
   N          = read_u16_le(ptr + size − 2)
   slot_table = ptr + size − 2 − 4·N
   hash_table = slot_table − N
   value_data_start = ptr + 1
   value_data_size  = (hash_table − ptr) − 1
   want = key_hash8(query_key)

   i = 0
   while i < N:
      // Hash prefilter — SIMD scan equivalent
      skip = position of first byte == want in hash_table[i .. N)
      if skip not found: return NotFound
      i = i + skip

      // Slot read
      slot     = read_u32_le(slot_table + i·4)
      off_i    = slot & 0x00FFFFFF
      key_size_byte = slot >> 24
      off_next = (i+1 < N)
                   ? slot_offset(read_u32_le(slot_table + (i+1)·4))
                   : value_data_size
      entry      = value_data_start + off_i
      entry_size = off_next − off_i

      if key_size_byte != 0xFF:
         klen, klen_bytes = key_size_byte, 0
      else:
         excess, klen_bytes = read_varuint(entry, entry_size)
         klen = 0xFF + excess

      // Verify
      stored_key = entry[klen_bytes .. klen_bytes + klen)
      if klen == |query_key| and stored_key == query_key:
         return parse_value(entry + klen_bytes + klen,
                            entry_size − klen_bytes − klen)

      // Hash collision — advance and re-scan
      i = i + 1
   return NotFound
```

### 11.1 Why this is fast

Object key lookup is **O(N) per level**, where N is the field count of
the one object being searched. In the asymptotic sense binary search
on sorted keys would be O(log N), but in practice the hash-prefilter
linear scan beats it at every realistic JSON field count:

* The prefilter scans N **single bytes** sequentially. Implementations
  use SWAR (8 bytes per cycle, branchless) or SIMD intrinsics. For an
  object of 25 fields the entire scan touches one cache line.
* On a hash hit (rare; ~1/256 false-positive rate per byte), one
  string compare. For typical-sized identifier keys this is also a
  single cache-line read.
* No sorted-key data structure → no dependent-load chain, no branchy
  tree traversal.

A balanced binary-search-of-strings at the same N pays log₂(N)
**string** compares, each with a data-dependent branch and pointer
chase to a non-adjacent node — all cache-unfriendly. The reference
implementation measures ~17 ns per object lookup on a 25-field object;
a comparable tree-of-strings implementation lands in the 50–100 ns
range. The asymptotic crossover where a sorted/tree-based structure
would actually win is thousands of fields per object — far beyond
what JSON documents contain in practice.

The format trades the asymptotic optimality of log(N) for cache-
locality optimality at small-to-medium N, which is where every
realistic JSON object lives.

## 12. Reference Algorithm — Encode

```
encode_value(value, out: byte buffer):
   case value of
      Null()        -> append 0x00
      Bool(false)   -> append 0x10
      Bool(true)    -> append 0x11
      Int(v) where 0 ≤ v ≤ 15
                    -> append 0x30 | v
      Int(v):
         zz = zz_encode(v)
         bc = byte_count(zz)         // smallest bc ∈ 1..16 fitting zz
         append 0x40 | (bc − 1)
         append zz as bc bytes LE
      Decimal(m, s):
         zz = zz_encode(m)
         bc = byte_count(zz)
         append 0x50 | (bc − 1)
         append zz as bc bytes LE
         append varscale(s)
      Double(d):
         append 0x60
         append d as 8 IEEE bytes LE
      String(bytes_in_escape_form):
         append 0x80
         append bytes
      Bytes(b):
         append 0xA0
         append b
      Array(children):
         append 0xB0
         start = out.position
         records = []
         for child in children:
            records.append((offset = out.position − (start + 1)))
            encode_value(child, out)
         for r in records:
            append slot_pack(r.offset, 0) as u32 LE
         append count = |children| as u16 LE
      Object(fields):
         append 0xC0
         start = out.position
         records = []
         for (key, child) in fields:
            records.append((hash = key_hash8(key),
                            offset = out.position − (start + 1),
                            key_size = min(|key|, 0xFF)))
            if |key| ≥ 0xFF:
               append varuint(|key| − 0xFF)
            append key bytes
            encode_value(child, out)
         for r in records:
            append r.hash as u8
         for r in records:
            append slot_pack(r.offset, r.key_size) as u32 LE
         append count = |fields| as u16 LE
```

The encoder is **single-pass forward-write**. No memmove, no
header reservation, no backpatching beyond writing the index at the
tail.

---

## 13. Versioning

This is **pjson v1**. Future revisions of the format will:

* Allocate previously-reserved tag codes (7, 9, 13–15) for new types.
* Allocate previously-reserved low-nibble bits as flags or extensions.
* Be detectable through application-layer means (file headers, MIME
  types, RPC protocol versions). The pjson value format itself does
  not carry a version byte.

A v1 parser encountering a v2 wire (e.g., a value with tag `0xD0`)
must reject the buffer rather than guess.

---

## 14. Conformance Tests

A conforming implementation must round-trip the following corpus
byte-for-byte through encode → decode → encode:

* All primitive values: `null`, `true`, `false`, integers covering each
  byte_count from 1 to 16, decimals with each varscale tier (1, 2, 3,
  4 bytes), all eight-byte IEEE doubles including 0.0, 1.0, NaN
  (encoded only — JSON forbids), small / medium / large strings, raw
  byte blobs of varying sizes, empty arrays, empty objects.
* Random nested structures up to depth 8 with mixed types.
* Object key collision on the hash byte (multiple fields share the
  same hash byte; lookup must return the right one for each).
* Long-key escape: object with a key of 254 bytes (no escape) and a
  key of 255 bytes (escape).

A reference test corpus lives at `libraries/psio/cpp/tests/pjson_tests.cpp`.

---

## 15. Canonical Encoding

The pjson format permits multiple wire encodings of the same logical
value (e.g. a small integer can use `uint_inline` or `int`). For
content-addressable hashing, signature verification, deduplication,
and equality comparison via byte-compare, implementations need to
agree on a single canonical form for every value.

### 15.1 Encoding rule (priority order)

A canonical encoder selects the encoding by applying these rules in
order:

1. **Round-trip JSON without precision loss.** Choose only encodings
   that preserve the value's information through a JSON-text →
   pjson → JSON-text round-trip. (Strings preserve their escape form
   per §4.7; numbers preserve their value bit-exactly per the rules
   below.)
2. **Smallest possible byte size.** Among encodings that satisfy
   rule 1, choose the encoding that produces the fewest bytes.
3. **Fastest decode on ties.** When two encodings produce the same
   byte count AND both preserve precision, choose the one that
   decodes fastest. For numbers this means: prefer `ieee_float` over
   `decimal` (raw 8-byte memcpy beats mantissa-and-scale parse).

This makes the canonical encoding **deterministic per value** —
given the same logical value, every conforming encoder produces the
same bytes for that value. (Field encounter order is excluded; see
§15.4.)

### 15.2 Numbers — concrete rules

For an integer value `v`:

| value range                          | canonical encoding              |
|--------------------------------------|---------------------------------|
| 0 ≤ v ≤ 15                           | `uint_inline` (1 byte)          |
| v < 0 or v > 15, fits i64            | `int` with smallest bc (1..8)   |
| beyond i64, fits i128                | `int` with smallest bc (9..16)  |

For an integer-valued double (e.g. `42.0`): treat as integer and
apply the table above. The fractional part is zero, so it round-trips
through any integer encoding losslessly.

For a fractional double `d` (non-zero fractional part):

1. Compute the **shortest round-trip decimal** `(mantissa, scale)`
   such that `from_double(d)` produces this pair (Ryu / Grisu /
   Dragon4-equivalent). Trim trailing zeros from the mantissa
   (incrementing `scale`) until either the last digit is non-zero
   or `scale = 0`.
2. Let `decimal_size = 1 + mantissa_bc + varscale_size(scale)`.
3. If `decimal_size < 9` → encode as `decimal`.
4. Else if `decimal_size > 9` → encode as `ieee_float`.
5. Else (tie at 9 bytes) → encode as `ieee_float` (faster decode).

For a fractional with no representable shortest decimal (e.g.
NaN, ±Inf — JSON-illegal but a non-JSON producer could emit them):
encode as `ieee_float`.

For a `pjson_number{m, s}` provided directly by typed code (not via
double): encode the (m, s) form regardless of whether `ieee_float`
would be shorter — the typed value is **already** a `decimal`
semantically; collapsing to double would lose its exact-decimal
identity. (Producers that want the smallest encoding regardless of
identity should construct from a double and run the rules above.)

### 15.3 Strings

The wire form of a string carries an encoding flag (§4.7) that
**identifies the source** rather than canonicalizing it:

- A string from a JSON text source encodes as `escape_form` —
  byte-identical to the source bytes between the JSON quotes.
- A string from a typed `std::string` (or equivalent) encodes as
  `raw_text` — the program-visible text, with no escape pass.
- A binary blob encodes as `binary`.

Two pjson values for the same logical "Hello world" — one from JSON,
one from a typed string — will NOT be byte-equal because they
deliberately preserve their different source representations. This
is the format's **byte-equality means same source** property.

For applications that want logical equality across sources, consume
both via `view::as_string()` and compare the unescaped bytes (which
requires applying JSON unescape rules to the `escape_form` value).
Or canonicalize at a higher layer before encoding.

### 15.4 Containers

- **Field encounter order is application-defined**, not canonical.
  Two pjson objects with the same fields in different orders are NOT
  byte-equal, even if their data is logically the same. Applications
  that need order-independent equality must canonicalize key order
  at a higher layer (typically: sort keys lexicographically before
  encoding).
- **Slot offsets** must be ascending and exactly cover the
  value_data region (no gaps, no overlap). The encoder's natural
  forward-write produces this; non-canonical layouts (e.g. jumbled
  slot offsets reordering the children) are valid pjson but not
  canonical.
- **Hash bytes** are deterministic from key bytes (low byte of
  XXH3-64 over suffix-stripped key per §5.3). Always canonical.
- **num_fields** is the actual N. Always canonical.

### 15.5 Strict canonical (byte-equality contract)

A pjson value is **strictly canonical** iff:

- Every nested numeric value satisfies §15.2.
- Every nested string carries the canonical flag for its source
  (§15.3).
- Every nested container has slots in ascending offset order with
  no gaps (§15.4).
- Field order matches the application's chosen canonicalization
  policy (which itself must be deterministic from the logical
  value — typically lexicographic key sort, or original input
  order if input order is part of the logical identity).

Two strictly-canonical pjson buffers are byte-equal iff their
logical values are equal under the application's order policy.

### 15.6 Producer guidance

Implementations marketed as "canonical pjson encoders" must:

- Apply §15.1 priority rules for every value.
- Document their field-order policy (lex-sort, source-order, etc.)
  so consumers can match.
- Never emit reserved low-nibble flag values (§3) on canonical
  output.

The reference C++ encoder follows §15.2 rules for numbers and
preserves source-order field encoding. It does NOT sort keys; that's
left to a higher layer.

## Appendix A. Glossary

| term | meaning |
|---|---|
| **value_data** | the contiguous bytes between the container's tag byte and its tail index, holding all child values (and inline keys, for objects). |
| **slot** | a packed `u32` per child holding `{ offset (24 bits), key_size (8 bits) }`. The high byte is the key length for objects, unused (must be 0) for arrays. |
| **slot table** | the array of N slots, located at `container_end − 2 − 4·N`. |
| **hash table** | the array of N hash bytes (objects only), located immediately before the slot table. |
| **count** | the `u16 LE` at the last 2 bytes of every container, giving N. |
| **canonical layout** | for an object, `hash[i] == key_hash8(field_i_name)` in declaration order matches a producer's expected order, enabling memcmp-based fast-path validation against a compile-time-derived hash array. |
| **suffix stripping** | the hash function ignores any trailing `.<suffix>` on the key, supporting presentation-tag conventions. |

## Appendix B. Worked Example

Compact JSON `{"a":1,"b":"two"}` (17 bytes) encodes as **20 bytes** of pjson:

```
0xC0                       tag: object
                           ── value_data (10 bytes) ──
0x61                       'a' (key for entry 0; key_size in slot)
0x31                       int_inline 1 (entry 0 value)
0x62                       'b' (key for entry 1)
0x80                       string tag (entry 1 value tag)
0x74 0x77 0x6F             't', 'w', 'o' (string content)
                           ── hash[2] (2 bytes) ──
0xH1                       low byte of XXH3_64("a")
0xH2                       low byte of XXH3_64("b")
                           ── slot[2] (8 bytes) ──
0x00 0x00 0x00 0x01        slot[0]: offset=0, key_size=1
0x02 0x00 0x00 0x01        slot[1]: offset=2, key_size=1
                           ── count (2 bytes) ──
0x02 0x00                  N = 2
```

Reading this back from an arbitrary `(ptr, size = 20)`:

```
N      = read_u16_le(ptr + 18) = 2
slot_table = ptr + 18 − 8 = ptr + 10
hash_table = ptr + 10 − 2 = ptr + 8
value_data_start = ptr + 1
value_data_size  = 8 − 1 = 7
```

Field `"a"`:
* hash[0] at `ptr[8]`, slot[0] at `ptr[10..14]` → offset 0, key_size 1.
* entry at `ptr+1`, key bytes `ptr[1..2)` = `'a'`.
* value at `ptr+2`, size = (slot[1].offset − slot[0].offset) − 1 = 1 byte.
* `parse_value(ptr+2, 1)` → tag `0x31` → int_inline = 1.

Field `"b"`:
* hash[1] at `ptr[9]`, slot[1] at `ptr[14..18]` → offset 2, key_size 1.
* entry at `ptr+3`, key bytes `ptr[3..4)` = `'b'`.
* value at `ptr+4`, size = value_data_size − slot[1].offset − 1 = 4 bytes.
* `parse_value(ptr+4, 4)` → tag `0x80` → string with content `"two"`.

End of spec.
