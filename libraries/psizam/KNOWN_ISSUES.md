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

### ~~Stale test code~~ — fixed in tree

The previously-listed `return_call.13`, `return_call_indirect.28`, `.29`
test failures no longer reproduce. The checked-in
`return_call_tests.cpp` / `return_call_indirect_tests.cpp` reference
`.wasm` indices that match the current `wast2json` output (1..12 and
12..27 respectively); regenerating with `spec_test_generator` produces
byte-identical output. ctest `-R return_call` passes 112/112.

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
- ~~**host function results** (1): Memory out-of-bounds on reference-typed returns~~ — fixed: jit's `_trampoline_ptrs[i]` was nullptr for non-fast-eligible host functions (those with pointer/reference params or returns), so the JIT-emitted direct call jumped to address 0 → SIGSEGV → `memory_trap`. Added `slow_trampoline_rev<F, ...>` (a per-host-function template instance, callable as a function pointer) and wire `mappings.fast_rev[i]` into `entry.rev_trampoline` so the JIT path always has a non-null trampoline. Used `decltype(auto)` in the slow path to preserve the reference category of the host function's return — `auto` strips it, which then routes a `T&` return through the integer branch of `as_result` and returns the byte VALUE instead of the pointer offset.
- ~~**simd_const_385, simd_const_387** (2): Segfault in SIMD code~~ — fixed: jit2 regalloc `ir_op::arg` needs v128 type to push 16 bytes for v128 params (ir_writer.hpp set `arg.type = ft.param_types[i]`)
- ~~**conversions_0, traps_2** (2): Segfault~~ — fixed upstream
- ~~**Host function / call depth / reentry** (5): Various segfaults~~ — fixed upstream
- ~~**`[call_depth]` tests: stack-overflow limit stops enforcing after first hit**~~ — fixed: `execution_context_base::execute()`'s LLVM branch saved/restored `_remaining_call_depth` with a `scope_guard`, but the native-JIT branch did not. jit2 writes the depth field on every call (`mov [rdi+16], ecx; dec ecx; mov ecx, [rdi+16]`) and a `stack_overflow` longjmp skips the inc cleanup, so the first call that exhausted the budget left the field at 0. Every subsequent call then wrapped 0→UINT32_MAX on dec and the JZ branch never fired again — the JIT silently stopped enforcing call depth. Added the same scope_guard to the native-JIT path; jit1 is unaffected in practice (it keeps the counter in `ebx` and never writes back to ctx) but the guard is harmless there.
- ~~**relaxed_madd_nmadd** (2), **relaxed_dot_product** (1): Wrong results~~ — fixed: jit2 regalloc v128_op handler now pushes the 3rd operand for all ternary ops (relaxed fma + dot-add), not just `v128_bitselect`; the missing push was causing softfloat helpers to read garbage for `a.lo/a.hi` and corrupt the stack
- ~~**multi-value `call` / `call_indirect` rax corruption**~~ — fixed: regalloc paths for direct `call` and `call_indirect` now skip `store_rax_vreg(inst.dest)` for multi-value returns (`ft.return_count > 1`). For multi-value returns the callee writes results to `ctx->_multi_return[]` and the IR writer emits separate `ir_op::multi_return_load` instructions; `rax` is undefined in that case, so storing it into the dest vreg's register could clobber a live vreg that regalloc reused the register for
- ~~**SIGBUS on nested try_table + regalloc** (fuzzer seed 263 module 174)~~ — worked around: jit2 now skips regalloc for any function that uses exception handling (`eh_data_count > 0`), falling back to stack-mode codegen. See `jit2 regalloc ↔ EH interaction bug` below for the full investigation and why the proper fix is deferred
- ~~**Unresolved imported function → memory_trap instead of wasm_link_exception**~~ — fixed: populate `_host_trampoline_ptrs[i]` with a stub that throws `wasm_link_exception` for imports that can't be resolved. Without this, the JIT's direct indirect call on `nullptr` produced SIGSEGV which the signal handler mapped to `memory_trap`, diverging from interp/jit_llvm's `rejected` outcome

**aarch64 only:**
- ~~**rem codegen clobber**: i32/i64 rem_u/rem_s hardcoded W2/X2 as scratch for UDIV/SDIV quotient, clobbering live vregs~~ — fixed: use X16 (intra-procedure-call scratch) instead
- ~~**IR corruption in dead code**: emit_br/emit_br_if/emit_try_table returned branch index 0 in unreachable code, corrupting IR instruction 0~~ — fixed: return UINT32_MAX instead

### aarch64 notes

The aarch64 JIT backends do not fully implement SIMD floating-point operations
with softfloat. These failures only appear on ARM64, not on x86_64.

---

## Open design issues

### ~~jit2 regalloc ↔ EH interaction bug~~ — fixed

**Status:** fixed via force-spill (commits `b88c3af` + `6e909ec`). Any vreg
in a function containing `ir_op::eh_setjmp` is marked
`ir_live_interval::crosses_setjmp = 1` and force-spilled to memory (no
phys_reg, no phys_xmm, spill_slot assigned). At catch entry (reached via
longjmp), every use reloads from its spill slot — which is in the function's
stack frame and survives longjmp unchanged. The old "skip regalloc for EH
functions" workaround is removed. Regalloc-mode codegen still runs
(so fusion, multi-value dispatch, etc. stay correct) — only the data-flow
goes through memory.

The original analysis below is kept for historical context and as a pointer
for improving the fix (e.g. narrower criteria for less spilling).

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

### ~~jit2 stack-mode fusion: missing `if_`/`br_if` fixup push~~ — no longer surfaces

**Status:** not an active issue. The stack-mode fusion bug is real (stack
mode doesn't implement the IR_FUSE_NEXT fast path) but EH-using functions
now go through the regalloc path (with force-spilled vregs), which DOES
implement fusion correctly. Non-EH functions already went through regalloc.
So no fuzz modules currently hit the stack-mode-with-fusion code path.

If stack-mode ever gets re-enabled for EH functions (or for any other reason),
apply the one-line `emit_fused_branch` check in the stack-mode `i32_eqz` /
`i64_eqz` / relop handlers.

**Reproducer.**

```wat
(func
  global.get 0
  i32.eqz
  if unreachable end
  try_table (catch_all 0)
    br 1
  end
)
(global (mut i32) (i32.const 100))
(start 0)
```

Interp / jit / jit_llvm: `ok`. jit2: `interp_trap (unreachable)`.

**Root cause.** The optimizer fuses a comparison (`i32.eqz`, `i32.eq`, etc.)
with a following `if_`/`br_if`: it sets `IR_FUSE_NEXT` on the comparison
and `IR_DEAD` on the consumer. Regalloc-mode codegen handles this via
`emit_fused_branch`. Stack-mode codegen unconditionally skips `IR_DEAD`
instructions (jit_codegen.hpp:583) and does not check `IR_FUSE_NEXT` on
the comparison. Result: the comparison emits `setcc+push` but the
consumer's `push_if_fixup` is never called. `end_if` then pops an outer
if's fixup and patches that outer branch to the wrong target, producing
spurious control-flow divergence.

Because my workaround for `jit2 regalloc ↔ EH interaction` forces stack
mode for all EH-using functions, this bug surfaces on every fuzzer module
that combines a counter `if (global==0) unreachable` pattern with
`try_table` — the majority of the `interp=ok jit2=interp_trap` and
`interp=memory_trap jit2=interp_trap` fuzz mismatches (~80+% of them).

**Attempted fix.** Adding `if (IR_FUSE_NEXT) emit_fused_branch` to the
`i32.eqz` / `i64.eqz` stack-mode handlers correctly fixes ~40 of the 60
mismatches on seed 31337×5K. But it exposes a separate jit2 bug: some
modules that previously took the wrong (`unreachable`) branch now
execute the correct path and hit a memory-corruption bug deeper in jit2.
Symptom: SIGABRT from an assertion in
`execution_context.hpp:854` ("Unexpected multi-value return type") where
the captured `const func_type& ft` has garbage `return_count`/`return_type`
fields after the JIT returned, indicating the JIT wrote outside its
allocated memory and trashed the module's type table. Also seen:
assertion `!"Unexpected function return type"` at
`execution_context.hpp:863`, timeouts (infinite loops), and additional
`memory_trap` outcomes. Net effect: -40 mismatches, +2 crashes,
+1 timeout, +17 `interp_trap→memory_trap` — crashes are strictly worse
than mismatches, so the fusion fix is reverted.

**Proper fix requires two commits together:**
1. Fix stack-mode fusion (the simple one-line add noted above).
2. Find and fix the jit2 memory-corruption bug it exposes.

Reproducer for (2) once (1) is applied: seed 31337 module 1925
(`crash_1925_seed31337.wasm`, 4142 bytes) — start function has
`throw 1` under a `try_table` with matching `catch`; after completion
the module's `func_type` table has been overwritten. Suspected root cause
is in the jit2 EH unwind path corrupting adjacent memory; not yet
diagnosed.

### ~~jit2 float-to-int trap surfaces as SIGSEGV in libgcc unwinder~~ — fixed

**Status:** fixed (commit `d30d84f`). Replaced the throw-based trapping
path (`longjmp_on_exception([&]() { _psizam_f*_trunc_i*(f); })`) with
inline NaN/overflow checks using `signal_throw`, which longjmps directly
to `trap_jmp_ptr` without walking JIT frames. Fuzz runs across 5 seeds ×
3K modules show the `interp=interp_trap jit2=memory_trap` class went
from 15 → 0. The original (unfixed) description is kept below for
context in case a related unwinder issue resurfaces.

**Reproducer.** Fuzz seed 31337 module 74/656/1692 — any WASM module that
calls `i32.trunc_f32_u` / `i32.trunc_f64_s` / `i64.trunc_f32_u` with a
NaN or out-of-range input via jit2's stack-mode path.

**Symptom.** interp reports `interp_trap (Error, f32.convert_u/i32
unrepresentable)`; jit2 reports `memory_trap`. Inside jit2, the softfloat
helper `_psizam_f32_trunc_i32u` correctly throws `wasm_interpreter_exception`,
but libgcc's `_Unwind_RaiseException` faults on a misaligned `movaps`
during its own stack scan. The exception is wrapped by
`longjmp_on_exception` *inside* the helper, so unwinding should not need
to walk JIT frames — yet the unwinder is walking past the handler,
suggesting an `.eh_frame` or LSDA mismatch for the stub helper.

**Note:** applies only on stack-mode codegen (active for EH-using
functions under the current workaround). Regalloc-mode is not tested for
this pattern but may or may not be affected.

### jit2 ctx-object vector state corruption (deeply nested try_table) — FIXED

**Status:** fixed.

**Symptom was:** crash inside `jit_execution_context::jit_eh_leave()` at
`_jit_eh_catches.resize(frame.first_catch_idx)` with garbage state, for
modules with nested `try_table` / `throw` / `catch` constructs. A minimal
repro is three nested `try_table (catch_all 0)` blocks containing a
`throw 0`.

**Root cause.** `emit_ir_inst`'s `eh_setjmp` emitted the sequence

```
push rdi        ; save ctx
push rsi        ; save mem
mov  rax, rdi   ; jmpbuf → arg1
mov  rsp, rax
and  rsp, -16
push rax        ; save pre-align rsp  ← stored at [rsp] = S
call __psizam_setjmp
pop  rsp        ; reads [S]
pop  rsi
pop  rdi
```

`__psizam_setjmp` is a naked tail-jump to glibc `setjmp`, which saves
`rsp+8` (post-ret) as the jmpbuf's rsp — pointing at the slot just
written by `push rax`. On longjmp return, `pop rsp` must read that slot.

But `__psizam_eh_throw` is a normal C++ function whose prologue runs
`push rbp; push rbx; sub $0xd8, rsp`. Called from the JIT at the same
rsp that was current before the setjmp sequence, its `push rbx` lands
*exactly* on the slot where the pre-align rsp was saved — overwriting it
with caller-preserved rbx. On longjmp, `pop rsp` then loads rbx's value
into rsp, and the subsequent `pop rsi` / `pop rdi` read from random
stack addresses. The observed symptom was `rdi = ctx + 0x170`, which
corresponds to `&ctx->_dropped_elems` — a coincidence of whatever
garbage the corrupted rsp happened to land on.

**Fix.** Reserve three rbp-relative frame slots (at the bottom of the
function's local frame, below the spill slots and callee-saved saves),
allocated only when `func.eh_data_count > 0`. `eh_setjmp` now saves
rdi/rsi/pre-align-rsp to those slots and reloads from them after the
call, replacing the push/pop dance. Frame-relative slots are above any
rsp the helper calls can reach, so they survive the throw path intact.

jit1 was unaffected — it preserves ctx in callee-saved `r12` across the
setjmp/longjmp and doesn't touch the stack slot at all. jit_llvm uses
LLVM's own invoke/landingpad mechanism and is also unaffected.

### jit1 native: return_call is not tail-call-optimized (open)

**Status:** open — by-design gap, not yet implemented.

**Reproducer.** `mismatch_179193_seed777777.wasm` (4231 B). interpreter/jit2/jit_llvm run OK; jit1 traps with `interp_trap`.

**Root cause.** `emit_tail_call` in both `x86_64.hpp` (line 571) and `aarch64.hpp` (line 776) forward to `emit_call` — a comment ("Tail calls: desugar to call + return in JIT1 (no frame reuse optimization)") confirms this is deliberate. A deeply nested `return_call` chain (as fuzzer commonly produces) then stack-overflows where the interpreter's true TCO (`execution_context::tail_call`) reuses the frame. The signal handler maps the native stack overflow to `interp_trap`, so it looks like a trap divergence rather than an OOM.

**Fix sketch.** Proper TCO requires tearing down the current activation frame (restore FP, adjust SP to caller-expected state, move args into the callee's expected positions) then `B` (jump) instead of `BL` (call) to the target. Multi-value and host/import callee paths need special handling. Non-trivial — multi-hundred-line change across the prologue/epilogue and call codegen.

### jit_llvm: opaque `unreachable` trap on deeply-nested fuzzer modules (open)

**Status:** open — root cause not narrowed.

**Reproducer.** `mismatch_365607_seed31415926.wasm` (3264 B). interpreter/jit/jit2 run OK; jit_llvm raises `interp_trap (unreachable)`. Minimization via `wasm-reduce` is blocked because binaryen's `wasm-opt` can't round-trip the fuzzer-generated module (validator rejects an `f32`/`i32` type mismatch in an xor inside a block, which psizam's parser accepts). Module uses `try_table`+tail-call+EH+v128 densely; likely a translation/lowering bug in `llvm_ir_translator.cpp` that only triggers on a specific opcode-sequence/type combination.

**Fix sketch.** Hand-minimize (binaryen tooling doesn't work). Alternatively, add a trap-site log to `llvm_ir_translator` that dumps the WASM PC when emitting the unreachable IR, to pinpoint the buggy path.

### jit2 non-deterministic timeout (counter+try_table loop)

**Status:** open — ~1 mismatch per 10K fuzzed modules; not consistently
reproducible (run-to-run hangs vs traps depending on stack contents).

**Reproducer.** `mismatch_1041_seed1776492727.wasm` (2555 B). All
non-jit2 backends trap with `interp_trap (unreachable)` instantly. jit2
hangs forever about 4 of every 5 runs; the remaining run traps
correctly. The wasm pattern is the standard "global counter + body that
decrements" with the body containing `try_table` constructs (also
common in fuzzer output to bound recursion).

**What was investigated:** the bug is NOT the eh_setjmp saved-rsp
corruption (that's fixed in commit `5c8d694`). It also isn't reproducible
with a minimal 2-level nested try_table inside a counter loop — the
original module has many nested try_tables with explicit catch_tag
handlers and `br_if` jumping out to the loop, which is what likely
exercises the buggy path. Likely the same family as the documented
"stack-mode fusion: missing if_/br_if fixup push" bug, but the path
through regalloc-mode codegen with all-spilled vregs hasn't been
narrowed down.

**Investigation hint:** the function loops on a global counter that
should reach 0 after ~100 iterations, then `unreachable`. jit2's
infinite loop suggests either the global decrement is wrong, the
condition is evaluated wrong, or the `br_if` direction is reversed in
the JIT. Trace `global.get`/`global.set` and `i32.eqz`/`br_if` codegen
in regalloc-mode for functions with `eh_data_count > 0` and large
operand stacks.
