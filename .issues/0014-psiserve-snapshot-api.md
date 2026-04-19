---
id: "0014"
title: psiserve snapshot API for blockchain state
status: backlog
priority: medium
area: psiserve, psitri
agent: ~
branch: ~
created: 2026-04-19
depends_on: ["0012"]
blocks: []
---

## Description
Snapshots must be stored in their own virtual database (Root[1] = "snapshots")
so guests cannot create reference cycles. The API must prevent guests from
directly storing subtree IDs.

## Proposed API
```
createSnapshot(dbRef, "snapshot-name")
releaseSnapshot(dbRef, "snapshot-name")
snapshotDbRef = openSnapshot(dbRef, "snapshot-name")
dbRef.restoreToSnapshot(snapshotDbRef)
```

## Acceptance Criteria
- [ ] Snapshot virtual database isolated from user databases (no cycle risk)
- [ ] createSnapshot / releaseSnapshot / openSnapshot / restoreToSnapshot implemented
- [ ] Blockchain use case validated: snapshot at block N, restore on fork
- [ ] Snapshot storage: Root[1] / "rootN-snapshot-name" → root value at snapshot time
