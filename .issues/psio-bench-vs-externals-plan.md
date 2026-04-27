# psio bench vs canonical externals — porting plan

**Status:** plan locked, 2026-04-27. Steps 1 (slim shapes.hpp) committed
b1536e5; rest pending.

## Goal

Replace the v1↔v3 framing with a single bench that compares **psio's
formats** against **canonical external libraries** across many shapes
× ops × modes, with snapshot CSV output for regression tracking.

## Source material (psio1 benches, reference-only)

```
psio1/cpp/benchmarks/bench_capnp.cpp       — vs libcapnp
psio1/cpp/benchmarks/bench_flatbuf.cpp     — vs libflatbuffers
psio1/cpp/benchmarks/bench_msgpack.cpp     — vs msgpack-cxx
psio1/cpp/benchmarks/bench_protobuf.cpp    — vs libprotobuf
psio1/cpp/benchmarks/bench_ssz_beacon.cpp  — vs sszpp on real BeaconState
```

Existing precedent on the v3 side:
```
psio/cpp/tests/format_perf_external.cpp    — psio::msgpack vs msgpack-cxx
                                              (working, 452 lines)
```

## Adapter layout

```
libraries/psio/cpp/benchmarks/adapters/
   capnp_adapter.hpp       ~75 LOC + bench_schemas.capnp
   flatbuf_adapter.hpp     ~80 LOC + bench_schemas.fbs
   protobuf_adapter.hpp    ~85 LOC + bench_schemas.proto + pre-built .pb.{h,cc}
   msgpack_adapter.hpp     ~60 LOC (no schema, MSGPACK_DEFINE inline)
   ssz_adapter.hpp        ~150 LOC + beacon types (deferred — Linux/GCC only)
```

Each adapter exposes a uniform contract:
```cpp
namespace cap_bench {
   std::vector<char>  encode(const Point& v);
   Point              decode(std::span<const char>);
   std::size_t        wire_size(const Point& v);
   void               view_id(std::span<const char>);   // single-field random
   void               view_all(std::span<const char>);  // for_each walk
}
```

Same shape × same op surface for every adapter; harness loops uniformly.

## Per-competitor cost ladder (smallest first)

| competitor | adapter LOC | schema setup | gating |
|---|---|---|---|
| msgpack-cxx  | ~60   | none (MSGPACK_DEFINE) | `find_package(msgpack-cxx)` |
| protobuf     | ~85   | pre-built .pb.{h,cc}  | `find_package(Protobuf)` |
| flatbuffers  | ~80   | flatc on-the-fly      | `find_package(flatbuffers)` |
| capnp        | ~75   | capnp_generate_cpp    | `find_package(CapnProto)` |
| sszpp        | ~150  | sszpp+intx+hashtree   | Linux/GCC only — defer |

## Shape coverage matrix

```
                  capnp  flatbuf  protobuf  msgpack  sszpp
Point              ✓      ✓        ✓         ✓       n/a
NameRecord         ✓      ✓        ✓         ✓       n/a
FlatRecord         ✓      ✓        ✓         ✓       n/a
Record             ~      ~        ~         ✓       n/a       (~ optional handling)
Validator          ✓      ✓        ~         ✓        ✓        (~ fixed-len bytes workaround)
Order              ✓      ✓        ✓         ✓       n/a
ValidatorList      ✓      ✓        ✓         ✓        ✓
BeaconState        n/a   n/a       n/a       n/a      ✓        (psio-only competitor)
```

## Snapshot CSV schema (task #65)

One file per run: `bench_snapshots/perf_<UTC-ISO>_<commit-short>.csv`.

Columns:
```
timestamp_utc, commit_short, platform, cpu, compiler, std,
shape, format, library, mode, op,
ns_min, ns_median, cv_pct, wire_bytes, iters, trials, notes
```

Where:
- `library` ∈ {`psio`, `capnp`, `flatbuffers`, `protobuf`, `msgpack-cxx`, `sszpp`}
- `format` ∈ psio's wire formats (when library=psio) or "(native)" for competitors
- `mode` ∈ {`static`, `dynamic`} — psio's templated path vs runtime-typed
- `op` ∈ {`size_of`, `encode_rvalue`, `encode_sink`, `decode`, `validate`,
  `view_one`, `view_all`}

Snapshots are committed by hand when the user accepts a perf change as
a new baseline. Default `.gitignore` covers anything in `bench_snapshots/`
unless explicitly `git add -f`'d.

## Snapshot diff tool (task #70)

Python script `tools/perf_diff.py`:
```
perf_diff.py <baseline.csv> <current.csv> [--threshold 1.05]
```
Joins on `(shape, format, library, mode, op)`, flags cells where
`current.ns_min / baseline.ns_min` exceeds threshold and the delta
exceeds combined-CV (so CV-bounded noise doesn't trigger).
Output: markdown table suitable for CI comments.

## Footguns identified across all 5 audits

1. **Builder/buffer construction inside the bench loop**
   capnp / flatbuf / protobuf each allocate a fresh builder/buffer per
   iteration. Measure both cold (alloc included) and warm (reuse) variants.

2. **No zero-copy view in protobuf / msgpack**
   "view" for these is decode-then-access. Cell label honestly:
   `view_one (post-decode)` to distinguish from capnp/flatbuf's
   pointer-into-buffer style.

3. **Schemaless msgpack overhead**
   Wire size is type-tagged; smaller messages pay tag overhead. Note
   this in the wire_bytes column rather than apologizing for it.

4. **sszpp C++23 ranges requirement**
   `std::views::chunk`/`enumerate` — libstdc++ GCC 15+ only.
   `find_package(sszpp)` plus a libstdc++ probe; on libc++ skip the
   sszpp adapter entirely.

5. **Calibration warmup**
   psio1 benches use a 30 µs calibration pass before timed runs.
   `harness.hpp::ns_per_iter` already does target_trial_ns auto-tuning;
   keep that.

6. **Compiler elimination**
   All adapters need `asm volatile("" : : "r,m"(x) : "memory")` to
   prevent dead-code elimination on the result.

## Phased execution

```
Phase 1: ✅ shapes.hpp slim + harness namespace rename     (committed b1536e5)
Phase 2: snapshot CSV schema in harness.hpp                (task #65)
Phase 3: scaffold bench_psio_vs_externals.cpp              (task #68 part 1)
            psio side only first — port the existing v3 timing
            functions from bench_perf_report into the new loop
Phase 4: msgpack-cxx adapter                               (task #68 part 2)
            simplest, precedent in format_perf_external
Phase 5: protobuf adapter                                  (task #68 part 3)
            pre-generated .pb files
Phase 6: flatbuffers adapter                               (task #68 part 4)
            on-the-fly flatc
Phase 7: capnp adapter                                     (task #68 part 5)
            on-the-fly capnp_generate_cpp
Phase 8: BeaconState shapes (separate add) + sszpp adapter (#69 + #68 part 6)
            Linux/GCC gated; bigger lift; do last
Phase 9: snapshot diff tool                                (task #70)
Phase 10: view-op + static/dynamic mode columns            (#66 + #67)
Phase 11: delete bench_perf_report.cpp                     (closes #71 leftover)
```

## Files to keep (not delete) from psio1

- `psio1/cpp/benchmarks/beacon_types.hpp` (221 LOC)
- `psio1/cpp/benchmarks/beacon_types_fulu.hpp` (251 LOC)
- These are the SSZ canonical type defs; v3 will reference them via a
  port (#69), not by importing psio1 headers in production code.
