---
id: psizam-gas-post-rebase-buildcheck
title: psizam gas metering — verify build after rebase over df5a6c0 / fb377db
status: open
priority: medium
area: psizam
agent: psiserve-agent-x86
branch: main
created: 2026-04-19
depends_on: [psizam-gas-heavy-opcodes]
blocks: []
---

## Description
When the gas-metering follow-on commits (f875b32 … 9ee5c95) were pushed,
the final rebase absorbed two upstream commits that landed during the
work:

- `df5a6c0 Fix jit2 v128 stack corruption from dead-code loops`
- `fb377db .agent file convention: each checkout names the agent running there`
- `683e35b Rename CAS layer to PFS (Planetary File System), assign to agent0`

The rebase applied cleanly (no textual conflicts) but the user requested
we skip the post-rebase build check to get the push in. `df5a6c0` is in
the exact backend (`jit_codegen.hpp` jit2 path) where we added
IR-annotation gas wiring, so there is a small semantic-collision risk
even though git showed no conflict.

## To Do
- [ ] Clean-build Debug with `PSIZAM_ENABLE_TESTS=ON`.
- [ ] Run `./build/Debug/bin/unit_tests "[gas]"` — expect all pass.
- [ ] Run the full `ctest -j$(nproc)` — expect no new regressions.
- [ ] If anything breaks, triage whether it's a real semantic conflict
      between jit2 v128 dead-code-loop fix and the new
      `ir_basic_block::loop_gas_extra` / codegen read path.

## Notes
Low risk — the v128 fix is orthogonal to gas accounting — but worth a
single build before the x86 agent picks up the next item
(`jit_llvm` or `interpreter` gas wiring).
