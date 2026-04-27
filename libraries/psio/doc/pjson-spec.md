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

| code | type         | low nibble use                         | raw bits after tag |
|------|--------------|----------------------------------------|--------------------|
| 0    | `null`       | reserved (must be 0)                   | 0 bytes |
| 1    | `bool_false` | reserved (must be 0)                   | 0 bytes |
| 2    | `bool_true`  | reserved (must be 0)                   | 0 bytes |
| 3    | `int_inline` | unsigned value 0..15 (the integer)     | 0 bytes |
| 4    | `int`        | mantissa byte count − 1 (range 1..16)  | `bc` bytes (zigzag-LE) |
| 5    | `decimal`    | mantissa byte count − 1 (range 1..16)  | `bc` mantissa bytes + varscale (1..4 bytes) |
| 6    | `ieee_float` | reserved (must be 0)                   | 8 bytes (raw IEEE-754 binary64, LE) |
| 7    | reserved     |                                        | — |
| 8    | `string`     | reserved (must be 0)                   | (size − 1) UTF-8 bytes (length implicit from `size`) |
| 9    | reserved     |                                        | — |
| 10 (A) | `bytes`    | reserved (must be 0)                   | (size − 1) raw bytes (length implicit from `size`) |
| 11 (B) | `array`    | reserved (must be 0)                   | container body — see §5 |
| 12 (C) | `object`   | reserved (must be 0)                   | container body — see §5 |
| 13–15  | reserved   |                                        | — |

Implementations must reject (return error) on any reserved tag code or
non-zero low-nibble bits in tags that mark them reserved.

---

## 4. Scalar Encodings

### 4.1 `null`, `bool_false`, `bool_true` (codes 0, 1, 2)

Tag byte alone. Container `size` for these values is exactly 1 byte.

### 4.2 `int_inline` (code 3)

Tag byte alone. The value is the unsigned integer encoded in the low
nibble (range `0..15`). Container `size` is exactly 1 byte.

### 4.3 `int` (code 4)

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

The `int_inline` form (code 3) is preferred over `int` for values
0..15.

### 4.4 `decimal` (code 5)

```
tag (1 B):       high = 5, low = bc − 1   where bc ∈ {1..16}
mantissa (bc B): zigzag-encoded mantissa, little-endian
varscale (1..4 B): scale, see §4.4.1
```

Represents `mantissa × 10^scale` as an exact decimal value.

#### 4.4.1 Varscale — 2-bit-prefix variable-length signed integer

The scale uses a compact variable-length encoding. The first byte's
top 2 bits give the total byte count of the varscale; the remaining
bits hold zigzag-encoded scale data, little-endian.

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

#### 4.4.2 Decimal vs IEEE float

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

### 4.5 `ieee_float` (code 6)

```
tag (1 B): high = 6, low = 0
bits (8 B): IEEE-754 binary64, little-endian
```

The 8 bytes are the raw memory representation of the `double`. NaN and
Infinity are technically representable but are forbidden in JSON; pjson
encoders reading from JSON will not produce NaN or Infinity values.
pjson encoders sourced from non-JSON inputs may emit them.

### 4.6 `string` (code 8)

```
tag (1 B): high = 8, low = 0
content (size − 1 B): UTF-8 bytes, length implicit from caller-provided size
```

The string content is the bytes between the JSON quotes: it **may**
contain JSON escape sequences (`\"`, `\\`, `\b`, `\f`, `\n`, `\r`,
`\t`, `\uXXXX`) as **literal text bytes**. This form is bit-identical
to the source between-quotes bytes when the value originates from
JSON. When emitting JSON, the bytes are wrapped in `"..."` with no
per-byte escape pass.

A consumer needing the unescaped (decoded) form runs a JSON-string
unescape over the content bytes.

The string length is `size − 1` where `size` is the caller-provided
byte length of the value. Strings carry no length prefix.

### 4.7 `bytes` (code 10)

```
tag (1 B): high = 10 (0xA), low = 0
content (size − 1 B): raw binary bytes
```

Same length convention as `string`: the byte count is `size − 1`. The
JSON text round-trip emits these as base64-encoded strings under a
key-suffix convention (e.g., `"foo.b64"` → `bytes`); see §7.

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
| `false` | 1 (bool_false) |
| `true` | 2 (bool_true) |
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
   0: return Null()
   1: return Bool(false)
   2: return Bool(true)
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
      Bool(true)    -> append 0x20
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
