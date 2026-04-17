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
- ~~**SIGBUS on nested try_table + regalloc** (fuzzer seed 263 module 174)~~ — worked around: jit2 now skips regalloc for any function that uses exception handling (`eh_data_count > 0`), falling back to stack-mode codegen. See `jit2 regalloc ↔ EH interaction bug` below for the full investigation and why the proper fix is deferred

**aarch64 only:**
- ~~**rem codegen clobber**: i32/i64 rem_u/rem_s hardcoded W2/X2 as scratch for UDIV/SDIV quotient, clobbering live vregs~~ — fixed: use X16 (intra-procedure-call scratch) instead
- ~~**IR corruption in dead code**: emit_br/emit_br_if/emit_try_table returned branch index 0 in unreachable code, corrupting IR instruction 0~~ — fixed: return UINT32_MAX instead

### aarch64 notes

The aarch64 JIT backends do not fully implement SIMD floating-point operations
with softfloat. These failures only appear on ARM64, not on x86_64.

---

## Open design issues

### jit2 regalloc ↔ EH interaction bug

**Status:** worked around in `libraries/psizam/include/psizam/detail/ir_writer.hpp`
by skipping regalloc for any function with `eh_data_count > 0` (commit `28229a0`).
The workaround is correctness-preserving but costs performance for any function
using `try_table` — those functions fall back to stack-mode codegen.

**Reproducer.** Differential fuzzer seed 263 module 174 (5184 bytes). Minimal
repro:

```
./bin/psizam-fuzz-diff --file-backend crash_174_seed263.wasm jit2   # → SIGBUS
```

Interpreter and `jit_llvm` correctly trap with `unreachable`. The module is a
deeply-nested (25+ levels) stack of `try_table` / `loop` / `block` with a
multi-value `call_indirect (type 3)` somewhere in the middle. The crash
disappears when any one of:

- regalloc is globally disabled for jit2, or
- all four EH regalloc handlers (`eh_enter`, `eh_setjmp`, `eh_leave`, `eh_throw`)
  are disabled together (individual disables don't work — mixing modes breaks
  the native-stack vs vreg-register convention), or
- the first `call_indirect (type 3)` is removed from the module.

**Symptom.** gdb at crash:

```
Program received signal SIGBUS, Bus error.
invoke_with_signal_handler (...) at signals.hpp:337
337      if((sig = setjmp(dest)) == 0) {
rip            <setjmp return site>
rsp            0x25dadba9ea279b37      ← garbage
rbp            0x4840dba9ff7bea39      ← garbage
r12..r15       valid stack addresses   ← preserved
```

Sequence: (1) a SIG* fires inside JIT code; (2) the handler `longjmp`s to
`trap_jmp_ptr` which points at the outer `invoke_with_signal_handler`'s
`jmp_buf dest`; (3) longjmp restores rbp/rsp from `dest`, but `dest` has been
overwritten — some JIT store landed on the host stack at the wrong address;
(4) the next C++ instruction (`mov %eax, -0x13c(%rbp)`) faults.

**Suspected mechanism.** Linear-scan regalloc treats `eh_setjmp` as a regular
call site — any vreg whose interval crosses it gets a callee-saved register
(rbx, r12–r15), which setjmp/longjmp preserves. That handles one direction
correctly. The problem is the *reverse* edge: when `longjmp` returns to the
setjmp point, control continues through the IR into the catch handler. At
the catch-handler IR position, regalloc assumes whatever register assignment
it computed by linear scan — but the actual register contents are whatever
`longjmp` restored from `jmp_buf`, i.e. the **setjmp-time** values.

If vreg X is live at the catch IR position but was *not* live at `eh_setjmp`
(e.g., X was defined inside the try body or in the EH dispatch prologue),
X's assigned callee-saved register holds a stale pre-setjmp value after
longjmp. Subsequent uses of X read garbage, and downstream stores using that
garbage as a base/offset can land anywhere — including on the host stack,
which is what corrupts `dest`.

**What was investigated and ruled out:**
- ❌ **Multi-value `call`/`call_indirect` rax clobber.** Fixed as a separate
  bug (commit `c013d77`) but does not resolve this crash on its own.
- ❌ **Caller-saved register assignment across a call.** Regalloc's
  `crosses_call` check (jit_regalloc.hpp:458) correctly forces callee-saved
  regs for intervals crossing any call, including `eh_setjmp`. XMM regs are
  never assigned to call-crossing intervals (all XMM are caller-saved on
  SysV x86_64). The eviction path (line 560) explicitly refuses to evict
  into caller-saved for a call-crossing interval.
- ❌ **Misaligned SSE or unaligned atomics.** No `movaps`/`lock cmpxchg` in
  the generated code path. JIT uses `movdqu` (unaligned) for spills.
- ❌ **Individual EH handler bug.** Disabling just `eh_throw`'s regalloc
  path doesn't stop the crash; disabling just `eh_enter` or `eh_setjmp` in
  isolation causes a different crash (stack-mode vs regalloc-mode state
  mismatch on operand stack). Only disabling all four together is safe.

**What remains unknown:** the exact IR position and register that gets a
stale value, and therefore the exact instruction that writes garbage onto
the host stack. gdb at crash time has already lost the state; stepping
through ~100 loop iterations of JIT code is impractical.

**Proper fix directions (not yet implemented):**
1. **Force-spill across `eh_setjmp`.** Treat `eh_setjmp` as clobbering *all*
   GPRs (not just caller-saved). Any vreg with an interval crossing it
   must be evicted to memory; reloads are inserted on both the normal-return
   and catch paths. This is how production JITs (V8, SpiderMonkey) typically
   handle setjmp/longjmp. Implementation cost: moderate — requires teaching
   regalloc about spill-insertion at specific IR points, which the current
   linear scan doesn't do (it only spills on full-register pressure).
2. **Extend catch-reachable live ranges back to `eh_setjmp`.** At
   `compute_live_intervals` time, for each catch handler entry position
   P_catch, extend every vreg live at P_catch so its `start` is ≤
   `eh_setjmp`. This unifies regalloc's view with the actual longjmp
   semantics: any vreg usable at catch must have been in a register at
   setjmp. Implementation cost: low — a post-processing pass over
   intervals. Risk: may over-extend intervals and cause spurious register
   pressure.
3. **Split regalloc at EH boundaries.** Treat `eh_setjmp` as a basic-block
   boundary that terminates all live ranges and restarts fresh. Reloads
   after setjmp come from spill memory. This is the cleanest model but
   requires changing the regalloc data flow.

Option (2) is the most tractable first step. If someone picks this up, start
with a repro via `psizam-fuzz-diff 200 263` and verify the fix keeps
`[eh]` compliance tests green and the fuzzer clean through seeds 42, 263,
and a few others.
