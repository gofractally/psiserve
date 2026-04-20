---
id: psizam-jit-false-trap-mismatches
title: "JIT backends falsely trap on modules the interpreter executes successfully"
status: fixed
priority: high
area: psizam
agent: agent-arm
branch: main
created: 2026-04-19
depends_on: []
blocks: []
---

## Description
Two distinct false-trap patterns where JIT backends trap but the interpreter
completes successfully:

### Case 1: jit2 + jit_llvm false "unreachable" trap
Both jit2 and jit_llvm hit an `unreachable` instruction that the interpreter
never reaches, suggesting a control flow divergence in JIT codegen.

### Case 2: All JITs false memory_trap
All three JIT backends (jit, jit2, jit_llvm) trap with `memory_trap` on a
module the interpreter handles fine. This suggests incorrect memory bounds
calculation or guard page setup shared across all JIT backends.

## Reproducers
- `libraries/psizam/tests/fuzz/regression/mismatch_103590_seed333.wasm` (531 bytes) — Case 1
- `libraries/psizam/tests/fuzz/regression/mismatch_151857_seed333.wasm` (2306 bytes) — Case 2

## Observed behavior
Case 1:
```
MISMATCH: interpreter=ok jit2=interp_trap (unreachable)
MISMATCH: interpreter=ok jit_llvm=interp_trap (unreachable)
```

Case 2:
```
MISMATCH: interpreter=ok jit=memory_trap
MISMATCH: interpreter=ok jit2=memory_trap
MISMATCH: interpreter=ok jit_llvm=memory_trap
```

## Notes
Case 2 is particularly interesting since ALL JITs agree on memory_trap while
the interpreter says ok — could be the interpreter that's wrong (missing a
bounds check) rather than the JITs. Investigate the interpreter path first.
