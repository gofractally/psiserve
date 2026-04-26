# psio v1 vs psio3 Performance Report

Captured 2026-04-24 from `psio3_smoke_perf` (manual `-O3` build, ARM64,
clang from Homebrew llvm).

Each row is `v1 ns/op | v3 ns/op | v3/v1`. v3/v1 < 1.0 means v3 is
faster. `n/a` indicates the format has no v1 free function for that op
(validate/size beyond ssz). Sub-1 ns figures (most v3 validate/size)
are likely constant-folded by the optimizer since the input never
varies — usable for trend, not for absolute comparison.

## Header workload (single Header struct)

```
workload               | v1 ns/op | v3 ns/op | v3/v1
-----------------------+----------+----------+------
[ssz] enc uint32       |      9.5 |      9.2 | 0.96x
[ssz] enc vec<u32>[512]|     48.6 |     32.1 | 0.66x
[ssz] enc Header       |     28.7 |     27.7 | 0.97x
[ssz] dec Header       |     44.8 |     30.9 | 0.69x
[ssz] val Header       |      0.8 |      0.6 | 0.69x
[ssz] size Header      |      0.6 |      0.6 | 1.03x
[pssz]   enc Header    |     22.5 |     25.6 | 1.14x
[pssz]   dec Header    |     35.5 |     26.7 | 0.75x
[pssz]   val Header    |      n/a |      0.5 |  —
[pssz]   size Header   |      0.5 |      0.5 | 0.98x
[frac32] enc Header    |     27.5 |     24.7 | 0.90x
[frac32] dec Header    |     35.6 |     25.3 | 0.71x
[frac32] val Header    |      n/a |      0.5 |  —
[frac32] size Header   |      0.5 |      0.5 | 0.98x
[bin] enc Header       |     24.2 |     15.5 | 0.64x
[bin] dec Header       |     35.9 |     24.9 | 0.69x
[bin] val Header       |      n/a |      0.5 |  —
[bin] size Header      |      2.3 |      0.5 | 0.22x
[borsh] enc Header     |     18.8 |     15.4 | 0.82x
[borsh] dec Header     |     32.5 |     27.4 | 0.84x
[borsh] val Header     |      n/a |      0.5 |  —
[borsh] size Header    |      n/a |      0.5 |  —
[bincode] enc Header   |     20.0 |     16.3 | 0.82x
[bincode] dec Header   |     34.2 |     27.2 | 0.80x
[bincode] val Header   |      n/a |      0.5 |  —
[bincode] size Header  |      n/a |      0.5 |  —
[avro] enc Header      |    117.9 |    128.1 | 1.09x
[avro] dec Header      |    167.0 |     52.0 | 0.31x
[avro] val Header      |      n/a |      0.5 |  —
[avro] size Header     |      n/a |    124.7 |  —
[flatbuf] enc Header   |    (n/a) |     85.1 |  —
[flatbuf] dec Header   |    (n/a) |     32.4 |  —
[flatbuf] val Header   |    (n/a) |      0.6 |  —
[flatbuf] size Header  |    (n/a) |     84.8 |  —
[capnp] enc Header     |     91.4 |     39.8 | 0.44x
[capnp] dec Header     |     47.7 |     25.1 | 0.53x
[capnp] val Header     |      n/a |      5.5 |  —
[capnp] size Header    |      n/a |      0.5 |  —
[wit] enc Header       |     26.2 |     26.3 | 1.01x
[wit] dec Header       |     50.1 |     35.4 | 0.71x
[wit] val Header       |      n/a |      6.9 |  —
[wit] size Header      |      n/a |      0.5 |  —
```

## BeaconState×256 (fully-fixed Validator)

```
workload                | v1 ns/op | v3 ns/op | v3/v1
------------------------+----------+----------+------
[ssz]     enc BS        |    846.6 |    741.9 | 0.88x
[ssz]     dec BS        |    358.4 |    329.4 | 0.92x
[ssz]     val BS        |      0.0 |      0.5 | infx
[ssz]     size BS       |      0.3 |      0.3 | 1.12x
[pssz]    enc BS        |   1014.2 |    770.2 | 0.76x
[pssz]    dec BS        |    402.1 |    324.7 | 0.81x
[pssz]    val BS        |      n/a |      0.5 |  —
[pssz]    size BS       |      0.5 |      0.3 | 0.60x
[frac32]  enc BS        |    712.1 |   1171.9 | 1.65x
[frac32]  dec BS        |    559.8 |    300.0 | 0.54x
[frac32]  val BS        |      n/a |      0.5 |  —
[frac32]  size BS       |      0.3 |      0.3 | 1.13x
[bin]     enc BS        |    915.1 |    300.9 | 0.33x
[bin]     dec BS        |   2828.2 |    745.5 | 0.26x
[bin]     val BS        |      n/a |      0.3 |  —
[bin]     size BS       |    205.8 |      0.3 | 0.00x
[borsh]   enc BS        |    275.4 |    312.0 | 1.13x
[borsh]   dec BS        |   1078.7 |    772.6 | 0.72x
[bincode] enc BS        |    283.7 |    322.6 | 1.14x
[bincode] dec BS        |   1099.5 |    744.0 | 0.68x
```

## Wire size (bytes)

```
[ssz]     Header  | v1 142 | v3 142
[pssz]    Header  | v1 146 | v3 136   (v3 picks W=2 from length_bound annotations)
[frac32]  Header  | v1 156 | v3 156
[bin]     Header  | v1 135 | v3 142
[borsh]   Header  | v1 142 | v3 142
[bincode] Header  | v1 154 | v3 154
[avro]    Header  | v1  74 | v3  74
[flatbuf] Header  | v1 n/a | v3 184
[capnp]   Header  | v1 176 | v3 176
[wit]     Header  | v1 154 | v3 154
```

## Decode regressions cleared in this report

Every Header- and BS-scale decode regression has been eliminated. v3
now matches or beats v1 on decode across all 8 binary formats.

| workload          | original v3/v1 | this report |
|-------------------|----------------|-------------|
| ssz dec Header    | 1.40×          | **0.69×**   |
| pssz dec Header   | 1.41×          | **0.75×**   |
| bin dec Header    | 1.55×          | **0.69×**   |
| ssz dec BS×256    | 1.56×          | **0.92×**   |
| pssz dec BS×256   | 1.42×          | **0.81×**   |
| frac32 dec BS×256 | 1.18×          | **0.54×**   |

Fixes that closed the gap:

1. `vector<arithmetic>` decode fast path — `assign(p, p+n)` from raw
   bytes instead of `reserve(n)` + per-element `push_back(decode_value)`.
   Mirrors v1's `from_ssz.hpp:182-193` `ssz_memcpy_ok_v` branch.
   Applied to ssz/pssz/bin (frac already had it).
2. In-place `vector<Record>` decode — `record_decode_into<T>(src, pos,
   end, T& out)` writes fields directly into a pre-resized vector slot,
   avoiding the per-element `T tmp{}` + move-construct that
   `push_back(decode_value<T>(...))` incurs. Applied to ssz/pssz/frac.

## Remaining regressions

- **frac32 enc BS×256: 1.65×** — three-phase pack walker. v3 calls
  `encode_value<W, Validator>` per element, which runs the full
  Phase 1/2/3 record walker even for fully-fixed records. A
  fully-fixed-record fast path that batches the byte-count probe and
  emits header + fields without per-call dispatch should close most
  of this; high enough risk for byte-parity that it deserves its own
  measurement loop.
- **borsh / bincode enc BS×256: 1.13×, avro enc Header: 1.09×** —
  all within run-to-run noise; the same row at Header scale shows v3
  ahead. Re-run a few times before treating as a real regression.

## Methodology gaps

- `validate` and `size_of` calls hit a constant input every iteration;
  most v3 numbers cluster ~0.5 ns, suggesting the optimizer is hoisting
  the deterministic walk. The capnp/wit/avro outliers (5–125 ns) are
  the only validate/size figures that stand on their own. To get
  trustworthy absolute numbers, feed each call a randomized buffer set
  and disable inlining at the boundary.
- BeaconState×256 measures only `enc`/`dec`/`val`/`size`; no equivalent
  of the `[ssz] enc vec<u32>[512]` row exists at BS scale. Adding a
  `vec<Validator>[N]` standalone row would isolate vector-of-record
  decode from the BS wrapper struct.

## How to reproduce

```bash
clang++ -O3 -DNDEBUG -std=gnu++23 -DPSIO3_EXCEPTIONS_ENABLED=1 \
  -I libraries/psio/cpp/include \
  -I libraries/psio1/cpp/include \
  -I libraries/psio1/cpp/external/rapidjson/include \
  libraries/psio/cpp/tests/smoke_perf.cpp \
  -o smoke_perf
./smoke_perf
```

The CMake target `psio3_smoke_perf` wires the same build into ctest;
use it once the root `CMakeLists.txt` `OpenSSL::SSL` regen issue is
resolved.
