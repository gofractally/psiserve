---
id: "0012"
title: psiserve WIT host/guest API for database access
status: backlog
priority: high
area: psiserve, psio
agent: ~
branch: ~
created: 2026-04-19
depends_on: ["0002", "0011"]
blocks: ["0013", "0014"]
---

## Description
Define and implement the WIT interface that wasm guests use to access psitri
databases through psiserve. Guests must never see raw subtree IDs (cycle risk).

## Proposed Guest API Shape
```
dbRef    = openDb("webserver.db")
tableRef = dbRef.createTable("users") / dropTable("users")
txRef    = tableRef.startTransaction()
txRef.insert / upsert / remove / update / removeRange
txRef.get / find / lowerBound / upperBound  →  curRef
curRef.valid / next / prev / key / value / begin / end / front / back
subTxRef = txRef.startSubTransaction()
subTxRef.abort / commit
txRef.abort / commit
```

## Acceptance Criteria
- [ ] WIT interface file authored and reviewed
- [ ] Host implementation in psiserve (db_host.hpp wired up)
- [ ] Guest bindings generated
- [ ] Permission model: RO/RW per virtual database per wasm process
- [ ] Resource accounting: key count, key bytes, value bytes per table
- [ ] Table listing/iteration API
