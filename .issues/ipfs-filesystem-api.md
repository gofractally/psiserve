---
id: ipfs-filesystem-api
title: IPFS filesystem API + WASM endpoint on top of chunked file storage
status: backlog
priority: medium
area: psiserve, psio, psitri
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psitri-chunked-file-storage]
blocks: []
---

## Motivation

Build IPFS HTTP API compatibility over the psitri chunked-file-storage
layer so psiserve-hosted WASM services can accept file uploads and serve
files by CID or path. This is the "file-serving shaped" counterpart to the
psitri DB API exposed to WASM.

We target **IPFS HTTP API compatibility** (Kubo RPC + gateway endpoints),
**not** full IPFS-network participation (no libp2p, no bitswap). The goal
is a useful local blockstore that speaks the same API as IPFS nodes, so
standard tooling and clients work unchanged.

## Scope

### Blockstore (C++ library on top of psitri-chunked-file-storage)
- `put(bytes) -> cid` — chunk, compute CID, store under a filepath
- `get(cid) -> bytes` — resolve CID → filepath via the name↔cid index,
  read chunks from psitri
- `has(cid) -> bool`
- `delete(cid)` — unlink the filepath and free its blocks
- `get_chunk(filepath, chunk_index) -> bytes` — raw chunk read for Range

Every file root has a CID; the CID→filepath map is a small secondary
btree (one entry per file, not per chunk) living inside psitri.

### Chunker
- Fixed-size 256KB (IPFS default).
- Optional Rabin rolling-hash chunker for workloads with shifted content
  (deferred to follow-up if not needed for v1).

### UnixFS DAG codec
- Encode file metadata + chunk list as a UnixFS-v1 protobuf (dag-pb).
- Emit a root CID that is **byte-compatible with Kubo** for the same
  input bytes and the same chunker parameters.
- Decode UnixFS protobuf on read for gateway traversal.

This byte-level compatibility is the main value of "being IPFS" — it
constrains the chunker and codec choices but is non-negotiable for
interop.

### MFS overlay (mutable path-based view)
- `files_write(path, bytes)`, `files_read(path)`, `files_ls(path)`,
  `files_mkdir`, `files_rm`
- MFS root CID tracked in a dedicated psitri key; updated atomically
  per-write through the normal DWAL path.

### Pinset (GC substitute)
- Each pinned root CID bumps the refcount of its file_node's blocks.
- Unpin decrements; file_node and its blocks are freed when refcount = 0.
- No mark-and-sweep GC needed — refcounting on the file_node covers it.

### HTTP API surface (WASM service speaking psiserve host API)
Minimum viable endpoints:

```
POST /api/v0/add                      upload a file, return root CID
GET  /api/v0/cat?arg=<cid>            stream the file bytes
GET  /api/v0/get?arg=<cid>            download as tar
GET  /api/v0/ls?arg=<cid>             list DAG links
POST /api/v0/pin/add?arg=<cid>        pin
POST /api/v0/pin/rm?arg=<cid>         unpin
POST /api/v0/files/write              MFS write
GET  /api/v0/files/read?arg=<path>    MFS read
GET  /api/v0/files/ls?arg=<path>      MFS ls
POST /api/v0/files/mkdir              MFS mkdir
POST /api/v0/files/rm                 MFS rm
```

Gateway endpoint for browser clients:
```
GET /ipfs/<cid>/<subpath>             resolve + stream (Range supported)
```

HTTP Range support via `(filepath, chunk_index)` addressing — compute
start/end chunk indices from the Range header, read only those chunks.

## Explicit Non-goals (v1)

- **libp2p / bitswap**: no participation in the public IPFS network.
  File as `ipfs-network-participation` if and when needed.
- **IPNS / DNSLink / key management**: defer.
- **Alternative hash functions beyond SHA-256**: multihash support reads
  any hash on fetch but writes only SHA-256.
- **CAR / CARv2 export/import**: nice to have, not required for v1.

## Acceptance Criteria

- [ ] Uploading a file via `POST /api/v0/add` produces a root CID that is
      bit-identical to what Kubo produces for the same bytes (fixed-size
      256KB chunker)
- [ ] `GET /api/v0/cat?arg=<cid>` returns identical bytes for any CID we
      stored
- [ ] HTTP Range requests on `/ipfs/<cid>/<path>` read only the chunks
      covering the requested byte range
- [ ] MFS round-trip: `files_write(path, X)` then `files_read(path)` = X;
      `files_ls` shows the expected structure
- [ ] Pin prevents deletion; unpin + no other refs → blocks freed
- [ ] A standard IPFS client (e.g., `ipfs-http-client` npm) can use our
      endpoint as a drop-in for the targeted subset of endpoints
- [ ] WASM service wrapping this API runs under psiserve and handles
      concurrent uploads/downloads via fibers

## Notes

- Depends on `psitri-chunked-file-storage` for the storage layer.
- The WASM service code lives alongside other psiserve-hosted services
  in `services/ipfs/` (or similar).
- Because there's no content-addressed dedup at the storage layer, same
  bytes uploaded under two different "adds" are stored twice — by design.
