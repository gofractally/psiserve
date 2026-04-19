---
id: "0002"
title: Expose psitri database to wasm instances via psiserve (psi-api)
status: in-progress
priority: high
area: psiserve, psitri
agent: psiserve-agent2
branch: main
created: 2026-04-19
depends_on: ["0001"]
blocks: ["0012"]
---

## Description
psiserve needs to expose the psitri database engine to running wasm instances.
This requires defining the public host API (psi-api), wiring psitri into the
psiserve build, and verifying the fiber_lock_policy works end-to-end.

## Acceptance Criteria
- [x] psitri embedded in psiserve build and compiling
- [x] `fiber_lock_policy` adapter verified
- [ ] `psi-api` library fully defined (in-progress: `libraries/psi-api/` exists but incomplete)
- [ ] `db_host.hpp` wired into psiserve host
- [ ] psiserve tests passing (`libraries/psiserve/tests/`)
- [ ] WIT guest API shape finalized (see #0012)

## Dirty Files (agent2 working set)
- `CMakeLists.txt`, `libraries/psiserve/CMakeLists.txt`
- `libraries/psi-api/` (new, untracked)
- `libraries/psiserve/include/psiserve/db_host.hpp` (new, untracked)
- `libraries/psiserve/tests/` (new, untracked)
- Many `libraries/psizam/tests/spec/*_tests.cpp` (spec test regeneration)
