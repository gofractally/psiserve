---
id: psizam-jitllvm-v128-type-rejection
title: "jit_llvm rejects valid module: v128 passed where i32 expected in table_get"
status: fixed
priority: medium
area: psizam
agent: agent-arm
branch: main
created: 2026-04-19
depends_on: []
blocks: []
---

## Description
jit_llvm fails LLVM module verification on a valid wasm module, reporting a
type mismatch where a `<2 x i64>` (v128) is passed to `__psizam_table_get`
which expects `i32`. The interpreter handles this module correctly (trapping
with "table index out of range").

## Reproducer
- `libraries/psizam/tests/fuzz/regression/mismatch_191445_seed222.wasm` (1509 bytes)

## Observed behavior
```
MISMATCH: interpreter=interp_trap jit_llvm=rejected
  interpreter what: table index out of range
  jit_llvm what: LLVM module verification failed:
    Call parameter type does not match function signature!
    %170 = load <2 x i64>, ptr %s27, align 16
    i32  %171 = call i32 @__psizam_table_get(ptr %ctx, i32 1, <2 x i64> %170) #0
```

## Expected behavior
jit_llvm should either execute the module and trap (like the interpreter) or
reject it during validation, not during LLVM IR verification.

## Notes
The LLVM backend is emitting a v128 load where it should emit an i32 load for
the table index argument. This is a type confusion in the LLVM IR emission for
`table.get` when the stack has v128 values.
