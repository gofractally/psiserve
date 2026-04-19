---
id: psizam-gas-differential-testing
title: psizam gas metering — differential test harness vs Wasmer / Near
status: ready
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psizam-gas-per-block-strategy]
blocks: []
---

## Description
Once `per_block` ships (`psizam-gas-per-block-strategy`), wire a differential-testing harness that
runs the same WASM module + same gas budget + matching opcode-weight table
through (a) our `per_block` mode, and (b) a reference runner — Wasmer
metering middleware and/or Near `pwasm-utils` injector. Assert both trap
at the same logical opcode count.

This is the validation payoff of shipping compatibility mode: any
divergence in trap point is a correctness bug in one of us. Much stronger
signal than our own internal tests alone can provide.

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Compatibility serves
adoption and validation" principle, and the `gas_per_block_compat` entry
in the Testing section.

## Approach
1. Corpus: start small — `bench_fib`, recursive call, a loop with an if,
   a loop with a div. Grow from there.
2. Reference runner: pick one of
   - Wasmer JS/Rust with `wasmer-middlewares::metering`
   - Near's `pwasm-utils` injecting `gas()` calls, executed under any
     engine consistent with the injected module
3. Run our `per_block` under a fixed budget, record trap counter value.
   Run reference under same budget + same weight table, record trap point.
4. Diff. Any mismatch reported as a test failure with enough context to
   narrow down which opcode the two engines differ on.

## Acceptance Criteria
- [ ] Harness infrastructure: script/test that feeds the same module +
      budget + weights through us and the reference, captures trap points
- [ ] Weight table alignment: document how to port our `gas_costs` into
      Wasmer's / Near's weighting API (or vice versa)
- [ ] Initial corpus of ≥5 modules exercising different control-flow
      patterns (straight-line, loop, nested loop, if/else, call_indirect)
- [ ] All corpus entries trap at the same logical opcode count across us
      and the reference runner
- [ ] CI hook: runs on every PR touching `psizam/` or `.issues/0016`,
      `0017`, or `0018`

## Notes
- Gracefully handle the case where the reference runner's weight table
  isn't expressible in ours (and vice versa): skip those opcodes or
  document the known gap rather than forcing the harness to agree by
  papering over real differences.
- The harness is as much a spec for our own correctness as it is a test:
  the output is "this is what the industry says this module should charge;
  here is what we say."
