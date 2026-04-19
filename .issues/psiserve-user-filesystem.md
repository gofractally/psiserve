---
id: psiserve-user-filesystem
title: Multi-tenant user filesystem (paths, directories, quotas, permissions → CIDs)
status: backlog
priority: medium
area: psiserve, psio, psitri
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psiserve-pfs]
blocks: []
---

## Motivation

Layer a human-facing filesystem on top of PFS (the content-addressable store).
Each tenant (user, account, organization) gets their own private namespace
with paths, directories, permissions, and a storage quota. From the
tenant's point of view, files are named and owned; the fact that two
tenants storing identical bytes end up pointing at one underlying CID is
invisible — each tenant "thinks they have the whole file."

This is where file-level dedup actually happens in practice: the
filesystem layer creates and destroys references to PFS CIDs; PFS
manages the refcount and underlying storage.

## Design

### Per-tenant namespace

Each tenant has a private filesystem tree. Entries are directories or
files:

```
fs/<tenant_id>/<path>    →  {
   type:       file | directory
   (file)    cid:         CID              // reference into CAS
             size_bytes:  u64              // = CAS entry's size (cached for quota)
             mode:        permission bits
             owner:       user_id          // within tenant
             mtime:       timestamp
   (dir)     permissions: permission bits
             owner:       user_id
             children:    (implicit — entries under this prefix)
}
```

### Sharding

Filesystem entries sharded by `hash(tenant_id)` → 256 shards. A single
tenant's entire namespace lives in one shard for prefix-scan locality
(listing a directory = prefix scan within one shard, O(count)).

If a single tenant gets pathologically large, this could be refined to
`hash(tenant_id, top-level-dir)`, but uniform tenant-sharding is the v1
assumption.

### Write path

```
files_write(tenant, path, bytes):
    cid = cas.put(bytes)                         # may hit dedup
    old = fs[tenant][path]
    fs[tenant][path] = { cid, size, mode, ... }
    quota_used[tenant] += bytes - (old.size if old else 0)
    enforce: quota_used[tenant] <= quota_limit[tenant]
    if old exists:
        cas.unpin(old.cid)                       # may hit refcount==0 and free
```

Both the CAS `put` and the filesystem entry update happen in a single
DWAL transaction across the two involved shards.

### Read path

```
files_read(tenant, path):
    entry = fs[tenant][path]
    check_permissions(user, entry)
    return cas.get(entry.cid)

files_read_range(tenant, path, start, end):
    entry = fs[tenant][path]
    return cas.get_range(entry.cid, start, end)
```

### Quotas

Each tenant carries a quota in a tenant-metadata entry:

```
fs/<tenant_id>/$meta  →  {
   quota_limit: u64
   quota_used:  u64      // logical size of all files in tenant's tree
   ...
}
```

- Quota is charged the **logical** file size (what the tenant sees).
- Actual storage can be much smaller due to CAS dedup.
- Admin dashboards report both `sum(quota_used)` and `actual_disk_bytes`
  to show the dedup factor.

Quota updates are part of the same transaction as the filesystem write.

### Permissions

Unix-style bits (owner/group/other × read/write/execute) plus:
- Per-entry `owner` (user_id within the tenant)
- Per-tenant ACL for cross-user sharing within the tenant
- Cross-tenant sharing requires an admin action that creates a new
  filesystem entry in the destination tenant pointing at the same CID

### Snapshots and cheap clones

A tenant snapshot =
- One psitri btree-subtree-copy of the tenant's filesystem namespace
  (structural sharing — almost free)
- Refcount bumps on every CID reachable under that subtree (linear in
  file count, not byte count)

Cross-tenant share of a file = one filesystem entry write + one CID
refcount bump. No bytes moved.

## API Surface

Two API styles, both served by a WASM service on psiserve:

### Native filesystem API
```
PUT   /fs/<tenant>/<path>            write file
GET   /fs/<tenant>/<path>            read file (Range supported)
DELETE /fs/<tenant>/<path>           delete file
POST  /fs/<tenant>/<path>?mkdir      make directory
GET   /fs/<tenant>/<path>?ls         list directory
POST  /fs/<tenant>/<path>?chmod      change permissions
GET   /fs/<tenant>/$quota            get quota info
POST  /fs/<tenant>/$snapshot         create snapshot
```

### IPFS MFS-compatible API (optional)
Mount a single tenant's namespace as "/" under the IPFS MFS API, so
`ipfs files ls /foo` works against this layer with a tenant authenticated
via the HTTP session:
```
POST /api/v0/files/write
GET  /api/v0/files/read?arg=<path>
GET  /api/v0/files/ls?arg=<path>
POST /api/v0/files/mkdir
POST /api/v0/files/rm
```

## Acceptance Criteria

- [ ] Per-tenant filesystem namespace with directories, paths, and files
- [ ] File entries reference CIDs in the CAS; write path routes through
      `cas.put` with dedup
- [ ] Delete path unpins CAS; storage freed only if CAS refcount hits 0
- [ ] Per-tenant quota enforced on write, charged on logical bytes
- [ ] Directory listings via prefix scan within a tenant's shard
- [ ] Unix-style permissions enforced on read/write/ls
- [ ] Cross-user-within-tenant sharing via ACL works
- [ ] Cross-tenant sharing via admin action works with zero byte copies
- [ ] Snapshot creates a O(file-count) new entry tree with refcount bumps
- [ ] IPFS MFS API works for single-tenant mode for drop-in Kubo CLI use
- [ ] Two tenants storing identical bytes → actual disk usage ≈ single copy,
      each tenant's `quota_used` charges full size

## Notes

- Sits on top of `psiserve-pfs`.
- Completely independent sharding dimension from PFS (filesystem sharded
  by tenant, PFS sharded by CID) — they compose cleanly.
- Quota accounting is at the application layer, separate from psitri's
  internal allocator accounting.
