---
id: psio-dynamic-schema-validation
title: psio dynamic binary validation against schema (FracPack + others)
status: backlog
priority: medium
area: psio
agent: ~
branch: ~
created: 2026-04-19
depends_on: []
blocks: [psiserve-wit-host-guest-api]
---

## Description
Given any schema (FracPack, FlatBuffers, CapNProto, etc.) and a binary blob,
dynamically verify whether the binary conforms. Compile schema into a fast
validator for row-level validation at database insert time.

## Acceptance Criteria
- [ ] Parse FracPack schemas at runtime
- [ ] Compile parsed schema to a fast binary validator
- [ ] Four compliance modes: exact, forward-compat, backward-compat, exact+canonical
- [ ] Schema parsing extended to FlatBuffers / CapNProto (or documented as future work)
- [ ] Integrated with psiserve database insert path (validates untrusted row data)

## Notes
All schema formats ultimately describe the same underlying types. The validator
compiles to a format-agnostic representation so one runtime handles all schemas.
