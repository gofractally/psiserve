---
id: "0016"
title: psizam LLVM AOT runtime crash on large WASM modules (clang.wasm, wasm-ld.wasm)
status: in-progress
priority: high
area: psizam
agent: psiserve-agent-psio
branch: main
created: 2026-04-19
depends_on: ["0003"]
blocks: []
---

## Description
LLVM AOT-compiled `.pzam` files produced from large real-world WASM modules
(clang.wasm ~95MB, wasm-ld.wasm ~25MB) **build successfully but crash at
runtime** with `wasm memory out-of-bounds`. The fault is deterministic and
identical across modules — indicates a codegen bug, not program bug.

Simple test programs compile and run correctly with the LLVM backend, so
the bug only manifests on sufficiently complex programs.

## Repro
```
./build/Release/bin/pzam-compile --target=aarch64 --backend=llvm -j18 \
    build/llvm-wasi/install/bin/wasm-ld.wasm -o /tmp/wasm-ld.pzam
./build/Release/bin/pzam-run /tmp/wasm-ld.pzam -- --version
```
Also reproduces with `clang.wasm` (same fault).

## Crash Signature (consistent across all inputs)
```
JIT FAULT sig=10 addr=0x...5245d45f pc=<in generated code> instr=0x38744a69
  instruction: LDRB W9, [X19, W20, UXTW]
  X19(ctx) = <memory base>          # correct
  X20      = 0x000000005245545f     # GARBAGE ("_TER" ASCII, upper 32 bits zeroed)
psizam error: wasm memory out-of-bounds
```

- Instruction is a byte load using `W20` as a 32-bit WASM memory offset.
- `0x5245545f` = ASCII bytes `_TER`, likely from rodata (e.g., part of
  `"_TERMINATED"` or similar symbol).
- Value is a 32-bit quantity zero-extended to 64-bit → strongly suggests a
  WASM i32 value read from somewhere was used as a pointer.
- Identical garbage value across 3 separate test builds (unchanged, volatile
  stores, and at `-j18`) — not a race or nondeterminism.

## Investigations Already Done
- [x] ADRP range overflow (0/2440 — none)
- [x] BL/call26 range overflow (0/3.39M — none)
- [x] Function offset table correctness
- [x] Cross-function relocation resolution (negative-addend encoding)
- [x] `.rodata` merging and addressing
- [x] All aarch64 relocation patching types (ADRP / ADD / LDR / BL)
- [x] Memory pointer reload after WASM↔WASM and indirect calls
- [x] Argument ordering and entry-wrapper unpacking
- [x] Parallel compilation determinism (-j18 output matches serial pattern)
- [x] Hypothesis: volatile loads + volatile stores on WASM memory — **DISPROVEN**
      (marking stores volatile did not change the crash)

## Remaining Hypotheses
- **O0 vs O2**: does disabling all LLVM optimization avoid the crash?
  (pending: re-test at `-j18 --opt-level=0`)
- **Register allocation / callee-saved preservation**: X20 is callee-saved in
  AAPCS. Something could be clobbering it across a call without restoring.
- **i32 ↔ i64 conversion**: a WASM i32 value being passed where a 64-bit
  pointer is expected, or a ZExt where a pointer-sized load was needed.
- **Stack-slot aliasing**: `mem_ptr_alloca`, `host_args_alloca`, or vreg
  allocas overlapping due to SROA / MergeSlots pass.
- **Uninitialized read** in generated code (an uninitialized WASM local or
  vreg being used as an address).

## Acceptance Criteria
- [ ] Root cause identified and documented
- [ ] Fix applied in `libraries/psizam/src/llvm_ir_translator.cpp` or
      `libraries/psizam/src/llvm_aot_compiler.cpp` as appropriate
- [ ] `clang.pzam` runs `clang --version` successfully
- [ ] `wasm-ld.pzam` runs `wasm-ld --version` successfully
- [ ] Regression test: a mid-size WASM module in the test suite that
      triggered the same fault pattern
- [ ] Debug diagnostics (ADRP/BL overflow checks in `pzam_run.cpp`) removed
      once the bug is fixed

## Relevant Files
- `libraries/psizam/src/llvm_ir_translator.cpp` — IR → LLVM IR translation
- `libraries/psizam/src/llvm_aot_compiler.cpp` — LLVM Module → ELF → relocs
- `libraries/psizam/include/psizam/detail/ir_writer_llvm_aot.hpp` — orchestration
- `libraries/psizam/include/psizam/detail/jit_reloc.hpp` — relocation patching
- `libraries/psizam/src/llvm_runtime_helpers.cpp` — `rt_get_memory` et al.
- `libraries/psizam/tools/pzam_run.cpp` — JIT signal diagnostics
