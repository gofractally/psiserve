---
id: psizam-jit2-v128-global-mismatch
title: "jit2 returns zero v128 instead of correct value through global.set/get"
status: open
priority: high
area: psizam
agent: agent-arm
branch: main
created: 2026-04-19
depends_on: []
blocks: []
---

## Description
jit2 returns all-zeros for a v128 value that the interpreter computes correctly.
The module exercises v128 through global.set/get and complex SIMD operation
chains. This is NOT the same v128 br stack corruption bug (fixed in b42d976) —
the v128 value flows through globals, not across block branches.

## Reproducer
- `libraries/psizam/tests/fuzz/regression/mismatch_158736_seed333.wasm` (2023 bytes)

## Observed behavior
```
RETURN VALUE MISMATCH export#0:
  interpreter = v128:0x6cb18f5a446e1301bccd52cded7ebc41
  jit2        = v128:0x00000000000000000000000000000000
```

## Expected behavior
jit2 should return the same v128 value as the interpreter.

## Notes
The all-zeros return suggests jit2 is either failing to load the v128 from
the global slot, or the global.set never wrote it. Check `global_set` /
`global_get` handling for v128 types in `jit_codegen_a64.hpp`.
