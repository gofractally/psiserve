---
id: psizam-externref-globals-crash
title: psizam externref globals crash in JIT backend (DoS vector)
status: backlog
priority: high
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psizam-jit2-segfault-fixes]
blocks: []
---

## Description
externref globals cause a crash in the JIT backend (global_0 test). Crash on
untrusted input is a DoS vector and must be fixed before psizam handles
untrusted wasm in production.

## Acceptance Criteria
- [ ] global_0 spec test passes
- [ ] No crash path reachable via externref global from untrusted wasm
