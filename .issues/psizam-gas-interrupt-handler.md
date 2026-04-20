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
immediate trap of an executing fiber by storing `0` into
`ctx.gas().deadline`. The next gas check observes
`consumed >= 0 (== deadline)` and invokes the handler (which throws
by default — null handler ⇒ `wasm_gas_exhausted_exception`).

No signals, no `mprotect`, no VMA tricks. Under the gas_state redesign
`deadline` is always `std::atomic<uint64_t>`, so cross-thread stores
are well-defined without any runtime toggle.

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Gas handler (policy
callback)" section, External interrupt variant.

## Mechanism
Canonical "trap on interrupt" handler is just the default null handler
(throws `wasm_gas_exhausted_exception`). The only thing this issue adds
is: (a) documented public API for the cross-thread trigger, (b) a test
that exercises it across backends.

```cpp
// From another thread — no runtime toggle needed:
bkend.get_context().gas().deadline.store(0, std::memory_order_relaxed);
// Next gas check in the fiber observes consumed >= 0 == deadline and
// the handler fires (or throws if handler is null).
```

## Acceptance Criteria
- [ ] Test: `gas_external_interrupt` exists across BACKEND_TEST_CASE
      (partial coverage already in `gas_metering_tests.cpp`;
      expand with a genuinely cross-thread variant that spawns a watcher
      thread rather than setting the counter before the call)
- [ ] Documented: how to use this to cancel a runaway / long-running
      WASM call from a supervisor thread
- [ ] Latency bound: ≤ one slice between the deadline store and the
      trap, documented in the design doc and verified in the test

## Notes
- `deadline` is always `std::atomic<uint64_t>`, so cross-thread stores
  are well-defined without any runtime toggle. On x86_64 and aarch64 the
  owner thread reads it with a plain `mov`/`ldr` (relaxed atomic load
  compiles to that on naturally-aligned u64).
- Existing `gas: external interrupt via deadline store(0)` test already
  fires the interrupt *before* the call starts — good smoke test but
  not a real cross-thread scenario. The expansion here is running the
  store from a separate live thread while the WASM call is in flight.
