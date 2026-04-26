# psio1::flatbuf — FlatBuffers Without Compromise

## Your Types. Your Code. Every Format.

PSIO lets you define your data structures once as plain C++ and serialize them
to any wire format — FlatBuffers, fracpack, JSON, Avro, Bincode — without code
generation, without schema files, and without giving up control of your types.

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
PSIO1_REFLECT(UserProfile, id, name, email, bio, age, score, tags, verified)
```

That's it. No `.fbs` file. No `flatc` invocation. No generated headers. No build
system integration for code generators. The struct *is* the schema.

## Key Selling Points

### Wire-Compatible Without Compromise

Our builder produces **byte-identical** output to the official FlatBuffers
library. Any language with a FlatBuffers reader — Python, Rust, Go, Java, C#,
TypeScript — reads our buffers natively. And we read theirs.

Every type is verified against the official library in both directions:

| Type          | Standalone reads Official | Official reads Standalone | Wire Size |
|---------------|:------------------------:|:------------------------:|----------:|
| Point         | OK                       | OK                       | 32 B      |
| Token         | OK                       | OK                       | 56 B      |
| UserProfile   | OK                       | OK                       | 264 B     |
| Order         | OK                       | OK                       | 576-608 B |
| SensorReading | OK                       | OK                       | 208 B     |

Full format coverage: scalars, strings, vectors, nested tables, enums,
`std::optional`, `std::variant` (unions), `std::array` (fixed-length),
`definitionWillNotChange` structs (inline), nested FlatBuffers, file
identifiers, size-prefixed buffers, and buffer verification.

### Better Developer Experience

**Official FlatBuffers (C++):**
```cpp
// 1. Write a .fbs schema file
// 2. Run flatc to generate monster_generated.h
// 3. Include generated header
// 4. Build with this:

flatbuffers::FlatBufferBuilder fbb(1024);
auto name   = fbb.CreateString("Alice");
auto email  = fbb.CreateString("alice@example.com");
auto tags   = fbb.CreateVectorOfStrings({"admin", "verified"});
auto user   = CreateUserProfile(fbb, 42, name, email, 0, 25, 98.6, tags, true);
fbb.Finish(user);
```

**psio (C++):**
```cpp
// 1. That's it. Use your struct.

psio1::fb_builder fbb;
fbb.pack(user);
```

Reading is just as clean:

```cpp
// Unpack to a real struct
auto user = psio1::fb_unpack<UserProfile>(buf);

// Zero-copy view with named field access
auto v = psio1::fb_view<UserProfile>::from_buffer(buf);
uint64_t         id   = v.id();           // scalar by value
std::string_view name = v.name();         // zero-copy
auto             tags = v.tags();         // fb_vec<string> — iterable
```

Nested types compose naturally:

```cpp
auto o = psio1::fb_view<Order>::from_buffer(buf);
std::string_view customer_email = o.customer().email();  // chained zero-copy
```

### Mutable Buffers

Official FlatBuffers are immutable. We provide two mutation modes:

**`fb_mut<T>`** — in-place scalar mutation, zero overhead:
```cpp
auto m = psio1::fb_mut<UserProfile>::from_buffer(buf.data());
m.id()       = 999;       // direct assignment via proxy reference
m.verified() = false;
m.score()    = 42.0;

// Nested tables too:
auto o = psio1::fb_mut<Order>::from_buffer(buf.data());
o.customer().age() = 30;  // mutate through sub-tables
```

**`fb_doc<T>`** — full document mutation including strings:
```cpp
auto doc = psio1::fb_doc<UserProfile>::from_value(user);
doc.name()  = "Bob";           // O(1) — appends new string, updates offset
doc.email() = "bob@new.com";   // old string becomes dead space

doc.canonicalize();  // unpack + repack → minimal size, deterministic bytes
```

String mutation is O(1): the new string is appended at the buffer's end and a
single offset is updated. No offset tree-walking, no memmove. Call
`canonicalize()` when you need minimal size or deterministic representation.

### Canonical Representation

FlatBuffers has no concept of canonical form — field order, vtable layout, and
padding can vary between implementations. Our deterministic builder +
PSIO1_REFLECT's fixed field order means `canonicalize()` always produces
byte-identical output for logically identical data. This enables:

- **Content-addressable storage** — hash the buffer, use as a key
- **Equality comparison** — `memcmp` two canonicalized buffers
- **Deduplication** — identical data produces identical bytes
- **Consensus** — deterministic serialization for distributed agreement

### Use Your Own Types

No generated wrapper classes. No `FlatBufferBuilder` ceremony. Your C++ types
are the API:

- `std::string` for strings (not `flatbuffers::String`)
- `std::vector<T>` for vectors (not `flatbuffers::Vector`)
- `std::optional<T>` for nullable fields
- `std::variant<A, B, C>` for unions (not generated union enums)
- `std::array<T, N>` for fixed-length arrays
- `enum class` for enumerations (with `PSIO1_REFLECT_ENUM`)
- Nested structs compose naturally — no forward-declaration scaffolding

Your types work with your debugger, your IDE, your existing code. FlatBuffers
becomes a wire format, not an API you must design around.

### Struct-Aware Defaults

C++ default member initializers are automatically used as FlatBuffer defaults:

```cpp
struct Config {
   int32_t timeout = 30;
   float   scale   = 1.0f;
   bool    enabled = true;
   uint16_t retries = 3;
};
PSIO1_REFLECT(Config, timeout, scale, enabled, retries)
```

The builder omits fields that match `T{}` defaults — producing smaller buffers.
The reader restores the correct non-zero defaults for absent fields. No
`force_defaults` attribute, no schema-level default annotations. The C++ type
*is* the single source of truth.

A default-valued `Config` serializes to **32 bytes**. With custom values: **52
bytes**. Round-trip is byte-identical.

### Schema Export

Need an `.fbs` file for other languages? Generate it from your types:

```cpp
std::string schema = psio1::to_fbs_schema<Order>();
```

Produces standard FlatBuffer schema, including nested table definitions, enums,
unions, structs, and nested flatbuffer annotations. The schema is derived from
your C++ types — never the other way around.

## Performance

Benchmarked on Apple M4 (ARM64), single-threaded, `-O2`. Ratio < 1.0 means
psio is faster.

| Type          |  Pack  | Unpack |  View  |
|---------------|:------:|:------:|:------:|
| Point         | 0.96x  | 0.97x  | 1.00x  |
| Token         | 0.95x  | 0.42x  | 1.00x  |
| UserProfile   | 0.91x  | 1.03x  | 0.95x  |
| Order         | 0.67x  | 0.65x  | 1.00x  |
| SensorReading | 1.20x  | 0.97x  | 1.04x  |

**Packing** is consistently faster — up to **33% faster** on complex types (Order)
because the compiler sees through template instantiations to the actual struct
layout. No vtable indirection, no virtual dispatch, no type-erased field
iteration. The official library's `CreateXxx` builder functions are opaque
calls; ours are fully inlined.

**Unpacking** is up to **2.4x faster** on string-heavy types (Token) because we
read directly into `std::string` members without intermediate object
construction. On complex nested types (Order), **35% faster**.

**Zero-copy views** are at parity — both implementations resolve to the same
pointer arithmetic. The difference is that our views use named fields
(`v.name()`) without any generated code.

Optional **vtable deduplication** (`fb_dedup::on`) produces smaller buffers for
types with repeated sub-table schemas, at a small packing cost (hash table
lookup per table).

## Code Once, Serialize Anywhere

The real power of PSIO1_REFLECT is that one struct definition unlocks every
format:

```cpp
struct Order { /* ... */ };
PSIO1_REFLECT(Order, id, customer, items, total, note)

// FlatBuffers (zero-copy, cross-language)
psio1::fb_builder fbb;
fbb.pack(order);

// fracpack (compact, mutable views)
auto packed = psio1::fracpack<Order>(order);

// JSON (human-readable, debugging)
auto json = psio1::to_json(order);

// Binary (minimal overhead)
auto bin = psio1::to_bin(order);

// Avro (Hadoop/Kafka ecosystem)
auto avro = psio1::to_avro(order);

// Bincode (Rust interop)
auto bc = psio1::to_bincode(order);

// Sortable key encoding (database indexes)
auto key = psio1::to_key(order);
```

You never get locked into a wire format. Migrate from JSON to FlatBuffers for
performance. Use fracpack for internal storage. Export Avro for analytics
pipelines. The struct doesn't change. The serialization code doesn't change.
Only the format call differs.

Every other serialization library makes you choose a format first, then design
your types around it. PSIO inverts this: **design your types first, choose
formats later** — or use several simultaneously.

## Feature Matrix

| Feature                        | psio1::flatbuf | Official C++ | Official Rust |
|--------------------------------|:------------:|:------------:|:-------------:|
| Code generation required       |      No      |     Yes      |      Yes      |
| Schema file required           |      No      |     Yes      |      Yes      |
| Use native language types      |     Yes      |      No      |       No      |
| Named field views              |     Yes      |   Yes (gen)  |   Yes (gen)   |
| In-place scalar mutation       |     Yes      | Yes (`--gen-mutable`) | No   |
| String/dynamic mutation        |     Yes      |      No      |       No      |
| Canonical representation       |     Yes      |      No      |       No      |
| Struct-aware defaults          |     Yes      |   Partial    |    Partial    |
| Schema export from types       |     Yes      |     N/A      |      N/A      |
| Multi-format from same types   |     Yes      |      No      |       No      |
| Vtable deduplication           |   Optional   |    Always    |    Always     |
| Buffer verification            |     Yes      |     Yes      |      Yes      |
| Nested FlatBuffers             |     Yes      |     Yes      |      Yes      |
| Unions (variant)               |     Yes      |     Yes      |      Yes      |
| Inline structs                 |     Yes      |     Yes      |      Yes      |
| File identifiers               |     Yes      |     Yes      |      Yes      |
| Size-prefixed buffers          |     Yes      |     Yes      |      Yes      |
| Header-only                    |     Yes      |      No      |       No      |
| Wire-compatible                |     100%     |   Baseline   |     100%      |

## Why This Matters

Every serialization framework in wide use — FlatBuffers, Protobuf, Cap'n Proto,
Thrift, Avro — exists because languages historically lacked compile-time struct
introspection. Without the ability to enumerate fields at compile time, you need
an external schema language and a code generator to produce the boilerplate.

PSIO1_REFLECT closes that gap for C++. One macro per struct gives the compiler
everything it needs to generate serialization code for any format, at compile
time, with full optimization visibility. The result is a library that is faster,
safer, simpler, and more flexible than the codegen approach — while remaining
100% wire-compatible with the ecosystems those code generators built.

Other languages already have this capability natively: Rust's `derive` macros,
Go's struct tags, Python's dataclasses, Java's reflection. C++ was the last
holdout. It isn't anymore.
