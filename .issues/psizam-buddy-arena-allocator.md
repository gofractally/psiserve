---
id: psizam-buddy-arena-allocator
title: Lock-free buddy arena allocator for checked-mode sub-instances
status: backlog
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-21
depends_on: []
blocks: []
---

## Summary

Replace the current malloc+budget approach for checked-mode sub-instance
memory with a lock-free buddy allocator over a pre-reserved virtual
address region. Eliminates malloc overhead and guarantees power-of-2
aligned allocation with O(1) find-first-free via hierarchical bitmaps.

## Current approach (PoC)

`aligned_alloc` + `free` with an `atomic<size_t>` budget counter.
Simple, correct, but relies on the system allocator for alignment and
doesn't avoid mmap churn (the system allocator may mmap/munmap behind
the scenes for large allocations).

## Target design

### Structure

One contiguous `mmap(PROT_NONE)` region (e.g. 16–32GB). Per-level
bitmaps track allocation state at each power-of-2 granularity:

```
Arena = 32GB, min block = 64KB (order 16), max = 32GB (order 35)
20 levels of bitmaps, ~128KB total metadata, fits in L2 cache
```

### Per-level bitmap

One bit per aligned block at that level. 1 = allocated, 0 = free.
All operations are `fetch_or` / `fetch_and` / `CAS` on `atomic<uint64_t>`
words.

### Hierarchical summary (O(1) find-first-free)

Per-level summary word: one bit per group of 64 bitmap words. Set = at
least one free block in this group. Updated via `fetch_xor` on
transitions:

- Free: if `fetch_and` on bitmap returns `old == ~0ULL` (we freed the
  first block in a full group), `fetch_xor` the summary bit.
- Alloc: if `fetch_or` on bitmap produces `new == ~0ULL` (we allocated
  the last block in a group), `fetch_xor` the summary bit.

XOR is order-independent: exactly one thread observes each full↔has-free
transition, so the parity is always correct.

### Buddy merge on free

```
free(ptr, order):
  1. CAS clear bit at order                    (we own it)
  2. Load buddy's bit at order
  3. If buddy free (0): CAS claim buddy (0→1)
     If CAS succeeds: we own both halves
       Clear both bits at order
       Recurse: free(merged_ptr, order+1)
     If CAS fails: buddy was just allocated, no merge
  4. If no merge: block stays free at current order
```

### Ambiguity problem: 0 = free OR split?

A zero bit at level N could mean "free 128KB block" or "split into two
64KB blocks managed at level N-1." Solutions explored:

- **Two bits per block** (free/allocated/split): doubles metadata,
  complicates atomics
- **Alloc checks children**: after claiming at level N, verify both
  children at N-1 are not independently allocated. Rollback if split.
- **XOR-parity single bitmap** (classic buddy): parent bit = XOR of
  children's states. Hard to reason about with concurrent atomics.

Recommended: alloc-checks-children. Two extra loads per alloc (same
cache line for nearby blocks), fully lock-free.

### No free lists

All operations are bitmap scans. No in-band pointers, no ABA problem,
no pointer chasing. Alloc = scan bitmap → CAS. Free = CAS → merge check.

## Acceptance criteria

- [ ] Lock-free buddy allocator with per-level atomic bitmaps
- [ ] Hierarchical summary for O(1) find-first-free per level
- [ ] Buddy merge on free with CAS-based claiming
- [ ] Thread-safe: concurrent alloc/free from multiple OS threads
- [ ] Unit tests: concurrent alloc/free stress test
- [ ] Benchmark vs malloc for typical contract workloads (many 64KB–4MB
      alloc/free cycles)
