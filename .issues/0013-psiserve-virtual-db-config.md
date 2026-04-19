---
id: "0013"
title: psiserve virtual database config and permissions
status: backlog
priority: medium
area: psiserve
agent: ~
branch: ~
created: 2026-04-19
depends_on: ["0012"]
blocks: []
---

## Description
Config file support for naming virtual databases (top-roots), assigning RO/RW
access rights per wasm process, and controlling which tables each process
can see.

## Example Config Shape
```
Root[0] = "webserver.db"
  permissions:
    "auth-service": RW
    "readonly-api": RO
  tables: [users, permissions, sessions]
```

## Acceptance Criteria
- [ ] Config file format designed and documented
- [ ] psiserve reads config at startup and enforces access rights
- [ ] Wasm process cannot access databases/tables not in its permission list
- [ ] Virtual database names mapped to numeric top-root indices internally
