# psio: follow-ups for bounded types, ext ints, and schema

## Context

Phase A1–A2.5 (completed in the 2026-04-23 session) added:

- `psio1::uint128`, `psio1::int128`, `psio1::uint256` (ext int types)
- `psio1::bounded_list<T, N>`, `psio1::bounded_string<N>`, `psio1::bounded_bytes<N>`
- `schema_types::BoundedList { type, maxCount }`
- `SchemaBuilder` branches for all of the above
- WIT emission for `BoundedList` via `list<T>/* @psio:max=N */` comment

This issue tracks everything those phases **deferred**. All items are
additive — nothing blocks Phase A3 (bitvector/bitlist) or Phase B (SSZ).

## 1. Schema emitters: cap'n proto and flatbuffers

Today `emit_wit.cpp` renders `BoundedList` with a comment convention.
`capnp_schema` and `fbs_parser` don't yet know about `BoundedList` at
all — any schema containing bounded types round-trips through those
formats losing the bound.

### Cap'n Proto

- Emit fields as `List(T) $Psio.maxCount(N)` using capnp annotations
- Define a `Psio` annotation namespace in a shared `.capnp` file
- Parser side: `schema_import/capnp_parser.rs` (Rust) must recognize
  `$Psio.maxCount(N)` and map to `bounded_list<T, N>` / `BoundedList`
- C++ parser at `capnp_parser.hpp` needs matching handling

### FlatBuffers

- Emit as `field_name: [T] (psio_max: N)` — FBS accepts unknown field
  attributes and preserves them
- Parser side: `fbs_parser.hpp` / `schema_import/fbs_parser.rs` must
  read the attribute and produce `BoundedList`

### JSON Schema export

- `BoundedList` → `{"type": "array", "maxItems": N}`
- `bounded_string` → `{"type": "string", "maxLength": N}`
- If `to_json(Schema)` path is used as an export format, update it too

## 2. Rust `DynamicSchema` BoundedList variant

`libraries/psio1/rust/psio/src/dynamic_schema.rs` has a `DynamicType`
enum that currently lacks a bounded-list variant. Needs:

- `DynamicType::BoundedList { inner: Box<DynamicType>, max_count: u64 }`
- Update `dynamic_schema.rs` serialization and validation paths
- Update `schema_export/mod.rs` to emit the bound via the appropriate
  per-format convention (see item 1)
- Update `schema_import/{capnp,fbs,wit}_parser.rs` to parse the
  respective annotations into `DynamicType::BoundedList`

Without this, the Rust `psio-tool codegen` command silently drops the
bound when generating code from a schema source.

## 3. Codegen for bounded types across target languages

`psio-tool codegen` supports cpp/rust/go/typescript/python/zig. Each
needs a decision on how to represent `BoundedList`:

| Target | Proposal |
|---|---|
| cpp | `psio1::bounded_list<T, N>` |
| rust | `psio1::bounded_list<T, N>` (needs Rust-side type; wraps `Vec<T>`) |
| go | `type FieldName struct { Items []T; MaxN uint64 }` — no generic bounds |
| typescript | `class BoundedList<T> { items: T[]; readonly maxN: number }` |
| python | `class BoundedList(List[T])` with `__init__` bound check |
| zig | `std.BoundedArray(T, N)` — first-class! |

Plus `bounded_string<N>` and `bounded_bytes<N>` analogues. See
`codegen_cpp` / `codegen_rust` / etc. in `psio-tool/src/main.rs:663-913`.

## 4. Variable-element `bounded_list<T, N>`

Current fracpack specialization (`packable_bounded_memcpy_impl` in
`fracpack.hpp`) only handles memcpy-safe element types — mirrors
`packable_container_memcpy_impl`. For `bounded_list<std::string, N>`
or `bounded_list<MyStruct, N>` (non-memcpy), fracpack falls through to
the generic struct path which fails.

Needs a parallel `packable_bounded_container_impl` covering the
variable-element case, matching `packable_container_impl`'s protocol
(per-element embedded_fixed_pack + embedded_variable_pack, with the
bounded length prefix at the front).

## 5. Member-level attribute propagation for bounds

The current design puts `maxCount` on the `BoundedList` type itself.
Alternative: keep `List` as-is and carry the bound via a `Member.attributes`
entry (`{"name": "max", "args": ["N"]}`).

Pros of attribute form: existing `List` stays transparent;
attribute syntax already travels through `Member`.

Cons: bound becomes member-level metadata instead of type-level,
so two fields of `list<T>` with different bounds are still the same
type in the schema (they'd only differ at their member's attribute).

Decision was made to go with `BoundedList` as a distinct type. This
item exists only to record that the alternative was considered and
rejected; reopen if a future requirement needs bound-at-member
granularity.

## 6. `__int128` stdlib integration

Current gaps with `unsigned __int128` / `__int128`:

- `std::numeric_limits` specialization: works on Clang under
  `-std=gnu++XX` but not reliably elsewhere. Provide our own
  `psio1::numeric_limits<psio1::uint128>` etc. if users need it.
- `std::hash` specialization: none today. Provide if container
  key usage becomes a pattern.
- `std::is_integral`: returns `false` historically on both
  Clang and GCC (per the C++ standard — extensions not integrals).
  Generic "is this an integer?" code using `is_integral_v` will
  miss them. Documented; likely doesn't need a workaround.
- Printing: no `operator<<(std::ostream&, __int128)` in libc++/libstdc++.
  Add `psio1::to_string(uint128)` / `psio1::to_string(int128)` helpers
  (base-10, optional base-16) if debug output becomes a pain.

All minor. Open only if concrete friction appears.

## 7. `uint256` API surface

Currently minimal: default construction, from-u64, from-uint128,
equality/ordering. No arithmetic. Document that users needing math
should convert to their preferred bignum (`intx::uint256`,
`boost::multiprecision::uint256_t`) via the LE limb array.

If an in-psio arithmetic implementation becomes warranted (e.g., for
Merkle root computation over validator balances, or SSZ hash_tree_root
of uint256 lists), the cleanest add is:

- `psio1::uint256` arithmetic ops (+, -, *, /, %, shifts, bitwise)
- Explicit conversion from/to `intx::uint256` if we adopt it as a
  dependency

Defer until a real use case appears.

## Priority ordering (recommended)

**Policy:** Rust-side work (items 1, 2, 3) stays deferred until the
C++ side is feature complete through Phase D. This keeps the design
malleable — premature Rust parity would pin decisions we'd want to
revisit as SSZ / bitvector / hash_tree_root land.

1. **Item 4 (variable-element bounded_list)** — needed for SSZ types
   with `List[Container, N]` where Container is variable-size. Promoted
   to next in line after Phase A2.5.
2. **Phase A3 (bitvector/bitlist)** then **Phase B/C/D** proceed on C++
3. **Items 1 + 2 (C++/Rust cross-format + Rust DynamicSchema)** —
   revisit after Phase D is green on C++. Do as a cohesive Rust sync
   pass rather than drip-feeding.
4. **Item 3 (codegen across languages)** — nice-to-have once Rust side
   is caught up; most users work from reflected types rather than
   schema codegen
5. **Items 5–7** — keep closed unless a real requirement shows up

## Tracking

- Phase A1–A3: ext int + bounded + bitvector/bitlist — completed
- Phase A2.5: schema support — completed
- This issue: bridges to other format consumers and stdlib
- Phase B: SSZ wire format — completed
- Phase B.5: ssz_view + ssz_validate — completed
- Phase C: hash_tree_root — pending
- Phase D: BeaconState type + real mainnet-genesis loading — completed
- Phase D.1: psio_bench_ssz_beacon scaffold with optional sszpp — completed

## 8. sszpp comparison: Linux/GCC only

The `psio_bench_ssz_beacon` CMake scaffold exists for head-to-head timing
against OffchainLabs/sszpp. Current state (2026-04-23):

- Default build (`PSIO1_BENCH_SSZPP=OFF`) is psio-only and works everywhere
- Enabling the competitor requires **libstdc++ / GCC 15+**. sszpp uses
  `std::views::chunk` and `std::views::enumerate`, C++23 range adaptors
  libc++ (Apple Clang / LLVM libc++) doesn't ship yet (as of LLVM 22).
- Deps chain: sszpp + chfast/intx + prysmaticlabs/hashtree (run `make`)
- Setup instructions are inline at the top of `bench_ssz_beacon.cpp`.

Follow-up work:
- If sszpp grows Phase 0 BeaconState support (or psio extends to Altair),
  revisit the full-state comparison. Today their beacon_state_t is Altair
  and can't round-trip our Phase 0 genesis.

## 8a. SSZ encode gap vs sszpp — **RESOLVED (2026-04-23)**

**Before:** psio encode 295 µs, sszpp 69 µs on a 21 063-validator list (2.43 MiB).

**Fix:** two changes.

1. **`has_bitwise_serialization<std::array<T, N>>`** now returns `true`
   when `T` is bitwise (stream.hpp). Previously returned false, which
   cascaded through every run-batching/memcpy predicate in the codebase.
2. **`ssz_memcpy_ok_v<T>`** (to_ssz.hpp) consults a shared
   `layout_detail::is_memcpy_layout_struct<T>()` that returns true when
   all fields are bitwise AND `sizeof(T) == sum of member sizes`. The
   SSZ container encoder uses it to collapse a packed struct to a single
   `stream.write(&obj, sizeof(T))`, and `std::vector<T>` / `std::array<T, N>`
   extend the same check for bulk memcpy of the whole container.
3. **`__attribute__((packed))` on `eth::Validator`** eliminates
   `sizeof(Validator) = 128` (alignment padding after `bool slashed`)
   so it matches the SSZ wire size of 121.

**After** (5 runs, ±1% variance):

| Op      | psio   | sszpp  | Outcome             |
|---------|-------:|-------:|---------------------|
| decode  | 78 µs  | 75 µs  | tied (within 4%)    |
| encode  | 40 µs  | 69 µs  | **psio 1.72× faster** |

Full BeaconState (Phase 0, 5.15 MiB, psio-only):
- decode ~214 µs, encode ~91 µs (down from 262 µs)

Encode throughput now ~60 GiB/s — close to memory bandwidth.

Because the fix is in stream.hpp, it propagates to every format
encoder (pack_bin DWNC, bincode, borsh, fracpack) whose run-batching
path checks `has_bitwise_serialization`. Any struct with fixed byte
arrays now takes the memcpy path that it couldn't before.

## 8b. GCC vs Clang perf gap on psio — mostly closed

After the 8a fix, both compilers hit the memcpy fast path. Encode is
now compiler-independent (~40 µs both). Decode still has a small gap
that's not worth chasing until it becomes a real bottleneck.

## 8c. Decode zero-init tax — **RESOLVED (2026-04-23)**

Even after 8a, psio SSZ decode was 10% slower than sszpp on the 260 MiB
REAL-mainnet validator list (8.7 ms vs 8.0 ms). Investigation on the
actual library sources (`/tmp/sszpp-build/sszpp/lib/lists.hpp` — "you
have their code, why speculate") and a direct A/B microbench showed
libstdc++ was value-initializing our `std::vector<eth::Validator>` on
`resize(n)` because `__attribute__((packed))` defeats its trivial-type
fast path:

```
vector<Validator (packed)>::resize(n) only              4.78 ms   (zero-init 260 MiB)
vector<Validator (packed)>::resize(n) + memcpy          8.72 ms   (2× write bw)
vector<Validator (packed)>::assign(p, p+n)              3.94 ms   (single memcpy)
vector<validator_t (unpacked)>::resize(n) only [sszpp]  2.15 ms   (no zero-init)
malloc + memcpy + free (hw floor)                       3.93 ms
```

**Fix:** replaced `v.resize(n); memcpy(...)` with
`v.assign(reinterpret_cast<const T*>(src+pos), ...+n)` in the memcpy
fast paths of:
- `from_ssz.hpp`: `from_ssz(std::vector<T>&, ...)` under `ssz_memcpy_ok_v`
- `fracpack.hpp`: `packable_container_memcpy_impl::unpack`

libstdc++'s `assign(first, last)` for trivially copyable T with pointer
iterators lowers to `__uninitialized_copy_a` → single memcpy, skipping
the zero-init pass.

**After** (REAL mainnet 260 MiB validator list):

| Op      | psio (was)  | psio (now)  | sszpp  | Outcome               |
|---------|------------:|------------:|-------:|-----------------------|
| decode  | 8.72 ms     | **3.93 ms** | 8.04 ms| **psio 2.05× faster** |
| encode  | 5.00 ms     | 4.98 ms     | 7.35 ms| psio 1.48× faster     |

psio decode now matches the `malloc+memcpy` floor (3.93 vs 3.95 ms) —
we are at single-core memory bandwidth ceiling (64.8 GiB/s on M5).
On the 310 MiB real Fulu BeaconState, decode runs at 62.5 GiB/s vs a
64.6 GiB/s memcpy ceiling.
