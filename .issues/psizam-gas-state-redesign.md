---
id: psizam-gas-state-redesign
title: psizam gas metering — gas_state redesign (consumed + deadline, per-mode codegen)
status: ready
priority: high
area: psizam
agent: ~
branch: ~
created: 2026-04-20
depends_on: [psizam-gas-metering]
blocks: [psizam-gas-interrupt-handler, psizam-gas-yield-handler, psizam-gas-timeout-handler]
---

## Problem

Two `gas_handler_t` aliases now live in `namespace psizam`:

- `gas.hpp:35` — `void(*)(void* ctx)` — old counter-decrement model, wired to every backend.
- `runtime.hpp:81` — `void(*)(gas_state* gas, void* user_data)` — new API surface, scaffolded but not connected to any charge site.

Any TU that includes both (the next step of wiring `runtime.cpp` into execution
context will force this) is a redefinition error. We need to pick one — and
an honest audit of the two models shows the new `gas_state` design is
structurally better, not just different, and covers scenarios the old one
handles awkwardly (accurate post-run `consumed`, external interrupt without
atomic RMW on the hot path, hard-cap + slice yielding, external threads
extending/shortening the deadline at will).

## Requirements

The design must cover:

1. **No metering** — zero overhead: no injection, no loads, no branches.
2. **Accurate `consumed`** — monotonic, observable post-run, unaffected by
   yield/restock accounting.
3. **Settable deadline** — the threshold at which the handler fires.
4. **External mutation of deadline** — any other thread may write any u64
   value: `0` (trap ASAP), `consumed+N` (extend), or any target — without
   forcing atomic RMW on the owning-thread hot path.
5. **Handler callback on deadline reached** — customizable policy: trap,
   yield-and-restock, wall-clock timeout, or combination.
6. **Best achievable hot-path cost** — matches or beats the old
   atomic-decrement model per charge site.
7. **Per-module counters, ported across module boundaries** — no mandatory
   `shared_ptr` on the hot path. Cross-module billing (when needed) is a
   runtime concern, not a per-charge concern. (Mechanism: TBD — not in
   scope for this issue.)
8. **Type-safe units** — gas amounts use `ucc::typed_int<uint64_t, tag>` at
   the API boundary so they can't be silently mixed with byte counts or
   other u64s.

Explicitly NOT required:

- `consumed` observed from another thread. Only the owning thread reads or
  writes `consumed`; it stays as plain `uint64_t`.
- Shared atomic billing across concurrent threads in one instance. WASM
  threads aren't in scope for psizam today.

## Design

### Type layout

```cpp
// libraries/psizam/include/psizam/gas.hpp
struct gas_tag {};
using gas_units = ucc::typed_int<uint64_t, gas_tag>;  // costs, consumed, deadline

struct gas_state {
   uint64_t              consumed  = 0;                        // plain — owner only
   std::atomic<uint64_t> deadline  = UINT64_MAX;               // any thread may store
   gas_handler_t         handler   = nullptr;                  // null = no checks
   void*                 user_data = nullptr;                  // handler closure

   // Typed-int API at the boundary; raw storage for codegen + atomic alignment.
   gas_units get_consumed()  const noexcept;
   gas_units get_deadline()  const noexcept;
   void      set_deadline(gas_units v) noexcept;
};

using gas_handler_t = void (*)(gas_state* gas, void* user_data);
```

Storage notes:
- `consumed` is plain `uint64_t` — written only by the owning execution
  thread, observed by handlers and the runtime at sync points.
- `deadline` is `atomic<uint64_t>` **always** — storage overhead is zero;
  modes that don't need external writes simply never issue cross-thread
  writes, and codegen reads it as a plain load (relaxed → plain `mov`/`ldr`
  on x86_64/aarch64 when the slot is naturally aligned).
- `UINT64_MAX` default deadline + null handler ⇒ metering inert; combined
  with `gas_insertion_strategy::off` the injector emits no check at all.

### Hot-path encoding — by mode and charge-site class

Charge sites come in two classes, already distinguished by the parser's
injector (`prologue_gas_extra` vs `loop_gas_extra`):

- **Forward sites** — function prologue, `br_if`/`br_table` forward branches.
  Bounded runtime by construction (functions terminate, call stack is
  bounded).
- **Back-edge sites** — loop header. These are the only sites that can
  execute unboundedly, so they are the only sites that need to observe
  external deadline updates in interrupt mode.

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
  — 2 instructions, 0 memory ops. Matches the old atomic-counter model's
  hot-path cost without the memory RMW.
- **Memory-deadline refresh** (back-edge in interrupt mode):
  `mov rTmp,[deadline]; sub rTmp,rConsumed; mov rDtd,rTmp; sub rDtd,cost; js slow`
  — 4 insns, 1 L1-hot memory load. Only emitted at back-edges in interrupt
  mode; forward sites still use the cheap DTD form.

### Interrupt latency

External thread writes `deadline.store(target, relaxed)`. Owning thread
observes it on its next **back-edge charge site**, which is the next loop
iteration (or the next function call if the loop is itself inside another
loop). Worst-case latency = one forward-only chain ≈ µs scale. Matches or
beats the old model (the old model's `cmp/js` after a fetch_sub has exactly
the same observation window).

### `consumed` accuracy

Reconstructed as `deadline_at_last_sync − rDtd` at any sync point (handler
entry, function epilogue, loop header in interrupt mode). `consumed` in
memory is refreshed at those points; between syncs, `rDtd` is the live
value and `consumed` in memory is stale-low by at most the current
function's partial charge. Billing queries performed while the module is
suspended (handler entry, yield, return) see the exact monotonic total.

### Handler contract

- Called when `consumed >= deadline` at a charge site.
- Receives `gas_state*` (can read/write `deadline`, read reconstructed
  `consumed`) and the user-data pointer (handler closure: slice size,
  hard cap, wall-clock deadline, etc.).
- Returns to resume execution with whatever deadline it has set. Throws to
  trap.
- Null handler ⇒ throw `wasm_gas_exhausted_exception` (matches today's
  default).

Canonical handler shapes, all expressible on top of this contract:

- **trap** — null (or handler that throws).
- **yield with slice** — `user_data = {slice}`; handler sets
  `deadline = consumed + slice`; returns.
- **yield with slice + hard cap** — `user_data = {slice, hard_cap}`;
  handler throws if `consumed >= hard_cap`, else sets
  `deadline = min(consumed + slice, hard_cap)`; returns.
- **wall-clock timeout** — `user_data = {wall_deadline, slice}`; handler
  throws if `now() >= wall_deadline`, else advances `deadline` by slice.
- **wall-clock + gas cap** — combine the above.

## Research — prior art

Before finalizing, audit how competitor runtimes split metering and
interruption:

- **Wasmer (wasmer-middlewares::metering)** — global counter, injected by
  compiler pass. Check whether the counter is atomic or plain, whether
  external cancellation is supported, and what the per-charge hot-path
  shape is. Our hypothesis: plain counter, no external interrupt path.
- **Wasmtime — `fuel` vs `epoch_interruption`** — fuel is the metering
  counter (per-store, single-threaded observation), epoch is a separate
  atomic u64 bumped by another thread for async cancellation. Wasmtime
  keeps the two concerns split. Our design unifies them into a single
  `deadline`.
- **WAMR** — check whether its gas limit is atomic or plain.
- **Near-vm / wasmer-derived chains** — compiler-pass-injected counters,
  consensus-safe. Check whether they support external interrupt at all or
  only trap-on-exhaust.
- **V8 / SpiderMonkey** — no gas, but they do have interrupt mechanisms
  (stack-guard polls). Reference design only.

Hypothesis to validate: runtimes that only support trap-on-exhaust use
plain (non-atomic) counters, and runtimes that need external interrupt use
a separate atomic deadline/epoch — nobody unifies them into one variable
parameterized by mode. If that's true, our design is strictly more flexible
than any single competitor's and the "interrupt mode = atomic deadline, no
interrupt = plain" split is a new contribution.

Deliverable: one-page "prior art" section in `gas-metering-design.md`
with citations and encoding diagrams.

## Public API surface (landed early in `gas.hpp`)

The capability bitflags and helpers are available now so the parallel
runtime-API work can build against them before the gas_state refactor
itself lands:

- `enum class meter_cap : uint16_t` — orthogonal feature bits
  (`gas_budget`, `wall_budget`, `yield`, `interrupt`, `pool`).
- Bitwise ops: `| & ^ ~`, compound assigns, `has()`, `any()`.
- `is_valid(caps)` — rejects internally-inconsistent combos
  (`yield`/`interrupt` without any budget, `pool` without `gas_budget`).
- `missing(have, want)` / `compatible(have, want)` — runtime ⊆ compile
  check for `runtime::instantiate(tmpl, policy)`.
- `unused(have, want)` — diagnostic for overcompiled modules.
- `meter_presets::{off,trap,yield,timeout,trap_interrupt,yield_interrupt,`
  `timeout_interrupt,both_poll,both_poll_pooled}` — sugar over bitmasks.
- `enum class codegen_shape { none, dtd_only, dtd_with_refresh }`
  + `shape_for(caps)` — collapses the cap set to one of three JIT
  emission shapes. Backends dispatch on this, not on the bit set.

These can be consumed today by `compile_policy` / `instance_policy` in
the parallel runtime-API work. The gas_state layout, handler rewrite,
and codegen changes (Phases B–E) still follow.

## Implementation plan

Phase A — spec and scaffolding (reviewable in isolation):
1. Extend `docs/gas-metering-design.md` with the gas_state section,
   encoding table, latency analysis, and prior-art section.
2. Land research findings into the doc.
3. Decide (in a follow-up comment) whether Phase B proceeds as "universal
   memory-deadline first, DTD later" or "dual-shape up-front".

Phase B — core types and interpreter:
4. Define `gas_state` + `gas_units` in `gas.hpp`; remove the duplicate
   `gas_handler_t` from `runtime.hpp`.
5. Replace `_gas_counter` / `_gas_handler` / `_gas_strategy` on
   `execution_context_base` with `gas_state`.
6. Rewrite `__psizam_gas_charge` / `__psizam_gas_exhausted_check` in
   `runtime_helpers.cpp`.
7. Port interpreter charge sites (trivial — no inlined codegen).
8. Port `gas_metering_tests.cpp` to the new signature. Build + test green.

Phase C — JIT codegens, one at a time, test green after each:
9. jit (x86_64 + aarch64).
10. jit2 (x86_64).
11. jit_llvm (IR annotation pass).
12. jit_profile.

Phase D — per-mode codegen (optional, post-correctness):
13. Implement DTD register-pinning for non-interrupt modes in each backend.
14. Implement back-edge deadline refresh for interrupt mode.
15. Benchmark: trap-mode hot path insn count and mem ops vs today.

Phase E — handler wiring:
16. Rewrite the trap / yield / timeout handler issues on top of gas_state.
    Likely collapses 3 issues into 1–2 short tests.

## Acceptance Criteria

- [ ] `docs/gas-metering-design.md` updated with the gas_state section,
      encoding table, latency analysis, and prior-art findings.
- [ ] Only one `gas_handler_t` definition in the codebase (in `gas.hpp`).
- [ ] `gas_state` drives every charge site; no reference to
      `_gas_counter` / `gas_atomic` remains.
- [ ] `ucc::typed_int` used for public gas-unit types.
- [ ] All five backends (interpreter, jit, jit2, jit_llvm, null) exercise
      the new path in `gas_metering_tests.cpp`.
- [ ] `gas: heavy-op prologue-extra cross-backend determinism` and
      `gas: heavy-op loop-extra per-backend` still pass under the new model.
- [ ] External-interrupt test: watcher thread stores a smaller `deadline`
      mid-execution; running backend traps within one loop iteration on
      modes that declare `interrupt=true`.
- [ ] No regression in unit or spec test counts.

## DX notes for the runtime API

A parallel DX draft for the runtime API (`runtime rt{...}` /
`compile_policy` / `gas_policy` / gas pools) was reviewed against this
design. It composes cleanly — the gas_state layout, handler contract,
DTD vs memory-deadline encoding, and typed_int boundary all fit without
change — with two items to reconcile on the runtime side:

1. **Compile-time vs runtime `poll_slice`.** The DX draft has a
   `poll_slice` field on both `compile_policy.metering` and
   `gas_policy::metered_and_timed{...}`. They are semantically different:
   the compile-time one sets injector check-site spacing (how dense the
   emitted checks are); the runtime one sets handler poll/yield cadence
   (how often the handler advances the deadline). Recommend renaming the
   compile-time field (e.g. `check_spacing`) so reviewers don't assume
   they must match, and documenting the invariant that
   runtime `poll_slice >= compile-time `check_spacing` (can't poll more
   often than checks exist).

2. **Consistency between `compile_policy.metering` and `gas_policy`.**
   The mode chosen at compile time determines which codegen shape the
   injector emits (from the per-mode matrix above). A module compiled
   with a narrower shape than the instance policy asks for won't work —
   e.g. `meter_shape::trap_only` at compile + `gas_policy::metered_and_timed`
   at instantiate leaves no wall-clock poll sites. Two acceptable
   resolutions: (a) pick the superset shape at compile time and let
   instances opt down via runtime config; or (b) validate compatibility
   on `runtime::instantiate(tmpl, policy)` and reject the mismatch with
   a clear diagnostic. Prefer (a) for the default path and (b) as a
   safety check. This belongs in the runtime-API design doc, not the
   gas docs.

3. **Gas pool.** The DX's `rt.create_gas_pool({.budget = N_gas})` concept
   is additive and compatible: the pool is a runtime-level ledger above
   per-instance `gas_state`. An instance is created with a lease from
   the pool; the handler debits more from the pool on yield; unused gas
   is credited back at instance teardown. The pool's own counter can
   be atomic (only touched on yield boundaries, never per-charge), so
   the instance hot path stays plain-consumed + atomic-deadline. No
   `shared_ptr<gas_state>` on the hot path — the pool is consulted only
   at handler entry.

## Notes

- Depends on `psizam-gas-metering` (umbrella) having landed Phases 1–4 —
  it has.
- Supersedes the atomic-counter-based mechanism described in
  `psizam-gas-interrupt-handler.md`; that issue's acceptance tests still
  apply but against the new `deadline`-store mechanism instead of
  `counter.store(-1)`.
- Cross-module gas portability (shared billing / per-module counter
  hand-off at module boundaries) is intentionally out of scope here.
  It's a runtime-level concern, to be sorted when `runtime.cpp`
  actually instantiates multi-module call chains.
- Typed-int suffixes (`_gas`, `_pages`, `_ms`, `_sbytes`) in the DX draft
  map to `ucc::typed_int` user-defined-literal operators. Consistent
  with the `gas_units` type chosen here; other units get their own
  tags.
