---
id: psitri-chunked-file-storage
title: Chunked file storage layer in psitri (256-shard, 4MB-block, direct-COW)
status: ready
priority: high
area: psitri
agent: ~
branch: ~
created: 2026-04-19
depends_on: []
blocks: [ipfs-filesystem-api]
---

## Motivation

psitri is a good fit for storing large files because of its MFU hot-cache
behavior — popular content stays resident, unlike LRU-based filesystem page
caches. This issue adds a dedicated chunked-file-storage layer that keeps
large blobs inside psitri (self-contained DB, consensus-deterministic,
backup-as-one-file) without exhausting the 4B control_block address space.

## Design Summary

### 256 top-root shards
Files are sharded by `hash(filepath)[0]` (256 virtual databases, one per
byte value). Writers to different shards proceed with zero synchronization
overhead — this is the existing psitri parallel-top-root guarantee. Reads
are O(1) to locate (hash the name, pick the root).

Shards share the same psitri instance and the same 4B control_block pool
(so the global 16 PB ceiling still holds), but write throughput scales
linearly with the number of active writer threads up to 256.

### Two-tier file representation
Each file is a single btree entry keyed by its path within its shard:

- **Inline (≤4MB)**: the file_node contains the file bytes directly.
  One control_block per file. Most files hit this path.
- **Indexed (>4MB)**: the file_node contains a list of 4MB-block refs.
  Each 4MB-block holds up to 16 × 256KB chunks.

### Logical vs physical chunk size
- **Logical chunk = 256KB** (IPFS-standard, for HTTP range addressing and
  API compatibility).
- **Physical allocation unit = 4MB** (one control_block per block, ~16
  chunks packed together). Amortizes control_block consumption.

At 16 chunks per 4MB block and 4B control_blocks, addressable capacity is
16 PB of chunked data — with fine-grained 256KB chunk-level read access.

### No content-addressed dedup
Chunks are **not** shared across files. Each 4MB block has exactly one
owner (one file, or one file-version under COW). Simplifications:

- No global CID → block_id index (no extra btree, no memory cost)
- No per-chunk refcount — one refcount per 4MB block
- No cross-shard reads — all chunks for a file live in its shard
- Compactor just defragments per-shard; never rewrites external indexes
- GC = delete file_node, free its 4MB blocks directly

**Temporal dedup comes for free via psitri's COW structural sharing**:
snapshot at block N, modify one chunk for N+1, and the new file_node
shares every unchanged 4MB block with the old version by subtree pointer.
That is the dedup case blockchain workloads actually need.

### Direct COW for large writes
Metadata (file_node root pointer updates) goes through DWAL as usual.
Large chunk writes bypass DWAL:

1. Allocate new SAL blocks for the chunks.
2. Write chunk bytes directly; fsync.
3. Only then commit the file_node metadata through DWAL.

Invariant: DWAL never points at chunks that aren't durable yet. Same
pattern ZFS uses for big writes bypassing the ZIL — write-amplification
stays proportional to data size rather than 2× (data + log).

### Addressing
Chunks within a file are addressable as `(filepath, chunk_index)`. This
enables HTTP Range requests and resumable downloads without needing a
content-addressed per-chunk namespace.

### Transparent compression via the compactor
Compression is piggybacked on work the compactor already does — writes
stay fast, compression is background work, reads pay near-zero extra cost.

- **Write path**: uncompressed. New 4MB blocks land in SAL raw.
- **Compactor**: when it repacks a block (liveness/age-driven), it also
  runs zstd/lz4 on the payload. If the ratio is poor (> ~95% of raw), it
  stores raw; otherwise it stores the compressed bytes and flips a
  compression flag in the block header.
- **Read path**: a SAL → WASM linear-memory copy is already mandatory.
  Swapping `memcpy` for `zstd_decompress` is a few-ns-per-byte step-up
  on a path that's already feeding a disk or network consumer. Net
  latency effectively unchanged.
- **Natural tiering**: hot blocks don't get compacted (MFU cache
  protects them), so hot data stays uncompressed. Cold data compresses
  on its next compaction pass. No explicit policy required.

CIDs and IPFS compatibility are unaffected — chunks are always
uncompressed before they cross the API boundary. Compression is purely a
disk-space / IO-bandwidth optimization below the chunk layer.

## Key Layout (within a shard)

```
files/<path>             → metadata + [inline bytes] or [list of block refs]
blocks/<block_id>        → 4MB block (header + up to 16 × 256KB chunks)
```

Block header holds per-slot liveness bits (for compactor) but only one
refcount (the owning file).

## Acceptance Criteria

- [ ] 256-shard filename hash routing implemented; writers to different
      shards verify no lock contention under load
- [ ] Inline ≤4MB path: file fits in one control_block end-to-end
- [ ] Indexed >4MB path: file_node contains block_id list; chunks read/write
      by `(filepath, chunk_index)`
- [ ] Direct-COW write path for large files: chunks fsync before DWAL
      metadata commit; crash tests verify no orphan metadata
- [ ] Compactor per-shard; repacks fragmented 4MB blocks without affecting
      file identities
- [ ] Temporal dedup via COW validated: snapshot → modify one chunk → new
      version shares all other 4MB blocks with old via structural sharing
- [ ] GC on file deletion frees all owned 4MB blocks immediately
- [ ] Byte-range reads translate to (chunk_index_start, chunk_index_end)
      and read only those chunks
- [ ] Compactor compresses (zstd or lz4) during repack when ratio is
      favorable; flag in block header marks compressed vs raw
- [ ] Read path decompresses into the destination memory on cache miss;
      benchmarks confirm end-to-end latency is unchanged vs uncompressed
      for both hot (cached decompressed) and cold (miss + decompress) cases

## Open Design Questions

- **4MB block header format**: how many bytes for slot liveness + chunk
  offsets + size + compression flag. Target: ≤64 bytes so a 4MB block is
  ~99.998% payload.
- **Partial-final-chunk handling**: last chunk of a file is usually <256KB.
  Inline the remainder in the file_node, or waste the slot?
- **SAL fragmentation with 4MB allocations**: check whether SAL can movably
  compact 4MB units, or whether they become pinned. If pinned, large-file
  churn could fragment the heap.
- **Shard rebalancing**: if filename hash is poorly distributed for a
  specific workload, one shard gets hot. Can we rehash without downtime?
  Likely "no, accept uniform hash assumption" for v1.
- **Compactor compression policy**: which algo (zstd-1 vs lz4), ratio
  floor threshold, age cutoff, and whether MFU cache stores compressed
  pages (one-time decompress on miss) or decompressed pages (decompress
  on every fetch). Start simple: zstd-1, skip if ratio > 0.95, decompressed
  MFU cache.

## Notes

- Supersedes the earlier design conversation around in-psitri chunking.
- Deliberately **not** including: content-addressed dedup, libp2p/bitswap,
  IPFS network participation.
- Blocks `ipfs-filesystem-api` which builds the blockstore/UnixFS/MFS/HTTP
  layer on top.
