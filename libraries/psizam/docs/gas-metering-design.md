# Gas Metering + Interrupt + Yield Design

Status: design (2026-04-19). Phase 1 complete. Phase 2a in progress.

## Phase 2a first measurement (call-boundary metering, all backends)

Simplest possible Phase 2a: a `gas_charge(cost)` hook called at every
function entry — inlined as a method for the interpreter, as a host
call to `__psizam_gas_charge` for the JIT backends. Placeholder cost
of 1 per call; no CFG walk yet (that comes in Phase 2b).

Recursive call-heavy workload (depth 100, 50K top-level invocations,
≈5M total gas_charge calls), Linux x86_64 Release:

| Backend | off ns/call | on ns/call | Δ ns/call | Overhead |
|---|---|---|---|---|
| interpreter | 26.5 | 32.4 | +5.9 | +22% |
| jit1 | 3.6 | 6.8 | +3.1 | +86% |
| jit2 | 4.1 | 7.2 | +3.1 | +75% |
| jit_llvm | 8.4 | 9.3 | +0.9 | +11% |

Observations:

- **Gas metering works on every backend.** One helper, one prologue
  hook per backend, same atomic-relaxed semantics.
- **Interpreter pays ~6 ns per call.** Dominated by `lock xadd`.
- **jit1/jit2 pay ~3 ns per call.** Pure host-call + atomic, cleaner
  because the JIT doesn't have interpreter dispatch overhead baked in.
- **jit_llvm pays ~1 ns.** The ORC JIT linker inlines/optimizes
  around `__psizam_gas_charge` tighter than the hand-written JIT
  templates do.
- **High % numbers on jit1/jit2 reflect very fast baselines** (3–4
  ns per WASM function call). Absolute cost is small.

This is the cost *at call granularity*. Phase 2b's WASM-binary
rewriter will hit function entries + loop headers (not every call),
with real CFG-computed costs per function. Phase 3+ per-backend
peepholes can replace the host call with an inline atomic decrement
(e.g., `lock sub` on x86_64) and drop the overhead further.

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
| `prepay_max` | Function entry + loop header only | Prepay max-over-all-paths at entry; loop body cost at each header | Best JIT perf; fewest checks; overpays when control takes a short path |
| `per_block` | Every basic-block boundary (after `loop`, `if`, `else`, `br`, `br_if`, `br_table`, `end`, `call`) | Pay exact block cost per block | Exact accounting; matches Wasmer/Near; more checks |
| `hybrid` | Prepay per function up to a threshold; fall back to per-block for functions above it | Mix | Balance, for workloads with mixed function sizes |
| `off` | None | — | Unmetered baseline for perf comparison |

Default: `prepay_max`. All four are testable; the Phase 2 microbench
informs the final default choice.

### prepay_max cost calculation

- **Per-function cost**: max over all control-flow paths through the
  function, summing the weight of each basic block on the path. Loop
  bodies are excluded from this sum (they're paid per iteration at
  the loop header).
- **Per-loop cost**: weight of one iteration of the loop body.

CFG max-path is a standard topological walk: for each merge point
take `max` of incoming path costs. `if/else` and `br_table` contribute
`max(arm_costs...)`, not the sum.

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

### Phase 1 — foundation (plumbing only, no behavior change)

- `gas_handler_t` typedef in `psizam/gas.hpp`
- `wasm_gas_exhausted_exception` in `exceptions.hpp`
- `insertion_strategy` enum
- `_gas_counter` (`std::atomic<int64_t>`) and `_gas_handler` fields
  on `execution_context_base`, with setters/getters
- `gas_cost_table` compile-time constant
- Build + full test suite passes with zero behavioral change

### Phase 2 — WASM injection + host helper

- Load-time CFG analysis + injection pass (one strategy at a time)
- `$gas_check` internal host helper resolved by the module loader
- `instance_policy` struct threaded through Options with gas fields
- Correctness tests via `BACKEND_TEST_CASE` (runs on every backend)
- **Microbench harness: measure overhead vs. `off` baseline on every
  backend.** This is an acceptance criterion — we lock in the default
  insertion strategy based on this measurement.

### Phase 3+ — per-backend peepholes (pure perf work)

- Phase 3: interpreter (bitcode_writer peephole)
- Phase 4: jit1 x86_64 + aarch64
- Phase 5: jit2 IR
- Phase 6: jit_llvm

Each phase is independent, additive, and tested via the same
`BACKEND_TEST_CASE` suite from Phase 2.

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
- Microbench — `off` vs each strategy, per backend
