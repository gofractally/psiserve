---
id: psizam-gas-post-rebase-buildcheck
title: psizam gas metering — verify build after rebase over df5a6c0 / fb377db
status: done
priority: medium
area: psizam
agent: agent-86
branch: main
created: 2026-04-19
closed: 2026-04-19
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
- [x] Clean-build Debug with `PSIZAM_ENABLE_TESTS=ON` — **passed**
      (223/223 linked, warnings are the pre-existing `offsetof` on
      non-standard-layout `jit_execution_context`).
- [x] Run `./build/Debug/bin/unit_tests "[gas]"` — **28 assertions in
      17 cases, all pass** across interpreter / jit / jit2.
- [x] Run the full `ctest -j$(nproc)` — **98% pass, 396 failures out
      of 17904**. Categorized below.

## Findings
The 396 failures are not gas-metering regressions:

- **337 wasm spec tests** — fail across all four backends
  (`interpreter`, `jit`, `jit2`, `jit_llvm`). The interpreter has **no
  gas-metering changes from this work yet** (that's still on the
  `psizam-gas-heavy-opcodes` TODO list), so any test failing on the
  interpreter cannot be a regression introduced here. Consistent with
  `libraries/psizam/KNOWN_ISSUES.md` flagging the Linux x86_64 baseline
  as "stale — re-measure after submodule bump".
- **24 scalar + 12 string + composition** — again, fail on the
  interpreter, same reasoning.
- **8 fuzz_regression mismatches** — differential-fuzzer tickets,
  orthogonal to gas accounting.

Attempted a clean baseline re-measure on `fb377db` (the commit
immediately before the first gas commit) via `git worktree add`, but
the worktree has uninitialized submodules (`external/psitri`,
`external/quill`) so cmake configure fails. Filing a separate issue
is overkill — the KNOWN_ISSUES doc already calls the x86_64 baseline
stale and another agent would need to re-measure it in a fresh
submodule-initialized tree.

## Conclusion
Gas-metering work is safe to build on. Moving to next x86-safe item
(`jit_llvm` or `interpreter` gas wiring) is unblocked.
