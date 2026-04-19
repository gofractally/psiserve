---
id: psizam-memory-sandboxing-modes
title: psizam memory sandboxing modes (guarded / checked / unchecked)
status: ready
priority: high
area: psizam
agent: agent-arm
branch: ~
created: 2026-04-19
depends_on: [psizam-jit2-segfault-fixes]
blocks: []
---

## Description
Implement the three `mem_safety` tiers defined in `runtime-api-design.md` so
that callers can select the right memory enforcement strategy per use case.
This is the memory dimension of the sandboxing policy — the compute dimension
(gas/metering) is tracked separately in #0004.

## Design Reference
`libraries/psizam/docs/runtime-api-design.md` — `instance_policy::mem_safety`
and the three concrete usage examples (block producer, replay node, HTTP server).

## Three Modes

### `mem_safety::guarded`
OS-level guard pages via `mmap + PROT_NONE`. Zero per-access software overhead.
SIGSEGV handler converts page fault to WASM trap. Best for: few instances,
large address space available (block producer).

### `mem_safety::checked`
Software bounds check on every linear memory access via `guarded_ptr<T>`.
Works with arena pool — many instances share a single mapped region.
Best for: many concurrent fibers (HTTP server / psiserve).

### `mem_safety::unchecked`
No bounds checks. Caller asserts the module has already been validated by
consensus. Best for: trusted replay nodes where max throughput matters.

## Acceptance Criteria
- [ ] `mem_safety` enum plumbed through `instance_policy` into instantiation
- [ ] `guarded` mode: guard-page pool allocation, SIGSEGV → trap handler
- [ ] `checked` mode: `guarded_ptr` wrappers active on all linear memory accesses across all backends
- [ ] `unchecked` mode: all bounds checks compiled out when policy is unchecked
- [ ] Backend code generation respects the policy at compile time (no runtime branch)
- [ ] Unit tests for each mode: OOB access traps in guarded/checked, undefined in unchecked (documented)
- [ ] Differential fuzzer (#0005) validates guarded == checked output for same inputs

## Notes
- Start after #0006 (jit2 segfaults) — fixing memory safety bugs first avoids
  conflating implementation bugs with intentional unchecked behavior
- `guarded_ptr` skeleton already exists in `include/psizam/detail/guarded_ptr.hpp`
- Arena allocator design in `plans/arena-allocator-design.md`
