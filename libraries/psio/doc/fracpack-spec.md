# Fracpack Wire Format Specification

Fracpack is a binary serialization format designed for zero-copy reads and
in-place mutation. It combines the read performance of flat serialization formats
with the ability to modify individual fields without deserializing the entire
message.

Key properties:
- Zero-copy reads via typed views over packed data
- In-place mutation with growth (fast mode and canonical mode)
- Bounded offset patching (self-relative offsets, not absolute)
- Forward-only relative pointers (no cycles, guaranteed bounded traversal)
- Well-defined canonical form for hashing, signing, and consensus
- Forward and backward schema compatibility via extensible structs

This document describes the binary wire format only. It does not describe the
C++ or Rust reflection, encoding, or decoding APIs.

## Table of Contents

- [Primitive Types](#primitive-types)
- [Fixed-Size vs Variable-Size Classification](#fixed-size-vs-variable-size-classification)
- [Offset Pointers](#offset-pointers)
- [Variable-Size Containers](#variable-size-containers)
- [Option\<T\>](#optiont)
- [Tagged Unions (Variants)](#tagged-unions-variants)
- [Structs](#structs)
- [Tuples](#tuples)
- [Fixed-Size Arrays](#fixed-size-arrays)
- [Trailing Optional Elision](#trailing-optional-elision)
- [Canonical Form](#canonical-form)
- [In-Place Mutation](#in-place-mutation)
- [Verification](#verification)
- [Serialization Compatibility](#serialization-compatibility)

---

## Primitive Types

All scalars are encoded in **little-endian** byte order with no padding or
alignment. They are **fixed-size**: their packed representation is always the
same number of bytes regardless of value.

| Type          | Size    | Encoding                  |
|---------------|---------|---------------------------|
| `bool`        | 1 byte  | 0x00=false, 0x01=true     |
| `u8`, `i8`    | 1 byte  | raw byte                  |
| `u16`, `i16`  | 2 bytes | little-endian             |
| `u32`, `i32`  | 4 bytes | little-endian             |
| `u64`, `i64`  | 8 bytes | little-endian             |
| `f32`         | 4 bytes | IEEE 754, little-endian   |
| `f64`         | 8 bytes | IEEE 754, little-endian   |

Scoped enums are encoded as their underlying integer type.

## Fixed-Size vs Variable-Size Classification

Fracpack classifies every type as either **fixed-size** or **variable-size**.
This classification determines how a value is embedded in its parent container.

A type is **fixed-size** if its packed representation is always the same number
of bytes. Fixed-size types are embedded inline in their parent's fixed region.

A type is **variable-size** if its packed representation can vary. Variable-size
types occupy a 4-byte offset pointer in their parent's fixed region, with their
actual data in the parent's heap region.

Classification rules:
- Primitives are fixed-size
- Non-extensible structs containing only fixed-size fields are fixed-size
- Fixed-length arrays of fixed-size elements are fixed-size
- Everything else is variable-size: strings, vectors, optionals, variants,
  extensible structs (the default), tuples, and any non-extensible struct
  containing a variable-size field

## Offset Pointers

All pointers in fracpack are **u32 relative offsets**. The offset value is added
to the absolute position of the offset field itself to compute the target
address:

```
target_address = offset_field_position + offset_value
```

Every offset is self-relative, not relative to the start of the message. This
property is critical for in-place mutation: changing one field's data only
requires patching that field's offset, not any sibling offsets.

All offsets point **forward** (to higher addresses). This structural invariant
prevents pointer cycles and guarantees bounded traversal depth — a property that
absolute-offset formats like FlatBuffers and Cap'n Proto cannot enforce
structurally.

### Reserved Offset Values

Values 0–3 point within the offset field itself and have special meaning:

| Value | Meaning |
|-------|---------|
| 0     | Empty container (empty String, empty Vec) |
| 1     | None/null (`Option<T>` with no value) |
| 2, 3  | Reserved for future use |

An offset >= 4 is a valid pointer to heap data.

An empty container MUST use offset 0, not an offset pointing to a zero-length
container on the heap.

## Variable-Size Containers

### String

```
[u32 byte_count][UTF-8 bytes]
```

- `byte_count`: number of UTF-8 bytes (not characters), as u32 LE
- Followed by exactly `byte_count` bytes of UTF-8 data
- Empty string: uses offset 0 at the embedding site (no data on heap)

Example: `"hello"` encodes as:
```
05 00 00 00  68 65 6c 6c 6f
^byte_count  ^UTF-8 data
```

### Vec\<T\>

```
[u32 data_size][element fixed regions][element heap data]
```

- `data_size`: total size in bytes of the element fixed regions =
  `len * T::FIXED_SIZE`
- For fixed-size T: elements are packed contiguously; `data_size` is the total
- For variable-size T: each element occupies `T::FIXED_SIZE` bytes in the
  fixed region (typically a 4-byte relative offset), followed by heap data
- Empty vector: uses offset 0 at the embedding site (no data on heap)

The vector length is derived: `len = data_size / T::FIXED_SIZE`. The
`data_size` must be an exact multiple of `T::FIXED_SIZE`.

Example: `vec![10u32, 20, 30]` encodes as:
```
0c 00 00 00   # data_size = 12 (3 elements * 4 bytes)
0a 00 00 00   # element 0 = 10
14 00 00 00   # element 1 = 20
1e 00 00 00   # element 2 = 30
```

Example: `vec!["hi", "bye"]` encodes as:
```
08 00 00 00   # data_size = 8 (2 offsets * 4 bytes)
08 00 00 00   # offset to "hi" (relative to this field's position)
0a 00 00 00   # offset to "bye" (relative to this field's position)
02 00 00 00   # "hi": byte_count = 2
68 69         # "hi": data
03 00 00 00   # "bye": byte_count = 3
62 79 65      # "bye": data
```

## Option\<T\>

Options are always variable-size, with a 4-byte offset in the fixed region.

### Encoding

| Value           | Offset | Heap Data               |
|-----------------|--------|--------------------------|
| `None`          | 1      | (none)                   |
| `Some(empty_T)` | 0      | (none — T is empty)      |
| `Some(value)`   | >= 4   | T's packed representation|

The heap data contains T's packed form directly (not wrapped in Option
encoding):
- `Some("hello")` stores `05 00 00 00 68 65 6c 6c 6f` on the heap (String
  encoding)
- `Some(42u32)` stores `2a 00 00 00` on the heap

### Distinction Between Offset=0 and Offset=1

- Offset 0: "empty container" — the optional **has a value**, but that value is
  empty (e.g., `Some("")` for `Option<String>`, `Some(vec![])` for
  `Option<Vec<T>>`)
- Offset 1: "no value" — the optional is `None`

For types that don't have a natural empty state, the encoder writes the full
value on the heap with an offset >= 4.

### Space-Saving for Nested Optionals

When `T` is itself a variable-size, non-optional type, `Option<T>` reuses T's
offset pointer directly. The offset either means "None" (value 1) or points to
T's data (same as T would use). This avoids an extra level of indirection.

## Tagged Unions (Variants)

Tagged unions (`std::variant` in C++, `enum` in Rust) are always variable-size.

### Encoding

```
[u8 tag][u32 data_size][variant data]
```

- `tag`: 0-indexed variant discriminant (0 for first alternative, 1 for second,
  etc.). The high bit must be clear, so the maximum tag value is 127.
- `data_size`: byte count of the variant's packed data, as u32 LE
- `variant data`: the packed representation of the selected alternative's type

The total packed size is always `5 + data_size`.

Example for `variant<u32, String>`:

Alternative 0 with value 255:
```
00              # tag = 0
04 00 00 00     # data_size = 4
ff 00 00 00     # u32 value = 255
```

Alternative 1 with value "sky":
```
01              # tag = 1
07 00 00 00     # data_size = 7 (4-byte length + 3 bytes)
03 00 00 00     # String byte_count = 3
73 6b 79        # "sky"
```

When a variant is a field of a struct, the fixed region contains a 4-byte
relative offset pointing to the `[tag][size][data]` block on the heap.

## Structs

Structs have two encoding modes controlled by the `definitionWillNotChange`
attribute (called `definition_will_not_change` in Rust).

### Non-Extensible (Definition Will Not Change) Structs

Non-extensible structs opt out of forward compatibility. Non-extensibility
requires an explicit annotation; fracpack considers structs **extensible by
default**.

Fields are packed directly with no extensibility header:

```
[field_0 fixed][field_1 fixed]...[field_N fixed][heap data]
```

- **Fixed region**: each field occupies its `FIXED_SIZE` bytes
  - Fixed-size fields (u32, bool, etc.): value is inline
  - Variable-size fields (String, Vec, etc.): 4-byte relative offset to heap
- **Heap region**: variable-size field data in field declaration order, packed
  contiguously with no gaps

A non-extensible struct containing only fixed-size fields is itself fixed-size.
Its total size is the sum of its fields' sizes. When embedded in another struct,
it is inline (no offset needed).

A non-extensible struct containing any variable-size fields is variable-size.
When embedded in another struct, it uses a 4-byte offset in the parent's fixed
region. It still has no `fixed_size` header of its own — the decoder knows the
fixed region size from the type definition.

Example:
```
struct Point { x: f32, y: f32 }  // definitionWillNotChange, all fixed-size
```

`Point { x: 1.0, y: 2.0 }` encodes as:
```
00 00 80 3f   # x = 1.0 (f32 LE)
00 00 00 40   # y = 2.0 (f32 LE)
```

This encoding is byte-identical to a `#pragma pack(1)` C struct on
little-endian platforms — no header, no offsets, just raw field values.

### Extensible Structs (Default)

Extensible structs prepend a **u16 `fixed_size`** header that enables forward
compatibility:

```
[u16 fixed_size][field_0 fixed]...[field_N fixed][heap data]
```

- `fixed_size`: number of bytes in the fixed region (after the u16 header,
  before the heap data). This does **not** include the u16 header itself or the
  heap data.
- Fixed-size fields are inline in the fixed region
- Variable-size fields have 4-byte offsets in the fixed region pointing to heap
  data
- Heap data follows the fixed region, in field declaration order, with no gaps

The `fixed_size` header enables a decoder built against an older schema to skip
unknown trailing fields added by a newer schema. If the decoder encounters a
`fixed_size` larger than expected, it knows the extra bytes are fields it
doesn't recognize and can safely skip them.

Extensible structs are always variable-size (the u16 header makes them so).
When embedded in another struct, they use a 4-byte offset in the parent's fixed
region.

The sum of all fields' fixed sizes must fit in a u16 (max 65535 bytes).

Example:
```
struct Message {
    id: u32,
    body: String,
    reply_to: Option<u64>,
}
```

`Message { id: 1, body: "hi", reply_to: Some(42) }`:
```
# Extensibility header:
0c 00                 # fixed_size = 12 (u32 + offset + offset)

# Fixed region (12 bytes):
01 00 00 00           # id = 1
0c 00 00 00           # body offset (relative to this field's position)
10 00 00 00           # reply_to offset (relative to this field's position)

# Heap:
02 00 00 00 68 69     # body: "hi" (length 2 + data)
2a 00 00 00 00 00 00 00  # reply_to inner: u64 = 42
```

## Tuples

Tuples have the same encoding as extensible structs: a `u16 fixed_size` header
followed by element fixed regions and heap data. Tuples are always extensible
(there is no non-extensible option for tuples), so they are always
variable-size.

```
[u16 fixed_size][elem_0 fixed]...[elem_N fixed][heap data]
```

Trailing optional elision applies to tuples the same way it applies to
extensible structs.

## Fixed-Size Arrays

`[T; N]` arrays have a fixed compile-time length N.

### Fixed-Size Element Type

All elements are packed inline, contiguously, with no length prefix:
```
[elem_0][elem_1]...[elem_{N-1}]
```

Total size: `N * T::FIXED_SIZE` bytes. The array is itself fixed-size.

### Variable-Size Element Type

When embedded in a struct, the array uses a single 4-byte offset in the parent's
fixed region. The offset points to:

```
[elem_0 offset][elem_1 offset]...[elem_{N-1} offset][heap data]
```

Each element has its own 4-byte relative offset pointing to its heap data.
There is no length prefix because the length is known from the type.

## Trailing Optional Elision

In extensible structs and tuples, trailing `Option` fields that are `None` are
**omitted from the packed representation** to save space:

### Packing Rule

1. Scan fields from first to last. Track the index of the last field that is
   either non-optional or optional-with-a-value.
2. Only encode fixed-region slots up to and including that index.
3. Set `fixed_size` to reflect the shortened fixed region.

Fields past the last non-None field are simply absent from the packed data.

### Unpacking Rule

When unpacking, if a field's fixed-region position falls at or past the end of
the fixed region (as indicated by `fixed_size`), the field is treated as its
default value: `None` for optionals.

Non-optional fields that fall past `fixed_size` indicate a newer-schema value
being read by an older decoder — the field is required by the type but not
present in the data. This is only valid when the field was added in a newer
schema version and the serialization compatibility rules allow it.

### Example

```
struct Config {
    name: String,           // required (index 0)
    opt_a: Option<u32>,     // optional (index 1)
    opt_b: Option<String>,  // optional (index 2)
}
```

`Config { name: "x", opt_a: Some(42), opt_b: None }`:
- Last non-None field is `opt_a` at index 1
- `opt_b` (index 2) is trailing and None, so it's elided
- `fixed_size` = `name`'s fixed_size + `opt_a`'s fixed_size = 4 + 4 = 8

`Config { name: "x", opt_a: None, opt_b: None }`:
- Last non-None field is `name` at index 0
- Both `opt_a` and `opt_b` are trailing and None, so they're elided
- `fixed_size` = `name`'s fixed_size = 4

### Canonical Constraint

In canonical form, the last fixed-region field MUST NOT be an empty optional
(offset=1). If it would be, it should have been elided. A decoder that
encounters a trailing offset=1 in the last position can reject the data as
non-canonical.

## Canonical Form

The **canonical form** of a fracpack message has these properties:

1. **No dead bytes**: every byte in the buffer is accounted for by the format
2. **Sequential heap layout**: heap data for field N appears before heap data
   for field N+1 (in declaration order)
3. **Minimal trailing elision**: trailing None optionals are omitted (not
   encoded with offset=1)
4. **Tight buffer**: buffer length equals exactly the packed size
5. **Empty containers use offset 0**: not a pointer to a zero-length container

The canonical form is what the standard `pack()` / `packed()` functions produce.
Two values are semantically equal if and only if their canonical forms are
byte-identical.

Non-canonical forms (dead bytes, out-of-order heap data, un-elided trailing
Nones) are valid and decodable. They arise naturally from fast-mode in-place
mutations. The canonical form is available when needed — for hashing, signing,
consensus comparison, or storage normalization.

## In-Place Mutation

Fracpack supports mutating individual fields within an existing packed buffer
without deserializing the entire message. Two mutation modes are available.

### Canonical Mode

Each setter immediately produces canonical output using splice-and-patch:

1. **Splice**: replace the old field data with the new packed data at the same
   position, shifting the buffer tail with `memmove`
2. **Patch**: adjust all sibling variable-size field offsets that point past the
   splice point by the size delta

**Cost per mutation**:
- Fixed-size field: O(1) overwrite
- Variable-size field: O(buffer_tail) memmove + O(num_variable_fields) patches

The buffer is always in canonical form after each setter call.

### Fast Mode

Setters produce non-canonical but readable data using overwrite-or-append:

**Shrink** (new value fits within old space):
1. Overwrite the field's heap data in place
2. Dead bytes remain after the new data (unused tail of old allocation)
3. No offset patching needed
4. Buffer size is unchanged

**Cost**: O(new_value_size) copy. No memmove, no offset patching.

**Grow** (new value is larger than old space):
1. Append the new packed data to the end of the buffer
2. Update the field's offset to point to the appended position
3. Old data at the original position becomes dead bytes
4. No sibling offset patching needed

**Cost**: O(new_value_size) copy + 4-byte offset write.

**Option Some-to-None**:
1. Set the offset to 1. Old heap data becomes dead bytes.

**Cost**: O(1) single 4-byte write.

**Option None-to-Some** (non-elided fields only):
1. Append T's packed data to the end of the buffer
2. Set the offset to point to the appended position

**Cost**: O(value_size) copy + 4-byte offset write.

### Compact

After fast-mode mutations, call `compact()` to restore canonical form. This
reads each field via offset-following getters, constructs the canonical
representation, and repacks. Cost: O(canonical_size).

### Why Relative Offsets Enable This

Self-relative offsets (`target = field_position + offset`) make in-place
mutation practical:

- **No global fixups on grow**: appending data and updating one offset doesn't
  affect any other offset, because each offset is relative to its own position
- **No global fixups on shrink**: overwriting in place doesn't change any
  positions or offsets
- **Canonical splice needs only sibling patches**: splicing shifts the buffer
  tail, but only offsets in the same struct's fixed region (whose targets are
  past the splice point) need adjustment. Parent and child offsets are
  unaffected.

Compare with absolute offsets (FlatBuffers): changing one field's size requires
patching every offset in the entire buffer that points past the edit point.

### Mutation vs Unpack-Repack

For modifying a single field in a large struct, in-place mutation avoids:

1. **N allocator calls**: unpack scatters data across the heap
2. **Cache pollution**: scattered allocations touch scattered cache lines
3. **Double copy**: unpack copies data out, repack copies it back

For fast mode with multiple edits, the total work is O(sum of new value sizes) +
one O(canonical_size) compact, compared to O(full_struct_size) * 2 for
unpack-modify-repack.

### Choosing a Mode

| Scenario | Recommended Mode |
|----------|-----------------|
| Single field edit, need canonical immediately | Canonical |
| Multiple edits, then store/transmit | Fast + compact() |
| Read-heavy with rare edits | Canonical |
| Batch updates (e.g., database page) | Fast + compact() |

## Verification

Verification validates that packed data conforms to canonical form. After
verification, zero-copy views can read fields without bounds checking.
Verification is O(n) in the data size and touches each byte at most once.

### Rules

- `bool` values MUST be 0 or 1
- All offset pointers MUST be in bounds
- Every regular offset pointer MUST point to the position immediately following
  the preceding object's data (sequential heap layout)
- An offset to an empty vector or string MUST be 0 (not a pointer to a
  zero-length container)
- The `data_size` of a vector MUST be an exact multiple of the element's
  fixed size
- The `data_size` of a variant MUST match the packed size of the inner type
  (when the inner type's size is statically known)
- The last field of a tuple or extensible struct MUST NOT be an empty optional
  (trailing Nones must be elided, not encoded)
- Unknown fixed data (from newer schema fields) SHOULD be a multiple of 4 bytes,
  and each 4-byte group SHOULD be a valid offset pointer
- Variant tags MUST have the high bit clear (max value 127)
- No extra bytes may remain after the packed data

## Serialization Compatibility

A serialized object of type `T` may be deserialized as type `U` under these
rules:

**Same type**: `T` is the same type as `U`.

**Non-extensible structs**: `T` and `U` are both non-extensible with the same
number of fields, and each field of `T` can be deserialized as the
corresponding field of `U`.

**Extensible structs and tuples (same size)**: `T` and `U` are both extensible
structs or both tuples, with the same number of fields, and each field of `T`
can be deserialized as the corresponding field of `U`.

**Extensible structs and tuples (T has fewer fields)**: `T` has fewer fields
than `U`. Every field of `T` can be deserialized as the corresponding field of
`U`. Every trailing field of `U` without a corresponding field in `T` must be
optional. Those trailing fields will be set to `None`.

**Extensible structs and tuples (T has more fields)**: `T` has more fields
than `U`. Every field of `T` that has a corresponding field in `U` can be
deserialized as the corresponding field of `U`. Every trailing field of `T`
without a corresponding field in `U` must be optional. Those trailing fields
will be dropped.

**Unions**: `U` has at least as many alternatives as `T`, and every alternative
of `T` can be deserialized as the corresponding alternative of `U`.

**Containers**: `T` and `U` are the same kind of container (optional, array, or
vector), and the value type of `T` can be deserialized as the value type of `U`.
