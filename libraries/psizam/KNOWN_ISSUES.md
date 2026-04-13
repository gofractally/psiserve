# psizam Known Issues

## Test Summary: 158 failures / 13186 tests (98.8% pass rate, x86_64)

### Multi-module (98 failures)

These tests require multi-module linking support which is not yet implemented.
Each test is counted across all 3 backends (interpreter, jit, jit2).

- **table_copy_1..18** (54): Import functions from a host module
- **table_init_1..6** (18): Import functions from a host module
- **elem_18..21** (12): Element segment references out-of-bounds indices from missing linked modules
- **elem_59, elem_60** (6): Import shared table from "module1"; exported functions only exist in module1
- **elem_68** (3): Imports funcref table from "module4"; `call_indirect` hits uninitialized entries
- **data_25, data_26** (6): Data segment out of range (depends on linked module memory)
- **global_0** (partially, 2): Imports `spectest.global_i32`/`global_i64` which default to 0 instead of 666;
  also uses `externref` globals not fully supported in JIT

### Missing proposal .wast files (21 failures)

Relaxed SIMD tests reference `.wasm` files from the relaxed-simd proposal,
which is not included in the main WebAssembly testsuite submodule:

- `i16x8_relaxed_q15mulr_s_0`, `i32x4_relaxed_trunc_0`, `i8x16_relaxed_swizzle_0`
- `relaxed_dot_product_0`, `relaxed_laneselect_0`, `relaxed_madd_nmadd_0/1`, `relaxed_min_max_0`

Each × 3 backends = 21. Fix: add relaxed-simd proposal `.wast` sources or
check in the pre-built `.wasm` files.

### Stale test code (9 failures)

Pre-generated `_tests.cpp` files reference `.wasm` numbers that no longer
exist in the current tail-call proposal `.wast`:

- `return_call.13.wasm` (3)
- `return_call_indirect.28.wasm` (3)
- `return_call_indirect.29.wasm` (3)

Fix: regenerate `return_call_tests.cpp` and `return_call_indirect_tests.cpp`
from the current `.wast` with `wast2json --enable-tail-call`.

### jit2 backend bugs (18 failures, x86_64 only)

All pass on interpreter and jit. jit2 is experimental.

- **table.grow** (7): Returns 0 (success) instead of -1 for invalid grow operations
- **table.size** (1): Always returns 0 regardless of actual table size
- **host function results** (1): Memory out-of-bounds on reference-typed host function returns
- **simd_const_385, simd_const_387** (2): Segfault in JIT-generated SIMD code
- **conversions_0** (1): Segfault during float conversion
- **traps_2** (1): Segfault during trap handling
- **Host function / call depth / reentry tests** (5): Various segfaults in
  host function binding, deep recursion, and module re-entry

### Multi-value returns (5 failures, all backends)

The engine parses multi-value block types but `call_with_return` only extracts
the first value. The updated testsuite `.wasm` files now contain multi-value
block types that trigger "wrong type" during parsing:

- `block_0`, `br_0`, `func_0`, `if_0`, `loop_0` — each fails on all 3 backends
  but only jit2 is counted (interpreter/jit pass because the multi-value blocks
  are in functions not exercised by the single-value CHECK assertions).

Note: The 5 jit2 failures for these are included in the jit2 count above.
Interpreter and jit pass these tests because they only check single-value
assertions that happen to work.

### aarch64 notes

The aarch64 JIT backends do not fully implement SIMD floating-point operations
with softfloat. These failures only appear on ARM64, not on x86_64.
