---
id: psizam-gas-yield-handler
title: psizam gas handler — yield variant (fiber cooperative scheduling)
status: ready
priority: high
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psizam-gas-metering]
blocks: []
---

## Description
Implement the yield-style gas handler. When `consumed >= deadline`,
the handler advances the deadline by a slice and yields the current
fiber; the scheduler picks the next fiber. Cooperative scheduling
falls out automatically — no async transforms, no explicit yield
points in WASM, any function becomes yieldable.

This is one of the three user-visible capabilities gas metering unlocks
(with `timeout` (`psizam-gas-timeout-handler`) and `external_interrupt` (`psizam-gas-interrupt-handler`)).

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Gas handler (policy
callback)" section, Yield variant.

## Mechanism
```cpp
struct yield_cfg { uint64_t slice; };

void yield_gas_handler(psizam::gas_state* gs, void* user_data) {
    auto* cfg = static_cast<yield_cfg*>(user_data);
    gs->deadline.store(gs->consumed + cfg->slice,
                       std::memory_order_relaxed); // let execution resume
    current_fiber().yield();                       // transfer to scheduler
}
```

Plus per-instance setup:
```cpp
static yield_cfg cfg{ .slice = psizam::gas_slice_default };
bkend.get_context().set_gas_budget(psizam::gas_units{psizam::gas_slice_default});
bkend.get_context().set_gas_handler(&yield_gas_handler, &cfg);
```

## Acceptance Criteria
- [ ] Canonical yield handler implemented (or documented as user-provided
      with a reference implementation in tests / examples)
- [ ] Integration with `psitri`'s fiber API (the
      `psiserve::fiber_lock_policy` adapter already exists)
- [ ] Test: `gas_handler_yield` — spawn N fibers doing compute loops with
      yield handler + small slice; verify all make progress and that the
      fibers actually park between yields (observe scheduler invocations)
- [ ] Benchmark: yield overhead per slice vs raw compute, on each backend

## Notes
- Careful about throwing across fiber boundaries: the handler does NOT
  throw; it restocks and yields. Exceptions out of the handler would
  unwind through the JIT-emitted call which has well-defined behavior
  but is semantically wrong for yield.
- Slice size tuning is a perf concern; default `gas_slice_default` (10K)
  is a reasonable starting point. Document the tradeoff between frequent
  scheduling (small slice) and throughput (large slice).
