---
id: "0007"
title: psizam aarch64 JIT SIMD softfloat (consensus divergence fix)
status: backlog
priority: high
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: ["0006"]
blocks: []
---

## Description
aarch64 JIT SIMD FP operations are not fully softfloat. ARM64 consensus nodes
will diverge from x86_64 on SIMD float results. Must be fixed before psizam
can be used in multi-architecture consensus.

## Acceptance Criteria
- [ ] All SIMD FP ops in aarch64 JIT route through softfloat library
- [ ] Differential fuzzer confirms no x86_64 vs aarch64 divergence on SIMD inputs
