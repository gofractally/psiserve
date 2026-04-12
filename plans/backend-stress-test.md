# Backend Stress Test: pzam-compile.wasm

## Goal

Benchmark every available WASM engine/backend compiling pzam-compile.wasm (the LLVM-in-WASM compiler) as a stress test. This is one of the most complex WASM modules possible — 19-25MB, 23-26k functions, heavy use of floats, bulk-memory, reference-types, and multivalue.

## Results So Far (AArch64-only binary, 19MB, 23k funcs)

### Quick task: compile fib_simple.wasm via JIT2 backend

| Engine | Time | Status |
|--------|------|--------|
| psizam interpreter | 0.16s | OK — no compilation overhead |
| wasmtime | 0.25s | OK — JIT compilation adds startup |
| psizam JIT1 (aarch64) | — | FAIL: softfloat unimplemented for float codegen |
| psizam JIT2 (aarch64) | — | FAIL: branch out of range (+-128MB B/BL limit) |
| psizam LLVM (ORC JIT) | 4:06 | OK but slow — O2 compiles all 23k funcs before executing |
| pzam-run (.pzam) | ~0.01s | OK — pre-compiled native, near-zero startup |

### Compute-heavy task: compile fib_simple.wasm via LLVM backend

| Engine | Time | Speedup |
|--------|------|---------|
| wasmtime | 0.5s | 18x |
| psizam interpreter | 9.0s | 1x |

### Key findings

1. **psizam interpreter beats wasmtime on quick tasks** (0.16s vs 0.25s) — zero startup cost
2. **wasmtime dominates compute-heavy tasks** (0.5s vs 9.0s) — JIT compilation pays off 18x
3. **LLVM ORC JIT is impractical for large modules** — 4 minutes to O2-compile 23k functions before any execution. Only useful if the compilation result is cached (which is exactly what .pzam does)
4. **JIT1 and JIT2 both fail on this module** — needs fixes before they can be benchmarked

## Blocking Issues

### JIT1: Softfloat float codegen not implemented

The aarch64 JIT backend calls `unimplemented()` for all f32/f64 operations when built with `PSIZAM_ENABLE_SOFTFLOAT=ON`. pzam-compile.wasm has 4,175 float instructions (LLVM cost modeling, printf, strtod, etc.).

**Fix options:**
- Implement softfloat trampolines in the JIT (call softfloat library functions instead of native float ops)
- Fix the `SOFTFLOAT=OFF` build (currently broken: SIMD softfloat stubs are referenced unconditionally in `interpret_visitor.hpp`)

### JIT2: Branch out of range on large modules

With 23k functions, the generated native code exceeds aarch64's +-128MB B/BL displacement limit. The JIT2 allocates all code into a single contiguous buffer.

**Fix options:**
- Veneer islands: insert branch trampolines at regular intervals in the code buffer
- Code partitioning: split the code buffer into chunks with indirect dispatch between them
- Lazy compilation: only JIT the functions that are actually called

## Remaining Tests

| Engine | Status | Notes |
|--------|--------|-------|
| wasmer (Cranelift) | TODO | `wasmer run --dir=. --dir=/tmp` |
| wasmer (LLVM) | TODO | `wasmer run --llvm --dir=. --dir=/tmp` |
| psizam JIT1 | BLOCKED | Needs softfloat fix |
| psizam JIT2 | BLOCKED | Needs branch range fix |
| pzam-run (.pzam) | TODO | Need to recompile arm binary to .pzam (~20 min) |

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

# psizam LLVM (ORC JIT)
time build/Release/bin/psizam-wasi --backend=llvm --dir=.:. --dir=/tmp:/tmp \
  build/wasi-arm/bin/pzam-compile --target=aarch64 --backend=llvm \
  ./fib_simple.wasm -o /tmp/out.pzam

# pzam-run (pre-compiled native)
time build/Release/bin/pzam-run --dir=.:. --dir=/tmp:/tmp \
  build/wasi-arm/bin/pzam-compile /tmp/pzam-compile-arm.pzam \
  --target=aarch64 --backend=llvm ./fib_simple.wasm -o /tmp/out.pzam
```
