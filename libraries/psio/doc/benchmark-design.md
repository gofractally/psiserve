# Fracpack Benchmark & Stress Test Framework

## Goals

1. **Correctness at scale** — verify pack/unpack/view/validate/JSON across data shapes from trivial to deeply nested, with element counts from 0 to 100K
2. **Fracpack vs competitors** — compare against JSON, MessagePack, and Protocol Buffers in each language
3. **Cross-language consistency** — same schemas, same data, same metrics across all 7 languages
4. **View advantage quantification** — measure the speedup of zero-copy views vs full unpack for partial field access

## Benchmark Schemas

All languages implement the same 8 data shapes, defined in `test_vectors/benchmark_schemas.json`. Each shape targets a specific performance characteristic.

### Tier 1: Micro (fixed-size, no heap allocation)

```
Point { x: f64, y: f64 }                          # 16 bytes, DWC
RGBA  { r: u8, g: u8, b: u8, a: u8 }              # 4 bytes, DWC
```
**Tests**: raw encode/decode throughput ceiling, memcpy-competitive packing.

### Tier 2: Small (1-2 variable fields)

```
Token { kind: u16, offset: u32, length: u32, text: string }    # ~30 bytes
```
**Tests**: offset indirection overhead, string decode cost.

### Tier 3: Medium (many fields, mixed types)

```
UserProfile {
    id:       u64,
    name:     string,
    email:    string,
    bio:      optional[string],
    age:      u32,
    score:    f64,
    tags:     vec[string],
    verified: bool
}                                                              # ~120 bytes
```
**Tests**: partial vs full decode tradeoff, optional handling, vec of strings.

### Tier 4: Nested (struct in struct)

```
LineItem { product: string, qty: u32, unit_price: f64 }

Order {
    id:       u64,
    customer: UserProfile,
    items:    vec[LineItem],
    total:    f64,
    note:     optional[string],
    status:   variant(pending, shipped: string, delivered: u64, cancelled: string)
}                                                              # ~300 bytes
```
**Tests**: nested struct offset chasing, variant encoding, deep field access.

### Tier 5: Wide (many fields)

```
SensorReading {
    timestamp: u64,
    device_id: string,
    temp: f64, humidity: f64, pressure: f64,
    accel_x: f64, accel_y: f64, accel_z: f64,
    gyro_x: f64, gyro_y: f64, gyro_z: f64,
    mag_x: f64, mag_y: f64, mag_z: f64,
    battery: f32,
    signal_dbm: i16,
    error_code: optional[u32],
    firmware: string
}                                                              # ~160 bytes
```
**Tests**: wide fixed region scan, trailing optional elision.

### Tier 6: Large arrays (scaling)

Each of these wraps a Tier 1-3 type into vectors of increasing size:

```
PointCloud1K   { points: vec[Point] }          # 1,000 points  ~20 KB
PointCloud100K { points: vec[Point] }          # 100,000 pts   ~2 MB
TokenStream    { tokens: vec[Token] }          # 10,000 tokens ~300 KB
UserBatch      { users: vec[UserProfile] }     # 1,000 users   ~120 KB
```
**Tests**: vec decode scaling (O(n) vs O(1) for view), TypedArray view path for numeric vecs, throughput at realistic payload sizes.

### Tier 7: Deep nesting

```
TreeNode {
    value:    u32,
    label:    string,
    children: vec[TreeNode]
}
```
Generate balanced binary tree of depth 10 (1,023 nodes, ~40 KB).

**Tests**: recursive offset resolution, stack depth in validators, deeply nested JSON.

### Tier 8: System catalog (largest realistic type)

```
FuncParam    { name: string, type_name: string, doc: optional[string] }
FuncDef      { name: string, params: vec[FuncParam], returns: string, doc: string }
TypeField    { name: string, type_name: string, offset: u32, doc: optional[string] }
TypeDef      { name: string, kind: variant(struct_: vec[TypeField], enum_: vec[string], alias: string), doc: string }
ModuleDef    { name: string, types: vec[TypeDef], functions: vec[FuncDef], doc: string }
SystemCatalog {
    version:  u32,
    modules:  vec[ModuleDef],
    metadata: vec[variant(author: string, license: string, url: string)]
}
```
Generate a catalog with 10 modules, 20 types each, 5 functions each (~200 KB).

**Tests**: realistic "system dictionary" workload, many levels of nesting, variants in vectors, optional-heavy structs.

## Operations Benchmarked

| Operation | What it measures |
|-----------|-----------------|
| `pack` | Value → bytes. Allocation + serialization cost. |
| `unpack` | Bytes → value. Full deserialization cost. |
| `view_one` | Bytes → view, access 1 field. The fracpack advantage. |
| `view_all` | Bytes → view, access every field. View overhead vs unpack. |
| `validate` | Bytes → valid/invalid. Zero-copy security check. |
| `json_write` | Value → canonical JSON string. |
| `json_read` | Canonical JSON string → value. |
| `roundtrip` | pack → unpack → pack. Verifies byte-exact reproduction. |

For **view_one**, always access the **last** string field (worst case — maximum offset chasing).

### Load-Modify-Store (Mutation Benchmarks)

The critical real-world pattern: read from storage, change one field, write back.
Each benchmark loads packed bytes, modifies a single field via MutView, and
extracts the result. Measured at various positions in the struct tree:

| Position | Schema | Field | What it tests |
|----------|--------|-------|---------------|
| First fixed | `UserProfile` | `id` (u64) | Best case: in-place scalar overwrite |
| Last fixed | `SensorReading` | `signal_dbm` (i16) | Offset into wide fixed region |
| Root string | `UserProfile` | `name` (string) | Variable-size splice at root level |
| Root string (grow) | `UserProfile` | `name` (short→long) | Buffer growth + offset patching |
| Root string (shrink) | `UserProfile` | `name` (long→short) | Buffer shrink + offset patching |
| Optional toggle | `UserProfile` | `bio` (None→Some) | Optional materialization |
| Nested scalar | `Order` | `customer.age` (u32) | Nested struct field mutation |
| Nested string | `Order` | `customer.name` | Nested struct string splice |
| Vec element | `Order` | `items[0].product` | Field inside vec element |
| Deep path | `SystemCatalog` | `modules[0].types[0].doc` | 3+ levels deep mutation |

Compare against the naive alternative: `unpack → modify field → repack`.

## Competitor Formats

Each language benchmarks fracpack against the most common alternatives:

| Format | C++ | Rust | Python | JS | Zig | Go | MoonBit |
|--------|:---:|:----:|:------:|:--:|:---:|:--:|:-------:|
| JSON (baseline) | Y | Y | Y | Y | Y | Y | Y |
| WIT Canonical ABI | Y | Y | - | - | - | - | - |
| MessagePack | Y | Y | Y | Y | - | Y | - |
| Protocol Buffers | Y | Y | Y | Y | - | Y | - |
| FlatBuffers | Y | Y | - | - | - | Y | - |

WIT Canonical ABI is the WASM Component Model's standard data encoding. It uses natural alignment, (pointer, length) pairs for strings/lists, and positional record layout. psio provides code-first WIT schema generation and `CView<T>` for zero-copy viewing of Canonical ABI data in WASM linear memory. Competitors only need to implement `pack` and `unpack` (no view/validate — that's fracpack's differentiator).

## Metrics Reported

Per operation per schema:

| Metric | Unit | Notes |
|--------|------|-------|
| `ops_per_sec` | ops/s | Primary throughput metric |
| `median_ns` | ns | Median latency |
| `p99_ns` | ns | Tail latency |
| `encoded_bytes` | bytes | Wire size (for pack only) |
| `allocs` | count | Heap allocations (where measurable) |

## Output Format

Each language writes results to `benchmark_results/{language}.json`:

```json
{
  "language": "go",
  "timestamp": "2024-01-15T10:30:00Z",
  "platform": {"os": "darwin", "arch": "arm64", "cpu": "Apple M2"},
  "results": [
    {
      "schema": "UserProfile",
      "format": "fracpack",
      "operation": "pack",
      "ops_per_sec": 5200000,
      "median_ns": 192,
      "p99_ns": 340,
      "encoded_bytes": 118,
      "iterations": 1000000
    },
    {
      "schema": "UserProfile",
      "format": "json",
      "operation": "pack",
      "ops_per_sec": 890000,
      "median_ns": 1123,
      "encoded_bytes": 187
    }
  ]
}
```

A cross-language summary script reads all `{language}.json` files and produces:
- Markdown comparison tables
- Encoded size comparison chart
- View speedup heatmap (schema x language)

## Per-Language Harness

| Language | Framework | Invocation |
|----------|-----------|------------|
| C++ | Google Benchmark | `./build/Release/bin/fracpack_bench` |
| Rust | `criterion` | `cargo bench` |
| Python | `pytest-benchmark` | `pytest benchmarks/ --benchmark-json=out.json` |
| JS | Custom `performance.now()` | `node --experimental-vm-modules benchmarks/run.mjs` |
| Zig | `std.time.Timer` | `zig build bench` |
| Go | `testing.B` | `go test -bench=. -benchmem ./fracpack/` |
| MoonBit | Custom `@time.now()` | `moon run benchmarks/` |

## Test Data Generation

`test_vectors/generate_benchmark_data.py` produces:
1. `benchmark_schemas.json` — type definitions in a language-neutral format
2. `benchmark_data.json` — pre-generated values for each schema at each size tier
3. `benchmark_packed.json` — pre-packed hex bytes for each value (verify all languages produce identical bytes)

Data is deterministic (seeded PRNG) so results are reproducible across runs and languages.

## Stress Tests

Beyond performance, these verify correctness at scale:

| Test | What it verifies |
|------|------------------|
| Pack 100K UserProfiles, unpack each, compare | No corruption at volume |
| Pack 1MB string, view it | Large heap object handling |
| 100 nested TreeNode levels | Deep recursion doesn't stack overflow |
| All-None optional struct → pack → validate | Trailing elision at all positions |
| Random mutation of packed bytes → validate rejects | Validator catches all corruptions |
| Pack → view field → mutate field → pack → compare | Mutation preserves other fields |
| Golden vectors: pack in lang A, unpack in lang B | True cross-language wire compat |

## Implementation Priority

1. **Shared data generation** — `generate_benchmark_data.py` + schemas
2. **Go benchmarks** — fastest iteration cycle, `testing.B` is built-in
3. **Python benchmarks** — `pytest-benchmark` already in deps
4. **JS benchmarks** — extend existing `view-perf.test.ts` pattern
5. **Rust benchmarks** — add `criterion` dependency
6. **Zig benchmarks** — custom timer harness
7. **C++ benchmarks** — Google Benchmark integration
8. **MoonBit benchmarks** — custom timer
9. **Cross-language summary script** — reads all results, produces tables
