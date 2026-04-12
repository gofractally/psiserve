# WASI LLVM Build Guide

How to build pzam-compile.wasm with LLVM optimization, and how the pruning works.

## Quick Start

```bash
# 1. Build LLVM for WASI (one-time, ~2-4 hours)
scripts/build-llvm-wasi.sh

# 2. Build pzam-compile.wasm with LLVM (single-target, ~1 min)
cmake -B build/wasi-arm -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=cmake/wasi-toolchain.cmake \
  -DCMAKE_BUILD_TYPE=Release \
  -DPSIZAM_ENABLE_TOOLS=ON \
  -DPSIZAM_ENABLE_LLVM=ON \
  -DLLVM_WASI_PREFIX=build/llvm-wasi/install \
  -DPSIZAM_LLVM_TARGETS="AArch64"
cmake --build build/wasi-arm --target pzam-compile

# 3. Use it (via wasmtime or any WASI runtime)
wasmtime run --dir=. --dir=/tmp -- build/wasi-arm/bin/pzam-compile \
  --target=aarch64 --backend=llvm input.wasm -o /tmp/output.pzam
```

## Target Selection

`PSIZAM_LLVM_TARGETS` controls which LLVM backends are linked. Each target adds ~5-7MB to the binary.

```bash
# AArch64 only (19.2MB, 23k functions)
-DPSIZAM_LLVM_TARGETS="AArch64"

# X86 only (19.5MB, 23k functions)
-DPSIZAM_LLVM_TARGETS="X86"

# Both targets (25.5MB, 26k functions) — the default
-DPSIZAM_LLVM_TARGETS="X86;AArch64"
```

Build separate binaries per target for smallest size. The JIT2 backend (non-LLVM) doesn't need target libraries, so a JIT2-only binary is only 620KB.

## How the LLVM Pruning Works

### Problem: PassBuilder pulls in everything

LLVM's `PassBuilder::registerFunctionAnalyses()` instantiates all 36+ analyses from `PassRegistry.def`, including analyses from IPO, Vectorize, Coroutines, HipStdPar, and Instrumentation libraries. Even though our optimization pipeline only uses 10 passes, the registration keeps ~20MB of code alive through symbol references.

### Solution: Manual analysis registration

Instead of:
```cpp
llvm::PassBuilder pb;
pb.registerModuleAnalyses(mam);   // registers ALL module analyses
pb.registerFunctionAnalyses(fam); // registers ALL function analyses
pb.registerLoopAnalyses(lam);     // registers ALL loop analyses
pb.crossRegisterProxies(lam, fam, cgam, mam);
```

We register only the 14 analyses our passes actually need:
```cpp
// Function analyses (14 of 36+)
fam.registerPass([&] { return llvm::DominatorTreeAnalysis(); });
fam.registerPass([&] { return llvm::AssumptionAnalysis(); });
fam.registerPass([&] { return llvm::TargetLibraryAnalysis(); });
fam.registerPass([&] { return llvm::TargetIRAnalysis(); });
fam.registerPass([&] { return llvm::LoopAnalysis(); });
fam.registerPass([&] { return llvm::PostDominatorTreeAnalysis(); });
fam.registerPass([&] { return llvm::OptimizationRemarkEmitterAnalysis(); });
fam.registerPass([&] { return llvm::MemorySSAAnalysis(); });
fam.registerPass([&] { return llvm::MemoryDependenceAnalysis(); });
fam.registerPass([&] { return llvm::ScalarEvolutionAnalysis(); });
fam.registerPass([&] { return llvm::BlockFrequencyAnalysis(); });
fam.registerPass([&] { return llvm::LastRunTrackingAnalysis(); });
fam.registerPass([&] { return llvm::BasicAA(); });
fam.registerPass([&] { return llvm::PassInstrumentationAnalysis(); });
fam.registerPass([&] {
   llvm::AAManager aa;
   aa.registerFunctionAnalysis<llvm::BasicAA>();
   return aa;
});
```

And manually set up the cross-manager proxies (same as `PassBuilder::crossRegisterProxies`).

### Impact

| Variant | Size | Functions | Reduction |
|---------|------|-----------|-----------|
| Before (PassBuilder, both targets) | 30.0MB | 34,149 | — |
| After (manual, both targets) | 25.5MB | 26,287 | -15% |
| After (manual, single target) | 19.2MB | 23,452 | -36% |

### Why the remaining ~19MB can't be pruned further

The remaining code is genuinely needed:
- **CodeGen** (14.5MB .a) — LLVM's code generation framework
- **Target backend** (~8MB .a each) — X86/AArch64 instruction selection, scheduling, register allocation
- **Core** (6.9MB .a) — IR infrastructure
- **Analysis** (8.6MB .a) — analyses required by both optimization and codegen
- **ScalarOpts** (6.3MB .a) — our 10 optimization passes

wasm-ld strips unreferenced functions, but CodeGen and target backends are tightly interconnected. The static libraries are large, but only ~19MB ends up in the final binary after gc-sections.

## Self-Compilation

pzam-compile.wasm can compile itself — the output is bit-identical to native compilation:

```bash
# Self-compilation (34 min, 1.7GB peak memory in wasmtime)
wasmtime run --dir=. --dir=/tmp -W max-memory-size=4294967296 -- \
  build/wasi/bin/pzam-compile \
  --target=aarch64 --backend=llvm \
  build/wasi/bin/pzam-compile -o /tmp/pzam-compile-self.pzam

# Native compilation (26 min, 780MB)
build/Release/bin/pzam-compile \
  --target=aarch64 --backend=llvm \
  build/wasi/bin/pzam-compile -o /tmp/pzam-compile-native.pzam

# Code blobs are bit-identical (6 header bytes differ)
```

## Backend Stress Test Results

Running pzam-compile.wasm (19MB, 23k functions) through different WASM engines:

### Quick task (compile fib_simple.wasm with JIT2 backend)

| Engine | Time | Notes |
|--------|------|-------|
| psizam interpreter | 0.16s | Fastest — no compilation overhead |
| wasmtime | 0.25s | JIT compilation of 23k funcs adds startup cost |
| psizam JIT (aarch64) | FAIL | Softfloat mode not implemented for float JIT codegen |

### Compute-heavy task (compile fib_simple.wasm with LLVM backend)

| Engine | Time | Speedup |
|--------|------|---------|
| psizam interpreter | 9.0s | 1x (baseline) |
| wasmtime | 0.5s | 18x faster |

For quick parse-heavy tasks, the interpreter wins (no JIT compilation overhead). For compute-heavy tasks, wasmtime's JIT compilation pays for itself many times over.

All engines produce bit-identical output.
