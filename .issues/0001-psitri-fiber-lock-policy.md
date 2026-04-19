---
id: "0001"
title: Pluggable lock policy for fiber-aware consumers
status: done
priority: high
area: psitri
agent: psiserve-agent2
branch: feature/pluggable-lock-policy
created: 2026-04-19
closed: 2026-04-19
depends_on: []
blocks: ["0002"]
---

## Description
Parameterize psitri's database and DWAL on a LockPolicy template so that fiber-based
callers (psiserve) can supply a fiber-yield-aware lock instead of blocking mutexes.

## Acceptance Criteria
- [x] `database` templated on LockPolicy
- [x] `DWAL` templated on LockPolicy
- [x] DWAL btree_layer subtree ref-count leak fixed
- [x] `psiserve::fiber_lock_policy` adapter written and compiling
- [x] psitri branch pushed to remote (was unpushed — resolved 2026-04-19)
- [x] psiserve bumped to new psitri commit

## Notes
Branch `feature/pluggable-lock-policy` on `gofractally/arbtrie` was committed but
not pushed; parent repo already referenced the unpushed HEAD. Pushed manually.
