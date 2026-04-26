# pssz (PsiSSZ): hybrid fracpack + SSZ format

Status: design, not yet prototyped. Conversation captured 2026-04-23.

## Motivation

Format comparison on the 5 standard psio bench types (REAL, honest numbers
after the SSZ float + std::optional bugs were fixed in that session):

| Type          | frac32 | frac16 | bin | bincode | ssz | wit |
|---------------|-------:|-------:|----:|--------:|----:|----:|
| BPoint        |     16 |     16 |  16 |      16 |  16 |  16 |
| Token         |     35 |     31 |  27 |      33 |  29 |  35 |
| UserProfile   |    211 |    179 | 156 |     210 | 178 | 223 |
| Order         |    449 |    377 | 317 |     413 | 377 | 454 |
| SensorReading |    157 |    147 | 140 |     152 | 148 | 161 |

Expectation going in: SSZ should beat fracpack on size by eliminating the
4-byte length prefix per variable field. After the SSZ fixes, frac16 and
honest SSZ are within 1–2 bytes of each other on every type — because
frac16 halved the offset width. The size race is closer than expected.

This suggests a format that combines:
- **SSZ's implicit sizing** (container-relative offsets, no length prefix)
- **fracpack's schema-evolution story** (extensibility header, trailing-
  optional pruning, unknown-field handling)
- **fracpack's DWNC memcpy fast path** for all-fixed structs
- **fracpack's u16/u32 offset-width switch** (pssz16 / pssz32)

…could beat both on size for extensible schemas while keeping the O(1)
random-access guarantee both enjoy.

## Wire format

### Per-container layout (non-DWNC)

```
+------------------+-----------------+------------------+
| u16 fixed_size   | fixed region    | heap (payloads)  |
| (extensibility)  | [offsets, fixed |                  |
|                  |  field values]  |                  |
+------------------+-----------------+------------------+
      2 B              N B              M B
```

`fixed_size` is identical to fracpack: the writer's commitment to the
number of bytes occupied by its fixed region (offset slots + inline fixed
values). A newer decoder with more fields can detect N' > N and treat
trailing fields as absent/default.

### Offsets

- **Container-relative**: each offset slot contains the byte distance from
  the start of the fixed region to the payload start. (SSZ style.)
- **No length prefix** in payload. Size of field i = offsets[i+1] −
  offsets[i] (or total_end − offsets[N-1] for the last variable field).

This is the core size win: **every non-empty variable field saves 4
bytes** vs fracpack's length-prefixed payloads.

### Empty fields

Implicit: `offsets[i] == offsets[i+1]` ⇒ field i is empty.
- Empty `std::string`, empty `std::vector<T>`, empty `bounded_list`:
  no payload, offset slots are equal.
- No sentinel is reserved in the offset table, so **neighbor lookups stay
  O(1)** — simple subtraction with no scan.

### Optionals

Rule: whether a selector byte is needed depends on whether
`min_encoded_size<T>` can be 0.

**No selector (min > 0):**
- All primitives (arithmetic, enums, ext_int types)
- `std::array<T, N>` where N > 0
- `bitvector<N>`, `bitlist<N>` (min 1 byte for terminator)
- Reflected structs (the u16 `fixed_size` header alone makes min ≥ 2)
- DWNC reflected structs (min = sizeof(T))

For these, `None` ↔ 0 bytes, `Some` ↔ some nonzero bytes. The decoder
disambiguates by size; no wire overhead for presence.

**1-byte Union selector (min == 0):**
- `std::string`, `std::vector<T>`, `bounded_string<N>`, `bounded_list<T,N>`
- `std::optional<T>` recursively (`Optional<Optional<X>>` can't self-
  disambiguate — outer None looks like outer Some(inner None) at 0 bytes)

Encoding: `[0x00]` for None, `[0x01][payload]` for Some.

This is the same as SSZ's `Union[null, T]` but applied selectively to
only the types that actually need disambiguation.

### DWNC fast path

Unchanged from fracpack: if a reflected struct has all fixed-size fields
and its in-memory layout matches the wire layout (no alignment padding or
user applied `__attribute__((packed))`), skip the u16 header and encode
via single memcpy. Decoder does the inverse.

### Trailing-optional pruning

The writer may omit trailing fields whose value equals the default (None
for Optional, zero for primitives, etc.). `fixed_size` in the header tells
the decoder how many fields' worth of fixed region were actually written.
The decoder synthesizes defaults for everything past `fixed_size`.

This is fracpack's existing mechanism; pssz inherits it untouched.

## Feature tally

### pssz wins over SSZ

- u16 `fixed_size` header → forward/backward schema compatibility
- Trailing-None pruning (≈5 B saved per pruned optional)
- Unknown-field tolerance at decode
- DWNC memcpy fast path for all-fixed structs (skips the u16 header)
- u16 offset variant (pssz16) for records < 64 KiB

### pssz wins over fracpack

- 4 B saved per non-empty variable field (implicit sizing)
- Simpler offset math for zero-copy views — container-relative means the
  view base pointer is constant across field accesses, no per-field base
  computation

### pssz costs vs SSZ

- +2 B per extensible container (u16 header; zero for DWNC)
- +1 B per None<variable T> (Union selector) — but this is the same cost
  SSZ already pays for all optionals

### pssz costs vs fracpack

- Subobjects are **not self-contained** — a view on a subobject needs the
  root pointer as context, same as SSZ views. This breaks the "fractal"
  property. Acceptable for storage/transmission formats; mutation stories
  already separate from these.

## Design question left open

Do we want pssz as:

1. **A new `frac_format_pssz32` / `_pssz16` variant** alongside
   existing frac_format_32/16 — user opts in per type. Easy to prototype,
   lets us compare in real workloads before committing.
2. **A replacement** for fracpack's existing 32/16 — cleaner long-term
   but breaks wire format for every existing fracpack user.

Recommendation: (1) first, real data decides, then possibly (2).

## Size traits: min and max

pssz needs two compile-time traits. Together they drive both the
selector-or-not decision (min rule) and the offset-width auto-select
(max rule).

### `min_encoded_size<T>` — decides if optional<T> needs a selector

| T                                 | min_encoded_size<T>                 |
|-----------------------------------|-------------------------------------|
| arithmetic, enum, ext_int         | sizeof(T)                           |
| std::array<T, N>                  | N × min<T>                          |
| bitvector<N>                      | (N + 7) / 8                         |
| bitlist<N>                        | 1                                   |
| std::string, string_view          | 0                                   |
| std::vector<T>, bounded_list<..>  | 0                                   |
| bounded_string<N>                 | 0                                   |
| std::optional<T>                  | 0                                   |
| Reflected (DWNC)                  | sizeof(T)                           |
| Reflected (extensible)            | hdr + Σ min<field>                  |

### `max_encoded_size<T>` — compile-time upper bound (superseding `psio-frac-max-dynamic-size.md`)

Reports `std::optional<std::size_t>`: a value when a bound exists,
`nullopt` when a member is truly unbounded (`std::vector<T>`,
`std::string`, unbounded recursive types).

| T                                 | max_encoded_size<T>                                   |
|-----------------------------------|-------------------------------------------------------|
| primitives (u8…u64, float, double)| sizeof(T)                                             |
| uint128 / uint256                 | 16 / 32                                               |
| std::array<T, N>                  | N × max<T>  (nullopt if max<T> nullopt)               |
| bitvector<N> / std::bitset<N>     | (N + 7) / 8                                           |
| bitlist<N>                        | sizeof(length_t<N>) + (N + 7) / 8 (pssz: just length) |
| bounded_list<T, N>                | N × max<T> + selector_overhead (pssz) — no length pfx |
| bounded_string<N>                 | N                                                     |
| std::optional<T>                  | (min<T>==0 ? 1 : 0) + max<T>                          |
| std::vector<T> / std::string      | **nullopt** — fall back to widest format              |
| Reflected (DWNC)                  | sizeof(T)                                             |
| Reflected (extensible)            | header_bytes + Σ max<field> + selector_bytes          |

A `PSIO_MAX_ENCODED_SIZE(T, N)` macro lets users commit to a bound
explicitly. Needed when a type contains a `std::vector` but the user
knows from context it won't exceed N elements, or when they want a
stable bound independent of what the compiler can currently deduce.

## Offset- and header-width auto-select

Given `max_encoded_size<T>()`, the narrowest width that safely encodes
every value of T is:

| max_encoded_size<T> | offset width | header width | format                 |
|---------------------|-------------:|-------------:|------------------------|
| ≤ 0xff              | u8 (1 B)     | u8           | internal auto tier     |
| ≤ 0xffff            | u16          | u16          | `frac_format_pssz16`   |
| ≤ 0xffffffff        | u32          | u32          | `frac_format_pssz32`   |
| unbounded           | u32          | u16 (as now) | `frac_format_pssz32u`  |

`frac_format_pssz8` exists only as the internal 8-bit auto tier. It is
not an official user-selected format facade; `pssz` may choose it when
`max_encoded_size<T>() <= 0xff`, while public override knobs start at
`pssz16` and `pssz32`.

For mixed embedding: each container picks its own width based on its
own `max_encoded_size`. A pssz32 container can embed a child whose
auto-selected width is the internal 8-bit tier; the child's offset
slots in its own fixed region are 1 B each, while the parent's offset
slot pointing at the child is 4 B. Both sides agree on each type's
format, so round-trips work.

## Auto-select policy

```cpp
template <typename T>
using auto_pssz_format_t =
    std::conditional_t<max_encoded_size<T>() && *max_encoded_size<T>() <= 0xff,
        frac_format_pssz8,
        std::conditional_t<max_encoded_size<T>() && *max_encoded_size<T>() <= 0xffff,
            frac_format_pssz16,
            frac_format_pssz32>>;
```

### Opt-in vs opt-out

Default behavior on `to_pssz(T, value)` / `psio::pssz`:

- Explicit format: `to_pssz<frac_format_pssz16>(v)` — user picks.
- Auto: `to_pssz(v)` with no format tag → `auto_pssz_format_t<T>`.
- No public `pssz8` override: the 8-bit width is selected only by auto
  after the compile-time bound proves the whole value fits.

A `PSIO_PSSZ_AUTO(T)` macro flips a per-type default if opt-out is
preferred later. Starting opt-in is safer — users explicitly ask for
the narrower format when they accept the 64 KiB / 256 B bound.

### Schema evolution hazard

A bounded type growing past a width boundary (adding a field that pushes
max past 64 KiB, or changing `bounded_list<T, 100>` to `<T, 100000>`)
**silently flips the wire format**. Old encoders emit narrower bytes;
new decoders expect wider. Mitigation:

- `PSIO_MAX_ENCODED_SIZE(T, N)` pins the compiler's view of max. If the
  type later grows past N, compile fails instead of silently flipping.
- Version-embed the format tag in the stored bytes (1-byte header
  prefix) — costs 1 B but catches mismatches at decode. Probably wanted
  for persisted storage; not for in-process RPC.

## Combined size win example

Token (u16 kind, u32 offset, u32 length, std::string text — the bench
type with 1 variable field, current frac32 = 35 B):

- frac32: 2 B hdr + (2 + 4 + 4) inline + 4 B offset + 4 B length + 4 B
  content = 24 B overhead + 4 B content = 35 B. Wait — the length
  prefix lives at the payload start so overhead is 18 B structural.
- pssz32: 2 B hdr + (2 + 4 + 4) inline + 4 B offset + 4 B content =
  31 B (no length prefix)
- pssz16: 2 B hdr + (2 + 4 + 4) inline + 2 B offset + 4 B content =
  29 B (SSZ parity)
- internal 8-bit pssz tier: 1 B hdr + (2 + 4 + 4) inline + 1 B offset + 4 B content =
  **27 B** (matches pack_bin, the current size leader)

UserProfile (max < 64 KiB once tags vector is bounded, 4 variable
fields):
- frac32 today: 211 B
- pssz16 (auto-selected): ~165 B (saves 4 B × 4 fields in length
  prefixes + 2 B × 4 fields in offset width vs frac32, no u8 — fixed
  region is ≥ 2 B offset × 4 = 8 B already, too wide for u8 header if
  total encoded can reach 8 KiB)

## Prototype path

Roughly 200–300 lines of codec changes, mostly in fracpack.hpp:

1. Add `frac_format_pssz32` / `_pssz16` with `using ssz_semantics = true`.
2. `pack`/`unpack` for std::string and std::vector: skip length prefix,
   derive size from caller-supplied span (caller computes via adjacent
   offset).
3. `is_packable<std::optional<T>>`: branch on `min_encoded_size<T> == 0`
   — selector path vs no-selector path.
4. Container `pack`: offset computation becomes start-of-fixed-region
   relative instead of slot-relative. One arithmetic change.
5. Container `unpack`: same, plus field size = next_offset - this_offset
   instead of reading a length prefix.

Tests:
- Round-trip all existing fracpack test types under the new format.
- Sentinel-free adjacency: `{""}`, `{"", ""}`, `{"", "x", ""}` all
  decode correctly.
- Optional<fixed> with no selector byte.
- Optional<string> with selector byte (None vs Some("") distinct).
- Cross-format: fracpack32, fracpack16, pssz32, pssz16 size comparison
  on BeaconState and bench types.

## Completed on 2026-04-23 overnight

- ✅ `min_encoded_size<T>` + `max_encoded_size<T>` traits (`to_pssz.hpp`)
- ✅ Format tags `frac_format_pssz16 / _32`, plus internal
  `frac_format_pssz8` selected only by auto
- ✅ Encoder with **single-pass backpatching** (no size recursion)
- ✅ Decoder with extensibility-header skip and auto-width
- ✅ `auto_pssz_format_t<T>` picks narrowest width via max_encoded_size
- ✅ `pssz_view<T, F>` zero-copy view (parallel to ssz_view / frac_view)
- ✅ Unit tests (10 pssz cases in `pssz_tests.cpp`)
- ✅ Benchmarked vs all formats on both unbounded and bounded types
- ✅ Generic `bounded<T, N>` wrapper added — legacy classes retained
- ✅ SSZ encoder inherited the single-pass backpatching fix
  (UserProfile pack 51→34 ns, Order pack 108→68 ns)
- ✅ Rust bootstrap port: `libraries/psio/rust/psio/src/pssz.rs` with
  primitives, String, Vec, Option. 13 tests, 6 of them cross-validate
  against C++ byte-for-byte.

## Deferred to pssz follow-up

- **Rust derive macro** for reflected-struct pssz (like `#[derive(Pack)]`)
- **`bounded<T, N>` migration**: sweep format specializations to use the
  generic wrapper + legacy class aliasing
- Schema IR + WIT/capnp/flatbuf/JSON schema emitters for pssz types
- `hash_tree_root` for pssz (if eth compatibility ever wanted)
- pssz ↔ SSZ transcoding (lossy — we carry extensibility info they don't)
- Avro / WIT support for `bounded<T, N>` (currently std-shadowed)

## Non-goals

- Ethereum consensus layer adoption. Eth has its own SSZ; pssz is for
  psio-ecosystem extensible schemas.
- In-place mutation. Mutators are a separate short-lived format by
  design; not a wire-format problem.
