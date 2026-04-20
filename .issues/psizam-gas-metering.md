---
id: psizam-gas-metering
title: psizam gas metering (instruction-count-based, consensus-safe)
status: in-progress
priority: high
area: psizam
agent: psiserve-agent-psio
branch: main
created: 2026-04-19
depends_on: []
blocks: [psizam-gas-state-redesign]
---

## Description
Implement deterministic instruction-count gas metering across all psizam backends
for consensus use. Signal-based watchdog (SIGALRM) is non-deterministic and
unsuitable for consensus — this replaces it with inline back-edge metering.

## Acceptance Criteria
- [x] Loop back-edge metering across all backends
- [x] Phase 3: inline non-atomic decrement + runtime atomic toggle
- [x] Inline strategy-off fast-path in every JIT
- [ ] Remaining dirty files committed and pushed

## Notes
Wall-clock (watchdog.hpp / SIGALRM) is explicitly non-consensus-eligible.
Gas metering doesn't capture native host costs; consensus trusts producer to
bill those separately.

## Follow-on
Phase 4 (heavy-opcode dynamic charges) is closed via
`psizam-gas-heavy-opcodes`. The next piece of work is
`psizam-gas-state-redesign` — replaces the atomic-counter model with a
`gas_state { consumed, atomic deadline }` layout and per-mode codegen
(DTD register-pinning for non-interrupt modes, back-edge deadline refresh
for interrupt mode). Driven by the collision between the old
`gas_handler_t` in `gas.hpp` and the new one in `runtime.hpp`.
