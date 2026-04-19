# psizam Known Issues

## Test Summary

| Platform | Failures | Total | Pass Rate |
|----------|----------|-------|-----------|
| macOS aarch64 | 4 | 12,576 | 99.97% |
| Linux x86_64 | (stale — re-measure after submodule bump) | | |

The spec testsuite submodule was bumped to `51279a9` and the SIMD block now
auto-regenerates like the other spec tests (see
`tests/spec_test_helpers.sh` + `tests/CMakeLists.txt`). That exposed a wider
set of tests: the previously-documented "multi-module" and "global_0"
failures no longer reproduce (the newer testsuite wraps those cases in
`assert_unlinkable` / `register`, which the generator now skips cleanly).
The categories below describe what remains.

### ~~Reftype parser gap — 60 failures~~ — fixed

Parser now accepts the typed-reference proposal encodings
(`0x64 heaptype` for `(ref HT)`, `0x63 heaptype` for `(ref null HT)`)
wherever reftypes appear: table element types, block signatures
(`block`/`loop`/`if`), element-segment reftypes, `ref.null`, and the
table-type init-expr prefix (`0x40 0x00 …`). Concrete type indices in
the heap-type slot collapse to `funcref`/`externref` since psizam's
runtime doesn't track per-slot signatures.

Non-null tables now also reject initializers whose reftype is nullable:
`parse_reftype` reports a `non_null` flag, `parse_table_type` records
it per-table, and `parse_elem_segment` enforces `(ref func)`-table ↔
`(ref func)`-segment (elem segments with flags 0–3 are implicitly
non-null because they enumerate function indices, which can't be null).

Also extended `br_table` validation to accept multi-type labels in
polymorphic (unreachable) state while still checking arity, so
`br_table_0` func 70's `block(result f64){block(result f32){unreachable;
i32.const 1; br_table 0 1 1}}` pattern parses cleanly, and the legacy
arity-mismatch rejection on `unreached-invalid.86` still fires.

Cleared: **elem_47..56** (10), **elem_57..62** (6), **br_table_0** (1),
**unreached-invalid_86/107/108/109** (4), and table-init-expr side
cases — 60+ failures across 4 backends.

### Multi-memory proposal — 4 failures (1 test × 4 backends)

- **memory_grow_2** (1 test): exports `grow1`/`grow2` over two
  memories and issues `memory.grow $mem2` (encoded `0x40 0x01`). The
  parser rejects with `"memory.grow must end with 0x00"`. The
  `memory.size`/`memory.grow` parsers hardcode the reserved byte to
  `0x00`; the multi-memory proposal replaces it with a memidx.
  Blocked on full multi-memory runtime support (load/store/init/copy
  operand encodings and per-memory dispatch in all five backends).

### ~~spectest harness imports~~ — fixed

`spectest_host_t` in `tests/utils.hpp` now provides `print` / `print_i32`
/ `print_i64` / `print_f32` / `print_f64` / `print_i32_f32` /
`print_f64_f64` as no-op member functions, registered via
`spectest_rhf::add<&spectest_host_t::print_i32>(...)`. This cleared
**binary-leb128_10..12** (3) and **token_11** (1) — 16 failures total.

### ~~Memory section limit / stale memory.wast artifacts~~ — fixed

Root cause was two-fold: (1) the parser hardcoded `count <= 1` for the
memory section (fine for WASM 1.0, but rejected valid 2-memory+
modules); (2) `memory.wast` in the upstream testsuite uses two
constructs newer `wabt` versions can't parse — `(module definition ...)`
and Memory64 oversize literals like `0x1_0000_0000` — so wast2json
returned an empty `.json` and `.wasm` files went stale. The
checked-in `memory_tests.cpp` expected a different set of modules than
the stale `.wasm` files carried. Fix:

- `parser.hpp`: replaced the hardcoded cap with
  `max_memory_section_elements` (default 16) via the `MAX_ELEMENTS`
  macro.
- `tests/spec_test_helpers.sh`: preprocess the `.wast` through awk
  before invoking wast2json, stripping any `module definition` form
  and any `assert_*` block containing `0x1_0000_0000`. Balanced-paren
  tracker handles multi-line blocks.

Cleared **memory_6..10, memory_25, memory_29, memory_grow_0** (8
tests × 4 backends = 32 failures). Also cleared **select_7_wasm** (4)
as a side benefit of the helper running fresh on all `.wast`
regenerations.

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
- ~~**C-style host function int/float args swapped** (jit + jit2)~~ — fixed: host-call arg-repack copy loop indexed the WASM operand stack as `(num_params - 1 - i) * 16`, producing a forward-ordered buffer (args[0] = first WASM param). The trampoline installed for native JIT is `fast_trampoline_rev_impl` (reads args[0] = last WASM param — matches x86_64's zero-copy stack layout). Mismatch only visible for heterogeneously-typed params: `apply(i32, f64, i32)` via a C-style host import got its int and float slots crossed. Fixed by flipping the src offset to `i * 16` in both `aarch64.hpp::emit_legacy_host_call_body` and `jit_codegen_a64.hpp`'s equivalent — same instruction count, simpler offset math. x86_64 was unaffected (zero-copy stack layout is naturally reverse-ordered). jit_llvm was fixed separately in commit `604bcb5` by picking the forward trampoline per-backend.

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

### ~~jit1 native: return_call is not tail-call-optimized~~ — narrow TCO landed (commit `648c23f`)

**Status:** fixed for the common case — native stack overflow on deep
`return_call`/`return_call_indirect` chains is resolved. Narrow TCO
guards: internal callee only, callee signature matches current function's,
empty param list, `!_stack_limit_is_bytes`. In that case both x86_64 and
aarch64 now tear down the caller frame and B/jmp directly to the callee.
Non-matching cases still fall back to emit_call + emit_return, which is
equivalent to the previous behavior.

Parser now emits `emit_eh_leaves_for_branch` BEFORE the tail-call
codegen so the caller's try_table scopes are popped before the callee
runs.

**Residual divergence on `mismatch_179193_seed777777.wasm`.** Stack
overflow is gone but a separate semantic divergence remains — jit1
reports `wasm_exception (uncaught throw)` where interpreter/jit2/jit_llvm
all run OK. See the new "jit1 EH: uncaught-throw divergence after tail
call" item below.

**Fix sketch (for broader TCO coverage).** Lifting the narrow guards
requires copying callee args into the caller-frame positions before the
teardown (non-empty param list), and moving-arg conventions for
differing signatures. Multi-hundred-line change; not yet prioritized.

### ~~jit1 EH: uncaught-throw divergence after tail call~~ — fixed

**Status:** fixed. Root cause was NOT in the tail-call path itself.

**Root cause.** `x86_64.hpp::emit_try_table` and
`aarch64.hpp::emit_try_table` returned early for 0-catch try_tables
(`if (catch_count == 0) return {}`) — skipping `__psizam_eh_enter`.
But the parser still marked the block as `is_try_table = true`, so
its `end` instruction emitted `emit_eh_leave` → `__psizam_eh_leave`.
This popped the WRONG frame (the enclosing try_table's frame) from
`jit_eh_stack`, leaving the EH stack empty when `throw` fired.

In `mismatch_179193_seed777777.wasm`, func 0 has
`try_table @3 (6 catches) { ... try_table @5 (0 catches) { } end; throw 0 }`.
The empty @5's `end` popped @3's frame, so `throw 0` found no handler.

**Fix.** Parser now sets `is_try_table = (num_catches > 0)`. A 0-catch
try_table is semantically a block (can never catch anything), so it
doesn't need an EH frame. This keeps `end` from emitting `eh_leave`
and keeps `count_eh_leaves_for_branch` from counting it.

### ~~jit_llvm: SIMD-chain divergence surfaces as `unreachable` trap~~ — fixed (commit `ef8c4e8`)

**Status:** fixed. Root cause was jit_llvm's `hw_deterministic` path
for `f32x4.demote_f64x2_zero` / `f64x2.promote_low_f32x4`, which used
LLVM's native `CreateFPTrunc` / `CreateFPExt` instead of the softfloat
helper. Hardware `fptrunc` on NaN produces canonical quiet NaN
(`0x7fc00000`) while softfloat preserves the NaN payload — that
mismatch cascaded through the module's control flow until the runaway
counter hit 0 on `unreachable`. The scalar `f32_demote_f64` /
`f64_promote_f32` already routed through softfloat in any non-`fast`
mode; the SIMD variants now do the same (and skip the follow-on
`maybe_canon` which would overwrite the softfloat-provided payload).

Reproducer `mismatch_365607_seed31415926.wasm` now runs ok on both
backends; all 4572 SIMD spec tests still pass.

**Reproducer.** `mismatch_365607_seed31415926.wasm` (3264 B, 1190-line
WAT via `wasm2wat --enable-all`). interpreter/jit/jit2/wasm3 run OK;
jit_llvm raises `interp_trap (unreachable)`.

**Narrowed location.** Diagnostic (temporary `ip` encoding in
`__psizam_trap`'s `trap_code` for `ir_op::unreachable`, reverted) shows
the firing site is `ip=4` in func 2 — the very first `unreachable`
under `(global.get 5; i32.eqz; if unreachable end)` at the top of
the function. That unreachable only fires when global 5 (the runaway
counter, initial 100) hits 0. Counter decrements only on every
recursive entry into func 2 via `return_call 2`, which is itself
guarded by `if (i64x2.bitmask != 0)` on a v128 produced by this
chain (func 2, right after the decrement):

```
i64.const 2123283510833915851
f64.convert_i64_s                 ; f64 stays on stack underneath
v128.const i32x4 0x839cbe5d 0x6c5be233 0x938d73bd 0x7f2ee05e
i32x4.trunc_sat_f32x4_s
i16x8.abs
f64x2.neg
f32x4.demote_f64x2_zero
i8x16.neg
f32x4.floor
f64x2.convert_low_i32x4_s
i64x2.bitmask                     ; → i32 feeding `if`
```

So interp's chain must produce `bitmask == 0` (→ fall past the
recursive tail call and return normally), while jit_llvm's produces
a non-zero result (→ enter the recursive tail call, decrement the
counter on each re-entry, and eventually trap when it hits 0). The
outcome `unreachable` is downstream — the real bug is in one of the
SIMD ops above.

**Likely suspects.** `i32x4.trunc_sat_f32x4_s` on a v128 whose f32
lanes are the raw bytes of the v128.const (likely NaN / out-of-range
for i32), and `f64x2.neg` / `f32x4.demote_f64x2_zero` /
`f64x2.convert_low_i32x4_s` on softfloat. LLVM's native codegen for
these intrinsics may use hardware semantics even under softfloat mode;
interp runs everything through the softfloat helpers.

**Fix sketch.** Build three micro-modules that each run the chain up
to one specific op and return the v128 via an exported global, and
diff interp vs. jit_llvm to find the first divergent op. Then inspect
`llvm_ir_translator.cpp`'s emission for that op. Don't start with
wasm-reduce — binaryen's `wasm-opt` can't round-trip the module
(validator rejects an `f32`/`i32` type mismatch in an xor inside a
block that psizam's parser accepts).

### ~~jit2 + jit_llvm: uncaught-exception trap classified as memory_trap~~ — fixed

**Status:** fixed — `signals.hpp` signal handler now reads the faulting PC and only classifies as `memory_trap` when the PC is inside the JIT code range. If the PC is outside the code range (a corrupted return address landing on the stack, a helper-function fault, or any LLVM-JIT PC — which isn't registered in `code_memory_range`), the handler sets `saved_exception = wasm_interpreter_exception{"jit control-flow corruption or stack overflow"}` and longjmps with `-1` so it surfaces as `interp_trap` instead of going through `handle_signal`'s `SIGSEGV → wasm_memory_exception` mapping.

**Reproducer.** `mismatch_140231_seed4242424.wasm` (4176 B). Now all backends agree on `interp_trap`.

**Shape.** Module is dense `try_table` + `throw 0` + `return_call`/`return_call_indirect` — a WASM `throw` propagates through several tail-call frames with no matching catch, so it's legitimately uncaught. The jit2 fault signature was `LR=PC=fault_addr=stack_address` (RET through a stomped LR slot); the jit_llvm fault was `LDR X8,[X8,#8]` with `X8=0` inside its personality/unwind helper. Both now classify cleanly as interp_trap.

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

### jit2 ctx pointer corrupted after throw+catch into helper calls

**Status:** mostly fixed by commit `371db25` (see below). One
reproducer remains open with a different-mechanism failure.

**Reproducers.**
- `mismatch_308_seed100.wasm` — **fixed**.
- `mismatch_6998_seed1776497367.wasm` — **fixed**.
- `mismatch_9677_seed1776497367.wasm` — **still open**, now crashes
  inside `__longjmp` instead of returning `memory_trap`. Orthogonal
  stack-inversion bug; see below.

All three historically: interpreter, jit1, and jit_llvm emit
`interp_trap (unreachable)` cleanly; jit2 returned `memory_trap`.

**Root cause (308/6998).** `eh_setjmp`'s `and $-16, %rsp` before the
setjmp call was not sufficient to keep the `call`'s pushed return
address out of the 3-slot `eh_save_slots` region. When body regalloc
raised rsp above the post-prologue floor (nested `try_table` patterns
in these modules did this), the aligned rsp could land exactly on the
ctx-save slot, and the call's push wrote the return address onto the
mem-save slot. On longjmp, rsi was restored from that corrupted
slot — rsi then pointed into JIT code, and the next `[rsi-0x1008]`
global access segfaulted. Signal handler classified as memory_trap
while the interpreter cleanly reported interp_trap.

**Fix (371db25).** Replace `and $-16, %rsp` with
`lea -abs_off(%rbp), %rsp` where `abs_off = |eh_rsp_save_offset()| +
16`, rounded up to 16-byte alignment. This pins rsp to a known-safe
16-byte-aligned slot strictly below all three save slots regardless
of where body regalloc left it, so the call's ret-push can never
land on a save slot.

**Workaround already landed.** `drop_elem` now throws
`wasm_interpreter_exception("elem segment index out of range")`
instead of asserting (commit `7565fa1`), so a corrupted ctx doesn't
auto-silence the bug in release builds — but this still reports a
memory trap outcome.

### ~~jit2 dest-jmpbuf clobbered under deep `try_table` (9677)~~ — fixed

**Status:** fixed — resolved by the 0-catch try_table fix (commit
`b2ee2f7`). Full fuzzer pass on seed 1776497367 (10K modules) now
shows 0 mismatches, 0 crashes. The module's deep `try_table` nesting
included 0-catch try_tables whose unmatched `eh_leave` corrupted the
EH stack, which cascaded into jmpbuf corruption. With 0-catch
try_tables no longer marked as `is_try_table`, the EH stack stays
consistent and the jmpbuf is never overwritten.

Previous analysis (kept for context):

**Reproducer.** `mismatch_9677_seed1776497367.wasm` (939 B).
interpreter/jit/jit_llvm report `interp_trap (unreachable)` cleanly;
jit2 previously SEGVed inside `__longjmp` at `jmp *%rdx` with
demangled PC pointing into random memory.

**Symptom was.** `signal_throw(wasm_interpreter_exception{"unreachable"})`
did `longjmp(*trap_jmp_ptr, -1)` where `trap_jmp_ptr` pointed to
`dest` in `invoke_with_signal_handler`'s frame. At the point of the
longjmp, memory at `dest`'s address no longer contained the mangled
register snapshot that `setjmp` wrote — it contained heap-pointer
data (looks like vector or `jit_eh_frame` contents). `__longjmp`
demangled PC from the corrupted slot and jumped to garbage.

**Previous hypothesis.** `stack_allocator` only allocates a separate native
stack when `min_size > 4MB`. For small functions it returns
`top() == nullptr`, and the JIT runs on the same native stack as
`invoke_with_signal_handler`. The JIT's local body (via spill slots,
catch_data reservations, etc.) pushes rsp past `dest`, and a write
through the corrupted rsp overwrites the jmp_buf contents.

**Fix sketch.** Either (a) always allocate a separate alt_stack so
the JIT can never touch the native stack frames that hold
invoke_with_signal_handler's dest, or (b) change
invoke_with_signal_handler to allocate `dest` in a TLS-backed heap
slot rather than on the stack, so JIT stack usage cannot reach it.
(a) is simpler but regresses startup cost for hot-path small
functions; (b) is intrusive but structurally sound.

### interpreter=ok vs all-JITs trap (`unreachable`)

**Status:** open — 1 reproducer.

`/tmp/fuzz/mismatch_17201_seed1776497367.wasm` (1096 B): interpreter
returns `ok`; jit, jit2, and jit_llvm all report
`interp_trap (unreachable)`. Since the three JITs AGREE and only the
interpreter disagrees, this is most likely an interpreter bug (or a
wasm-gen bug producing a module that falls into UB by spec). Not
critical — matching behaviour across JITs means no
backend-consistency issue. Worth tracing if someone is already in
`interpret_visitor` for something else.

### interp/jit/jit2 timeout vs jit_llvm trap

**Status:** open — 2 reproducers.

`/tmp/fuzz/mismatch_18536_seed1776497367.wasm`,
`/tmp/fuzz/mismatch_2190_seed1776492783.wasm`: interpreter, jit1, and
jit2 all time out on the fuzz watchdog; jit_llvm reports
`interp_trap (unreachable)`. jit_llvm's constant folding likely
resolves the loop-exit condition and hits `unreachable` directly,
while the others run the counter out. Not a correctness divergence
per se (all eventually would trap), but the fuzzer flags it.

### ~~select validation: mismatched numeric types~~ — fixed

**Status:** fixed (side effect of the `memory.wast` regen via the new
helper script). A fresh regen of `select.json` produced correctly-numbered
`.wasm` files and the checked-in `select_tests.cpp` now matches them.

### unit_tests heap corruption running full suite

**Status:** pre-existing, not an engine bug.

`./bin/unit_tests` (running every test) aborts near the end with
`corrupted double-linked list` / `corrupted size vs. prev_size` /
`malloc(): unaligned tcache chunk detected`. Individual test-case
groups all pass clean when run in isolation. Catch2 v2.7.2 (the
version shipped under `external/Catch2/`) has known fixture-cleanup
ordering issues; the crash reproduces independent of the JIT code
path. Upgrading Catch2 is the likely fix.

### ~~~150 spec_test "wasm file not found" failures~~ — fixed

**Status:** fixed by `tests/spec_test_helpers.sh` (the wast2json step
now emits the `.json` under CWD with a bare basename, then moves it
to its final location). Commit `47eee1c` had silently regressed this
by switching to an absolute `-o <path>` — wast2json emits its `.wasm`
side-effects next to the `-o` argument, so the `.wasm` files had been
landing in `build/Debug/libraries/psizam/tests/spec/` instead of the
`build/Debug/wasms/` directory the tests read from. With the helper
fixed, the ~150 "file not found" runtime failures disappear.

### ~~~150 spec_test multi-module import failures~~ — no longer reproduces

**Status:** closed — the newer spec testsuite (`51279a9`) wraps the
old multi-module cases in `assert_unlinkable` / `register`, which
the generator cleanly skips (no standalone-harness invocation). If
multi-module linking is implemented later, the tests will need to
be regenerated against a testsuite that exercises the feature
directly.
