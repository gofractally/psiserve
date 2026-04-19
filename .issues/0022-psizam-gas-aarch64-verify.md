---
id: "0022"
title: psizam gas metering — aarch64 jit2 wiring + jit1 hardware verification
status: open
priority: high
area: psizam
agent: _unassigned (needs arm64 host)_
branch: main
created: 2026-04-19
depends_on: ["0016"]
---

## Description
Carved out of `#0016` because the committed and queued aarch64 work
needs to actually run on aarch64 hardware. The x86 agent wrote and
compiled it blind (compile-only on x86_64 catches syntax, not
instruction-level correctness), so an arm64 agent must both verify
the landed code and finish the remaining piece.

## Scope

### 1. Verify jit1 aarch64 on real hardware
Commit `f16eceb` ("Gas metering Phase 4: jit1 aarch64 heavy-opcode
patching") rewrote `libraries/psizam/include/psizam/detail/aarch64.hpp`
so `emit_gas_charge` always emits a fixed 4-insn `MOVZ+3×MOVK` chain into
X10 followed by `SUBS X9, X9, X10`, and added `prologue_gas_handle` /
`last_loop_gas_handle` / `patch_gas_imm_add_extra`. All x86_64 CI
showed green, but the instruction-level correctness was not verified.

- [ ] Build on an aarch64 host (Linux or macOS) with `PSIZAM_ENABLE_TESTS=ON`.
- [ ] Run `./build/Debug/bin/unit_tests "[gas]"` — expect all cases to pass.
- [ ] Run the full `ctest` — expect no new regressions.
- [ ] Spot-check with `objdump -d` that the emitted sequence for a
      `i64.div_s`-in-a-loop module matches the design (MOVZ+3×MOVK
      encoding is correct, `patch_gas_imm_add_extra` reads/writes the
      imm16 fields at bits [20:5]).

### 2. jit2 aarch64 wiring (originally box 6 of `#0016`)
`libraries/psizam/include/psizam/detail/jit_codegen_a64.hpp` still has
the pre-existing TODO in `mark_block_start` and no `emit_gas_charge`
call in `emit_function_prologue`. The IR-annotation infrastructure
it needs is already landed (`ir_basic_block::loop_gas_extra`,
`ir_function::prologue_gas_extra`, `ir_writer` handles, parser SFINAE),
so this is purely a codegen-side change.

- [ ] Add `emit_gas_charge(int64_t cost)` to `jit_codegen_a64`. Since
      codegen runs after accumulation, final cost is known at emit
      time — **no patching is needed**. Use the compact `SUBS` imm12
      fast path when the cost fits and fall back to `MOVZ/MOVK + SUBS
      reg-reg` otherwise. Mirror the three-way structure
      (strategy-off / non-atomic fast path / atomic helper path) from
      `aarch64.hpp::emit_gas_charge`.
- [ ] Add `emit_save_context` / `emit_restore_context` /
      `emit_call_c_function` helpers (this file does not have them yet).
- [ ] Wire `emit_function_prologue`:
      `emit_gas_charge(body_bytes + func.prologue_gas_extra)`.
- [ ] Wire `mark_block_start` for `is_loop` blocks:
      `emit_gas_charge(1 + block.loop_gas_extra)`. Replace the
      `// TODO: gas metering on jit2 aarch64` comment.
- [ ] `./build/Debug/bin/unit_tests "[gas]"` passes.
- [ ] `objdump -d` sanity-check for a div-in-a-loop module.

## Design Reference
- `libraries/psizam/docs/gas-metering-design.md` — Phase 4 section.
- Commit `62943eb` (jit2 x86_64 IR annotation) — the structural template.
- `libraries/psizam/include/psizam/detail/aarch64.hpp::emit_gas_charge`
  — the working aarch64 reference for the three-way emission.

## Once verified
Check off the two aarch64 boxes on `#0016` and keep the rest of that
issue (jit_llvm, interpreter, cross-backend test, benchmark) with the
x86 agent since they're architecture-neutral.
