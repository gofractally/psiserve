---
id: psizam-memory-sandboxing-modes
title: psizam memory sandboxing modes (guarded / checked / unchecked)
status: ready
priority: high
area: psizam
agent: agent-arm
branch: ~
created: 2026-04-19
depends_on: [psizam-jit2-segfault-fixes]
blocks: []
---

## Description
Implement the three `mem_safety` tiers defined in `runtime-api-design.md` so
that callers can select the right memory enforcement strategy per use case.
This is the memory dimension of the sandboxing policy — the compute dimension
(gas/metering) is tracked separately in #0004.

## Design Reference
`libraries/psizam/docs/runtime-api-design.md` — `instance_policy::mem_safety`
and the three concrete usage examples (block producer, replay node, HTTP server).

## Three Modes

### `mem_safety::guarded`
OS-level guard pages via `mmap + PROT_NONE`. Zero per-access software overhead.
SIGSEGV handler converts page fault to WASM trap. Best for: few instances,
large address space available (block producer).

### `mem_safety::checked`
Software bounds check on every linear memory access via `guarded_ptr<T>`.
Works with arena pool — many instances share a single mapped region.
Best for: many concurrent fibers (HTTP server / psiserve).

### `mem_safety::unchecked`
No bounds checks. Caller asserts the module has already been validated by
consensus. Best for: trusted replay nodes where max throughput matters.

## Acceptance Criteria
- [ ] `mem_safety` enum plumbed through `instance_policy` into instantiation
- [ ] `guarded` mode: guard-page pool allocation, SIGSEGV → trap handler
- [ ] `checked` mode: `guarded_ptr` wrappers active on all linear memory accesses across all backends
- [ ] `unchecked` mode: all bounds checks compiled out when policy is unchecked
- [ ] Backend code generation respects the policy at compile time (no runtime branch)
- [ ] Unit tests for each mode: OOB access traps in guarded/checked, undefined in unchecked (documented)
- [ ] Differential fuzzer (#0005) validates guarded == checked output for same inputs

## Implementation Plan

### Design: Deferred Read + Immediate Write

**Key insight**: A bad read only matters when garbage escapes to a host call
(side effects). Pure WASM computation with garbage produces more garbage — no
harm. So defer read validation to check points (function prologues, loop
backedges). Writes must be checked per-access (OOB write corrupts other
instances in the arena).

#### Three-Layer Safety

1. **Arena boundary guard pages** — protects the HOST. One large mmap (256GB
   virtual), PROT_NONE at boundaries. Any access escaping the arena → SIGSEGV
   → trap.

2. **Per-write software bounds check** — protects OTHER INSTANCES. CMP + JA
   before every store. Prevents cross-instance corruption within the arena.

3. **Deferred read water mark** — WASM CORRECTNESS. Track max read address via
   dedicated register, validate at check points. Combined with gas check into
   a single branch via SBB+AND trick (0 extra branches).

#### Two Sub-Modes: Strict vs Relaxed

Parameterized via `checked_mode { strict, relaxed }` for perf comparison and
debugging:

**Strict** (`cmova` — 2 instructions per read):
- Exact max tracking: `cmp rax, r15; cmova r15, rax`
- Works with any page count. Exact fault address for debugging.
- Differential-fuzz validated against guarded mode.

**Relaxed** (`or` — 1 instruction per read):
- OR accumulator: `or r15, rax`
- Requires power-of-2 page count allocation (allocator rounds up).
- No false positives (valid range `[0, 2^k-1]` is closed under OR).
- Reads in padding between actual and rounded-up size return zeros (not trapped).

#### Write Path (both modes)
```asm
lea  rcx, [rax + access_size]
cmp  rcx, [rsi + mem_size_offset]
ja   on_memory_error
```

#### Combined Gas + Water Mark at Backedges (both modes)
Single branch covers gas exhaustion AND OOB detection:
```asm
cmp   r15, [rsi + mem_size_offset]   ; CF = (watermark < mem_size)
sbb   rcx, rcx                       ; rcx = -1 if OK, 0 if OOB
and   gas_reg, rcx                   ; OOB → zeros gas; OK → identity
sub   gas_reg, cost                  ; normal gas decrement
jle   combined_handler               ; ONE branch catches BOTH

combined_handler:                    ; cold path — resolve reason
  cmp  r15, [rsi + mem_size_offset]
  ja   memory_trap
  xor  r15, r15
  jmp  gas_handler
```

#### Batched OR Optimization (JIT2/LLVM)
Within basic blocks, accumulate addresses into independent temp registers,
OR pairs in parallel, merge into R15 once at block exit:
```asm
or  rax, addr1       ; pair A
or  rcx, addr2       ; pair B (parallel)
or  rax, addr3       ; pair A
or  rcx, addr4       ; pair B (parallel)
or  rax, rcx         ; merge
or  r15, rax         ; one write to accumulator per block
```

#### Arena Allocator
- Each slot = max_pages × 64KB (strict) or next_pow2(max_pages) × 64KB (relaxed)
- Instance reset via `madvise(MADV_DONTNEED)` — no memset
- `memory.grow` = update mem_size counter
- Memory size stored at fixed negative offset in prefix page (`mem_size_offset()`)

#### Performance
| Mode | Read cost | Write cost | Memory/instance |
|------|-----------|------------|-----------------|
| guarded | 0 | 0 | 8GB virtual |
| checked strict | 2 instr (cmp+cmova) | 3 instr (lea+cmp+ja) | max_pages×64KB |
| checked relaxed | 1 instr (or) | 3 instr (lea+cmp+ja) | pow2(max_pages)×64KB |
| unchecked | 0 | 0 | max_pages×64KB |

### Implementation Phases

1. **Infrastructure + Interpreter** — `options.hpp` (enums), `allocator.hpp`
   (mem_size_offset, arena allocator), `execution_context.hpp` (water mark),
   `interpret_visitor.hpp` (split pop_memop_addr into read/write)
2. **JIT x86_64** — R15 water mark, per-write check, combined gas+wm at backedges
3. **JIT aarch64** — X22 water mark, same pattern
4. **JIT2 code gen** — same + batched OR optimization
5. **LLVM backend** — same + LLVM scheduler batches ORs naturally
6. **Arena allocator + integration** — madvise zero-fill, power-of-2 rounding
7. **Differential fuzzing** — checked_strict in backend matrix, validate == guarded
8. **Bulk memory ops** — bounds validation for memory.copy/fill/init

Full plan: `.claude/plans/pure-bouncing-allen.md`

## Notes
- Start after #0006 (jit2 segfaults) — fixing memory safety bugs first avoids
  conflating implementation bugs with intentional unchecked behavior
- `guarded_ptr` skeleton already exists in `include/psizam/detail/guarded_ptr.hpp`
- Arena allocator design in `plans/arena-allocator-design.md`
