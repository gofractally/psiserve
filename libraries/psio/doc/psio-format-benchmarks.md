# PSIO: One Reflection Macro, Every Wire Format

## The Headline

PSIO reads Cap'n Proto wire format **10–36x faster** than the official Cap'n Proto library. It reads and writes FlatBuffers wire format **at parity** with the official FlatBuffers library. It does both from a single `PSIO_REFLECT` macro — no IDL files, no code generation, no generated types.

```cpp
struct UserProfile {
   uint64_t                   id;
   std::string                name;
   std::string                email;
   std::optional<std::string> bio;
   uint32_t                   age;
   double                     score;
   std::vector<std::string>   tags;
   bool                       verified;
};
PSIO_REFLECT(UserProfile, id, name, email, bio, age, score, tags, verified)

// That one macro gives you:
auto fp = psio::fracpack(user);           // Fracpack binary
auto cp = psio::cp::pack(user);           // Cap'n Proto wire format
auto fb = psio::to_flatbuf(user);         // FlatBuffers wire format
auto wt = psio::wit::pack(user);          // WIT Canonical ABI
auto js = psio::to_json(user);            // JSON (RapidJSON)
auto bn = psio::to_bin(user);             // Compact binary
auto bc = psio::to_bincode(user);         // Bincode (Rust compat)
auto av = psio::to_avro(user);            // Apache Avro

// Zero-copy views over any format:
auto fv = psio::view<UserProfile, psio::frac>::from(fp);   // Fracpack view
auto cv = psio::view<UserProfile, psio::cp>::from(cp);     // Cap'n Proto view
auto bv = psio::fb_view<UserProfile>::from(fb);            // FlatBuffers view
auto wv = psio::view<UserProfile, psio::wit>::from(wt);    // WIT view
```

One set of types. Eight wire formats. Zero codegen.

---

## Cap'n Proto: PSIO vs Official Library

PSIO reads Cap'n Proto wire format using pointer arithmetic over the same 8-byte-aligned segments. The official library uses `StructReader` with per-access pointer validation and `MessageReader` construction overhead. PSIO's `view<T, cp>` skips reader construction entirely — it computes field offsets at compile time from the reflected struct layout.

### View-All: Read Every Field (ns)

| Schema              | PSIO `view<T,cp>` | Official Cap'n Proto | Speedup |
|---------------------|-------------------:|---------------------:|--------:|
| **Point**           |              0.6   |                17    | **28x** |
| **Token**           |              0.9   |                21    | **23x** |
| **UserProfile**     |              1.6   |                58    | **36x** |
| **Order**           |              1.6   |                39    | **24x** |
| **SensorReading**   |              2.9   |                33    | **11x** |

PSIO views are **11–36x faster** than the official Cap'n Proto reader for viewing all fields. The official library's `StructReader` performs pointer validation, segment bounds checks, and default-value XOR on every field access. PSIO resolves offsets at compile time and reads fields via raw pointer arithmetic.

### View-One: Single Field Access (ns)

| Schema.field               | PSIO `view<T,cp>` | Official Cap'n Proto | Speedup |
|----------------------------|-------------------:|---------------------:|--------:|
| **UserProfile.id**         |              0.4   |                18    | **45x** |
| **UserProfile.name**       |              0.7   |                21    | **30x** |
| **Order.customer.name**    |              1.1   |                25    | **23x** |

For single-field access, PSIO is **23–45x faster**. Accessing `UserProfile.id` costs 0.4ns (one pointer read) vs 18ns with the official library (MessageReader + StructReader construction + pointer traversal).

### Pack: Build Wire Bytes (ns)

| Schema              | PSIO `cp::pack` | Official Cap'n Proto | Speedup |
|---------------------|----------------:|---------------------:|--------:|
| **Point**           |             39  |               117    | **3.0x** |
| **Token**           |             57  |               109    | **1.9x** |
| **UserProfile**     |            115  |               136    | **1.2x** |
| **Order**           |            201  |               205    |  ~1.0x   |
| **SensorReading**   |            120  |               112    |  ~1.0x   |

PSIO pack is **1–3x faster** for simple types. The official library's `MallocMessageBuilder` has fixed overhead (~100ns) regardless of struct size, making it particularly slow for small types.

### Unpack: Wire Bytes to Native Struct (ns)

| Schema              | PSIO `cp::unpack` | Official Cap'n Proto | Speedup |
|---------------------|------------------:|---------------------:|--------:|
| **Point**           |             0.5   |                20    | **40x** |
| **Token**           |             4.7   |                28    | **6.0x** |
| **UserProfile**     |              47   |               123    | **2.6x** |
| **Order**           |             101   |               233    | **2.3x** |
| **SensorReading**   |              14   |                33    | **2.4x** |

PSIO unpack is **2–40x faster**. The official library's reader API doesn't have a direct "unpack to plain struct" — it always goes through reader objects with per-field accessor overhead.

### Wire Compatibility

Both produce identical wire bytes. Same segments, same pointer layout, same data sections. PSIO's output is readable by official Cap'n Proto readers and vice versa.

### Why the Gap?

The official Cap'n Proto library was designed for safety-first multi-segment messages with:
- `MessageReader` construction per access (~15ns baseline)
- `StructReader` pointer traversal with bounds checking
- Far pointer resolution (multi-segment support PSIO skips)
- Per-access default-value XOR

PSIO's `view<T, cp>` assumes single-segment messages (the common case) and resolves all offsets at compile time via the reflected struct layout. Field access is a computed pointer offset — the same thing the CPU does for native struct access.

---

## FlatBuffers: PSIO vs Official Library

PSIO implements FlatBuffers wire format in two ways:
- **`psio::to_flatbuf` / `psio::from_flatbuf`** — reflection-based, uses `PSIO_REFLECT`
- **`psio::fb_builder`** — standalone builder with zero dependency on the FlatBuffers library

Both produce wire-compatible output verified against the official FlatBuffers reader.

### View-All: Read Every Field (ns)

| Schema              | PSIO `fb_view` | Official FlatBuffers | Ratio |
|---------------------|---------------:|---------------------:|------:|
| **Point**           |           0.7  |                0.7   | 1.0x  |
| **Token**           |           1.1  |                1.0   | 1.1x  |
| **UserProfile**     |           2.1  |                2.4   | 0.9x  |
| **Order**           |           1.5  |                1.8   | 0.8x  |
| **SensorReading**   |           4.1  |                5.2   | 0.8x  |

Views are **at parity** — both are sub-5ns for all types. FlatBuffers' vtable-based layout is inherently efficient for reads. PSIO matches this despite generating accessors from reflection rather than from `flatc` codegen.

### Pack: Build Wire Bytes (ns)

| Schema              | PSIO `to_flatbuf` | PSIO `fb_builder` | Official FlatBuffers | PSIO vs Official |
|---------------------|------------------:|------------------:|---------------------:|-----------------:|
| **Point**           |              31   |              52   |                36    |  0.9x            |
| **Token**           |              42   |              43   |                40    |  ~1.0x           |
| **UserProfile**     |             103   |             112   |               107    |  ~1.0x           |
| **Order**           |             268   |             261   |               336    |  **1.3x faster** |
| **SensorReading**   |              83   |             110   |                82    |  ~1.0x           |

Pack is **at parity** for most types. For nested types (Order), PSIO's reflection-based pack is **1.3x faster** than official FlatBuffers — the `flatc`-generated builder code has more overhead for nested table construction.

### Unpack: Wire Bytes to Native Struct (ns)

| Schema              | PSIO `from_flatbuf` | Official `UnPack()` | Speedup |
|---------------------|--------------------:|--------------------:|--------:|
| **Point**           |              0.7    |              0.6    |  ~1.0x  |
| **Token**           |              6.8    |               15    | **2.2x** |
| **UserProfile**     |               58    |               55    |  ~1.0x  |
| **Order**           |              117    |              260    | **2.2x** |
| **SensorReading**   |               14    |               21    | **1.5x** |

PSIO unpack is **up to 2.2x faster** for types with strings (Token, Order). PSIO unpacks directly into user-defined structs. Official `UnPack()` produces generated `*T` types that must be manually converted to application types.

### Wire Size

| Schema              | PSIO (dedup) | Official | Difference |
|---------------------|-------------:|---------:|-----------:|
| **Point**           |        32 B  |    32 B  | identical  |
| **Token**           |        56 B  |    56 B  | identical  |
| **UserProfile**     |       264 B  |   264 B  | identical  |
| **Order**           |       576 B  |   584 B  | PSIO 8B smaller |
| **SensorReading**   |       208 B  |   208 B  | identical  |

Wire sizes are identical or PSIO is slightly smaller (string deduplication in Order).

### Schema Generation

PSIO can also generate the `.fbs` schema file from reflected types — useful for interop with other FlatBuffers-using systems:

```cpp
// Auto-generated from PSIO_REFLECT:
table UserProfile {
  id: ulong;
  name: string;
  email: string;
  bio: string;
  age: uint;
  score: double;
  tags: [string];
  verified: bool;
}
```

---

## WIT Canonical ABI: Zero-Copy Views of WASM Component Model Data

PSIO implements the WASM Component Model's Canonical ABI encoding — natural alignment with (pointer, length) pairs for strings and lists. `WView<T>` provides zero-copy views, and `wit_mut<T>` supports in-place scalar mutation.

### Performance (ns)

| Schema              | WIT Pack | WIT Unpack | WIT View-All | WIT View-One |
|---------------------|:--------:|:----------:|:------------:|:------------:|
| **Point**           |    10    |     <1     |      <1      |     —        |
| **Token**           |    11    |      3     |      0.6     |     —        |
| **UserProfile**     |    31    |     40     |      2.6     |     0.2      |
| **Order**           |    59    |     82     |      3.9     |     0.3      |
| **SensorReading**   |    27    |     14     |      2.9     |     0.2      |

WIT pack is competitive with fracpack (sometimes faster due to simpler layout). Views are sub-4ns for all types. In-place scalar mutation is sub-nanosecond (0.3ns).

---

## All Formats from One Macro: Comparative Summary

### Wire Size (bytes)

| Schema              | Fracpack | WIT    | Cap'n Proto | FlatBuffers | Binary | Bincode | Avro   | JSON   |
|---------------------|:--------:|:------:|:-----------:|:-----------:|:------:|:-------:|:------:|:------:|
| **Point**           |  16      |  16    |  32         |  32         |  16    |  16     |  16    |  43    |
| **Token**           |  35      |  35    |  56         |  56         |  26    |  33     |  20    |  62    |
| **UserProfile**     | 211      | 223    | 264         | 264         | 154    | 210     | 149    | 231    |
| **Order**           | 449      | 454    | 544         | 576         | 308    | 413     | 286    | 602    |
| **SensorReading**   | 157      | 161    | 184         | 208         | 138    | 152     | 136    | 325    |

### View-All Speed (ns, zero-copy formats only)

| Schema              | Fracpack | WIT WView | PSIO cp view | PSIO fb view | Official Cap'n Proto | Official FlatBuffers |
|---------------------|:--------:|:---------:|:------------:|:------------:|:--------------------:|:--------------------:|
| **Point**           |  0.3     |  <1       |  0.6         |  0.7         |  17                  |  0.7                 |
| **Token**           |  1.0     |  0.6      |  0.9         |  1.1         |  21                  |  1.0                 |
| **UserProfile**     |  1.3     |  2.6      |  1.6         |  2.1         |  58                  |  2.4                 |
| **Order**           |  1.0     |  3.9      |  1.6         |  1.5         |  39                  |  1.8                 |
| **SensorReading**   |  2.9     |  2.9      |  2.9         |  4.1         |  33                  |  5.2                 |

Every PSIO view implementation — fracpack, WIT, Cap'n Proto, FlatBuffers — runs at **1–4ns** for all types. The official Cap'n Proto library runs at **17–58ns** for the same wire format, the same data.

### Pack Speed (ns)

| Schema              | Fracpack | WIT    | PSIO cp pack | PSIO fb pack | Binary | Bincode | Avro   | JSON    |
|---------------------|:--------:|:------:|:------------:|:------------:|:------:|:-------:|:------:|:-------:|
| **Point**           |  11      |  10    |  39          |  31          |  10    |  9      |  10    |  501    |
| **Token**           |  23      |  11    |  57          |  42          |  21    |  21     |  28    |  366    |
| **UserProfile**     |  40      |  31    | 115          | 103          |  40    |  36     |  47    | 1,508   |
| **Order**           | 100      |  59    | 201          | 268          |  79    |  89     | 104    | 5,030   |
| **SensorReading**   |  28      |  27    | 120          |  83          |  31    |  29     |  43    | 3,805   |

Fracpack and WIT are fastest for packing. Cap'n Proto and FlatBuffers wire formats have inherently more overhead (alignment padding, pointer construction, vtable building).

---

## What This Means

**You don't choose a serialization format at design time and live with it forever.** With PSIO, you write your types once with `PSIO_REFLECT` and serialize to whatever format the consumer needs:

- **Storage**: Fracpack (smallest zero-copy format, in-place mutation, schema evolution)
- **WASM boundaries**: WIT Canonical ABI (Component Model standard)
- **Interop with Cap'n Proto systems**: Cap'n Proto wire format, 10–36x faster than the official library
- **Interop with FlatBuffers systems**: FlatBuffers wire format, at parity with the official library
- **Network/logs**: JSON
- **Rust interop**: Bincode
- **Compact archival**: Avro or Binary

All from the same types, the same reflection, the same codebase. No IDL files to maintain. No codegen steps in the build. No generated types to convert to and from.

---

**Platform**: Apple M5 Max, macOS, 2026-04-15. All benchmarks use `std::chrono::high_resolution_clock` with 200 warm-up iterations, 30ms calibration, and auto-batched timing. Dead-code elimination prevented via `asm volatile` compiler barriers.
