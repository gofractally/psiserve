---
id: "0008"
title: psizam multi-module linking
status: backlog
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: []
blocks: []
---

## Description
psizam currently fails 96-98 tests due to missing multi-module linking support.
This is a feature gap (not a bug) that blocks real module composition and the
COW/fork story for wasm instances.

## Acceptance Criteria
- [ ] Multi-module import/export linking implemented
- [ ] 96-98 currently-failing spec tests pass
- [ ] COW/fork use case validated
