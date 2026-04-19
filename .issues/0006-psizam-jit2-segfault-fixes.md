---
id: "0006"
title: psizam jit2 segfault and memory-safety fixes
status: in-progress
priority: critical
area: psizam
agent: psiserve-agent3
branch: main
created: 2026-04-19
depends_on: ["0005"]
blocks: []
---

## Description
jit2 has known memory-safety bugs (segfaults on valid/invalid wasm) that make
it unsafe for untrusted input and disqualify it from consensus use. Fix all
exploitable paths before jit2 is consensus-eligible.

## Acceptance Criteria
- [x] v128 native stack tracking fixed (aarch64 JIT)
- [x] jit2 v128 stack corruption from dead-code loops fixed
- [x] ref-typed local null initialization fixed across all JIT backends
- [ ] simd_const_385/387 — SIMD segfault
- [ ] conversions_0, traps_2 — segfault
- [ ] host function / call depth / reentry — 5 segfaults (exploitable on untrusted wasm)

## Notes
jit2 MUST NOT be consensus-eligible until all segfaults are resolved.
These are exploitable DoS vectors on untrusted WASM input.
