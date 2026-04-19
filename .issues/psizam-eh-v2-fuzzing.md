---
id: psizam-eh-v2-fuzzing
title: psizam EH v2 differential fuzzing (try_table/throw/throw_ref)
status: backlog
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psizam-fuzzing-infrastructure]
blocks: []
---

## Description
EH v2 (try_table/throw/throw_ref) was just merged across all backends but
has no differential fuzzing coverage yet. Exception unwinding must be
deterministic across all four backends before it is load-bearing in production.

## Acceptance Criteria
- [ ] Fuzzer extended to generate EH v2 wasm (try_table, throw, throw_ref)
- [ ] No cross-backend divergence found on EH inputs
- [ ] Regression harness added for any crashes found
