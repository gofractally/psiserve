# psizam Known Issues

## Test Summary

| Platform | Failures | Total | Pass Rate |
|----------|----------|-------|-----------|
| macOS aarch64 | 104 | 13,258 | 99.2% |
| Linux x86_64 | 124 | 13,186 | 99.1% |

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

### jit2 backend bugs (x86_64: 17, aarch64: 5)

All pass on interpreter and jit. jit2 is experimental.

**Both platforms:**
- ~~**block_0, if_0**: IR virtual stack underflow~~ — fixed: emit_throw/emit_rethrow/emit_throw_ref now set `_unreachable = true`; emit_end synthesizes implicit else for if-without-else blocks with params
- ~~**func_0**: Wrong return value on break-i32-f64~~ — fixed by same unreachable tracking fix
- ~~**conversions_0, traps_2** (on aarch64): Segfault~~ — fixed by same unreachable tracking fix
- ~~**if_96..103** (12, jit2+jit_llvm): SEGFAULT on invalid modules with typed if params~~ — fixed: emit_if now checks param_count <= vstack_depth() before subtracting, matching emit_block/emit_loop
- **global_0**: Imported global init (counted above)

**x86_64 only:**
- ~~**table.grow** (7): Returns 0 instead of -1 for invalid operations~~ — fixed: regalloc path for `ir_op::table_grow` now calls `__psizam_table_grow` and stores rax to dest vreg (was falling through to stack-mode codegen whose `push rax` was invisible to the regalloc consumer)
- ~~**table.size** (1): Always returns 0~~ — fixed: same root cause as `table.grow`; regalloc path now stores the helper's return value to the dest vreg
- **host function results** (1): Memory out-of-bounds on reference-typed returns
- ~~**simd_const_385, simd_const_387** (2): Segfault in SIMD code~~ — fixed: jit2 regalloc `ir_op::arg` needs v128 type to push 16 bytes for v128 params (ir_writer.hpp set `arg.type = ft.param_types[i]`)
- ~~**conversions_0, traps_2** (2): Segfault~~ — fixed upstream
- ~~**Host function / call depth / reentry** (5): Various segfaults~~ — fixed upstream
- ~~**relaxed_madd_nmadd** (2), **relaxed_dot_product** (1): Wrong results~~ — fixed: jit2 regalloc v128_op handler now pushes the 3rd operand for all ternary ops (relaxed fma + dot-add), not just `v128_bitselect`; the missing push was causing softfloat helpers to read garbage for `a.lo/a.hi` and corrupt the stack
- ~~**multi-value `call` / `call_indirect` rax corruption**~~ — fixed: regalloc paths for direct `call` and `call_indirect` now skip `store_rax_vreg(inst.dest)` for multi-value returns (`ft.return_count > 1`). For multi-value returns the callee writes results to `ctx->_multi_return[]` and the IR writer emits separate `ir_op::multi_return_load` instructions; `rax` is undefined in that case, so storing it into the dest vreg's register could clobber a live vreg that regalloc reused the register for
- ~~**SIGBUS on nested try_table + regalloc** (fuzzer seed 263 module 174)~~ — worked around: jit2 now skips regalloc for any function that uses exception handling (`eh_data_count > 0`), falling back to stack-mode codegen. Root cause: the linear-scan regalloc treats setjmp as just another call site, but a catch handler reached via longjmp inherits the *setjmp-time* callee-saved register state — not the state regalloc assumes at the catch's IR position. If a vreg X is live at the catch entry but was not live at setjmp, X's assigned callee-saved register holds a stale pre-setjmp value after longjmp, causing arbitrary corruption (in this case, eventually trashing the outer `invoke_with_signal_handler`'s `jmp_buf` and producing SIGBUS on longjmp return). A proper fix requires the catch handler's entry live set to be a subset of setjmp's live set, or per-path register state reconciliation

**aarch64 only:**
- ~~**rem codegen clobber**: i32/i64 rem_u/rem_s hardcoded W2/X2 as scratch for UDIV/SDIV quotient, clobbering live vregs~~ — fixed: use X16 (intra-procedure-call scratch) instead
- ~~**IR corruption in dead code**: emit_br/emit_br_if/emit_try_table returned branch index 0 in unreachable code, corrupting IR instruction 0~~ — fixed: return UINT32_MAX instead

### aarch64 notes

The aarch64 JIT backends do not fully implement SIMD floating-point operations
with softfloat. These failures only appear on ARM64, not on x86_64.
