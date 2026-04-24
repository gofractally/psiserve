# psio3

v2-architecture port of `libraries/psio/`. Header-only C++23 serialization
library implementing the design in `.issues/psio-v2-design.md`.

Lives alongside `libraries/psio/` (v1) until every format passes
byte-parity + perf gates; at that point v1 is archived and `psio3/` is
renamed to `psio/`.

## Status

**Formats** (v1 byte-parity tested): ssz, pssz, frac, bin, borsh,
bincode, key, avro, json, flatbuf (native), flatbuf_lib (Google adapter).

**Formats** (Phase 14 MVP, round-trip via conformance sweep): capnp,
wit. MVP shape set is primitives + strings + vectors + reflected
records; variants / optionals / resources land in a follow-up once the
psio3 annotation surface is complete.

**33 ctest targets, all green.** Conformance harness sweeps 11 fixtures
× 9 symmetric binary formats (capnp-aware) + 8 record-only binary
formats for variant / bitvector / uint256 coverage. Cross-format
transcode hits every pair.

## The four DX patterns

### 1. End user — encode and decode

```cpp
#include <psio3/ssz.hpp>

struct Point { std::int32_t x, y; };
PSIO3_REFLECT(Point, x, y)

Point p{3, -7};
auto bytes = psio3::encode(psio3::ssz{}, p);        // generic CPO
auto bytes2 = psio3::ssz::encode(p);                // scoped sugar

auto back  = psio3::decode<Point>(psio3::ssz{}, std::span{bytes});
auto back2 = psio3::ssz::decode<Point>(std::span{bytes});

if (auto st = psio3::validate<Point>(psio3::ssz{}, std::span{bytes});
    !st.ok())
   return st.error();
```

All per-format operations flow through six CPOs: `encode`, `decode`,
`size_of`, `validate`, `validate_strict`, `make_boxed`. The tag type
carries the format identity; CPO selects the tag_invoke overload.

### 2. Type author — annotations without touching the struct

```cpp
struct Validator {
   std::string pubkey;
   std::uint64_t effective_balance;
   bool slashed;
};
PSIO3_REFLECT(Validator, pubkey, effective_balance, slashed)

// Add annotations after the fact:
PSIO3_FIELD_ATTRS(Validator, pubkey, psio3::length_bound{.exact = 48})
PSIO3_ATTRS(Validator,
   (effective_balance, (psio3::field_num_spec{.value = 3})),
   (slashed,           (psio3::field_num_spec{.value = 4})))
PSIO3_TYPE_ATTRS(Validator, psio3::definition_will_not_change{})
```

The struct stays clean. Size bounds, field numbers, and wire-stability
commitments live on the reflection, not on the type.

### 3. Format author — add a new format

```cpp
struct myfmt : psio3::format_tag_base<myfmt>
{
   // Hidden-friend tag_invoke overloads. encode/decode/size_of/
   // validate/validate_strict/make_boxed. See psio3/bin.hpp for the
   // simplest working template.
   template <typename T>
   friend std::vector<char> tag_invoke(
      decltype(::psio3::encode), myfmt, const T& v) { … }
   template <typename T>
   friend T tag_invoke(decltype(::psio3::decode<T>), myfmt, T*,
                       std::span<const char> bytes) { … }
   // …
};
```

Adding the tag to `PSIO3_FOR_EACH_SYMMETRIC_BINARY_FMT` in
`psio3/conformance.hpp` auto-extends the conformance harness — every
fixture round-trips through the new format with no per-format test code.

### 4. Dynamic / schema-driven — bytes without a compile-time T

```cpp
auto schema = psio3::schema_of<Validator>();
auto dv     = psio3::to_dynamic(validator);

auto bytes  = psio3::encode_dynamic(psio3::json{}, schema, dv);
auto decoded = psio3::decode_dynamic(psio3::json{}, schema, bytes);

// Cross-format transcode through the dynamic path:
auto frac_bytes = psio3::transcode(schema, psio3::json{}, in,
                                   psio3::frac32{}, out);
```

Gateway / RPC services that receive schemas at runtime use the same
CPOs — `encode_dynamic` / `decode_dynamic` / `transcode` — and get
byte-identical output to the static path.

## Architecture — five layers

```
Layer 5: Sugar                  scoped sugar via format_tag_base
Layer 4: CPOs                   psio3::encode / decode / size_of / …
Layer 3: Format dispatch        tag_invoke on the format tag
Layer 2: Storage                buffer<T, F>, view<T, F>, mutable_view<T, F>
Layer 1: Shape concepts         Primitive / FixedSequence / …
                                + reflection annotations
```

Every layer is monomorphized at compile time. No virtuals, no type
erasure in hot paths.

## Wire guarantees

Each format specifies its wire contract; psio3 encoders are
**byte-identical to v1 psio** on the shape set both support.

| Format  | Wire                                                 |
| ------- | ---------------------------------------------------- |
| ssz     | Ethereum SSZ (raw LE / offset tables / union tag)    |
| pssz    | SSZ variant with parametric offset width W ∈ {1,2,4} |
| frac    | fracpack v1 (u16 header + pointer-rel offsets)       |
| bin     | raw LE primitives + u32-length-prefixed containers   |
| borsh   | NEAR/Solana Borsh (u32 lengths, u8 variant tags)     |
| bincode | Rust bincode default (u64 lengths, u32 variant tags) |
| key     | memcmp-sortable: BE + sign-flip + \0\0 terminator   |
| avro    | Apache Avro (zig-zag varint integers)                |
| json    | JSON (positional variants: `[index, value]`)         |
| flatbuf | FlatBuffers — native + Google-runtime adapter        |

## Directory layout

```
cpp/include/psio3/
  <format>.hpp          — tag + encode/decode/... for one format
  dynamic_<format>.hpp  — runtime schema-driven codec
  reflect.hpp           — Layer-1 reflection
  annotate.hpp          — annotations + PSIO3_ATTRS / PSIO3_FIELD_ATTRS
  ext_int.hpp           — uint128 / int128 / uint256
  wrappers.hpp          — bounded<T,N> / utf8_string<N> / bitvector<N> / …
  schema.hpp            — runtime psio3::schema value
  dynamic_value.hpp     — runtime dynamic_value variant
  transcode.hpp         — format-to-format conversion
  conformance.hpp       — PSIO3_FOR_EACH_SYMMETRIC_BINARY_FMT harness

cpp/tests/
  <format>_tests.cpp            — per-format MVP coverage
  v1_parity_<format>_tests.cpp  — byte-identical to v1 on every fixture
  conformance_tests.cpp         — format-neutral sweep
  static_dynamic_parity_tests.cpp — encode == encode_dynamic
```

## Build

```bash
cmake -B build/Debug -G Ninja -DPSIO3_ENABLE_TESTS=ON
cmake --build build/Debug
cd build/Debug && ctest -R '^psio3_' -j$(nproc)
```
