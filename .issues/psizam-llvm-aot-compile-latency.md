---
id: psizam-llvm-aot-compile-latency
title: psizam LLVM AOT compile latency — parallelism, batching, context reuse
status: in-progress
priority: high
area: psizam
agent: psiserve-agent-psio
branch: main
created: 2026-04-19
depends_on: [psizam-llvm-wasi-toolchain]
blocks: []
---

## Description
The LLVM AOT pipeline compiles **each WASM function into its own
`LLVMContext` + `Module` + `TargetMachine` + ELF object**, producing
pathological per-function overhead. On `clang.wasm` (~140k functions,
95MB input) this takes **~60+ minutes serial** to produce a `.pzam`.

This latency is unacceptable for deployed build processes where WASM→pzam
compilation happens as part of deployment or hot reload.

**Determinism is a hard requirement** — output must be bit-identical
regardless of thread count, batch layout, or scheduling.

## Baseline (2026-04-19, 18-core M-series)
| Input            | Size   | Functions | Serial     | -j18 (per-func)  | -j18 batched (1024) |
|------------------|--------|-----------|-----------:|-----------------:|--------------------:|
| `wasm-ld.wasm`   | 25 MB  | ~40k      | ~30 min    | 3:44             | **1:06**            |
| `clang.wasm`     | 95 MB  | ~140k     | ~60–120 min| ~15–20 min est.  | **1:23**            |

Two changes landed:
1. `-jN` flag wired to CLI — exposes the existing
   `ir_writer_llvm_aot::parallel_llvm_compile_all` path. ~8× on wasm-ld.
2. Batch compilation (1024 funcs per LLVM module) — amortizes
   `LLVMContext` / `TargetMachine` / ELF emission across many functions.
   Additional ~3.4× on top of parallelism.

Total vs. serial: wasm-ld ~**27×**, clang.wasm ~**40–80×**.
Parallel efficiency (CPU/wall): ~13× of 18 cores utilized on clang.wasm.

## Determinism Constraint
Upstream (C++ → WASM) has already performed all inlining and cross-function
optimization. The LLVM AOT layer must produce **bit-identical output** for
the same input regardless of:
- Number of threads
- Batch size
- Thread scheduling / timing
- Order workers happen to finish

This is achieved by:
- Fixed batch assignment: batch `i` contains function indices `[i·N, (i+1)·N)`
- Stable concatenation order: batches laid out in batch-index order
- No cross-function inlining or attribute propagation (`NoInlinePass` +
  `noinline` attr on every function)
- No LTO

## Optimization Levels (pick freely, ordered by impact)

### Level 1 (done)
- [x] Expose the existing parallel path via `pzam-compile -jN`
      (default: `std::thread::hardware_concurrency()`)

### Level 2 (per-worker reuse — future)
- [ ] Reuse `LLVMContext` across batches compiled by one worker thread
      (currently recreated per batch — would save ~140/batch_size × setup)
- [ ] Reuse `llvm::TargetMachine` across batches per worker (parses triple,
      loads MC backend, sets up ABI — not cheap to re-create)
- [ ] Reuse `PassBuilder` + prebuilt pass pipelines per worker

### Level 3 (batch compilation — done)
- [x] Compile 1024 functions per LLVM Module → one MC emission,
      one ELF object, one parse per batch instead of per function
- [x] Shared runtime-helper declarations in the batch module
      (all declared once in batch's translator; same for all functions)
- [ ] Within-batch `wasm_func_i` → `wasm_func_j` calls become direct LLVM
      IR calls (no relocation needed). Not yet: currently same-batch refs
      still emit negative-addend relocs that get resolved in the merge
      phase. Correct but wasteful (redundant reloc records).
- [x] Deterministic batch layout (fixed chunks of consecutive function
      indices `[i*1024, (i+1)*1024)`) — independent of thread count

### Level 4 (incremental / caching — future)
- [ ] Cache compiled batches by `hash(wasm_bytes of batch + compiler_identity)`
      so re-building a mostly-unchanged WASM only recompiles affected batches
- [ ] This implies stable batch boundaries across edits — needs thought

## Acceptance Criteria
- [ ] `LLVMContext` / `TargetMachine` / `PassBuilder` reused per worker (L2)
- [x] Batch compilation path (fixed batch size = 1024 in
      `ir_writer_llvm_aot::k_batch_size`)
- [ ] LLVM inlining / LTO disabled via `NoInlinePass` + `noinline` attr
      (currently relying on LLVM's default per-function pipeline not doing
      cross-function inlining without LTO; needs explicit enforcement)
- [ ] Bit-identical output across thread counts (determinism test running)
- [ ] Runtime-equivalent output vs. current per-function pipeline (same
      crash pattern reproduces on clang.wasm + wasm-ld — correctness
      confirmed; bit-for-bit not required since batch layout differs from
      per-function layout)
- [x] wasm-ld.wasm compile time < 2 min @ -j18 (actual: 1:06)
- [x] clang.wasm compile time < 5 min @ -j18 (actual: 1:23)
- [ ] No regression in `.pzam` runtime correctness (pending #0016 fix)

## Relevant Files
- `libraries/psizam/tools/pzam_compile.cpp` — CLI, already has `-jN`
- `libraries/psizam/include/psizam/detail/ir_writer_llvm_aot.hpp` — parallel driver
- `libraries/psizam/src/llvm_aot_compiler.cpp` — per-module LLVM → ELF path
- `libraries/psizam/src/llvm_ir_translator.cpp` — per-function IR → LLVM IR
- `libraries/psizam/include/psizam/detail/llvm_aot_compiler.hpp` — API

## Notes
- This is a pure-performance refactor once #0016 (runtime crash) is fixed
  and correctness is confirmed at `-j18`. Starting before #0016 is resolved
  is viable since parallel output has been shown to be equivalent to serial
  (same crash pattern reproduces identically).
- For deterministic consensus use, the batch scheme and compiler identity
  hash must both be stable across releases — any change to batching or
  LLVM version invalidates all cached `.pzam` outputs.
