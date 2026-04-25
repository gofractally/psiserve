# psio3 Benchmark Status

Tracks where we have measured numbers, where we have inherited baselines,
and what's needed for real v3-vs-competitor head-to-heads.

## v3 vs v1 — measured

See `libraries/psio3/PERF_V1_V3_REPORT.md` for the full table. Captured
2026-04-24 via `libraries/psio3/cpp/tests/smoke_perf.cpp`.

Headline numbers (v3/v1, smaller = v3 faster):

| op            | best v3 win                  | sole regression          |
|---------------|------------------------------|--------------------------|
| Header decode | bin / ssz / pssz: 0.69-0.79× | —                        |
| BS×256 decode | bin: 0.27×, frac32: 0.54×    | —                        |
| Header encode | bin: 0.64×, capnp: 0.42×     | —                        |
| BS×256 encode | bin: 0.33×                   | **frac32 enc BS: 1.65×** |
| size_of       | bin BS: 0.00× (formula)      | —                        |

v3 wins or ties v1 on every measured op except frac32 encode of
BeaconState×256, which the audit attributes to the three-phase pack
walker's per-element cost.

## v3 vs competing native libraries — not yet measured

`libraries/psio3/cpp/benchmarks/` is a stub (CMakeLists only). The
infrastructure that already exists for v1/v2:

| comp bench file                 | location                          | competitor target                |
|---------------------------------|-----------------------------------|----------------------------------|
| `bench_ssz_beacon.cpp`          | `libraries/psio2/cpp/benchmarks/` | OffchainLabs/sszpp (C++)         |
| `bench_capnp.cpp`               | `libraries/psio2/cpp/benchmarks/` | libcapnp (C++)                   |
| `bench_flatbuf.cpp`             | `libraries/psio2/cpp/benchmarks/` | libflatbuffers (Google)          |
| `bench_standalone_flatbuf.cpp`  | `libraries/psio2/cpp/benchmarks/` | libflatbuffers (Google, plain)   |
| `bench_protobuf.cpp`            | `libraries/psio2/cpp/benchmarks/` | protobuf-cpp (baseline)          |
| `bench_msgpack.cpp`             | `libraries/psio2/cpp/benchmarks/` | msgpack-c (baseline)             |
| `bench_wit.cpp`                 | `libraries/psio2/cpp/benchmarks/` | (psio-only — no competitor yet)  |

Each of these is 550-975 lines and uses psio2-specific APIs. Porting to
psio3 is mostly mechanical (CPO replacement: `psio2::to_ssz` →
`psio3::encode(psio3::ssz{}, …)`, etc.) but requires per-API verification
and a working build of the competitor library.

## Inherited v1 baselines vs competitors

From `libraries/psio2/PERF_SSZ_RUST_COMPETITORS.md`,
`libraries/psio2/PERF_SSZPP_GCC.txt`, and
`.issues/format-parity-audit.md`:

| format  | competitor                         | v1 vs competitor                       |
|---------|-------------------------------------|----------------------------------------|
| ssz     | OffchainLabs/sszpp (C++, Linux+GCC)| psio v1 **1.47× enc / 2.04× dec**      |
| ssz     | Lighthouse `ethereum_ssz` (Rust)   | psio v1 Rust **7-8× enc / 40-49× dec** |
| ssz     | `ssz_rs` (Rust)                    | psio v1 Rust **185-216× enc / 49-69× dec** |
| ssz     | view / field access                | psio v1 Rust **176-217×** (vs full decode) |
| capnp   | libcapnp (C++)                     | psio v1 **3-45× faster**               |
| flatbuf | libflatbuffers (Google C++)        | psio v1 **≥ parity**                   |
| wit     | wit-bindgen / wasm-tools           | not benched                            |
| borsh   | `borsh` crate (Rust)               | not directly benched                   |
| bincode | `bincode` crate (Rust)             | not directly benched                   |
| avro    | `apache-avro` crate (Rust)         | not directly benched                   |

## Inferred v3 vs competitor ratios

Because v3 is **at parity or faster than v1 on every measured workload
except frac32 enc BS**, we can derive a lower bound on v3-vs-competitor:

| format  | inferred v3 lower bound        | derivation                         |
|---------|--------------------------------|------------------------------------|
| ssz     | ≥ v1's 1.47× / 2.04× vs sszpp  | v3 ≥ v1 on encode and decode       |
| capnp   | ≥ v1's 3-45× vs libcapnp       | v3 0.42-0.53× of v1 on Header      |
| flatbuf | ≥ v1's parity vs libflatbuffers| v3 capnp/flatbuf only, no v1 decode|

These are inferences, not measurements. **Don't quote them as ground
truth — measure before publishing.**

## What's needed to produce real v3-vs-competitor numbers

### Environment
- **Linux + GCC 15+** for sszpp head-to-head. sszpp uses C++23 ranges
  (`std::views::chunk` / `enumerate`) that libc++ doesn't yet implement
  (LLVM 22 as of 2026-04). macOS clang+libc++ can't build sszpp.
- libcapnp can build on macOS via Homebrew (`brew install capnp`).
- libflatbuffers ditto (`brew install flatbuffers`).
- libprotobuf, msgpack-c likewise.

### External dependencies (clone paths used by psio2's bench CMake)
- `git clone --depth 1 https://github.com/OffchainLabs/sszpp /tmp/sszpp`
- `git clone --depth 1 https://github.com/chfast/intx /tmp/intx`
- `git clone --depth 1 https://github.com/prysmaticlabs/hashtree /tmp/hashtree && (cd /tmp/hashtree && make)`

### Fixture data
- mainnet-genesis.ssz (~5.15 MiB) from `eth-clients/mainnet/raw/main/metadata/genesis.ssz`

### Porting effort
For each `bench_<X>.cpp`:
1. Copy from `libraries/psio2/cpp/benchmarks/` to `libraries/psio3/cpp/benchmarks/`.
2. Replace psio2 API calls with psio3 CPOs:
   - `psio2::convert_to_ssz(v)` → `psio3::encode(psio3::ssz{}, v)`
   - `psio2::convert_from_ssz<T>(b)` → `psio3::decode<T>(psio3::ssz{}, b)`
   - `psio2::ssz_size(v)` → `psio3::size_of(psio3::ssz{}, v)`
   - `psio2::ssz_validate<T>(b)` → `psio3::validate<T>(psio3::ssz{}, b)`
   - `psio2::to_frac(v)` → `psio3::encode(psio3::frac32{}, v)`
   - `psio2::convert_to_pssz<F>(v)` → `psio3::encode(psio3::pssz{}, v)`
   - similarly for to/from_capnp, to/from_flatbuf, etc.
3. Port `beacon_types.hpp` / `beacon_types_fulu.hpp` to psio3 reflection
   (`PSIO3_REFLECT` + `length_bound` annotations replace
   `psio2::bounded_list<T, N>`).
4. Wire into `libraries/psio3/cpp/benchmarks/CMakeLists.txt` with
   conditional `PSIO3_BENCH_<X>` flags matching psio2's
   `PSIO2_BENCH_<X>` pattern.

Estimated effort: 4-6 hours for ssz + capnp + flatbuf benches with a
working Linux+GCC environment. Without that environment, the work
becomes "port the code, can't run it locally" — the porting mechanical,
verification deferred.

## Recommended priority order

1. **`bench_ssz_beacon.cpp`** — the most interesting comparison
   (Lighthouse / sszpp / ssz_rs are real, widely-used libraries).
2. **`bench_capnp.cpp`** — psio v1's 3-45× lead is unusual; worth
   confirming v3 doesn't regress it.
3. **`bench_flatbuf.cpp`** + standalone — psio v1 was at parity, so
   the question is whether v3's clean architecture costs anything.
4. The remaining benches (msgpack, protobuf, wit) for completeness.
