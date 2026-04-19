# psiserve Project Backlog

Last updated: 2026-04-19

## Issue Conventions

- **No numeric IDs.** Each issue is a markdown file at `.issues/<slug>.md`.
  Sequential numbers race between agents; slugs do not.
- **Cite issues by relative path**, not by number. Example: `.issues/psio-wit-resource-drop-specializations.md`.
- **Before creating a new issue**: `git pull` first, check for a similar
  existing issue, then add a new file with a descriptive slug.
- **Dependency fields** in frontmatter (`depends_on`, `blocks`) use slugs
  matching the filename without `.md`.

## Status Key
`done` | `in-progress` | `ready` | `backlog` | `blocked`

## Issue Index

| Issue | Status | Priority | Area | Agent | Blocks |
|-------|--------|----------|------|-------|--------|
| [psitri-fiber-lock-policy](.issues/psitri-fiber-lock-policy.md) | done | high | psitri | agent2 | psiserve-psitri-integration |
| [psiserve-psitri-integration](.issues/psiserve-psitri-integration.md) | in-progress | high | psiserve,psitri | agent2 | psiserve-wit-host-guest-api |
| [psizam-llvm-wasi-toolchain](.issues/psizam-llvm-wasi-toolchain.md) | in-progress | medium | psizam | agent-psio | — |
| [psizam-gas-metering](.issues/psizam-gas-metering.md) | in-progress | high | psizam | agent-psio | — |
| [psizam-fuzzing-infrastructure](.issues/psizam-fuzzing-infrastructure.md) | in-progress | high | psizam | agent3 | psizam-eh-v2-fuzzing |
| [psizam-jit2-segfault-fixes](.issues/psizam-jit2-segfault-fixes.md) | in-progress | critical | psizam | agent3 | — |
| [psizam-aarch64-simd-softfloat](.issues/psizam-aarch64-simd-softfloat.md) | backlog | high | psizam | — | — |
| [psizam-multi-module-linking](.issues/psizam-multi-module-linking.md) | backlog | medium | psizam | — | — |
| [psizam-externref-globals-crash](.issues/psizam-externref-globals-crash.md) | backlog | high | psizam | — | — |
| [psizam-eh-v2-fuzzing](.issues/psizam-eh-v2-fuzzing.md) | backlog | medium | psizam | — | — |
| [psio-dynamic-schema-validation](.issues/psio-dynamic-schema-validation.md) | backlog | medium | psio | — | psiserve-wit-host-guest-api |
| [psiserve-wit-host-guest-api](.issues/psiserve-wit-host-guest-api.md) | backlog | high | psiserve,psio | — | psiserve-virtual-db-config, psiserve-snapshot-api |
| [psiserve-virtual-db-config](.issues/psiserve-virtual-db-config.md) | backlog | medium | psiserve | — | — |
| [psiserve-snapshot-api](.issues/psiserve-snapshot-api.md) | backlog | medium | psiserve,psitri | — | — |
| [psizam-memory-sandboxing-modes](.issues/psizam-memory-sandboxing-modes.md) | ready | high | psizam | agent3 | — |
| [psio-wit-resource-drop-specializations](.issues/psio-wit-resource-drop-specializations.md) | ready | high | psio,psiserve,wasi | — | psiserve-wit-host-guest-api |
| [psitri-chunked-file-storage](.issues/psitri-chunked-file-storage.md) | ready | high | psitri | — | ipfs-filesystem-api |
| [ipfs-filesystem-api](.issues/ipfs-filesystem-api.md) | backlog | medium | psiserve,psio,psitri | — | — |

## Dependency Graph

```
psitri-fiber-lock-policy (done)
  └→ psiserve-psitri-integration (in-progress)
       └→ psiserve-wit-host-guest-api
            ├→ psiserve-virtual-db-config
            └→ psiserve-snapshot-api

psizam-gas-metering (in-progress)    [compute sandboxing]

psizam-fuzzing-infrastructure (in-progress)
  └→ psizam-eh-v2-fuzzing

psizam-jit2-segfault-fixes (in-progress)
  ├→ psizam-memory-sandboxing-modes [memory sandboxing]
  ├→ psizam-aarch64-simd-softfloat
  └→ psizam-externref-globals-crash

psio-dynamic-schema-validation
  └→ psiserve-wit-host-guest-api

psio-wit-resource-drop-specializations
  └→ psiserve-wit-host-guest-api

psitri-chunked-file-storage
  └→ ipfs-filesystem-api
```

## Conflict Map (agents touching overlapping areas)
- agent2 + agent-psio both touch `libraries/psizam/` — coordinate before merging
- agent2 touches `external/psitri` (submodule) — must push psitri before parent merge
- agent3 touches `libraries/psizam/tests/` — agent2 also modifies spec tests — merge order matters
