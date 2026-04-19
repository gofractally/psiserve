---
id: psizam-gas-interrupt-handler
title: psizam gas handler — external interrupt (cross-thread cancel)
status: ready
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psizam-gas-metering]
blocks: []
---

## Description
Implement the external-interrupt gas handler. Any thread can trigger
immediate trap of an executing fiber by calling
`ctx->gas_counter().store(-1, std::memory_order_relaxed)`. The next
gas check observes the negative counter and invokes the handler,
which throws.

No signals, no `mprotect`, no VMA tricks. The atomic counter already
exists and the runtime `gas_atomic` toggle (from commit `96e19a2`) is
designed to accommodate this path.

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Gas handler (policy
callback)" section, External interrupt variant.

## Mechanism
Canonical "trap on interrupt" handler is just the default null handler
(throws `wasm_gas_exhausted_exception`). The only thing this issue adds
is: (a) documented public API for the cross-thread trigger, (b) a test
that exercises it across backends.

```cpp
// From another thread:
bkend.get_context().set_gas_atomic(true);  // ensure cross-thread visibility
bkend.get_context().gas_counter().store(-1, std::memory_order_relaxed);
// Next gas check in the fiber observes -1 and throws.
```

## Acceptance Criteria
- [ ] Test: `gas_external_interrupt` exists across BACKEND_TEST_CASE
      (partial coverage already in `gas_metering_tests.cpp`;
      expand with a genuinely cross-thread variant that spawns a watcher
      thread rather than setting the counter before the call)
- [ ] Documented: how to use this to cancel a runaway / long-running
      WASM call from a supervisor thread
- [ ] Latency bound: ≤ one slice between the store(-1) and the trap,
      documented in the design doc and verified in the test

## Notes
- Relies on `set_gas_atomic(true)` being set before the call starts —
  non-atomic mode does a plain store/load sequence that cross-thread
  visibility isn't guaranteed on (even if typical hardware makes it
  work in practice).
- Existing `gas: external interrupt via atomic store(-1)` test already
  fires the interrupt *before* the call starts — good smoke test but
  not a real cross-thread scenario. The expansion here is running the
  store from a separate live thread while the WASM call is in flight.
