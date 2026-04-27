# psch — Compact Binary Schema for pSSZ

**Status:** design draft.

## Motivation

pSSZ wire bytes are self-locating (fixed-field offsets are static; variable
fields have an explicit offset table at the front of each container). To
convert pSSZ ↔ JSON dynamically — without compile-time codegen — a runtime
schema needs only:

- **The type of each field** (so the decoder knows how many bytes to consume
  and how to interpret them).
- **The name of each field** (so the JSON emitter has a key string and the
  parser can resolve `obj["name"]` at runtime).

`psch` (pSSZ-schema binary) encodes that schema graph in a compact form
designed for:

- **Small wire size** — sub-3 KB for representative blockchain schemas
  (Ethereum BeaconState ~30 types / 100 fields).
- **Constant-time queries** — `type(id)` is one indexed load;
  `field(type, name)` is one PHF lookup + one memcmp.
- **mmap-ready** — single contiguous byte string, all offsets file-relative,
  no parse step before queries.
- **No versioning** — the schema *is* the version. Two schemas with
  byte-different content are different schemas; consumers content-hash for
  identity if needed.

## Goals

1. Schema for any pSSZ value can be encoded and read back losslessly.
2. Type-by-id and field-by-name lookups complete in **O(1)** with bounded
   constants — no scan, no string compare beyond a single verification.
3. Schemas fit in a few KB for typical app shapes; encoder and reader are
   header-only / zero-allocation on the read side.
4. References between types are 1-byte ids when ≤ 255 types (the common
   case); widen automatically to 2-byte ids for larger schemas.

## Non-goals

- Versioning, migration, schema evolution. A schema is what it is; consumers
  with different schemas are talking about different data.
- Embedding rendering policy (decimal-vs-hex, base64-vs-hex). That's a thin
  policy applied alongside the schema, not part of it.
- Runtime mutation. Schemas are built once (at compile time, code-gen time,
  or schema-registration time), then read-only.

## Conventions

- All multi-byte integers are little-endian.
- `u8/u16/u24/u32/u40/u64` are fixed-width unsigned ints.
- A `varuint8` is `1 byte if < 0xFF, else 0xFF + u16`. A `varuint16` is
  `2 bytes if < 0xFFFF, else 0xFFFF + u32`. Used at the schema header to
  keep small schemas tight while supporting large ones losslessly.
- "Type id" is a 1-byte index into the type_table for schemas with
  type_count ≤ 254. Schemas with type_count > 254 set the `wide_type_ids`
  flag in the header; all type-id references in such schemas are u16.

## Top-level layout

```
[magic: 4 bytes "PSCH"]
[flags: 1 byte]
   bit 0: wide_type_ids (0 = u8 ids, 1 = u16 ids)
   bits 1..7: reserved (must be 0)
[type_count: varuint8]
[name_pool_size: varuint16]
[fields_pool_size: varuint16]
[variants_pool_size: varuint16]
[root_type_id: varuint8 (or u16 if wide_type_ids)]

[name_pool: name_pool_size bytes, packed UTF-8]
[type_table: type_count × 8 bytes]
[fields_pool: fields_pool_size bytes]
[variants_pool: variants_pool_size bytes]
```

The `magic` distinguishes psch bytes from raw pSSZ data; readers can
sanity-check before treating bytes as a schema.

The `flags` byte is *intra-schema* width selection, not a version. The same
schema graph encoded narrow vs wide is byte-different but semantically
identical; canonical form picks the narrowest that fits.

## Type table

`type_count` entries, fixed 8-byte stride:

```
[kind: u8] [payload: 7 bytes]
```

Direct indexing: `type_table[id]` is a single load. The kind selects how
the payload bytes are interpreted:

| kind | name | payload (7 bytes) |
|------|------|-------------------|
| 0x01 | bool | unused |
| 0x02 | u8 | unused |
| 0x03 | u16 | unused |
| 0x04 | u32 | unused |
| 0x05 | u64 | unused |
| 0x06 | u128 | unused |
| 0x07 | u256 | unused |
| 0x08 | bytes (variable, list of u8) | unused |
| 0x09 | bytes_n (fixed) | length (u32) + 0:24 |
| 0x0A | container | fields_offset (u32) + K (u8) + 0:16 |
| 0x0B | vector | elem_type_id (u8 or u16) + length (u40 or u32) |
| 0x0C | list | elem_type_id (u8 or u16) + 0 |
| 0x0D | union | variants_offset (u32) + variant_count (u8) + 0:16 |

Reserved kind values (0x00, 0x0E–0xFF) cause the reader to reject the
schema. Future revisions of `psch` are not envisaged — if richer kinds
are needed, the answer is a new format with its own magic.

## Container fields (in `fields_pool`)

Each container's fields are stored as a self-contained block at the
`fields_offset` recorded in its type_table entry. The block contains
both an ordered field list (for in-order iteration) and a perfect-hash
table (for O(1) name lookup).

```
[K: u8]                                       field count
[seed: u8]                                    PHF seed (xxh3 secondary input)
[size_log2: u8]                               PHF table size = 1 << size_log2
[phf_lookup[1<<size_log2]: u8 each]           PHF slot → ordered field index;
                                               0xFF = empty slot
[ordered_fields[K]: 8 bytes each]
   { name_off:24, name_len:8, type_id:u16, pad:u16 }
```

`name_off` and `name_len` reference the `name_pool`. `type_id` is u16
in storage for layout uniformity (high byte = 0 in narrow schemas);
encoder does NOT widen reads — readers use the schema's
`wide_type_ids` flag to decide how many bytes to consume.

(Implementation note: keeping the slot at 8 bytes regardless of width
eliminates a stride decision at access time. The 8 bytes per slot are
the same for narrow and wide schemas; the 1-byte vs 2-byte question
only affects `type_id` interpretation, not slot layout.)

### PHF construction (build-time)

```
function build_phf(names: [string]) -> (seed: u8, size_log2: u8, lookup):
   K = |names|
   for size_log2 in (ceil_log2(K) .. 8):
      table_size = 1 << size_log2
      mask = table_size − 1
      for seed in 0 .. 255:
         occupied = [false] × table_size
         lookup   = [0xFF] × table_size
         ok       = true
         for i, name in enumerate(names):
            idx = xxh3_64(name, seed) & mask
            if occupied[idx]: ok = false; break
            occupied[idx] = true
            lookup[idx]   = i
         if ok: return (seed, size_log2, lookup)
   error "PHF construction failed; raise size_log2 ceiling"
```

For K up to ~30, `size_log2 = ceil_log2(K)` succeeds within the first 256
seeds in practice. K > 64 may require enlarging by one or two log2 steps.

### Lookup: name → field index

```
function field_index_by_name(container_off, name) -> ordered_idx | NOT_FOUND:
   block      = fields_pool + container_off
   K          = block[0]
   seed       = block[1]
   size_log2  = block[2]
   table_size = 1 << size_log2
   mask       = table_size − 1
   phf        = block + 3
   ordered    = phf + table_size

   slot_idx = xxh3_64(name, seed) & mask
   ord_idx  = phf[slot_idx]
   if ord_idx == 0xFF: return NOT_FOUND

   field = ordered + ord_idx × 8
   stored_name = name_pool[field.name_off .. field.name_off + field.name_len)
   if stored_name == name: return ord_idx
   return NOT_FOUND
```

Two memory loads (`phf[idx]`, `ordered[ord_idx]`) and one memcmp. The
memcmp is required because PHF is collision-free for *in-set* names only;
out-of-set names that hash to a populated slot are rejected by the
verify step.

### Lookup: ordered index → field

```
function field_by_index(container_off, j) -> (name, type_id):
   block   = fields_pool + container_off
   K       = block[0]
   if j >= K: error
   ordered = block + 3 + (1 << block[2])      // skip header + phf table
   field   = ordered + j × 8
   return (name_pool[field.name_off .. + field.name_len), field.type_id)
```

Single load. Used by in-order iteration (e.g. JSON emit walks fields by
ordinal to preserve declaration order).

## Union variants (in `variants_pool`)

Each union's variant list is at the `variants_offset` from its type_table
entry:

```
[variant_type_id[N]: u8 or u16 each, per wide_type_ids]
```

Selector is on the pSSZ wire, not in the schema. `union_variant(type, n)`
is a single indexed load.

## Name pool

Packed UTF-8 bytes, no separators, no length prefix. Field names are
located via `(name_off:24, name_len:8)` slots in `ordered_fields`. The
24-bit offset bounds the `name_pool` to 16 MiB; the 8-bit length bounds
each name to 255 bytes (longer field names are rejected at build time).

## Wire-size estimates

Ethereum BeaconState reference (30 distinct types, ~100 total fields,
names average ~15 ASCII bytes):

| section | bytes |
|---|---|
| header | ~16 |
| name_pool | ~1 500 |
| type_table (30 × 8) | 240 |
| fields_pool (sum of per-container blocks) | ~1 200 |
| variants_pool | ~50 |
| **total** | **~3 KB** |

Independent of nesting depth — the schema is a flat graph of types
referenced by id.

## Comparison

| format | typical Eth BeaconState | name lookup |
|---|---|---|
| Cap'n Proto schema (compiled) | ~5 KB | medium (hash table) |
| FlatBuffers reflection (.bfbs) | ~6 KB | linear scan |
| Protobuf descriptor | ~10 KB | linear scan |
| **psch** | **~3 KB** | **PHF: 1 hash + 1 memcmp** |

## Validation

A `psch_validate(bytes)` pass checks:

- magic, flags, sizes from header are consistent (each section's offset +
  size lies within the buffer).
- Every `type_id` reference resolves to an in-range type_table entry.
- Every container's `fields_offset` lies within `fields_pool`; its `K`
  matches the field count derivable from the block.
- For every container's PHF: every `phf_lookup[j]` is either 0xFF or in
  `[0, K)`. Every name referenced by `ordered_fields[j].name_off/len` lies
  within `name_pool` and contains valid UTF-8 (optional UTF-8 check).
- For every union's `variants_offset`: lies within `variants_pool`,
  variant_count consistent.
- No cycles in `vector.elem` / `list.elem` chains except those involving
  containers — recursive types are allowed only through container fields
  (containers can hold lists/vectors of themselves; bare `list<list<T>>`
  doesn't introduce a cycle).

Linear in `bytes.size()`. Successful validation guarantees subsequent
queries are bounds-safe without per-query checks.

## Use with pSSZ

The schema does not duplicate pSSZ wire layout knowledge — pSSZ's own
rules give field offsets within containers (fixed fields concatenated,
variable fields' offsets table at front). The schema only answers:

- Given `(container_type_id, field_name)`: what is field's type and its
  position in the container's field order?
- Given `type_id`: what kind of value is it (so the decoder knows whether
  to read a fixed-size value, a variable-size offset, or recurse into a
  container)?

Combining: a `pssz_view + psch_view` pair lets a caller resolve any
`view["a"]["b"][i]["c"]` path against the wire bytes at runtime.

## Render policy (sibling, not part of schema)

A small render-policy struct alongside the `psch_view` carries:

- For numeric types: emit as decimal vs hex string (Ethereum convention:
  u128/u256 as `"0x..."` hex strings; everything else as decimal).
- For `bytes`/`bytes_n`: emit as hex (`"0x..."`), base64, or UTF-8 text.
- For `bool`: always JSON `true`/`false`.

Same schema can drive Ethereum-style hex JSON or "everything-decimal" JSON
depending on policy. Default policy mirrors Ethereum conventions.

## Open questions (for review before prototyping)

- [ ] **Whether to fold `bytes_n` and `vector<u8, N>` into the same kind**
   — they're wire-equivalent in pSSZ. Current design keeps them distinct
   for JSON-rendering policy reasons (a `bytes_n` typically renders as
   `"0x..."`; a `vector<u8>` might render as `[1, 2, 3]` if the schema
   author wanted a list-of-numbers).
- [ ] **PHF size cap** — 256 entries (size_log2 = 8). Containers with K >
   256 fields would fail PHF construction. Accept as out-of-scope (nobody
   has 256-field structs in practice) or fall back to hash-prefilter for
   such mega-containers.
- [ ] **Recursive type detection during validate** — currently allowed via
   container fields; cycle traversal needs a visited set during validate.

## Status

Design ready for prototyping. Implementation lands as
`libraries/psio/cpp/include/psio/psch_writer.hpp` (build-time) and
`libraries/psio/cpp/include/psio/psch_view.hpp` (read-only, header-only).
