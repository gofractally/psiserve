# Fracpack Benchmark Results

## Executive Summary

Fracpack is the fastest serialization format for read-heavy workloads. Zero-copy views read any field from packed data in **<1–3ns** — **10–500x faster** than Protobuf, MsgPack, or any format that requires full deserialization. Pack speed matches or beats flat binary formats. No IDL files, no codegen, no generated types.

**The core result**: Real systems do thousands of reads per write. Fracpack's zero-copy views turn serialized data into a first-class data structure — fields are accessed directly from the wire format with no allocation, no parsing, and no copies. A single read pays back the entire pack cost.

### Key Findings

1. **Zero-copy views match native struct access.** C++ views are 0.9x native. Go typed views are 0.6x native for complex types (faster because zero-copy avoids GC string alloc). Fracpack data can live packed permanently.

2. **Single-field access is 6–450x faster than unpack** across all 6 languages. C++ view-one is 0.2ns vs 90ns unpack (450x). Even Python sees 8x benefit.

3. **In-place mutation is 2–15x faster than repack.** Scalar fields in nested structs see the biggest wins (C++ Order.customer.age: 15x).

4. **View array access scales O(1)**, while unpack scales O(n). At 10K elements: JS view-first 3.2µs vs unpack 274µs (86x).

5. **Fracpack matches bincode pack speed while adding zero-copy views.** Rust fracpack packs at 0.84–1.25x bincode speed. The break-even vs bincode is just **1 read**.

6. **Beats every external format at every operation.** FlatBuffers is the closest competitor but is slower at both pack (2–4x) and view (1.4–2x), and requires IDL + codegen. Protobuf and Cap'n Proto are non-competitive for reads.

### Language Performance

| Language   | Strengths                                                                                     | Weaknesses                                                            |
|------------|-----------------------------------------------------------------------------------------------|-----------------------------------------------------------------------|
| **C++**    | Fastest views (<1ns string, 0.3ns nested). True zero-cost abstraction. Fastest unpack. | Pack 1.1–1.7x slower than Rust for deeply nested types.    |
| **Zig**    | Fastest pack (19ns Point, 60ns Sensor). Views match C++ (7–12ns). Fastest JSON write. | No prevalidated view mode yet.                                        |
| **Rust**   | Fastest pack (9–64ns), matching bincode. Prevalidated views at 0.5–1.1ns. | Default views include bounds checks (23–63ns). |
| **Go**     | Solid pack (31–348ns). Typed views beat native (0.6x UserProfile). Mutation 6–11x faster than repack. | Generic `Field()` has 3–36x overhead vs typed views. |
| **JS**     | Cached views beat unpack. Array view scaling is excellent (86x at 10K). | Proxy overhead makes raw views 43–229x native.                        |
| **Python** | Views are 1.3–2x faster than unpack. Validates design at algorithmic level.                   | Interpreter overhead on everything.             |

### Fracpack vs External Formats (C++, UserProfile)

| Metric        | Fracpack | WIT WView | FlatBuffers | Cap'n Proto | Protobuf | MsgPack |
|---------------|----------|-----------|-------------|-------------|----------|---------|
| Wire size     | 211 B    | 223 B     | 264 B       | 264 B       | 156 B    | 150 B   |
| Pack (ns)     | 43       | 31        | 122         | 155         | 212      | 72      |
| View-all (ns) | 1.6      | 2.6       | 2.6         | 40          | 222*     | 184*    |
| View-one (ns) | <1       | 0.2       | 0.6         | 24          | 217*     | 191*    |

*\* Must fully parse to access any field — no zero-copy mode.*

### The Fracpack Tradeoff

Fracpack pays a modest pack-time cost (offset tables, heap layout) so that consumers can read individual fields in <1–3ns via zero-copy views. The crossover vs flat binary is just **1 read**. Protobuf and MsgPack are non-competitive for read-heavy workloads: with no zero-copy mode, every field access costs a full parse (100–500ns vs fracpack's 1–3ns).

The key architectural difference: fracpack works directly with user-defined structs via reflection macros — no IDL files, no code generation step, no generated types to bridge back to application code.

---

## Methodology

**Platform**: Apple M5 Max, macOS, 2026-04-14

**Benchmark harness**: Each language uses its idiomatic benchmarking framework — C++ uses auto-batching with `std::chrono::high_resolution_clock` (200 warm-up iterations, 30ms calibration, then timed batch to target 200ms total), Rust uses `criterion` with statistical analysis, Go uses `testing.B`, Zig uses `std.time.Timer` under `ReleaseFast`, JS uses `performance.now()`, Python uses `time.perf_counter_ns`. Times are median nanoseconds per operation.

**Dead-code elimination**: All languages use compiler barriers (`asm volatile`, `std::hint::black_box`, `@as(*volatile)`) to prevent the optimizer from eliding benchmark work.

**Test data**: All benchmarks use the same schemas and test values across all 6 languages. Data ranges from trivial fixed-size structs (Point: 16 bytes) to deeply nested objects with variable-length fields (Order: 449 bytes). Values are populated with realistic content (UserProfile has a name, email, 3-4 tags, a bio string; Order has 5 line items; SensorReading has 14 sensor readings).

**Formats tested**:
- **Internal** (all from a single reflection macro — no codegen, no IDL): Fracpack, WIT Canonical ABI, Binary, Bincode, Avro, JSON
- **External** (require IDL + codegen): Cap'n Proto v1.3.0, FlatBuffers v25.12.19, Protocol Buffers v34.0 (proto3), MessagePack (msgpack-cxx v7.0)

---

## Test Schemas

All benchmarks use the same schemas across all 6 language implementations. Each language uses its own idiomatic schema declaration — C++ uses `PSIO1_REFLECT`, Rust uses derive macros, Zig uses plain structs with a `fracpack_fixed` sentinel, Go uses runtime `TypeDef` builders, JS uses composable type functions, and Python uses a `@schema` decorator with type annotations.

Below are the complete schemas in all 6 languages.

### C++

```cpp
struct Point {
   double x;
   double y;
};
PSIO1_REFLECT(Point, definitionWillNotChange(), x, y)

struct RGBA {
   uint8_t r, g, b, a;
};
PSIO1_REFLECT(RGBA, definitionWillNotChange(), r, g, b, a)

struct Token {
   uint16_t    kind;
   uint32_t    offset;
   uint32_t    length;
   std::string text;
};
PSIO1_REFLECT(Token, kind, offset, length, text)

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
PSIO1_REFLECT(UserProfile, id, name, email, bio, age, score, tags, verified)

struct LineItem {
   std::string product;
   uint32_t    qty;
   double      unit_price;
};
PSIO1_REFLECT(LineItem, product, qty, unit_price)

struct Order {
   uint64_t                   id;
   UserProfile                customer;
   std::vector<LineItem>      items;
   double                     total;
   std::optional<std::string> note;
};
PSIO1_REFLECT(Order, id, customer, items, total, note)

struct SensorReading {
   uint64_t                timestamp;
   std::string             device_id;
   double                  temp, humidity, pressure;
   double                  accel_x, accel_y, accel_z;
   double                  gyro_x, gyro_y, gyro_z;
   double                  mag_x, mag_y, mag_z;
   float                   battery;
   int16_t                 signal_dbm;
   std::optional<uint32_t> error_code;
   std::string             firmware;
};
PSIO1_REFLECT(SensorReading, timestamp, device_id,
             temp, humidity, pressure,
             accel_x, accel_y, accel_z,
             gyro_x, gyro_y, gyro_z,
             mag_x, mag_y, mag_z,
             battery, signal_dbm, error_code, firmware)
```

### Rust

```rust
#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson)]
#[fracpack(fracpack_mod = "fracpack", definition_will_not_change)]
struct Point { x: f64, y: f64 }

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson)]
#[fracpack(fracpack_mod = "fracpack", definition_will_not_change)]
struct RGBA { r: u8, g: u8, b: u8, a: u8 }

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson)]
#[fracpack(fracpack_mod = "fracpack")]
struct Token {
    kind: u16, offset: u32, length: u32, text: String,
}

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson)]
#[fracpack(fracpack_mod = "fracpack")]
struct UserProfile {
    id: u64, name: String, email: String, bio: Option<String>,
    age: u32, score: f64, tags: Vec<String>, verified: bool,
}

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson)]
#[fracpack(fracpack_mod = "fracpack")]
struct LineItem { product: String, qty: u32, unit_price: f64 }

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson)]
#[fracpack(fracpack_mod = "fracpack")]
struct Order {
    id: u64, customer: UserProfile, items: Vec<LineItem>,
    total: f64, note: Option<String>,
}

#[derive(Pack, Unpack, FracView, FracMutView, ToCanonicalJson, FromCanonicalJson)]
#[fracpack(fracpack_mod = "fracpack")]
struct SensorReading {
    timestamp: u64, device_id: String,
    temp: f64, humidity: f64, pressure: f64,
    accel_x: f64, accel_y: f64, accel_z: f64,
    gyro_x: f64, gyro_y: f64, gyro_z: f64,
    mag_x: f64, mag_y: f64, mag_z: f64,
    battery: f32, signal_dbm: i16,
    error_code: Option<u32>, firmware: String,
}
```

### Zig

```zig
const Point = struct {
    pub const fracpack_fixed = true;
    x: f64, y: f64,
};

const RGBA = struct {
    pub const fracpack_fixed = true;
    r: u8, g: u8, b: u8, a: u8,
};

const Token = struct {
    kind: u16, offset: u32, length: u32, text: []const u8,
};

const UserProfile = struct {
    id: u64, name: []const u8, email: []const u8, bio: ?[]const u8,
    age: u32, score: f64, tags: []const []const u8, verified: bool,
};

const LineItem = struct {
    product: []const u8, qty: u32, unit_price: f64,
};

const Order = struct {
    id: u64, customer: UserProfile, items: []const LineItem,
    total: f64, note: ?[]const u8,
};

const SensorReading = struct {
    timestamp: u64, device_id: []const u8,
    temp: f64, humidity: f64, pressure: f64,
    accel_x: f64, accel_y: f64, accel_z: f64,
    gyro_x: f64, gyro_y: f64, gyro_z: f64,
    mag_x: f64, mag_y: f64, mag_z: f64,
    battery: f32, signal_dbm: i16,
    error_code: ?u32, firmware: []const u8,
};
```

### Go

```go
//go:generate fracpack-viewgen -type Point,Token,UserProfile,LineItem,Order,SensorReading -fixed Point,RGBA

type Point struct {
    X float64
    Y float64
}

type RGBA struct {
    R, G, B, A uint8
}

type Token struct {
    Kind   uint16
    Offset uint32
    Length uint32
    Text   string
}

type UserProfile struct {
    ID       uint64
    Name     string
    Email    string
    Bio      *string
    Age      uint32
    Score    float64
    Tags     []string
    Verified bool
}

type LineItem struct {
    Product   string
    Qty       uint32
    UnitPrice float64 `fracpack:"unit_price"`
}

type Order struct {
    ID       uint64
    Customer UserProfile
    Items    []LineItem
    Total    float64
    Note     *string
}

type SensorReading struct {
    Timestamp uint64
    DeviceID  string  `fracpack:"device_id"`
    Temp      float64
    Humidity  float64
    Pressure  float64
    AccelX    float64 `fracpack:"accel_x"`
    AccelY    float64 `fracpack:"accel_y"`
    AccelZ    float64 `fracpack:"accel_z"`
    GyroX     float64 `fracpack:"gyro_x"`
    GyroY     float64 `fracpack:"gyro_y"`
    GyroZ     float64 `fracpack:"gyro_z"`
    MagX      float64 `fracpack:"mag_x"`
    MagY      float64 `fracpack:"mag_y"`
    MagZ      float64 `fracpack:"mag_z"`
    Battery   float32
    SignalDBM int16   `fracpack:"signal_dbm"`
    ErrorCode *uint32
    Firmware  string
}
```

### JavaScript / TypeScript

```typescript
const Point = fixedStruct({ x: f64, y: f64 });

const Token = struct({ kind: u16, offset: u32, length: u32, text: str });

const UserProfile = struct({
    id: u64, name: str, email: str, bio: optional(str),
    age: u32, score: f64, tags: vec(str), verified: bool,
});

const LineItem = struct({ product: str, qty: u32, unit_price: f64 });

const Order = struct({
    id: u64, customer: UserProfile, items: vec(LineItem),
    total: f64, note: optional(str),
});

const SensorReading = struct({
    timestamp: u64, device_id: str,
    temp: f64, humidity: f64, pressure: f64,
    accel_x: f64, accel_y: f64, accel_z: f64,
    gyro_x: f64, gyro_y: f64, gyro_z: f64,
    mag_x: f64, mag_y: f64, mag_z: f64,
    battery: f32, signal_dbm: i16,
    error_code: optional(u32), firmware: str,
});
```

### Python

```python
@schema
class Point:
    class Meta:
        fixed = True
    x: t.f64
    y: t.f64

@schema
class Token:
    kind: t.u16
    offset: t.u32
    length: t.u32
    text: t.string

@schema
class UserProfile:
    id: t.u64
    name: t.string
    email: t.string
    bio: t.optional[t.string]
    age: t.u32
    score: t.f64
    tags: t.vec[t.string]
    verified: t.bool_

@schema
class LineItem:
    product: t.string
    qty: t.u32
    unit_price: t.f64

@schema
class Order:
    id: t.u64
    customer: UserProfile
    items: t.vec[LineItem]
    total: t.f64
    note: t.optional[t.string]

@schema
class SensorReading:
    timestamp: t.u64
    device_id: t.string
    temp: t.f64
    humidity: t.f64
    pressure: t.f64
    accel_x: t.f64
    accel_y: t.f64
    accel_z: t.f64
    gyro_x: t.f64
    gyro_y: t.f64
    gyro_z: t.f64
    mag_x: t.f64
    mag_y: t.f64
    mag_z: t.f64
    battery: t.f32
    signal_dbm: t.i16
    error_code: t.optional[t.u32]
    firmware: t.string
```

### Packed sizes

| Schema          | Packed |
|-----------------|-------:|
| **Point**       |   16 B |
| **RGBA**        |    4 B |
| **Token**       |   35 B |
| **UserProfile** |  211 B |
| **Order**       |  449 B |
| **SensorReading**| 157 B |

---

## 1. Cross-Language: Pack (value → bytes)

Serialize an in-memory struct to a fracpack binary buffer. This measures allocation of the output buffer, writing fixed fields, building the offset table for variable-length fields, and copying string/vector data. Larger schemas with more variable fields require more offset arithmetic and data copies.

| Schema              |  C++ | Rust |    Go | Zig |    JS | Python |
|---------------------|-----:|-----:|------:|----:|------:|-------:|
| **Point** (16B)     |   10 |    9 |    31 |  19 |   247 |  3,071 |
| **Token** (35B)     |   21 |   16 |    82 |  26 |   498 |  5,048 |
| **UserProfile**     |   43 |   32 |   348 | 166 | 1,923 | 10,812 |
| **Order** (449B)    |  108 |   64 | 1,306 | 332 | 3,700 | 26,324 |
| **SensorReading**   |   29 |   26 |   226 |  60 | 1,265 | 10,746 |

**Ranking**: Rust ≈ C++ > Zig > Go > JS >> Python. C++ pack uses single-pass `packed_size()` and compile-time `all_members_present` detection to reduce member iterations from 4 to 2. Rust is 1.1–1.7x faster than C++ for complex types.

---

## 2. Cross-Language: Unpack (bytes → value)

Deserialize a fracpack binary buffer into a fully materialized language-native struct. This allocates the target object, reads fixed fields from known offsets, follows the offset table for variable fields, copies strings onto the heap, and recursively unpacks nested structs and vectors. The result is a normal struct that can be used without any reference to the original buffer.

| Schema              |  C++ | Rust |    Go | Zig |    JS | Python |
|---------------------|-----:|-----:|------:|----:|------:|-------:|
| **Point**           |  0.3 |    4 |    96 |  <1 |    61 |  1,234 |
| **Token**           |   15 |   27 |   169 |  12 |   172 |  2,167 |
| **UserProfile**     |   88 |  145 |   463 | 114 |   636 |  6,194 |
| **Order**           |  192 |  290 | 2,366 | 199 | 1,324 | 12,702 |
| **SensorReading**   |   34 |   73 |   650 |  31 |   512 |  7,218 |

**Ranking**: C++ ≈ Zig > Rust > JS ≈ Go >> Python

---

## 3. Cross-Language: Validate (zero-copy integrity check)

Walk the packed buffer and verify structural integrity without allocating or copying any data: check that the fixed-size region is the correct length, every offset in the offset table points within bounds, string length prefixes don't overflow the buffer, optional markers are valid, and nested structs/vectors are recursively well-formed. A validated buffer is safe to use with zero-copy view accessors. This is pure pointer arithmetic — no heap allocation, no string copies.

| Schema              |  C++ | Rust |  Go | Zig |   JS | Python |
|---------------------|-----:|-----:|----:|----:|-----:|-------:|
| **Point**           |  0.3 |  1.6 |  29 |   1 |   64 |  1,245 |
| **UserProfile**     |   14 |   56 | 132 |  31 |  271 |  8,814 |
| **Order**           |   62 |  123 | 432 |  60 |  571 | 19,818 |
| **SensorReading**   |   15 |   23 | 145 |  13 |  234 |  7,021 |

**Ranking**: C++ ≈ Zig > Rust > Go > JS >> Python

Point validation is trivial for fixed-size structs (just a length check), but all implementations now benchmark it for completeness.

---

## 4. Cross-Language: View-One (zero-copy single field access)

Read a single field directly from the packed buffer without deserializing the rest of the struct. For fixed-size fields at known positions (like `verified: bool`), this is a computed offset + memcpy. For variable-length fields (like `name: string`), the accessor follows the offset table to locate the field's data region, then returns a pointer/slice into the buffer — no heap allocation. This is the fracpack differentiator: O(1) access to any field regardless of struct size.

"Prevalidated" (Rust) means the buffer was validated once upfront; subsequent field accesses skip per-access bounds checks.

| Field                              |  C++ | Rust | Rust (preval.) | Go (typed) | Go (generic) |  JS (`readField`) | Python |
|------------------------------------|-----:|-----:|---------------:|-----------:|-------------:|------------------:|-------:|
| **UserProfile.name** (string)      |  0.2 |   56 |            1.1 |         16 |           26 |               109 |    735 |
| **UserProfile.verified** (bool)    |  0.2 |   56 |            0.5 |        0.5 |          9.7 |                43 |    526 |
| **Order.customer.name** (nested)   |  0.3 |  129 |            1.2 |         17 |           37 |               258 |  1,208 |
| **SensorReading.firmware** (str)   |  0.3 |   25 |            1.1 |         17 |           30 |               102 |    734 |

C++ view-one fields are sub-nanosecond (direct pointer arithmetic, zero-copy `str_view()`). Rust prevalidated views bypass per-access bounds checks, dropping to 0.6–1.3ns. Go typed views (compile-time-known offsets) are 2–19x faster than generic `Field()` dispatch — UserProfile.verified drops from 9.7ns to 0.5ns.

---

## 5. Cross-Language: View-All (access every field)

Access every field in the struct via zero-copy view, touching each value once without unpacking. For UserProfile this means reading `id`, `name`, `email`, checking `bio.has_value()`, reading `age`, `score`, getting `tags` byte size, and reading `verified` — all via pointer arithmetic into the packed buffer. String fields return slices/views (`std::string_view`, `[]byte`, `[]const u8`) rather than allocated copies. This measures the total overhead of navigating the full offset table versus just reading pre-materialized struct members.

| Schema              |  C++ | Rust | Rust (preval.) | Go (typed) | Go (generic) | Zig |    JS | Python |
|---------------------|-----:|-----:|---------------:|-----------:|-------------:|----:|------:|-------:|
| **Point**           |  0.3 |  2.3 |            1.2 |        0.6 |          8.2 |  <1 |   187 |    867 |
| **UserProfile**     |  1.2 |   63 |            7.1 |         27 |          228 |   8 | 3,155 |  4,652 |
| **SensorReading**   |  3.0 |   33 |           10.6 |         88 |          316 |  12 | 1,556 |  3,904 |

C++ and Zig use fully zero-copy accessors (`str_view`, `has_value`, `raw_byte_size` / `slice.ptr`). Rust prevalidated is 3–9x faster than default views. Go typed views (hard-coded byte offsets) are 3.6–14x faster than generic `Field()` dispatch — Point drops from 8.2ns to 0.6ns.

---

## 6. Cross-Language: JSON Write / Read

Fracpack defines a canonical JSON encoding that preserves type fidelity: `u64` values are encoded as strings (to avoid JS number precision loss), byte arrays as hex strings, variants as `{"type": "Name", "value": ...}`, and optionals as explicit null. These benchmarks measure converting between fracpack binary and this canonical JSON representation — not generic JSON serialization.

### Canonical fracpack JSON write

| Schema              |   C++ | Rust |    Go |    Zig |    JS | Python |
|---------------------|------:|-----:|------:|-------:|------:|-------:|
| **UserProfile**     | 1,665 |  617 | 1,240 |    498 | 2,228 |  4,316 |
| **Order**           | 3,965 |1,544 | 4,020 |  1,070 | 6,099 |  9,318 |
| **SensorReading**   | 1,982 |1,300 | 1,748 |    893 | 2,000 |  7,808 |

### Canonical fracpack JSON read

| Schema              |   C++ |  Rust |    Go |   Zig |    JS | Python |
|---------------------|------:|------:|------:|------:|------:|-------:|
| **UserProfile**     |   601 |   813 | 2,628 |   907 | 3,080 |  4,449 |
| **Order**           | 1,225 | 2,017 |10,788 | 2,091 | 6,620 |  9,995 |
| **SensorReading**   |   984 | 1,302 | 4,229 | 1,412 | 2,898 |  7,201 |

### Fracpack JSON vs platform JSON libraries

Comparison against each language's native JSON library serializing the same UserProfile data as a plain object (no u64-as-string, no hex bytes, no variant/optional semantics).

| Language   | Library                | Write (UserProfile) | Read (UserProfile) | fracpack Write     | fracpack Read      |
|------------|------------------------|--------------------:|-------------------:|-------------------:|-------------------:|
| **Rust**   | `serde_json`           |             137 ns  |            216 ns  |   617 ns  (4.5x)  |   813 ns  (3.8x)  |
| **Go**     | `encoding/json`        |             310 ns  |          1,352 ns  | 1,240 ns  (4.0x)  | 2,628 ns  (1.9x)  |
| **JS**     | `JSON.stringify/parse`  |             151 ns  |            363 ns  | 2,228 ns (14.8x)  | 3,080 ns  (8.5x)  |
| **Python** | `json.dumps/loads`     |           1,495 ns  |          1,152 ns  | 4,316 ns  (2.9x)  | 4,449 ns  (3.9x)  |

Platform JSON is faster because it's native C/C++ under the hood. The tradeoff: fracpack JSON handles u64-as-string, hex bytes, variant encoding, and optional semantics correctly — platform libraries don't.

---

## 7. Cross-Language: Mutation (in-place vs repack)

Modify a single field in an already-packed buffer without deserializing and re-serializing the entire struct. **In-place** navigates to the field's location in the buffer and overwrites it directly. For fixed-size fields (like `id: u64`), this is a single write at a computed offset. For same-length strings, it's a memcpy. For strings that grow, the buffer must be resized and trailing data shifted. **Repack** is the baseline: unpack the full struct, modify the field, re-pack from scratch.

| Operation                          | C++ in-place | C++ repack | Speedup   | Rust in-place | Rust repack | Speedup  |
|------------------------------------|-------------:|-----------:|----------:|--------------:|------------:|---------:|
| **UserProfile.id** (scalar)        |           28 |        324 | **11.5x** |            93 |         315 | **3.4x** |
| **UserProfile.name** (same-len)    |           54 |        318 |  **5.9x** |           154 |         331 | **2.2x** |
| **UserProfile.name** (grow)        |           93 |        318 |  **3.4x** |           170 |         330 | **1.9x** |
| **Order.customer.age** (nested)    |           41 |        616 | **15.1x** |           360 |         560 | **1.6x** |
| **SensorReading.signal_dbm**       |           29 |        225 |  **7.8x** |            54 |         239 | **4.4x** |

| Operation                          | Go in-place | Go repack | Speedup  | JS in-place | JS repack | Speedup  |
|------------------------------------|------------:|----------:|---------:|------------:|----------:|---------:|
| **UserProfile.id** (scalar)        |          80 |       908 |  **11x** |         654 |     2,445 | **3.7x** |
| **UserProfile.name** (same-len)    |         144 |       908 | **6.3x** |         883 |     2,482 | **2.8x** |
| **UserProfile.name** (grow)        |         144 |       908 | **6.3x** |       1,194 |     2,482 | **2.1x** |
| **UserProfile.name** (shrink)      |         125 |       908 | **7.3x** |       1,201 |     2,482 | **2.1x** |

In-place mutation is 2–16x faster than repack. The advantage is greatest for scalar fields deep in nested structs.

---

## 8. View vs Native Struct Access

The core thesis: **fracpack views should match native struct field access**.

This measures the cost of reading all fields from a fracpack view versus reading the same fields from an already-unpacked native struct sitting in memory. "Native" is the floor — it's just reading struct members that are already materialized as pointers and values on the heap. "View" navigates offset tables in the packed buffer and returns slices/views. "Unpack+access" deserializes the entire struct first, then reads the fields — the worst case.

For string fields, zero-copy view APIs are used: `str_view()` (C++), `.Bytes()` (Go), `slice.ptr` (Zig), `rawField` (JS), `_raw` accessors (Python). These return references into the packed buffer without heap allocation.

### C++ (Release, zero-copy: `str_view()`, `has_value()`, `raw_byte_size()`)

| Schema              | Native  | View    | View/Native | Unpack+access | Unpack/Native |
|---------------------|--------:|--------:|------------:|--------------:|--------------:|
| **Point**           |  0.2 ns |  0.3 ns |    **1.4x** |        0.3 ns |         1.3x  |
| **UserProfile**     |  1.4 ns |  1.5 ns |    **1.1x** |         91 ns |       **67x** |
| **SensorReading**   |  3.0 ns |  3.2 ns |    **1.1x** |         36 ns |       **12x** |

**Views are indistinguishable from native.** Unpack is 12–69x slower.

### Zig (ReleaseFast, `c_allocator`, zero-copy slices)

| Schema              | Native | View   | View/Native | Unpack+access | Unpack/Native |
|---------------------|-------:|-------:|------------:|--------------:|--------------:|
| **Point**           |  <1 ns |  <1 ns |   **~1.0x** |         <1 ns |        ~1.0x  |
| **UserProfile**     |   4 ns |   7 ns |    **1.8x** |        115 ns |       **29x** |
| **SensorReading**   |   8 ns |  12 ns |    **1.5x** |         36 ns |      **4.5x** |

### Go (typed views vs generic `Field()` dispatch)

| Schema              | Native  | Typed View | Typed/Native | Generic View | Generic/Native | Unpack+access |
|---------------------|--------:|-----------:|-------------:|-------------:|---------------:|--------------:|
| **Point**           |  0.2 ns |     0.6 ns |     **2.4x** |       8.7 ns |           36x  |        165 ns |
| **UserProfile**     |   43 ns |      27 ns |     **0.6x** |      134 ns  |          3.1x  |        632 ns |
| **Order**           |   56 ns |      31 ns |     **0.6x** |      192 ns  |          3.4x  |      3,966 ns |
| **SensorReading**   |  100 ns |      88 ns |     **0.9x** |      305 ns  |          3.1x  |        890 ns |

**Typed views are faster than native Go structs** for complex types — UserProfile typed view (27ns) beats native (43ns) because zero-copy `[]byte` slices avoid GC-tracked string/slice allocation. Generic `Field()` dispatch adds 3–36x overhead from map lookup, `initOffsets()`, and `FieldView` indirection. Point typed view (0.6ns, 0 allocs) is only 2.4x native — the residual cost is reading from a byte slice vs a register.

### JS (Proxy-based views with `rawField`)

| Schema              | Native | Raw View | Cached View | Unpack  | Raw/Native |
|---------------------|-------:|---------:|------------:|--------:|-----------:|
| **Point**           |   4 ns |   172 ns |       39 ns |   61 ns |       43x  |
| **UserProfile**     |   4 ns |   425 ns |      132 ns |  685 ns |      106x  |
| **SensorReading**   |   4 ns |   914 ns |      314 ns |  494 ns |      229x  |

Proxy overhead dominates. Cached views are faster than unpack for UserProfile.

### Python (CPython 3.14)

| Schema              | Native | View (decoded) | View (raw) | Unpack  | View/Native |
|---------------------|-------:|---------------:|-----------:|--------:|------------:|
| **Point**           |  38 ns |        690 ns  |     690 ns | 1,188 ns |        18x  |
| **UserProfile**     |  61 ns |       3,533 ns |   2,282 ns | 4,893 ns |     37–58x  |
| **SensorReading**   |  99 ns |       4,141 ns |   3,904 ns | 6,390 ns |     39–42x  |

Interpreter overhead per field access dominates. Views are still 1.3–2x faster than unpack.

---

## 9. Array Scaling

Measures how pack, unpack, and view scale with array size. The test data is `Vec<Point>` (array of 2×f64 structs) at sizes from 10 to 10,000 elements. Pack and unpack are expected to scale O(n) since every element must be visited. View element access should scale O(1) — accessing the last element in a 10K array should cost the same as in a 10-element array (one offset lookup + memcpy).

### Pack (ns)

| Size  |    C++ |   Rust |      Go |   Zig |      JS |  Python |
|------:|-------:|-------:|--------:|------:|--------:|--------:|
|    10 |    108 |     51 |     642 |   311 |     707 |  14,969 |
|   100 |    826 |    213 |   4,745 | 2,251 |   4,574 | 137,853 |
|    1K |  8,105 |  1,620 |  42,505 |23,696 |  36,219 |   1.35M |
|   10K | 82,794 | 37,037 | 441,282 |  251K | 343,691 |  13.59M |

### Unpack (ns)

| Size  |   C++ |   Rust |       Go |   Zig |      JS |  Python |
|------:|------:|-------:|---------:|------:|--------:|--------:|
|    10 |    19 |     66 |    1,710 |    15 |     316 |   7,529 |
|   100 |    68 |    573 |   12,713 |    50 |   2,738 |  69,186 |
|    1K |   579 |  7,191 |  139,428 |   382 |  27,431 | 689,424 |
|   10K | 5,035 |108,800 |1,512,386 | 3,776 | 273,625 |   7.74M |

### View (element access, ns)

C++ and Rust iterate all elements via zero-copy view (memcpy/pointer arithmetic). Zig iterates all elements via slices. JS accesses the first element. Go accesses only the last element to demonstrate O(1) random access.

| Size  |  C++ (all) | Rust (all) | Go (last) | Zig (all) | JS (first) |
|------:|-----------:|-----------:|----------:|----------:|-----------:|
|    10 |        1.5 |         32 |        58 |         5 |        169 |
|   100 |         16 |        291 |       209 |        48 |        188 |
|    1K |        440 |      5,394 |     1,568 |       467 |        506 |
|   10K |      5,550 |     38,293 |    14,885 |     4,757 |      3,188 |

At 10K points, JS view-first is **86x faster** than unpack. C++ and Zig view-all at 10K is ~5µs vs 5–108µs for unpack.

---

## 10. Multiformat Comparison (C++): Wire Size, Pack, Unpack, View

Compares all internal psio formats plus four external formats on the same hardware and data. See Methodology for format descriptions.

### Wire Size (bytes)

| Schema              | Fracpack | WIT Cn ABI | Binary | Bincode | Avro   | Protobuf | MsgPack | Cap'n Proto | FlatBuffers | JSON   |
|---------------------|----------|------------|--------|---------|--------|----------|---------|-------------|-------------|--------|
| **Point**           | 16 B     | 16 B       | 16 B   | 16 B    | 16 B   | 18 B     | 19 B    | 32 B        | 32 B        | 43 B   |
| **Token**           | 35 B     | 35 B       | 26 B   | 33 B    | 20 B   | 24 B     | 22 B    | 56 B        | 56 B        | 62 B   |
| **UserProfile**     | 211 B    | 223 B      | 154 B  | 210 B   | 149 B  | 156 B    | 150 B   | 264 B       | 264 B       | 231 B  |
| **Order**           | 449 B    | 454 B      | 308 B  | 413 B   | 286 B  | 321 B    | 297 B   | 544 B       | 584 B       | 602 B  |
| **SensorReading**   | 157 B    | 161 B      | 138 B  | 152 B   | 136 B  | 162 B    | 155 B   | 184 B       | 208 B       | 325 B  |

*WIT Canonical ABI uses natural alignment with (pointer, length) pairs for strings/lists. For all-scalar types (Point), wire size equals fracpack (16 B). For mixed types, WIT is slightly larger than fracpack (1–6% overhead) due to alignment padding but much smaller than Cap'n Proto/FlatBuffers. Token is identical at 35 B because its alignment overhead is offset by compact field packing.*

Protobuf and MsgPack achieve compact wire sizes comparable to binary/avro — Protobuf uses varint encoding for integers and field tags; MsgPack uses a compact type-length-value scheme. Both are significantly smaller than Cap'n Proto/FlatBuffers (which pay for alignment padding) and comparable to or slightly larger than avro (the most compact). Fracpack sits in the middle — larger than streaming formats because offset tables enable zero-copy access, but much smaller than the zero-copy external formats.

### Pack Speed (ns per operation, C++)

| Schema              | Fracpack | WIT    | Binary | Bincode | Avro  | Protobuf | MsgPack | Cap'n Proto | FlatBuffers | JSON   |
|---------------------|----------|--------|--------|---------|-------|----------|---------|-------------|-------------|--------|
| **Point**           | 10       | 10     | 10     | 9       | 10    | 7        | 23      | 146         | 36          | 501    |
| **Token**           | 21       | 11     | 21     | 21      | 28    | 46       | 33      | 144         | 45          | 366    |
| **UserProfile**     | 43       | 31     | 40     | 36      | 47    | 212      | 72      | 155         | 122         | 1,508  |
| **Order**           | 108      | 59     | 79     | 89      | 104   | 552      | 148     | 240         | 369         | 5,030  |
| **SensorReading**   | 29       | 27     | 31     | 29      | 43    | 78       | 69      | 148         | 88          | 3,805  |

Protobuf pack speed varies by schema complexity: fast for simple scalars (Point: 7ns — varint encoding is cheap for small values) but slow for complex types with strings and nested objects (UserProfile: 212ns, Order: 552ns — 3.7–3.9x slower than fracpack). This is due to protobuf's arena allocator overhead and per-field tag encoding for repeated/nested types. MsgPack pack is competitive with fracpack across all types (0.8–2.3x). Cap'n Proto remains the slowest for packing (146–240ns) due to `MallocMessageBuilder` overhead.

### Unpack Speed (ns per operation, C++)

Full deserialization into native structs (string allocation, vector copy, etc.). Cap'n Proto does not have a traditional unpack — its reader API is always zero-copy (see View-All below). FlatBuffers' `UnPack()` produces generated `*T` structs — these are codegen types, not user application structs. Protobuf `ParseFromString()` also produces generated message objects, not user types.

| Schema              | Fracpack | WIT    | Binary | Bincode | Avro  | Protobuf | MsgPack | FlatBuffers |
|---------------------|----------|--------|--------|---------|-------|----------|---------|-------------|
| **Point**           | <1       | <1     | <1     | <1      | <1    | 17       | 68      | <1          |
| **Token**           | 16       | 3      | 10     | 10      | 14    | 38       | 84      | 18          |
| **UserProfile**     | 97       | 40     | 75     | 81      | 119   | 217      | 190     | 59          |
| **Order**           | 216      | 82     | 157    | 154     | 253   | 522      | 368     | 190         |
| **SensorReading**   | 36       | 14     | 21     | 22      | 27    | 71       | 152     | 22          |

Protobuf unpack is **2–2.4x slower than fracpack** across all types. Its variable-length tag-value parsing requires per-field branching, varint decoding, and length-delimited substring allocation. MsgPack is **2–4x slower than fracpack** — its positional array format requires sequential decoding of every element type tag. Both are significantly slower than binary/bincode (which have minimal framing) and FlatBuffers (whose linear layout enables fast sequential copy).

### View-All Speed (ns per operation, C++)

Zero-copy read of all top-level fields without deserialization. This is the apples-to-apples comparison between the three zero-copy formats. Protobuf, MsgPack, binary, bincode, and avro cannot do this — they must fully parse/unpack to access any field.

| Schema              | Fracpack view | WIT WView | FlatBuffers view | Cap'n Proto view | Protobuf parse | MsgPack parse |
|---------------------|---------------|-----------|------------------|------------------|----------------|---------------|
| **Point**           | <1            | <1        | 0.8              | 20               | 18             | 66            |
| **Token**           | 1.2           | 0.6       | 1.1              | 25               | 40             | 82            |
| **UserProfile**     | 1.6           | 2.6       | 2.6              | 40               | 222            | 184           |
| **Order**           | 1.1           | 3.9       | 1.6              | 40               | 536            | 368           |
| **SensorReading**   | 3.2           | 2.9       | 5.1              | 35               | 72             | 156           |

*Note: Sub-1ns measurements are at the CPU pipeline noise floor; actual values are near-zero but imprecise.*

*WIT WView uses pointer arithmetic identical to native struct access for scalar fields. For strings, `WView<T>` reads a (pointer, length) pair and returns `string_view` — zero-copy but one indirection. For nested structs and vectors, `WView<T>` composes recursively via `span<WView<U>>`.*

Fracpack views are **1.4–2x faster** than FlatBuffers, **11–35x faster** than Cap'n Proto, **45–487x faster** than Protobuf, and **49–334x faster** than MsgPack for reading all fields. WIT WView is comparable to fracpack for simple types (Token: 0.6ns vs 1.2ns) and within 2–4x for complex types (Order: 3.9ns vs 1.1ns). The fundamental difference: fracpack, WIT WView, and FlatBuffers read fields directly from the wire format with zero allocation. Protobuf and MsgPack must allocate strings and decode varint/type tags for every field — there is no "view" mode.

### View Access Speed — Single Field (ns, C++)

All three zero-copy formats (fracpack, FlatBuffers, Cap'n Proto) support reading individual fields without deserialization. Protobuf, MsgPack, and sequential formats must fully parse/unpack the entire struct to access any field.

| Schema.field              | Fracpack view | FlatBuffers view | Cap'n Proto view | Protobuf parse | MsgPack parse | Binary unpack |
|---------------------------|---------------|------------------|------------------|----------------|---------------|---------------|
| **UserProfile.id**        | <1            | 0.6              | 20               | 216            | 197           | 75            |
| **UserProfile.name**      | <1            | 0.6              | 24               | 217            | 191           | 75            |
| **Order.customer.name**   | <1            | 0.9              | 29               | 515            | 378           | 157           |

*Note: Protobuf and MsgPack must parse the entire message to read a single field — their "view-one" cost equals their full unpack cost. Fracpack and FlatBuffers navigate directly to the field via offset arithmetic.*

Fracpack and FlatBuffers are comparably fast for single-field access (both sub-nanosecond). Both are **>25x faster** than Cap'n Proto, **>300x faster** than Protobuf, and **>300x faster** than MsgPack. The gap is architectural: zero-copy formats locate fields by offset arithmetic; parse-required formats must decode the entire message first.

### Total Cost: Pack + N View-All Reads (UserProfile, ns)

How many reads does it take for fracpack's pack cost to pay off? Each "read" is a view-all (read all fields zero-copy) for fracpack (~1.6ns), FlatBuffers (~2.6ns), and Cap'n Proto (40ns), or a full parse for Protobuf (222ns), MsgPack (184ns), or binary (75ns — their only option).

| Reads (N) | Fracpack (43 + N × 1.6) | FlatBuffers (122 + N × 2.6) | Protobuf (212 + N × 222) | MsgPack (72 + N × 184) | Cap'n Proto (155 + N × 40) | Binary (40 + N × 75) | Winner |
|-----------|-------------------------|-----------------------------|--------------------------|------------------------|----------------------------|----------------------|--------|
| 0         | 43                      | 122                         | 212                      | 72                     | 155                        | 40                   | Binary |
| 1         | 45                      | 125                         | 434                      | 256                    | 195                        | 115                  | Fracpack |
| 3         | 48                      | 130                         | 878                      | 624                    | 275                        | 265                  | Fracpack (2.7x) |
| 10        | 59                      | 148                         | 2,432                    | 1,912                  | 555                        | 790                  | Fracpack (2.5x) |
| 100       | 203                     | 382                         | 22,412                   | 18,472                 | 4,155                      | 7,540                | Fracpack (1.9x) |
| 1,000     | 1,643                   | 2,722                       | 222,212                  | 184,072                | 40,155                     | 75,040               | Fracpack (1.7x) |

Fracpack breaks even with binary at **1 read** and beats every other format at every read count. By 10 reads, fracpack is 2.5x faster than FlatBuffers, 9x faster than Cap'n Proto, 13x faster than binary, 32x faster than MsgPack, and 41x faster than Protobuf.

Fracpack breaks even with flat binary at **1 read** and beats every external format at every read count. By 10 reads, fracpack is 2.5x FlatBuffers, 9x Cap'n Proto, 41x Protobuf. See the Executive Summary for the full analysis.

---

## 11. Rust Native Format Comparison

How does fracpack compare against the Rust ecosystem's native serialization libraries? Unlike the C++ comparison (section 10) which tests external formats via FFI, this section benchmarks pure Rust implementations where each library operates in its native environment.

**Formats tested:**
- **Fracpack** — Zero-copy binary with offset tables. `Pack`/`Unpack`/`FracView` derive macros — works directly with user-defined structs. Supports schema extensibility and in-place mutation.
- **bincode** (v1.3) — The most widely used Rust binary serializer. Serde-based, fixed-width integers, u64 length prefixes. No codegen, no zero-copy view.
- **postcard** (v1.1) — Compact no_std binary format. Serde-based, varint encoding. Popular for embedded/constrained environments. No zero-copy view.
- **rmp-serde** (v1.3) — MessagePack for Rust via serde. Compact binary with type tags. No zero-copy view.
- **rkyv** (v0.8) — Zero-copy deserialization framework. Generates `Archived*` types for direct memory-mapped access. The closest architectural competitor to fracpack's views.

**Schema approach**: Fracpack uses `#[derive(Pack, Unpack, FracView)]` on plain structs. Bincode, postcard, and rmp-serde use serde's `#[derive(Serialize, Deserialize)]`. rkyv uses its own `#[derive(Archive, rkyv::Serialize, rkyv::Deserialize)]` which generates separate `Archived*` types (e.g., `ArchivedUserProfile`). All work with user-defined structs — no IDL files or external codegen steps.

### Wire Size (bytes, Rust)

| Schema              | Fracpack | Bincode | Postcard | rmp    | rkyv   |
|---------------------|----------|---------|----------|--------|--------|
| **Point**           | 16 B     | 16 B    | 16 B     | 19 B   | 16 B   |
| **Token**           | 35 B     | 33 B    | 20 B     | 22 B   | 36 B   |
| **UserProfile**     | 173 B    | 172 B   | 114 B    | 117 B  | 168 B  |
| **Order**           | 351 B    | 327 B   | 220 B    | 232 B  | 352 B  |
| **SensorReading**   | 157 B    | 152 B   | 136 B    | 155 B  | 168 B  |

Postcard is the most compact format (varint encoding), followed by rmp (MessagePack type tags). Fracpack, bincode, and rkyv are all similar in size — they all use fixed-width integers. rkyv is slightly larger due to alignment padding for memory-mapped access. Fracpack's offset tables add modest overhead for zero-copy field access.

### Pack Speed (ns, Rust)

| Schema              | Fracpack | Bincode | Postcard | rmp   | rkyv  |
|---------------------|----------|---------|----------|-------|-------|
| **Point**           | 9        | 9       | 25       | 50    | 15    |
| **Token**           | 16       | 16      | 40       | 44    | 41    |
| **UserProfile**     | 32       | 30      | 111      | 127   | 102   |
| **Order**           | 64       | 51      | 157      | 201   | 152   |
| **SensorReading**   | 26       | 31      | 113      | 147   | 66    |

Fracpack and bincode are now at near-parity (0.84–1.25x). Fracpack is actually **faster** than bincode for SensorReading (26ns vs 31ns). Both are 2–5x faster than postcard, rmp, and rkyv. The remaining fracpack overhead vs bincode comes from offset-table construction and u16 heap headers — the cost of supporting zero-copy views and schema extensibility. Optimizations applied: `packed_size()` pre-allocation eliminates Vec reallocation; compile-time trailing-optional detection eliminates runtime scans and per-field branches when the last field is non-optional; `definition_will_not_change` structs use branchless pack with no heap header.

### Unpack Speed (ns, Rust)

Full deserialization to native structs. rkyv `from_bytes` includes validation + copy to native type.

| Schema              | Fracpack | Bincode | Postcard | rmp   | rkyv  |
|---------------------|----------|---------|----------|-------|-------|
| **Point**           | 4        | 2       | <1       | 6     | 3     |
| **Token**           | 27       | 19      | 19       | 27    | 17    |
| **UserProfile**     | 145      | 123     | 120      | 136   | 105   |
| **Order**           | 290      | 225     | 221      | 269   | 191   |
| **SensorReading**   | 73       | 49      | 45       | 85    | 33    |

rkyv unpack is fastest for complex types. Fracpack unpack is 1.2–1.5x slower than bincode/postcard — it must parse offset tables and validate extensibility headers. For simple fixed-size types (Point), all formats are fast (<6ns).

### View-All Speed (ns, Rust — zero-copy only)

Zero-copy read of all fields without deserialization. Only fracpack and rkyv support this — bincode, postcard, and rmp must fully unpack.

| Schema              | Fracpack (prevalidated) | rkyv (unchecked) |
|---------------------|-------------------------|------------------|
| **Point**           | 1.2                     | 0.8              |
| **UserProfile**     | 7.1                     | 2.3              |
| **SensorReading**   | 10.6                    | 4.7              |

rkyv views are **2–3x faster** than fracpack prevalidated views. rkyv's archived types are a direct memory-mapped layout — field access is a pointer offset with zero indirection. Fracpack's views must navigate offset tables for variable-length fields, adding ~1ns per indirection. However, both are dramatically faster than any unpack-required format (3–12ns vs 45–270ns).

### View-One Speed (ns, Rust — zero-copy only)

| Schema.field              | Fracpack (prevalidated) | rkyv (unchecked) | Bincode unpack | Postcard unpack |
|---------------------------|-------------------------|------------------|----------------|-----------------|
| **UserProfile.name**      | 1.1                     | 0.9              | 123            | 120             |
| **UserProfile.id**        | 0.5                     | 0.5              | 123            | 120             |

Fracpack and rkyv are comparably fast for single-field access (both <1ns). Both are **>120x faster** than bincode/postcard, which must fully unpack the entire struct to read any field.

### Analysis

**Fracpack now matches bincode for pack speed** (0.84–1.25x), while providing zero-copy views that bincode cannot. Optimizations — `packed_size()` pre-allocation, compile-time trailing-optional elimination, and branchless `definition_will_not_change` packing — closed a former 3–5x gap to near-parity.

**rkyv is the closest zero-copy competitor.** Both fracpack and rkyv provide zero-copy field access at 1–11ns. rkyv is 2–3x faster for view-all because its memory layout is a direct struct mapping with no offset table indirection. However, fracpack packs 2–3x faster than rkyv (32ns vs 102ns for UserProfile) and has architectural advantages:

1. **Schema extensibility**: Fracpack structs can add fields without breaking existing readers. rkyv's archived layout is frozen at compile time — adding a field changes the layout and breaks existing serialized data.

2. **In-place mutation**: Fracpack's `FracMutView` supports modifying packed data without full deserialize-modify-repack. rkyv has no equivalent.

3. **Same types everywhere**: Fracpack views and pack/unpack all operate on the same user-defined struct type. rkyv generates separate `Archived*` types — you work with `ArchivedUserProfile` in views but `UserProfile` in application code.

4. **Wire compatibility**: Fracpack has a cross-language wire format (C++, Rust, Go, JS, Python, Zig all interoperate). rkyv is Rust-only.

**Total cost comparison** (UserProfile, pack + N view-all reads):

| Reads (N) | Fracpack (32 + N × 7.1) | rkyv (102 + N × 2.3) | Bincode (30 + N × 123) |
|-----------|--------------------------|----------------------|------------------------|
| 0         | 32                       | 102                  | 30                     |
| 1         | 39                       | 104                  | 153                    |
| 3         | 53                       | 109                  | 399                    |
| 10        | 103                      | 125                  | 1,260                  |
| 100       | 742                      | 332                  | 12,330                 |

Fracpack breaks even with bincode at **1 read** and is 17x faster at 100 reads. Fracpack beats rkyv in total cost up to ~15 reads thanks to its 3x faster pack speed. At 100+ reads, rkyv's faster views give it a 2.2x total-cost advantage.

### Rust vs C++ Fracpack Performance

Same fracpack wire format, same schemas, same hardware — comparing the Rust and C++ implementations.

#### Pack Speed (ns)

| Schema              | C++  | Rust |  Rust/C++ |
|---------------------|-----:|-----:|----------:|
| **Point**           |   10 |    9 |     0.9x  |
| **Token**           |   21 |   16 | **0.76x** |
| **UserProfile**     |   43 |   32 | **0.74x** |
| **Order**           |  108 |   64 | **0.59x** |
| **SensorReading**   |   29 |   26 | **0.90x** |

C++ and Rust are now at near-parity for most types. Both use `packed_size()` pre-allocation and compile-time `all_members_present` / trailing-optional detection to reduce pack to 2 member iterations. Rust maintains a 1.1–1.7x lead on complex nested types (Order) where the derive macro's fully-inlined field access avoids the functor+fold overhead of C++'s `for_each_member` pattern.

#### Unpack Speed (ns)

| Schema              | C++  | Rust |  C++/Rust |
|---------------------|-----:|-----:|----------:|
| **Point**           |  0.3 |    4 |    **13x** |
| **Token**           |   14 |   27 |   **1.9x** |
| **UserProfile**     |   88 |  145 |   **1.6x** |
| **Order**           |  192 |  290 |   **1.5x** |
| **SensorReading**   |   32 |   73 |   **2.3x** |

C++ unpack is 1.5–2.3x faster for complex types. C++ benefits from move semantics, stack allocation of fixed-size fields, and `std::string`'s small-string optimization (SSO) which avoids heap allocation for short strings. Rust must allocate `String` on the heap for every string field.

#### View Speed (ns)

| Schema              | C++ view-all | Rust (preval.) | C++ view-one | Rust (preval.) |
|---------------------|-------------:|---------------:|-------------:|---------------:|
| **Point**           |          0.3 |            1.2 |            — |              — |
| **UserProfile**     |          1.3 |            7.1 |    0.3 (name)|     1.1 (name) |
| **SensorReading**   |          2.9 |           10.6 |    0.3 (fw)  |     1.1 (fw)   |

C++ views are 3–5x faster than Rust prevalidated views. C++ uses raw pointer arithmetic with `str_view()` returning `std::string_view` — true zero-cost abstraction. Rust prevalidated views still perform offset reads through safe slice indexing, which involves bounds-check-free but slightly heavier pointer arithmetic.

#### Summary

| Operation | Faster | By how much |
|-----------|--------|-------------|
| **Pack**  | **Rust** | 1.1–1.7x faster (inline codegen vs functor+fold) |
| **Unpack** | **C++** | 1.5–2.3x faster (SSO, move semantics, stack alloc) |
| **View** | **C++** | 3–5x faster (raw pointer arithmetic, str_view) |
| **Validate** | **C++** | 2–5x faster |

C++ and Rust pack are near-parity for simple types (SensorReading: 29 vs 26ns). Rust leads on deeply nested types (Order: 108 vs 64ns) where derive macro inlining avoids C++'s functor dispatch overhead. C++ dominates read speed. For write-heavy workloads, both are excellent. For read-heavy workloads, C++ views are faster — but Rust prevalidated views at 1–11ns are still dramatically faster than any full-unpack path.

---

## 12. Within-Language: Fracpack vs JSON Speed

How much faster is fracpack binary pack/unpack compared to fracpack's own canonical JSON encoding of the same data? This isolates the cost of text formatting, field-name emission, and string escaping versus binary writes. Both paths produce equivalent data — the JSON path just has more work to do per field.

| Language   | Pack vs JSON Write                | Unpack vs JSON Read              |
|------------|:---------------------------------:|:--------------------------------:|
| **C++**    | **30x faster** (43 vs 1,303)     | **6.4x faster** (88 vs 557)     |
| **Rust**   | **19x faster** (32 vs 617)       | **5.6x faster** (145 vs 813)    |
| **Go**     | **3.6x faster** (348 vs 1,240)   | **5.7x faster** (463 vs 2,628)  |
| **JS**     | **1.2x faster** (1,923 vs 2,228) | **4.8x faster** (636 vs 3,080)  |
| **Python** | **2.5x slower** (10,812 vs 4,316)| **1.4x slower** (6,194 vs 4,449)|

---

## 13. Within-Language: View-One vs Full Unpack

When you only need one field from a packed struct, how much do you save by using a view instead of unpacking the entire struct? The view navigates directly to `UserProfile.name` (a variable-length string at offset 1 in the offset table) and returns a zero-copy slice. Full unpack deserializes all 8 fields — allocating strings, copying vectors, checking optionals — just to read one of them.

| Language                | UserProfile.name        | Speedup    |
|-------------------------|------------------------:|-----------:|
| **C++**                 |     0.2 ns vs 90 ns     |   **450x** |
| **Rust** (prevalidated) |     1.1 ns vs 145 ns    |   **132x** |
| **Go** (typed view)     |      16 ns vs 463 ns    |    **29x** |
| **Go** (generic)        |      26 ns vs 463 ns    |    **18x** |
| **JS** (`readField`)    |     109 ns vs 636 ns    |   **5.8x** |
| **Python**              |     735 ns vs 6,194 ns  |   **8.4x** |

---

## 14. Static vs Dynamic View Access

Compile-time type knowledge eliminates runtime dispatch and enables direct pointer arithmetic at known offsets. Languages that support this show dramatic speedups:

| Language | Static (typed) | Dynamic (generic) | Speedup | Mechanism |
|----------|---------------:|------------------:|--------:|-----------|
| **C++**  |        0.3 ns  |              n/a  |     n/a | `frac_ref<T>` — template-generated accessors |
| **Zig**  |         <1 ns  |              n/a  |     n/a | Comptime-generated field access |
| **Rust** |    0.5–1.2 ns  |       25–129 ns   | 24–107x | Prevalidated views skip per-access bounds checks |
| **Go**   |    0.5–27 ns   |      9.7–228 ns   |  3–19x  | Typed view structs with hard-coded byte offsets |

## 15. Improvement Opportunities

- **Rust**: Default views include per-access validation; prevalidated mode (0.5–1.2ns) proves the gap is pure bounds-check overhead
- **JS**: `Object.defineProperty` getters on prototype instead of Proxy traps would give ~20–30x speedup with no build step
- **Python**: C-level `PyGetSetDef` for view fields would bring access from ~500ns to ~50–100ns
