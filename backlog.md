# psiserve Project Backlog

Last updated: 2026-04-19

## Status Key
`done` | `in-progress` | `ready` | `backlog` | `blocked`

## Issue Index

| ID | Title | Status | Priority | Area | Agent | Blocks |
|----|-------|--------|----------|------|-------|--------|
| [0001](.issues/0001-psitri-fiber-lock-policy.md) | psitri fiber lock policy | done | high | psitri | agent2 | 0002 |
| [0002](.issues/0002-psiserve-psitri-integration.md) | psiserve psitri integration / psi-api | in-progress | high | psiserve,psitri | agent2 | 0012 |
| [0003](.issues/0003-psizam-llvm-wasi-toolchain.md) | psizam LLVM-WASI toolchain (ThinLTO, mimalloc, web-IDE) | in-progress | medium | psizam | agent-psio | — |
| [0004](.issues/0004-psizam-gas-metering.md) | psizam gas metering (consensus-safe) | in-progress | high | psizam | agent-psio | — |
| [0005](.issues/0005-psizam-fuzzing-infrastructure.md) | psizam differential fuzzing (libFuzzer) | in-progress | high | psizam | agent3 | 0010 |
| [0006](.issues/0006-psizam-jit2-segfault-fixes.md) | psizam jit2 segfault / memory-safety fixes | in-progress | critical | psizam | agent3 | — |
| [0007](.issues/0007-psizam-aarch64-simd-softfloat.md) | psizam aarch64 SIMD softfloat (consensus divergence) | backlog | high | psizam | — | — |
| [0008](.issues/0008-psizam-multi-module-linking.md) | psizam multi-module linking | backlog | medium | psizam | — | — |
| [0009](.issues/0009-psizam-externref-globals-crash.md) | psizam externref globals crash (DoS) | backlog | high | psizam | — | — |
| [0010](.issues/0010-psizam-eh-v2-fuzzing.md) | psizam EH v2 differential fuzzing | backlog | medium | psizam | — | — |
| [0011](.issues/0011-psio-dynamic-schema-validation.md) | psio dynamic schema validation (FracPack+) | backlog | medium | psio | — | 0012 |
| [0012](.issues/0012-psiserve-wit-host-guest-api.md) | psiserve WIT host/guest DB API | backlog | high | psiserve,psio | — | 0013,0014 |
| [0013](.issues/0013-psiserve-virtual-db-config.md) | psiserve virtual DB config + permissions | backlog | medium | psiserve | — | — |
| [0014](.issues/0014-psiserve-snapshot-api.md) | psiserve snapshot API (blockchain) | backlog | medium | psiserve,psitri | — | — |
| [0015](.issues/0015-psizam-memory-sandboxing-modes.md) | psizam memory sandboxing modes (guarded/checked/unchecked) | ready | high | psizam | agent3 | — |

## Dependency Graph

```
0001 (done) → 0002 (in-progress) → 0012 → 0013
                                          → 0014
0004 (in-progress) [compute sandboxing]
0005 (in-progress) → 0010
0006 (in-progress) → 0015 [memory sandboxing]
                   → 0007
                   → 0009
0011 → 0012
```

## Conflict Map (agents touching overlapping areas)
- agent2 + agent-psio both touch `libraries/psizam/` — coordinate before merging
- agent2 touches `external/psitri` (submodule) — must push psitri before parent merge
- agent3 touches `libraries/psizam/tests/` — agent2 also modifies spec tests — merge order matters
