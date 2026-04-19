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

## Investigation Log (2026-04-19)

### Narrowed down
- **Interpreter / JIT backend executes the same WASM correctly** → the bug
  is in our LLVM AOT translation, not the WASM itself.
- Crash reproduces with or without same-batch relocation optimization →
  not a bug in that optimization.
- Skipping Phase 2 optimization passes (`PSIZAM_LLVM_SKIP_PHASE2=1`:
  Reassociate / GVN / LICM) does **not** fix the crash → bug is in
  translation or Phase 1 passes (mem2reg / SROA / InstCombine / SimplifyCFG).
- Zero-initializing vreg allocas does not fix the crash → not uninitialized
  vreg reads.

### Crash site (wasm-ld at -j18 batched)
- Crashes inside `wasm_func_40125` (code_idx 40097, body @ code_offset
  0x4005eac, size 2176 bytes).
- Crash is ~1156 bytes into that function body.
- Called from `wasm_func_40014` (code_idx 39986).
- Function 40125 is an allocator-like routine (loads from a memory header,
  masks low 3 bits, navigates free-list pointers at memory offsets
  0x2864D8 / 0x2864DC / 0x2864C8).
- The WASM bytecode for this function contains **no byte-load opcodes**,
  yet the crashing native instruction is `LDRB W9, [X19, W20, UXTW]`.
  Either LLVM codegen split a wider load into bytes, or the crash is in
  compiler-generated runtime-helper inlining, or our translation emitted
  an i8 load where it shouldn't.

### Deterministic-but-layout-sensitive garbage
- X20 garbage is always ASCII byte data (`"_TER"` = 0x5245545f in batched
  layout; `"MINA"` = 0x414e494d with same-batch opt disabled). Changing
  the build subtly changes which garbage gets read, but the fault
  **always** is an OOB WASM memory load with garbage offset.
- The garbage looks like text (rodata) read as an i32 → strongly suggests
  either a wrong load type (i32 read where i8 stored) or a wrong pointer
  (loading from rodata-adjacent memory instead of a local/vreg alloca).

### Diagnostics added (available in the tree)
- `signals.hpp`: dumps all 29 GPRs + 16 preceding instructions on JIT fault.
- `pzam-compile --dump-layout=FILE`: writes `(off, size, kind, code_idx)`
  rows for every entry wrapper and body in the final merged code blob.
  Enables `awk` lookup of which function a given PC lands in.
- `PSIZAM_LLVM_DUMP_FUNC=N,M,...`: dumps pre-optimization IR for specific
  wasm_func_N to `/tmp/wasm_func_N_pre.ll`.
- `PSIZAM_LLVM_DUMP_FUNC_POST=N,M,...`: dumps post-optimization IR.
- `PSIZAM_LLVM_SKIP_PHASE2=1`: run pipeline with only canonicalize + cleanup.
- `PSIZAM_DUMP_CODE_BLOB=FILE`: dump raw merged code bytes for external
  disassembly with `llvm-mc -disassemble -triple=aarch64`.

### Narrowed further (2026-04-19 late)
Corrected PC→function mapping: the crash is in **wasm_func_40107** (a
`memcmp`-like function; `code_idx 40079`), NOT `wasm_func_40125` as
initially thought. The callee's disassembled body:
```
mov w22, w4   ; arg 4 (length)
mov w20, w3   ; arg 3 (2nd pointer) ← the garbage register
mov w23, w2   ; arg 2 (1st pointer)
mov x21, x0   ; ctx
mov x19, x1   ; mem
...
ldrb w9, [x19, w20, uxtw]   ; crash: read byte at mem[arg3]
```
So the garbage is **the 2nd pointer argument passed by the caller**.

The caller is **wasm_func_40014**. Its call site passes `W23` (loaded
from WASM memory) as that argument. Tracing through the IR:
```
%60 = getelementptr i8, ptr %5, i64 2342500   ; mem[2342500]
%61 = load volatile i32, ptr %60              ; = local1
%63 = zext i32 %61 to i64
%64 = getelementptr i8, ptr %5, i64 %63       ; mem[local1]
%65 = load volatile i32, ptr %64              ; = local2, passed to memcmp
```
The value `mem[mem[2342500]]` is `0x5245545f` at crash time — but
the **interpreter executes the exact same IR semantic and this value is
valid enough to not OOB**. So some earlier WASM function wrote
something to WASM memory that our LLVM path wrote differently.

### Next steps for debugging
1. Instrument `__psizam_call_host` and WASM-to-WASM call wrappers to
   log args passed and return values — compare interpreter vs LLVM
   traces for divergence.
2. `wasm_func_40014` stores to `mem[2342500]` in `bb2_exit` (value
   from `wasm_func_40126` or constant `2646500`). Check whether the
   initial store path matches the interpreter's.
3. Bisect Phase 1 passes individually (SROA / EarlyCSE / InstCombine
   / SimplifyCFG). Use a small batch size to avoid OOM.
4. Use `lldb` on `pzam-run` to break at `wasm_func_40107` entry and
   inspect the caller's args live.

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
