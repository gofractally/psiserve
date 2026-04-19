---
id: psizam-gas-per-block-strategy
title: psizam gas metering — per_block insertion strategy (industry-compat)
status: ready
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psizam-gas-heavy-opcodes]
blocks: [psizam-gas-differential-testing]
---

## Description
Add the `per_block` insertion strategy as a load-time, opt-in compatibility
mode. Charges at every basic-block boundary (after `loop`, `if`, `else`,
`br`, `br_if`, `br_table`, `end`, `call`) — matching Wasmer metering
middleware and Near `pwasm-utils` injection granularity.

The goal is not "more accurate metering" — our `prepay_max` default is
cheaper and good enough. The goal is **adoption + validation**: Wasmer/Near
users can migrate with bit-compatible charge points (they discover
`prepay_max` is faster still), and we get a differential-testing harness
against those engines (handled in `psizam-gas-differential-testing`).

Design principle: this mode must be **faster than the competition at their
own game**. Wasmer and Near both emit a host-call per check; we emit an
inline `sub imm, counter; jns ok` via the Phase 3 peephole. Per-check cost
stays ~3 cycles where theirs is ~20–50ns.

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Insertion strategies"
section and "Compatibility serves adoption and validation" principle.

## Scope
- **Runtime switchable via `execution_context::set_gas_strategy(per_block)`.**
  Same compiled module runs under `prepay_max`, `off`, or `per_block`
  without re-parsing. Charge points emitted for *all* strategies; strategy
  just gates which ones actually run (see `gas_charge`'s strategy check).

  *Open question:* if we only want to emit charges under `per_block` when
  that strategy is selected, parser needs strategy at parse time. Current
  design is parse-time unconditional — simpler, tolerable overhead.

## Acceptance Criteria
- [ ] Cost table consumed: per-opcode weights from `gas_costs` used to
      compute each block's straight-line cost at parse time
- [ ] Block-cost computation in the parser (reuses `gas_injection_state`
      shape; tracks active-block cost alongside loop stack)
- [ ] Per-backend emission of block-boundary gas charges, each using the
      Phase 3 inline peephole. Reuses the `reserve/patch` machinery from
      `psizam-gas-heavy-opcodes` (emit with placeholder, patch at block close).
- [ ] `gas_insertion_strategy::per_block` wired end-to-end; selecting it
      activates the block-boundary charges at runtime
- [ ] Test: `gas_per_block_overpay` — verify charge totals match expected
      block costs; contrast with `prepay_max` on the same module
- [ ] Benchmark: measure overhead per backend with `per_block` vs `off`;
      must beat Wasmer's host-call per-check on a matched workload

## Notes
- `hybrid` strategy was **dropped** from the design (per principles in the
  design doc). Only `off`, `prepay_max`, and `per_block` ship.
- Implementation ordering: finish `psizam-gas-heavy-opcodes` first so all backends have the
  reserve/patch infrastructure; per_block then only adds new emission
  sites, not new primitives.
