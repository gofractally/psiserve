# Gas Metering + Interrupt + Yield Design

Status: design (2026-04-19). Phases 1, 2a, and 2b complete. Phase 3 in progress.

## Design principles

1. **Approximation is cheaper than precision.** `prepay_max` overrun is
   bounded by one function's cost and fires at most once per trap (on
   the terminal path, by definition). `per_block` precision overhead is
   per basic block per iteration, unbounded with execution length. For
   any real workload the cost of measuring gas precisely exceeds the
   cost of approximating it. Approximation is not a compromise — it's
   the correct default.

2. **Compatibility is a runtime flag.** Selecting an insertion strategy
   is a load-time decision. Users who pick `prepay_max` pay zero for
   `per_block`'s existence (and vice versa). The strategies coexist
   without cross-contamination.

3. **Compatibility serves adoption and validation.** We ship
   `per_block` not because it's technically superior — it isn't — but
   because it earns two things our own model can't provide on its own:
   (a) a migration path for Wasmer/Near users (they get bit-compatible
   charge points, then discover `prepay_max` is faster still), and
   (b) a differential-testing harness: same module + same budget +
   matching opcode weights should trap at the same logical point across
   engines; any divergence is a correctness bug in one side.

4. **Faster than the competition at their own game.** Our `per_block`
   uses the Phase 3 inline `sub imm, counter; jns` peephole across
   every backend. Wasmer's middleware and Near's pwasm-utils both use
   host-call-per-check. We ship the same injection granularity at a
   fraction of the per-check cost. That's the hook.

## Phase 2b — per-function cost proxy (implemented)

Instead of the full CFG max-path computation originally planned, Phase 2b
ships a simpler proxy: per-function cost = WASM body byte count (code
section bytes after the locals header). Loop back-edges still charge 1
per iteration (that stays Phase 3's behavior).

Why the proxy over a full CFG walk:

- Body bytes are ~2-3 per opcode in WASM, so the cost scales with function
  size roughly the way a CFG sum-of-weights would, at a tiny fraction of
  the implementation cost.
- It's computed at parse time with no extra passes and no new data
  structures — just one uint32 snapshot in `function_body`.
- It preserves the design's determinism requirement across backends.

### Cross-backend determinism fix

`function_body::size` is NOT deterministic across backends:
`bitcode_writer::finalize()` overwrites it with a bitcode opcode count,
while native JIT finalizers leave the parser's WASM byte count in place.
Gas costs therefore use a dedicated `wasm_body_bytes` field populated in
the parser (right after locals are stripped) and never mutated. All five
backends (interpreter, jit1 x86_64 + aarch64, jit2, jit_llvm) read the
same integer and charge identically — the `gas: cross-backend
determinism` test asserts this.

### Charge-point differences between backends

The JIT prologues fire on every native function entry, including the
top-level `bkend.call("export", ...)` invocation. The interpreter's
`execute()` sets PC directly and does NOT route through
`context.call()`, so the interpreter's top-level call is not charged.
Both paths are charged for every *internal* WASM `call` opcode.

The cross-backend determinism test observes the counter value at the
first handler invocation and every backend sees the same number — both
backends perform the same per-charge cost, so their first charge (at
their own first charge point) decrements by the same amount. The
*logical program point* at which the trap fires differs by one frame
between JIT and interpreter; callers that care about frame-exact
alignment (e.g. trap-style handlers that must abort before the body
runs) need to pick one backend family or add a top-level charge to the
interpreter's `execute()`.

## Phase 2a measurements — call-boundary metering on all backends

Simplest possible Phase 2a: a `gas_charge(cost)` hook called at every
function entry — inlined as a method for the interpreter, as a host
call to `__psizam_gas_charge` for the JIT backends. Placeholder cost
of 1 per call; no CFG walk yet (that comes in Phase 2b).

### Workload spectrum (Release, Linux x86_64)

| Workload | Call density | jit1 | jit2 | jit_llvm |
|---|---|---|---|---|
| recursive depth=100 × 50K iter | ~5M calls, 0 work/call | +85% | +77% | +11% |
| bench_fib(1M) × 1000 | tight inner loop, 1K calls | +0.01% | +0.07% | ≈noise |
| bench_sort(10K) × 10 | bubble sort, 10 calls | +0.04% | ≈noise | ≈noise |
| bench_matmul(10K) × 10 | 8×8 matmul, 10 calls | ≈noise | +0.02% | +0.05% |

### Interpretation

- **Gas metering is free for real compute-heavy WASM.** The +0.01–0.07%
  overheads on compute workloads are at or below measurement noise.
  A million-iter `bench_fib` inner loop fires gas_charge exactly once.
- **The +80% "overhead" on recursive is a worst-case stress number.**
  Each WASM call does literally nothing except decrement + recurse;
  the denominator is ~3 ns of call machinery, the numerator is ~3 ns
  of gas machinery. Not representative of any real workload.
- **Per-backend hooks work uniformly.** One helper, one prologue hook
  per backend (x86_64.hpp, aarch64.hpp, jit_codegen.hpp, and an LLVM
  IR call in llvm_ir_translator.cpp). Same atomic-relaxed semantics.

### When Phase 2a matters

Call-heavy WASM (e.g., a nested interpreter written in WASM; a tight
dispatch loop calling many small functions) will see measurable
overhead — in the 10–85% range depending on how small the called
functions are. Phase 2b's CFG-based cost computation and Phase 3+
per-backend peepholes can both reduce this further.

Everything else — cryptography, data processing, compression, JIT'd
scripts doing real work per call — sees overhead buried in noise.

## Opt-in at runtime (no compile flag)

Gas metering is always compiled in but inactive by default. The counter
starts at `INT64_MAX`, the handler is null, and the load-time injection
pass only runs when a caller selects an `insertion_strategy` other than
`off` in their options. Existing modules + existing callers see zero
behavioral change. The structural cost is 24 bytes per
`execution_context_base` and zero instructions on any WASM hot path.

## Overview

A unified mechanism that provides deterministic gas metering, external
interrupt, cooperative fiber yielding, and wall-clock timeout through a
single load-time WASM-rewriting pass. The rewriter injects a tiny
canonical sequence (`i64.const <cost>; call $gas_check`) at
policy-selected program points. The "out of gas" handler is a per-
instance C++ callback — the same injected code supports trap, yield,
timeout check, external interrupt, or unlimited execution depending on
which handler is installed at instantiation time.

## Key design decisions

1. **WASM-level injection, not per-backend instrumentation.** One pass
   rewrites the module; every backend (interpreter, jit1, jit2,
   jit_llvm, null_backend) gets gas metering correctness for free.
   Matches prior art: eWASM sentinel, Wasmer middleware, Near, and
   pwasm-utils all inject at the WASM level.

2. **Deterministic by construction.** Gas cost is computed at load time
   from a fixed per-opcode weight table (Near-style, mostly 1's).
   Every backend sees the same injected constants and traps at the
   same logical point. The cost does NOT depend on native instruction
   count, JIT optimization level, unrolling, or ISA.

3. **Atomic counter with relaxed ordering.** The gas counter is
   `std::atomic<int64_t>`. The owning fiber does atomic
   `fetch_sub`; external threads can `store(-1)` to trigger interrupt.
   On x86_64 this is free (aligned 64-bit mov is atomic at hardware).
   On aarch64 it's a few cycles more than plain loads.

4. **Policy-based insertion strategy.** The injector supports four
   strategies chosen at load time. We ship more than one so we can
   measure and pick defaults empirically.

5. **Host-call injection primitive, peephole-optimized per backend.**
   Phase 2 ships with an actual host function call per check —
   correctness-complete on every backend. Phase 3+ adds per-backend
   peephole passes that inline the call. Correctness never depends
   on the optimization.

## Insertion strategies

| Strategy | Where checks go | Cost model | Use case |
|---|---|---|---|
| `prepay_max` | Function entry + loop header only | Body-size proxy at entry; loop body cost at each header | Default. Fewest checks, approximation is cheap, overrun is bounded |
| `per_block` | Every basic-block boundary (after `loop`, `if`, `else`, `br`, `br_if`, `br_table`, `end`, `call`) | Pay straight-line block cost per block | Compatibility + differential testing against Wasmer/Near; opt-in |
| `off` | None | — | Unmetered baseline for perf comparison |

Default: `prepay_max`. `per_block` is opt-in — a caller who doesn't
select it pays nothing for its existence.

`hybrid` was considered (prepay under a size threshold, per-block
above) and dropped: the threshold adds a support-surface without
solving a real user problem. Either you want compat with Wasmer/Near
(pick `per_block`) or you don't (pick `prepay_max`). A mode that
silently switches between them is a debugging nightmare nobody asked
for.

### prepay_max cost calculation

- **Per-function cost**: WASM body byte count (`wasm_body_bytes`)
  snapshotted by the parser — a proxy for opcode count at ~2-3 bytes
  per opcode. Charged once at function entry. See Phase 2b above for
  the rationale for preferring the proxy over a full CFG max-path
  walk.
- **Per-loop cost**: 1 per back-edge iteration. Heavy opcodes
  (`i64.div_*`, `call_indirect`, `memory.grow`, bulk memory) charge
  additional dynamic costs inside their handlers (see Cost table).

### per_block cost calculation

- Each basic block's cost is the sum of opcode weights for its straight-
  line body. Check is emitted at the block's entry.

## Cost table

Borrow Near's `wasm_regular_op_cost` shape. Flat weight of 1 for most
opcodes, with a small table of exceptions:

| Opcode class | Weight |
|---|---|
| Regular arithmetic, locals, globals, comparisons, control flow | 1 |
| `nop`, `drop`, `block`, `loop`, `end`, `else` (structural) | 0 |
| `i64.div_*`, `i64.rem_*`, `f*.div` | ~10 |
| `memory.grow`, `table.grow` | ~100 + operand-scaled component (separate dynamic charge) |
| `call_indirect` | 5 |
| `memory.copy`, `memory.fill`, `table.copy`, `table.init` | operand-scaled (separate dynamic charge) |

Exact values finalized as part of Phase 2. The weight table is
compile-time constant; changes require recompiling the engine and
every module.

## Injected WASM sequence

Canonical 2-opcode injection:

```
i64.const <cost>
call $gas_check
```

`$gas_check` is a well-known imported function index that the backend
recognizes and resolves to an internal runtime helper. Its semantics:

```cpp
void gas_check(execution_context* ctx, int64_t cost) {
   int64_t prev = ctx->gas_counter().fetch_sub(cost, std::memory_order_relaxed);
   if (prev - cost < 0) {
      auto h = ctx->gas_handler();
      if (h) h(ctx);
      else   throw wasm_gas_exhausted_exception{"gas exhausted"};
   }
}
```

## Gas handler (policy callback)

```cpp
using gas_handler_t = void(*)(void* ctx);
```

Installed per-instance at instantiation. Invoked when the counter
transitions to negative. Common handlers:

- **Trap (default).** Throws `wasm_gas_exhausted_exception`.
- **Yield.** `ctx->restock_gas(slice)`; `fiber.yield()`. Cooperative
  scheduling falls out automatically. No async transforms; no special
  yield points.
- **Timeout check.** If wall-clock elapsed > deadline, trap. Else
  restock and continue. Worst-case latency to notice timeout = one
  slice (~1µs at typical slice sizes).
- **External interrupt.** Any thread calls
  `ctx->gas_counter().store(-1, std::memory_order_relaxed)`. The next
  check observes it and invokes the handler. No signal, no mprotect,
  no VMA tricks.
- **Unlimited.** `restock_gas(INT64_MAX)`. Effectively disables
  metering for trusted / replay modes.

## Interaction with optimization

**Zero interference.** The injected pattern is a single host call
between two stack-pushed constants. Every basic-block-internal
optimization the JIT does is unaffected — the injection points are
the same structural barriers the JIT already recognizes (function
entry, loop header). Cost constants are computed at load time from
WASM opcodes, not JIT output, so optimization level does not change
the gas charged.

## Per-backend cost (Phase 2, unoptimized host-call path)

| Backend | Overhead per check | Notes |
|---|---|---|
| Interpreter | ~5–15 ns (one host-call dispatch) | Phase 3 can collapse to a single bitcode opcode |
| jit1 (x86_64, aarch64) | ~20–50 ns (host-call boundary with register spills) | Phase 3 peephole → ~3–5 ns inline `lock sub` + branch |
| jit2 (x86_64) | Same as jit1 | Phase 3 folds the sequence to an IR node |
| jit_llvm | LLVM may already inline the import helper | Confirm via microbench |

## Per-backend peephole (Phase 3+)

Each backend adds a lightweight recognizer for the canonical pattern:

- **bitcode_writer** (interpreter): detects `i64.const; call $gas_check`
  during bitcode emission and replaces with a single synthetic
  `gas_check(imm)` bitcode op. Handler in `interpret_visitor` is a
  5-line switch case.
- **jit2 IR**: peephole in the IR pass, single `gas_check` IR node,
  codegen emits `sub [r12+gas_offset], imm ; js stub`.
- **jit1 (x86_64/aarch64)**: pre-pass marks the two-opcode pattern;
  emission routine emits the tight native sequence instead of the
  per-opcode templates.
- **jit_llvm**: either let LLVM inline the helper naturally, or
  custom-lower the intrinsic in `llvm_ir_translator`.

Peephole correctness: the output must be semantically identical to
the unoptimized host-call path (same atomic semantics, same trap
condition).

## Implementation plan

### Phase 1 — foundation (complete)

- `gas_handler_t` typedef in `psizam/gas.hpp`
- `wasm_gas_exhausted_exception` in `exceptions.hpp`
- `insertion_strategy` enum
- `_gas_counter` (`std::atomic<int64_t>`) and `_gas_handler` fields
  on `execution_context_base`, with setters/getters
- `gas_cost_table` compile-time constant
- Build + full test suite passes with zero behavioral change

### Phase 2a — call-boundary metering on all backends (complete)

Per-backend charge hook at every function entry. Placeholder cost of
1. All five backends (interpreter, jit1 x86_64 + aarch64, jit2,
jit_llvm) route through the same atomic-relaxed counter.

### Phase 2b — per-function cost proxy (complete)

Function entry charges `wasm_body_bytes` (parser snapshot). Loop
back-edges still charge 1 per iteration. Cross-backend determinism
test asserts all five backends trap at identical counter values.

### Phase 3 — inline peephole across all backends (in progress)

- Interpreter: bitcode_writer peephole → synthetic `gas_check(imm)` op
- jit1 x86_64 + aarch64: inline `sub imm, counter; jns done` in
  prologue and loop back-edge emission (imm8 fast path, imm32/reg-reg
  fallback for larger costs)
- jit2 IR: single `gas_check` IR node, codegen emits the inline form
- jit_llvm: confirm LLVM inlines naturally, else custom-lower

### Phase 4 — heavy-opcode dynamic charges

Dynamic charges inside opcode handlers for opcodes whose weight
varies by 10× or more from the flat baseline: `i64.div_*`/`rem_*`,
`f*.div`, `call_indirect`, `memory.grow`, `table.grow`,
`memory.copy`/`fill`, `table.copy`/`init`. No new injection points.
Fires under every insertion strategy.

### Phase 5 — handler variants

`yield` (fiber), `timeout` (wall-clock), `external_interrupt`
(cross-thread `store(-1)`). Each is a ~dozen lines on top of the
existing handler dispatch, tested per-backend.

### Phase 6 — `per_block` strategy (compatibility + validation)

Load-time injection at every basic-block boundary, matching
Wasmer/Near pwasm-utils granularity. Uses the Phase 3 inline
peephole, so per-check cost is ~3 cycles vs. their host-call. Opt-in
via `insertion_strategy::per_block`.

### Phase 7 — differential test harness

Shared-module/shared-budget trap-point agreement between our
`per_block` mode and a reference runner (Wasmer middleware, Near
pwasm-utils). Any divergence is a correctness bug somewhere.

Phases are additive and each is tested via `BACKEND_TEST_CASE` suites
on every backend.

## Testing

- Unit tests per insertion strategy, per backend
- `gas_handler_trap` — verify default trap on exhaustion
- `gas_handler_yield` — fiber-based test, N yields, verifies progress
- `gas_handler_timeout` — wall-clock detection within one slice
- `gas_external_interrupt` — another thread stores -1, next check traps
- `gas_determinism` — same module + same budget traps at same logical
  point across all backends (interp/jit1/jit2/jit_llvm)
- `gas_prepay_max` — verify function overpays when taking a shorter
  path; verify total matches max path when taking the longest path
- `gas_per_block_compat` — same module + matching opcode weights
  trap at the same logical point under our `per_block` and under a
  reference runner (Wasmer middleware, Near pwasm-utils)
- Microbench — `off` vs each strategy, per backend
