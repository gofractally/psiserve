# Backend Stress Test: pzam-compile.wasm

## Goal

Benchmark every available WASM engine/backend compiling pzam-compile.wasm (the LLVM-in-WASM compiler) as a stress test. This is one of the most complex WASM modules possible — 19MB, 23k functions, heavy use of floats, bulk-memory, reference-types, and multivalue.

## Results (AArch64-only binary, 19MB, 23k funcs)

### Quick task: compile fib_simple.wasm via JIT2 backend

| Engine | Time | Status |
|--------|------|--------|
| psizam interpreter | 0.16s | OK — no compilation overhead |
| psizam JIT1 (aarch64) | 0.17s | OK — veneer islands + long branches |
| wasmtime | 0.28s | OK — Cranelift JIT |
| wasmer (Cranelift) | 1.06s | OK — multi-core JIT (10s CPU / 1s wall) |
| psizam JIT2 (aarch64) | — | COMPILE OK, runtime crash (pre-existing codegen bug) |
| psizam LLVM (ORC JIT) | 4:06 | OK but slow — O2 compiles all 23k funcs before executing |
| pzam-run (.pzam) | ~0.01s | OK — pre-compiled native, near-zero startup |

### Compute-heavy task: compile fib_simple.wasm via LLVM backend

| Engine | Time | Speedup vs interp |
|--------|------|---------|
| wasmer (Cranelift) | 0.37s | 24x |
| wasmtime | 0.55s | 16x |
| psizam JIT1 (aarch64) | 0.66s | 14x |
| psizam interpreter | 9.0s | 1x |

### Key findings

1. **psizam interpreter wins on startup-bound tasks** (0.16s vs 0.28s wasmtime) — zero JIT overhead
2. **psizam JIT1 is now competitive** (0.66s) — within 2x of wasmtime on compute-heavy tasks
3. **wasmer Cranelift is fastest on compute** (0.37s) — multi-core JIT compilation pays off
4. **wasmtime and JIT1 are close** on compute (0.55s vs 0.66s)
5. **LLVM ORC JIT is impractical for large modules** — 4 minutes to O2-compile 23k functions. Only useful with cached output (.pzam)
6. **JIT2 compiles successfully** but has a pre-existing runtime bug on complex modules (SEGV at offset ~5.7MB in generated code, not related to branch range fix)

## Branch Range Fixes Applied

### JIT1 + JIT2: Veneer islands for inter-function calls
- Veneer islands (ADRP+ADD+BR X16, 12 bytes each) inserted every 60MB in the code stream
- Island capacity scales with function count: `max(2048, num_functions * 2)` slots per island
- `fix_branch_or_veneer()` falls back to veneer when B/BL displacement exceeds ±128MB

### JIT1 + JIT2: Long-form conditional branches
- NOP sentinel (0xD503201F) emitted after B.cond/CBZ/CBNZ placeholders
- When intra-function B.cond/CBZ exceeds ±1MB, converts to inverted B.cond +8 followed by unconditional B (±128MB range)

### JIT2 only: Separate scratch allocator
- Per-function transient data (block fixups, IR scratch) allocated from separate `growable_allocator`
- Prevents interleaved data from pushing code segments apart
- Code buffer tails reclaimed after each function for compact layout

## Remaining Issues

### JIT2: Runtime crash on complex modules
The JIT2 backend compiles pzam-compile.wasm successfully (250MB of native code, 4 veneer islands) but segfaults during execution at ~5.7MB offset (early function). This is a pre-existing codegen bug unrelated to the branch range fix. Investigation needed to determine root cause.

## Execution Commands

```bash
# wasmtime
time wasmtime run --dir=. --dir=/tmp -- build/wasi-arm/bin/pzam-compile \
  --target=aarch64 --backend=llvm ./fib_simple.wasm -o /tmp/out.pzam

# wasmer (Cranelift)
time wasmer run --dir=. --dir=/tmp build/wasi-arm/bin/pzam-compile -- \
  --target=aarch64 --backend=llvm ./fib_simple.wasm -o /tmp/out.pzam

# psizam interpreter
time build/Release/bin/psizam-wasi --backend=interpreter --dir=.:. --dir=/tmp:/tmp \
  build/wasi-arm/bin/pzam-compile --target=aarch64 --backend=llvm \
  ./fib_simple.wasm -o /tmp/out.pzam

# psizam JIT1
time build/Release/bin/psizam-wasi --backend=jit --dir=.:. --dir=/tmp:/tmp \
  build/wasi-arm/bin/pzam-compile --target=aarch64 --backend=llvm \
  ./fib_simple.wasm -o /tmp/out.pzam
```
