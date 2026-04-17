# psizam Known Issues

## Test Summary

| Platform | Failures | Total | Pass Rate |
|----------|----------|-------|-----------|
| macOS aarch64 | 104 | 13,258 | 99.2% |
| Linux x86_64 | 135 | 13,186 | 99.0% |

The difference is entirely jit2 backend bugs (x86_64 vs aarch64 codegen).

### Multi-module (96–98 failures, all platforms)

These tests require multi-module linking support which is not yet implemented.
Each test is counted across all 3 backends (interpreter, jit, jit2).

- **table_copy_1..18** (54): Import functions from a host module
- **table_init_1..6** (18): Import functions from a host module
- **elem_18..21** (12): Element segment indices from missing linked modules
- **elem_59, elem_60** (6): Import shared table from "module1"
- **elem_68** (3): Imports funcref table from "module4"
- **data_25, data_26** (6): Data segment out of range (depends on linked module memory)

### global_0 (3 failures, all backends, all platforms)

Imports `spectest.global_i32` and `spectest.global_i64` which should be 666,
but the test harness uses `standalone_function_t` so imported globals default
to 0. Also uses `externref` globals which crash the jit backend.

### Stale test code (9 failures, all backends)

Pre-generated `_tests.cpp` files reference `.wasm` numbers that no longer
exist in the current tail-call proposal `.wast`:

- `return_call.13.wasm`, `return_call_indirect.28.wasm`, `return_call_indirect.29.wasm`

Fix: regenerate from current `.wast` with `wast2json --enable-tail-call`.

### jit2 backend bugs (x86_64: 28, aarch64: 5)

All pass on interpreter and jit. jit2 is experimental.

**Both platforms:**
- **block_0, if_0**: IR virtual stack underflow
- **func_0**: Wrong return value on break-i32-f64
- **global_0**: Imported global init (counted above)

**x86_64 only:**
- **table.grow** (7): Returns 0 instead of -1 for invalid operations
- **table.size** (1): Always returns 0
- **host function results** (1): Memory out-of-bounds on reference-typed returns
- ~~**simd_const_385, simd_const_387** (2): Segfault in SIMD code~~ — fixed: jit2 regalloc `ir_op::arg` needs v128 type to push 16 bytes for v128 params (ir_writer.hpp set `arg.type = ft.param_types[i]`)
- **conversions_0, traps_2** (2): Segfault
- **Host function / call depth / reentry** (5): Various segfaults
- **relaxed_madd_nmadd** (2), **relaxed_dot_product** (1): Wrong results

### aarch64 notes

The aarch64 JIT backends do not fully implement SIMD floating-point operations
with softfloat. These failures only appear on ARM64, not on x86_64.
