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
Implement a wall-clock timeout gas handler. When `consumed >= deadline`,
compares elapsed wall-clock time against a per-instance wall deadline.
If past the wall deadline, traps; otherwise advances the gas deadline
by a slice and continues.

Worst-case detection latency = one slice (≈1µs at `gas_slice_default=10K`
on current backends). This is the replacement for the SIGALRM-based
watchdog, which is non-deterministic and unsuitable for consensus but
perfectly fine for serving request timeouts in psiserve.

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Gas handler (policy
callback)" section, Timeout variant.

## Mechanism
Per-instance state (passed via the gas_state user_data pointer):
```cpp
struct timeout_cfg {
    std::chrono::steady_clock::time_point wall_deadline;
    uint64_t                              slice;
};

void timeout_gas_handler(psizam::gas_state* gs, void* user_data) {
    auto* cfg = static_cast<timeout_cfg*>(user_data);
    if (std::chrono::steady_clock::now() >= cfg->wall_deadline) {
        throw wasm_timeout_exception{"wall-clock deadline exceeded"};
    }
    gs->deadline.store(gs->consumed + cfg->slice,
                       std::memory_order_relaxed);
}

// Setup:
static timeout_cfg cfg{ .wall_deadline = now + 10ms, .slice = 10'000 };
bkend.get_context().set_gas_budget(psizam::gas_units{cfg.slice});
bkend.get_context().set_gas_handler(&timeout_gas_handler, &cfg);
```

No new context slot needed — the gas_state already carries
`user_data` alongside the handler pointer.

## Acceptance Criteria
- [ ] `wasm_timeout_exception` (or reuse `wasm_gas_exhausted_exception`
      with a flag; design choice)
- [ ] Canonical timeout handler implementation (uses gas_state.user_data)
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
