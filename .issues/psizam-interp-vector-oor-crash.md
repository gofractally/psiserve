---
id: psizam-interp-vector-oor-crash
title: "Interpreter crashes with std::out_of_range: vector on valid wasm"
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
The interpreter throws an uncaught `std::out_of_range: vector` exception on
certain valid wasm modules, causing a process abort (signal 6). The JIT backends
handle the same modules correctly (trapping with "unreachable"), indicating
the interpreter has an out-of-bounds vector access in its opcode dispatch or
stack management.

## Reproducers
- `libraries/psizam/tests/fuzz/regression/crash_398116_seed111.wasm` (691 bytes)
- `libraries/psizam/tests/fuzz/regression/crash_482002_seed111.wasm` (175 bytes)

## Observed behavior
```
MISMATCH: interpreter=exception jit=interp_trap
  interpreter what: vector
  jit what: unreachable
libc++abi: terminating due to uncaught exception of type std::out_of_range: vector
CRASH: child killed by signal 6
```

## Expected behavior
Interpreter should trap cleanly (like the JIT does) rather than crashing.

## Notes
The 175-byte reproducer (`crash_482002`) is minimal and should be the starting
point for debugging. Likely a missing bounds check in `interpret_visitor.hpp`
on the operand stack, local vector, or control stack.
