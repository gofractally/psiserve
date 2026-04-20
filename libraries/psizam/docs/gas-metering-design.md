# Gas Metering + Interrupt + Yield Design

Status: design (2026-04-20). Phases 1–4 landed. Phase 5 (gas_state
redesign — see §"gas_state redesign" at the end of this document)
replaces the atomic-counter model described in §"Injected WASM sequence"
and §"Gas handler (policy callback)" with a `consumed`/`deadline` pair.
The earlier sections are preserved for historical accuracy; the
gas_state section is authoritative for the current codebase.

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

## gas_state redesign

The earlier sections describe the atomic-counter model
(`_gas_counter : std::atomic<int64_t>`, `fetch_sub` per charge,
`store(-1)` for external interrupt). That model has four honest
weaknesses:

1. `fetch_sub` is an atomic RMW on the hot path, paid every charge
   site. On aarch64 this is a `ldaxr/stlxr` loop in the worst case;
   on x86_64 it's a `lock`-prefixed instruction.
2. "How much did the run consume?" requires a separate budget tracker
   outside the counter — the counter is a *remaining* value, not a
   monotonic total.
3. Handler contract couples three independent concerns (trap policy,
   yield slice size, wall-clock deadline) through one opaque
   `void* ctx`, forcing each handler to recover its state through the
   context instead of having a dedicated closure.
4. External interrupt and metering share one atomic u64 via a magic
   `-1` sentinel — extending the deadline and shortening the deadline
   use different operations (an `fetch_add` and a `store`), and the
   sentinel collides with any legitimate negative value a yield-style
   handler might restock with.

The gas_state redesign splits the concerns across two fields, both
living in one `gas_state` struct.

### Type layout

```cpp
// libraries/psizam/include/psizam/gas.hpp
struct gas_tag {};
using gas_units = ucc::typed_int<uint64_t, gas_tag>;  // costs, consumed, deadline

struct gas_state {
   uint64_t              consumed  = 0;                                    // plain — owner-thread only
   std::atomic<uint64_t> deadline  = std::numeric_limits<uint64_t>::max(); // any thread may store
   gas_handler_t         handler   = nullptr;                              // null = throw on exhaustion
   void*                 user_data = nullptr;                              // handler closure
};

using gas_handler_t = void (*)(gas_state* gas, void* user_data);
```

`consumed` is plain `uint64_t` (written only by the owning execution
thread). `deadline` is `std::atomic<uint64_t>` — any thread may store
any u64 value (0 to trap ASAP, `consumed + slice` to extend, or any
target). Storage is zero-overhead: relaxed atomic loads on x86_64 and
aarch64 compile to plain `mov`/`ldr` when the slot is naturally aligned,
which it is.

Raw `uint64_t` inside `gas_state` (rather than `gas_units`) is
load-bearing: under `PSITRI_PLATFORM_OPTIMIZATIONS` the typed_int is
packed (alignment 1) and can't be inside `std::atomic<>`. `gas_units`
appears at the public API boundary; the raw storage is what the codegen
and atomic stores target.

### Hot-path encoding — by mode and charge-site class

Charge sites split into two classes, distinguished at parse time by
the injector:

- **Forward sites** — function prologue, `br_if`/`br_table` forward
  branches. Bounded runtime by construction (functions terminate, call
  stack is bounded).
- **Back-edge sites** — loop header. The only sites that can execute
  unboundedly, so the only ones that need to observe external
  deadline updates in interrupt mode.

| Mode                           | Forward-site check                | Back-edge-site check                           |
|--------------------------------|-----------------------------------|------------------------------------------------|
| `off`                          | no injection                      | no injection                                   |
| `trap` (no interrupt)          | DTD: `sub rDtd,cost; js slow`     | Same (DTD)                                     |
| `yield` (slice, no interrupt)  | DTD                               | Same (DTD; handler refreshes rDtd on resume)   |
| `timeout` (wall-clock)         | DTD                               | Same (DTD; handler checks clock on slow path)  |
| `trap + interrupt`             | DTD (stale deadline OK)           | Memory-deadline: re-read `[deadline]`, refresh rDtd, branch |
| `yield + interrupt`            | DTD                               | Same as trap+interrupt                         |

Where:
- **DTD** = "deadline-to-death", a pinned register holding
  `deadline − consumed`. Per charge site: `sub rDtd, cost; js exhausted`
  — 2 instructions, 0 memory ops. Matches the old atomic-counter
  model's hot-path cost without the memory RMW. Not yet emitted; the
  current landed codegen uses the universal memory-deadline shape
  below at every site (Phase 5a).
- **Memory-deadline refresh** (landed — universal shape in Phase 5a):
  `mov rTmp,[consumed]; add rTmp,cost; mov [consumed],rTmp; cmp [deadline],rTmp; jb done`
  — 5 insns, 1 load, 1 store, 1 cmp-with-mem. Emitted at every charge
  site in the current code. Phase 5b will split forward vs back-edge
  emission per the table above.

### Interrupt latency

External thread writes `deadline.store(target, relaxed)`. The owning
thread observes it on its next back-edge charge site (universal shape:
every charge site), which in the worst case is the next loop iteration
or function call. Worst-case latency = one forward-only chain on the
order of microseconds — the `gas: cross-thread interrupt via watcher
thread` test measures this and asserts the trap happens within one
second of the watcher's store, which on any real hardware is at least
a thousand times slower than the real latency.

### `consumed` accuracy

`consumed` is monotonic from instance creation. After the run (at
function return, handler entry, or a quiescent point between calls)
a billing query returns the exact total charged. Between charge sites
`consumed` lags by at most the cost that the current site is about to
add — and the current site's own addition is always observed before the
next charge.

### Handler contract

- Called when `consumed >= deadline` at a charge site.
- Receives `gas_state*` (can read `consumed`, can store into
  `deadline`) and `user_data` (handler closure: slice size, hard cap,
  wall-clock deadline, pool pointer, etc.).
- Returning resumes execution with whatever deadline it has set.
- Throwing traps the module. JIT-emitted charge sites call
  `__psizam_gas_exhausted_check` which routes throws through the
  backend's setjmp trampoline (`escape_or_throw`) so unwinding is
  correct across jit2 / jit_llvm frames.
- Null handler ⇒ throws `wasm_gas_exhausted_exception` (matches the
  prior default).

Canonical handler shapes, all expressible on top of this contract:

- **trap** — null handler.
- **yield with slice** — `user_data = {slice}`; handler stores
  `consumed + slice` into `deadline`; returns.
- **yield with slice + hard cap** — `user_data = {slice, hard_cap}`;
  handler throws if `consumed >= hard_cap`, else stores
  `min(consumed + slice, hard_cap)` into `deadline`; returns.
- **wall-clock timeout** — `user_data = {wall_deadline, slice}`;
  handler throws if `now() >= wall_deadline`, else advances `deadline`
  by slice.
- **wall-clock + gas cap** — combine the above.

### Capability bits and codegen shape

The full mode matrix collapses to three distinct JIT emission shapes.
`meter_cap` (in `gas.hpp`) is a bitmask of orthogonal feature bits
(`gas_budget`, `wall_budget`, `yield`, `interrupt`, `pool`) that
classify a metering configuration; `shape_for(caps)` reduces the cap
set to one of:

| Shape              | When                                                  | Per-site cost |
|--------------------|-------------------------------------------------------|---------------|
| `none`             | No gas budget and no wall budget                      | zero          |
| `dtd_only`         | Budget present, no interrupt                          | 2 insns (sub imm, js slow) |
| `dtd_with_refresh` | Budget present + `interrupt` bit set                  | 2 insns forward; 4 insns + 1 L1-hot load back-edge |

Phase 5a lands the universal memory-deadline shape (unchanged per
site regardless of caps). Phase 5b adds the three-way split so
non-interrupt modes drop to `dtd_only`.

`compatible(have, want)` and `missing(have, want)` validate an
instance policy (`want`) against a compiled module (`have`) at
instantiation time, so an instance that asks for `interrupt` against
a module compiled without `meter_cap::interrupt` fails with a clear
diagnostic instead of silently running without the back-edge refresh.

### Prior art

Every major consensus / sandboxed WASM runtime meters, but no two use
the same split. The pattern worth naming: **runtimes that only trap on
exhaust use a plain counter; runtimes that need external cancellation
use a separate atomic epoch.** The gas_state redesign unifies both
concerns into one `deadline` variable, parameterized by mode.

- **Wasmer — wasmer-middlewares::metering.** Compiler-pass injection at
  basic-block boundaries (matches our `per_block`). The "points" field
  is a plain `u64` — no atomicity, no external-cancel path. A separate
  `InternalEvent::GetMiddlewareFail` mechanism exists but is not
  configured for cross-thread interrupt. Source:
  [wasmer-middlewares metering.rs](https://github.com/wasmerio/wasmer/blob/main/lib/middlewares/src/metering.rs).

- **Wasmtime — `fuel` vs `epoch_interruption`.** Two independent
  mechanisms in the same engine. `fuel` is the metering counter
  (`Store::get_fuel` / `Store::set_fuel`, per-store, single-thread
  read/write contract documented). `epoch_interruption` is a separate
  atomic `u64` bumped by another thread (`Engine::increment_epoch`) for
  async cancellation. Both are checked at loop back-edges and function
  entries; wiring is orthogonal. The two counters don't share state.
  Source: `wasmtime::Store` and `Engine::increment_epoch` in the
  Wasmtime API docs.

- **WAMR (Intel).** The `wasm_runtime_set_instruction_count_limit` API
  sets a plain per-instance counter with trap-on-exhaust semantics.
  Cross-thread cancellation is a separate `wasm_runtime_terminate`
  call that sets an atomic flag checked at loop back-edges —
  essentially the same split as Wasmtime, just with different names.

- **Near-vm (wasmer-derived).** Consensus-safe. Uses
  `pwasm-utils`-style gas injection at basic-block boundaries, plain
  counter, trap-only. No external interrupt path — the chain's
  deterministic replay contract makes async cancellation actively
  unsafe.

- **V8 / SpiderMonkey.** No gas metering. They do support thread-level
  interrupt via stack-guard polls (`Isolate::RequestInterrupt` in V8,
  `JS_RequestInterruptCallback` in SpiderMonkey) — reference design
  only, not a metering model.

Hypothesis validated: runtimes that support only trap-on-exhaust use
plain counters; runtimes that support external interrupt add a
separate atomic variable for it. Nobody unifies them. The gas_state
redesign's unification gives us three things the split-variable
approach can't:

1. The `consumed >= deadline` check is one comparison, not two
   (skipping "is interrupt pending AND is gas exhausted").
2. Yield-style handlers advance the *same* variable that external
   interrupts shorten — so a yield handler that just raced an
   external cancel will observe the cancel on its next charge site
   with no extra state to track.
3. Policies that combine gas + wall-clock + interrupt collapse to a
   single deadline, computed as `min(gas_deadline, next_poll_point,
   current_deadline)`. The handler recomputes on entry.

### Implementation phases (Phase 5)

- **5a — landed (ae5d854, 07b132b).** `gas_state` struct replaces
  `_gas_counter`/`_gas_handler`/`_gas_strategy` on
  `execution_context_base`. New handler signature
  `(gas_state*, void*)`. `ucc::typed_int<uint64_t, gas_tag>` as the
  public `gas_units`. Universal memory-deadline codegen emitted by
  every JIT backend (x86_64, aarch64, jit2 x86_64, jit_llvm).
  `__psizam_gas_charge` / `__psizam_gas_exhausted_check` rewritten.
  `gas_metering_tests.cpp` ported; cross-backend determinism,
  heavy-op prologue-extra, heavy-op loop-extra, and a new
  `gas: cross-thread interrupt via watcher thread` test all pass
  across interpreter, jit, jit2, and jit_llvm.

- **5b — next.** DTD register-pinning for non-interrupt modes (drop
  `dtd_only` sites from 5 insns + mem ops to 2 insns, 0 mem ops).
  Split forward vs back-edge emission so `dtd_with_refresh` is paid
  only at back-edge sites in interrupt mode.

- **5c — deferred.** `meter_cap` enforcement at
  `runtime::instantiate(tmpl, policy)`: reject runtime caps not a
  subset of compile-time caps. Currently `meter_cap` is defined in
  `gas.hpp` but not consulted by the instance path. This belongs in
  the runtime-API work, not the engine.

- **5d — deferred.** Handler issues (`psizam-gas-yield-handler`,
  `psizam-gas-timeout-handler`, `psizam-gas-interrupt-handler`) port
  to the new `(gas_state*, void*)` signature. Mostly cosmetic now
  that the storage layer is settled — the tests in 5a already cover
  the behavior; what's left is a public example / reference handler
  in the docs and/or a small `psizam/handlers.hpp` with canonical
  implementations.

### Superseded sections

The following earlier parts of this document describe the
atomic-counter model and are kept for historical reference only:

- "Injected WASM sequence" (`i64.const <cost>; call $gas_check`) —
  superseded by the inline memory-deadline shape.
- "Gas handler (policy callback)" — the `void(*)(void*)` signature
  has been replaced by `void(*)(gas_state*, void*)`.
- "Atomic counter with relaxed ordering" (Key design decision #3) —
  the hot path is now a plain load/store of `consumed`; the atomic
  is on `deadline`, and only external threads ever write it.
- "External interrupt: `gas_counter().store(-1)`" — now
  `gas().deadline.store(0)` (or any value ≤ consumed).

