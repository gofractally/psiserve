# Backend Stress Test: pzam-compile.wasm

## Goal

Benchmark every available WASM engine/backend compiling pzam-compile.wasm (the LLVM-in-WASM compiler) as a stress test. This is one of the most complex WASM modules possible — 19-25MB, 23-26k functions, heavy use of floats, bulk-memory, reference-types, and multivalue.

## Test Matrix

### Engines to test

| Engine | Platform | Notes |
|--------|----------|-------|
| wasmtime | any | Production JIT, baseline comparison |
| wasmer (Cranelift) | any | Alternative JIT |
| wasmer (LLVM) | any | LLVM-based AOT |
| psizam JIT1 (aarch64) | macOS ARM | Requires softfloat=OFF build |
| psizam JIT1 (x86_64) | Linux x86 | Requires softfloat=OFF build |
| psizam JIT2 (x86_64) | Linux x86 | Two-pass JIT, x86_64 only |
| pzam-run (.pzam) | aarch64 | Pre-compiled native code |

### Task to benchmark

Compile `fib_simple.wasm` using the LLVM backend inside pzam-compile.wasm:
```bash
<engine> pzam-compile.wasm --target=aarch64 --backend=llvm fib_simple.wasm -o /tmp/out.pzam
```

This exercises: module parsing, IR translation, LLVM optimization (O2), LLVM codegen, ELF parsing, relocation processing, .pzam serialization.

### Metrics to capture

- Wall-clock time (startup + execution)
- Peak RSS
- Output correctness (diff against reference .pzam)

## Prerequisites

### 1. Fix softfloat=OFF build

The aarch64 JIT and JIT2 backends can't run pzam-compile.wasm because:
- `PSIZAM_ENABLE_SOFTFLOAT=ON` → JIT doesn't implement float ops (calls `unimplemented()`)
- `PSIZAM_ENABLE_SOFTFLOAT=OFF` → build fails: SIMD softfloat stubs referenced unconditionally in `interpret_visitor.hpp` and `aarch64.hpp`

Fix: guard the softfloat SIMD function references with `#ifdef PSIZAM_SOFTFLOAT` so the non-softfloat build compiles. The JIT backends already have native float codegen — they just need to be buildable without softfloat.

Note: running pzam-compile.wasm without softfloat gives non-deterministic float results (native IEEE-754 instead of softfloat). This is fine for benchmarking — the output .pzam may differ in float-dependent codegen decisions but should still be functionally correct.

### 2. Install wasmer

```bash
curl https://get.wasmer.io -sSfL | sh
```

### 3. Build psizam-wasi with each backend

```bash
# Native float build (for JIT testing)
cmake -B build/Release-nativefp -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DPSIZAM_ENABLE_SOFTFLOAT=OFF -DPSIZAM_ENABLE_TOOLS=ON -DPSIZAM_ENABLE_LLVM=ON

# For JIT2 on x86_64 (needs x86 machine or Rosetta)
# JIT2 is x86_64-only
```

### 4. Pre-compile .pzam for pzam-run test

```bash
build/Release/bin/pzam-compile --target=aarch64 --backend=llvm \
  build/wasi-arm/bin/pzam-compile -o /tmp/pzam-compile-arm.pzam
```

## Execution

```bash
# wasmtime
time wasmtime run --dir=. --dir=/tmp -- build/wasi-arm/bin/pzam-compile \
  --target=aarch64 --backend=llvm ./fib_simple.wasm -o /tmp/out_wasmtime.pzam

# wasmer (Cranelift)
time wasmer run --dir=. --dir=/tmp build/wasi-arm/bin/pzam-compile -- \
  --target=aarch64 --backend=llvm ./fib_simple.wasm -o /tmp/out_wasmer_cl.pzam

# wasmer (LLVM)
time wasmer run --llvm --dir=. --dir=/tmp build/wasi-arm/bin/pzam-compile -- \
  --target=aarch64 --backend=llvm ./fib_simple.wasm -o /tmp/out_wasmer_llvm.pzam

# psizam JIT1 (requires softfloat=OFF build)
time build/Release-nativefp/bin/psizam-wasi --backend=jit --dir=.:. --dir=/tmp:/tmp \
  build/wasi-arm/bin/pzam-compile --target=aarch64 --backend=llvm \
  ./fib_simple.wasm -o /tmp/out_jit1.pzam

# pzam-run (pre-compiled native)
time build/Release/bin/pzam-run --dir=.:. --dir=/tmp:/tmp \
  build/wasi-arm/bin/pzam-compile /tmp/pzam-compile-arm.pzam \
  --target=aarch64 --backend=llvm ./fib_simple.wasm -o /tmp/out_pzam.pzam

# Verify all outputs
for f in /tmp/out_*.pzam; do
  diff /tmp/out_wasmtime.pzam "$f" > /dev/null 2>&1 && echo "MATCH: $f" || echo "DIFFER: $f"
done
```

## Expected Results

Rough estimates based on preliminary data:

| Engine | Expected Time | Why |
|--------|--------------|-----|
| pzam-run (.pzam) | ~0.01s | Near-native, no compilation |
| wasmtime | ~0.5s | Fast JIT, good optimization |
| wasmer (Cranelift) | ~0.5-1s | Similar to wasmtime |
| wasmer (LLVM) | ~5-30s | LLVM AOT compilation of 23k WASM funcs, then fast execution |
| psizam JIT1 | ~0.5-2s | Single-pass JIT, less optimization than wasmtime |
| psizam JIT2 | ~0.5-2s | Two-pass with IR optimization, x86_64 only |

The key comparison is psizam JIT vs wasmtime/wasmer — this reveals whether psizam's JIT is competitive on real-world complex modules, not just spec tests.
