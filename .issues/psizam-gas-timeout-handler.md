---
id: psizam-gas-timeout-handler
title: psizam gas handler — wall-clock timeout variant
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
Implement a wall-clock timeout gas handler. On counter exhaustion, compares
elapsed wall-clock time against a per-instance deadline. If past the
deadline, traps; otherwise restocks a slice and continues.

Worst-case detection latency = one slice (≈1µs at `gas_slice_default=10K`
on current backends). This is the replacement for the SIGALRM-based
watchdog, which is non-deterministic and unsuitable for consensus but
perfectly fine for serving request timeouts in psiserve.

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Gas handler (policy
callback)" section, Timeout variant.

## Mechanism
Per-instance state (stored alongside the counter):
```cpp
struct timeout_state {
    std::chrono::steady_clock::time_point deadline;
    int64_t slice;
};

void timeout_gas_handler(void* ctx_raw) {
    auto* ctx = static_cast<execution_context_base*>(ctx_raw);
    auto* ts  = static_cast<timeout_state*>(ctx->handler_userdata());
    if (std::chrono::steady_clock::now() >= ts->deadline) {
        throw wasm_timeout_exception{"wall-clock deadline exceeded"};
    }
    ctx->restock_gas(ts->slice);
}
```

Requires a `handler_userdata` slot on `execution_context_base` (or passing
the timeout state via the ctx's allocator-backed scratch area).

## Acceptance Criteria
- [ ] `wasm_timeout_exception` (or reuse `wasm_gas_exhausted_exception`
      with a flag; design choice)
- [ ] `handler_userdata` (or equivalent) slot on `execution_context_base`
- [ ] Canonical timeout handler implementation
- [ ] Test: `gas_handler_timeout` — set a 10ms deadline, run an unbounded
      loop, verify the exception fires within (deadline + one slice)
- [ ] Replaces the `watchdog.hpp`/SIGALRM path for non-consensus timeout
      use cases. `watchdog.hpp` stays available but documented as legacy /
      non-consensus.

## Notes
- Wall clock is **not** consensus-safe — issue `psizam-gas-metering`'s guidance is
  explicit on this. This handler is for psiserve request-timeout use,
  not for replayable consensus execution.
- Slice tuning affects worst-case overshoot; document the tradeoff in the
  design doc.
