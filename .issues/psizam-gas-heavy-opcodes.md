---
id: psizam-gas-heavy-opcodes
title: psizam gas metering — heavy-opcode dynamic charges (Phase 4)
status: in-progress
priority: high
area: psizam
agent: psiserve-agent-x86
branch: main
created: 2026-04-19
depends_on: [psizam-gas-metering]
blocks: [psizam-gas-per-block-strategy]
---

## Description
Extend the gas-metering engine so opcodes whose hardware weight dramatically
exceeds the flat baseline charge their real cost. Without this the accountant
badly undercharges compute-heavy workloads (a loop of `i64.div_s` runs at the
same billed rate as a loop of `i64.add` — ~10× undercharge per iteration).

Design principle, per `docs/gas-metering-design.md`: **approximation is
cheaper than precision**. Do not add new per-site injection points for the
common case. Instead, sum heavy-op extras during parse into the existing
prologue/loop-header gas-charge emissions. One charge per back-edge, just
bigger when the loop contains heavy ops.

## Design Reference
`libraries/psizam/docs/gas-metering-design.md` — "Phase 4 — heavy-opcode
dynamic charges" section. Weights live in `psizam/gas.hpp::gas_costs`.

## Mechanism
- `gas_injection_state` in `include/psizam/detail/gas_injector.hpp`: shared
  single-pass accumulator. Tracks `prepay_extra` (non-loop) and a per-loop
  cost stack that pops at loop close.
- Parser (`parser.hpp`) drives it via `on_opcode` / `on_loop_enter` /
  `on_loop_exit` and patches the backend's previously-reserved gas-charge
  immediates with the final sums.
- Each backend exposes `emit_gas_charge` (returning a ptr/handle to the
  patchable immediate), `prologue_gas_handle`, `last_loop_gas_handle`, and
  `patch_gas_imm_add_extra(handle, extra)`. SFINAE in the parser means
  backends opt in as they're wired; un-wired backends stay on the flat
  Phase 2b behavior.

## Acceptance Criteria
- [x] State machine (`gas_injection_state`) with `prepay_extra` + loop stack
- [x] Parser plumbing (on_opcode / on_loop_enter / on_loop_exit / prologue patch)
- [x] jit1 x86_64: always-imm32 emit + patch API wired
- [x] jit1 aarch64: fixed-size MOVZ/MOVK sequence + patch API wired
      (written on x86_64 host; aarch64-hardware verification tracked in `psizam-gas-aarch64-verify`)
- [x] jit2 x86_64 (`jit_codegen.hpp`): IR-node annotation, no byte patching
- [x] jit2 aarch64 (`jit_codegen_a64.hpp`): IR-node annotation + fill the
      Phase 2a/3 prologue TODO — landed by arm64 agent in commit `b42d976`
- [x] jit_llvm: uses the same ir_writer IR-annotation path as jit2, so
      emit_gas_prologue_check reads the final prologue_gas_extra /
      loop_gas_extra directly. No runtime IR patching needed since LLVM
      codegen runs after the full parser pass completes.
- [x] interpreter: new `gas_charge_t` bitcode op + `interpret_visitor` dispatch
      (emitted at every loop header by `bitcode_writer::emit_loop`; prologue
      extras stored in `function_body::prologue_gas_extra` and charged by
      `execution_context::call`)
- [ ] Cross-backend test: same module + budget traps at same counter across
      all five backends when heavy ops are present
- [ ] Benchmark: compute-heavy workload (matmul or similar with divs) shows
      gas charged ≈ opcode-weight-weighted count, not raw opcode count

## Notes
- `gas_heavy_extra_for` currently handles only base-opcode heavy ops
  (`i64.div_*`/`rem_*`, `f*.div`, `call_indirect`). Extended-prefix
  operand-scaled ops (`memory.grow`, `memory.copy`/`fill`, `table.grow`/
  `copy`/`init`) are out of scope here — they need runtime operand-scaled
  charges emitted at the opcode site.
- emit_gas_charge now always emits REX.W 0x81 /5 imm32 on x86_64 (was imm8
  fast path + imm32 fallback). Cost: +3 bytes per gas charge, which is the
  price of patchability without jump relocation.
