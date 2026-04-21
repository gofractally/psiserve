---
id: psizam-memory64-not-enforced
title: "memory64: addresses silently clamp to 32 bits, no >4GB support"
status: open
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-21
depends_on: []
blocks: []
---

## Summary

memory64 WASM modules parse correctly but memory access silently clamps
64-bit addresses to 32 bits. The JIT (`x86_64.hpp:6601`) shifts the
i64 address right by 32 and traps if any bits remain -- effectively
enforcing a 4GB cap identical to memory32. The interpreter
(`interpret_visitor.hpp:171`) follows the same path through 32-bit
`current_linear_memory()`. memory64 modules get guard-page safety by
accident (the low 32 bits index into the same 4GB guarded region),
but any module expecting >4GB linear memory will silently get wrong
behavior or spurious traps.

The `backend.hpp` feature table already documents this:

```
// MEMORY64
//   i64 addressing               red     red       red        red       red      red
```

## What existing engines do

### The guard-page trick (memory32 only)

All major engines (V8, SpiderMonkey, Wasmtime) use the same optimization
for memory32 on 64-bit hosts: reserve 4GB + guard pages of virtual
address space per instance. Since a 32-bit pointer physically cannot
exceed 4GB, any OOB access lands in the guard region and hardware-traps.
**Zero software bounds checks on every load/store.** This is why memory32
is fast.

### memory64: software bounds checks, mandatory

memory64 breaks this trick. A 64-bit pointer can address the entire
host address space -- no amount of guard pages can fence it. All major
engines fall back to explicit software bounds checks:

**SpiderMonkey** (Firefox 134+, shipped Jan 2025): emits `cmp` + `jb` +
`ud2` before every memory access in memory64 mode:

```asm
movq 0x08(%r14), %rax       ;; load memory size from instance
cmp %rax, %rdi              ;; compare address to limit
jb .load                    ;; if in bounds, jump to load
ud2                         ;; trap
.load:
movl (%r15,%rdi,1), %eax    ;; load from base + offset
```

Mozilla's own analysis: 10-100% slowdown vs memory32 depending on
workload. This is fundamental to the architecture, not a lack of
optimization. (Source: spidermonkey.dev/blog/2025/01/15/is-memory64-
actually-worth-using.html)

**V8** (Chrome 133+, shipped Jan 2025): same approach -- explicit bounds
checks for memory64, guard-page elimination for memory32.

**Wasmtime** (enabled by default Jan 2025, after phase 4 in Nov 2024):
configurable memory strategies, but memory64 always requires bounds
checks. Cannot use the virtual memory guard optimization.

### Consensus

"The only reason to use Memory64 is if you actually need more than
4GB of memory." -- Mozilla SpiderMonkey team. Every engine pays the
bounds-check cost for memory64. There is no hardware shortcut.

## The deeper issue: checked mode unification

memory64 bounds checking and memory32 checked mode are the same
operation:

- memory32 checked: `if (addr + size > limit) trap;` where addr is u32
- memory64 checked: `if (addr + size > limit) trap;` where addr is u64
- wasm16 (custom): addr is u16, limit is 64KB -- no check needed

The code paths should unify. psizam's checked memory mode
(`mem_safety::checked`) already does software bounds checking for
memory32. memory64 support is just widening the address operand in
that same path. The guarded mode is a memory32-only optimization that
becomes unavailable at 64-bit.

## Address space granularity for checked mode

For sub-instance allocation (child instances using slices of a parent's
guarded address space), the checked-mode allocator should support
power-of-2 address reservations rather than full 4GB:

- 64KB (1 page -- wasm16 sweet spot, zero bounds-check cost)
- 1MB (16 pages)
- 4MB (64 pages)
- 16MB, 64MB, etc.

The child's `memory.grow` bumps the bounds-check limit within its
reserved slice. No mmap, no mprotect, no kernel interaction after
the parent's initial guarded allocation. Just a bump allocator
handing out offsets within the parent's already-committed region.

## Deferred watermark vs per-load bounds check

psizam's checked mode uses a **deferred read watermark**: each load
does a branchless `max(watermark, addr + size)` (CSEL on aarch64,
umax intrinsic on LLVM). Zero conditional branches per read. At
checkpoint boundaries (loop back-edges, host calls, gas charges),
one single check validates the watermark against mem_size. Writes
use immediate per-access bounds checks (side effects can't be
deferred).

SpiderMonkey, V8, and Wasmtime use **per-load bounds checks**: `cmp`
+ conditional branch + trap before every load and store. One branch
per memory access.

Tradeoff summary:

- **Watermark**: fewer branches in hot loops (reads are 2-4x more
  frequent than writes in typical WASM). Lower branch predictor
  pressure, smaller code size. Cost: OOB read traps at next
  checkpoint rather than immediately (but before any observable side
  effect).

- **Per-load**: immediate trap, fully spec-compliant. But every load
  adds a predicted-taken branch that consumes BTB entries and
  increases code size.

The delayed-trap semantics are acceptable for a server runtime where
we control the trust model. Browsers need per-load checking for spec
compliance. Both modes should be available behind a flag so we can
benchmark the actual difference on real workloads.

### Action: benchmarking flag

Add a `checked_mode` flag (already partially exists as
`checked_mode::strict` vs `checked_mode::relaxed`) to also select
between deferred-watermark reads and per-load bounds checks. The
LLVM backend already has `emit_read_watermark_update` (deferred) and
`emit_read_bounds_check` (per-load) -- wire both to the flag so
`bench_compare` can measure the difference across workloads.

## Sub-instance memory allocation

A guarded parent process (blockchain.wasm, 4GB+ reserved) can host
sub-instances (contracts, UI modules) by allocating their linear
memory from the parent's own address space. No mmap, no mprotect,
no kernel interaction.

The sub-instance uses checked-mode bounds checking against its slice.
The parent does `malloc` (or bump-allocate) from its already-committed
region, passes the base pointer and max size to the host, which
constructs a `wasm_allocator` (or equivalent) backed by that slice.

Host-side per-instance overhead (instance_impl + instance_be_impl +
execution context) is ~4-16KB -- trivially small compared to even
the minimum 64KB linear memory allocation. The dominant cost is the
linear memory itself, which comes from the parent's budget.

This means:
- Sub-instance count is bounded by the parent's address space budget,
  not by a separate instance counter
- `destroy()` returns the slice to the parent's free list
- No kernel VMA churn regardless of how many contracts are spawned
  and torn down per block
- The `wasm_allocator` for sub-instances needs a mode where it takes
  a pre-allocated (base, max_size) pair instead of calling mmap

## What to do

1. Document memory64 as "parses but not functionally supported beyond
   4GB" in KNOWN_ISSUES.md -- the current silent 32-bit clamp is a
   spec violation (should either trap or work correctly, not silently
   wrap)

2. Unify checked-mode bounds checking for memory32 and memory64 --
   same code path, parameterized on address width. This is equivalent
   work to what SpiderMonkey and V8 do for their memory64 support.

3. Support power-of-2 address space reservations in the checked
   allocator for sub-instance use cases (children in parent's
   guarded region)

4. Consider whether the current 32-bit clamp should be a parse-time
   rejection (`memory64 not supported`) rather than silent truncation
   until real support lands

5. Add benchmarking flag to compare deferred-watermark vs per-load
   bounds checking across real workloads (the infrastructure for both
   paths already exists in the LLVM backend)

6. Add `wasm_allocator` mode that accepts a pre-allocated (base,
   max_size) pair for sub-instance use, bypassing mmap entirely

## Spec reference

WebAssembly memory64 proposal (phase 4, Nov 2024): addresses are i64,
`memory.grow` takes i64, `memory.size` returns i64. Linear memory can
exceed 4GB. All load/store instructions use i64 addresses. OOB access
traps. JS API caps at 16GB.
