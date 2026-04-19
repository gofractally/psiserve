---
id: psiserve-pfs
title: PFS — Planetary File System (local IPFS-compatible CAS with file-level dedup)
status: ready
priority: high
area: psiserve, psio, psitri
agent: agent0
branch: ~
created: 2026-04-19
depends_on: [psitri-chunked-file-storage]
blocks: [psiserve-user-filesystem]
---

## Name

**PFS** = IPFS minus the "I" (Interplanetary). Same content model,
same CIDs, same HTTP API surface — but local-only, no libp2p / bitswap /
network participation. The three-letter name captures the scope
exactly: planetary, not interplanetary.

## Motivation

PFS is the layer that makes files identifiable by a content hash (CID)
and enables **file-level dedup**: two different users who upload the
same bytes end up sharing one underlying copy via CID collision, while
each sees the file independently in their own filesystem namespace
above.

File-level dedup is the right granularity for real workloads (duplicate
docs, shared Docker images, photo re-uploads, snapshot replicas) without
the complexity cost of chunk-level dedup (which would require a 4B-entry
global index and per-chunk refcounts).

## Design

### Storage shape

One psitri btree entry per unique CID, keyed by CID:

```
content/<CID>  →  {
   refcount:    u32      // # of pins + filesystem entries pointing here
   size_bytes:  u64
   chunk_count: u32
   payload:     inline bytes           (if total ≤ 4MB)
              | [block_id_0, block_id_1, ...]   (if larger)
}
```

- Small files inline in the CAS entry directly (one psitri entry, one
  control_block).
- Large files carry a `block_id` list pointing at 4MB blocks allocated by
  the `psitri-chunked-file-storage` layer.
- `refcount` is an **application-level counter** maintained by this layer
  — not SAL's internal storage refcount. This preserves layering: SAL
  tracks "is this allocation still referenced by any btree node"; CAS
  tracks "how many user-visible pins reference this CID."

### Sharding

CAS entries sharded by `hash(CID)` → 256 shards, independent parallel
writers. Since uploads from different users typically produce different
CIDs (unless dedup-hit), concurrent uploads scale naturally.

### Write path

```
bytes_in → chunker → CID
if content/<CID> exists:
    atomic_increment(content/<CID>.refcount)   # dedup hit
else:
    allocate blocks via chunked-file-storage
    write content/<CID> with refcount=1
return CID
```

All under one DWAL transaction. Dedup-hit path is O(1) btree lookup +
one refcount increment — no byte copies.

### Delete path

```
on delete:
    refcount = atomic_decrement(content/<CID>.refcount)
    if refcount == 0:
        free blocks (chunked-file-storage layer)
        delete content/<CID>
```

SAL will reclaim the freed blocks on the next compaction pass.

### Read path

```
get(cid)              → look up content/<CID> → read payload
get_range(cid, a, b)  → compute chunk range → read only those chunks
```

HTTP Range requests map directly to chunk ranges via the underlying
chunked-file-storage layer.

## HTTP API (IPFS-compatible subset)

The CAS directly backs these IPFS HTTP RPC endpoints, served by a WASM
service running on psiserve:

```
POST /api/v0/add                  upload bytes, return root CID
GET  /api/v0/cat?arg=<cid>        stream file bytes (Range supported)
GET  /api/v0/get?arg=<cid>        download as tar
GET  /api/v0/ls?arg=<cid>         list DAG links (UnixFS introspection)
POST /api/v0/pin/add?arg=<cid>    increment refcount (pin)
POST /api/v0/pin/rm?arg=<cid>     decrement refcount (unpin)
GET  /api/v0/pin/ls               list all CIDs with refcount > 0
```

Browser-facing gateway:
```
GET /ipfs/<cid>/<subpath>         resolve + stream with Range support
```

### CID computation

Root CIDs are **byte-compatible with Kubo/go-ipfs** for the same input
bytes, using:
- Fixed-size 256KB chunker (IPFS default)
- UnixFS v1 protobuf (dag-pb) codec for chunk trees
- SHA-256 multihash, CIDv1 base32

This is the main value of "being IPFS-compatible" — stock clients
(`ipfs-http-client`, Kubo CLI, browser extensions) work unchanged against
our endpoints.

## Explicit Non-goals

- **libp2p / Bitswap**: no network participation. File separately if ever.
- **IPNS / DNSLink / key management**: defer.
- **Multi-tenant namespaces**: belong in `psiserve-user-filesystem`,
  not here. CAS is single-flat-namespace by CID.
- **Chunk-level dedup**: explicitly rejected — file-level dedup captures
  real-world savings at a fraction of the complexity.
- **CAR / CARv2 export/import**: nice to have, defer.

## Acceptance Criteria

- [ ] CAS keyed by CID with refcount, size, chunk_count, inline-or-
      block-list payload
- [ ] Uploading identical bytes twice produces one CAS entry with
      refcount=2 (dedup works)
- [ ] CID byte-identical to Kubo for fixed-size 256KB chunker + UnixFS v1
- [ ] `GET /api/v0/cat?arg=<cid>` returns exact uploaded bytes
- [ ] HTTP Range on `/ipfs/<cid>` reads only chunks covering the range
- [ ] Pin/unpin round-trip: refcount increments, blocks freed only when
      refcount hits 0
- [ ] Standard IPFS HTTP client works as a drop-in for these endpoints
- [ ] Concurrent uploads of different CIDs to different CID-shards show
      no lock contention
- [ ] WASM service wrapper runs under psiserve with fiber-per-request

## Notes

- Sits directly on `psitri-chunked-file-storage` for block storage.
- Blocks `psiserve-user-filesystem` (multi-tenant paths/quotas/permissions).
- Refcount semantics are independent of SAL's storage refcount — this is
  the application-visible, consensus-deterministic, snapshotable counter.
