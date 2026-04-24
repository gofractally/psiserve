---
id: psizam-memory16-design
title: "memory16: zero-overhead memory sandboxing for 64KB WASM instances"
status: consideration
priority: medium
area: psizam
agent: ~
branch: ~
created: 2026-04-20
depends_on: [psizam-memory-sandboxing-modes]
blocks: []
---

## Summary

Standard wasm32 with `max_pages = 1` enables a fourth sandboxing mode: flat
64KB memory with zero per-access bounds checks, zero guard pages, zero kernel
VMAs, and zero `mprotect` calls. The `movzwl` instruction (16-bit zero-extend)
replaces the existing `mov eax, eax` (32-bit zero-extend) at the same cost.
Parse-time offset validation (`max_memory_offset ≤ 0xFFFF`) completes the
static safety proof.

Primary use case: smart contract execution where millions of instances run
concurrently and 64KB of linear memory is sufficient.

## Key Insight

Any wasm32 module declaring `max_pages ≤ 1` has a 64KB address space. A 16-bit
address can only reach 0x0000–0xFFFF. If the JIT emits `movzwl` to truncate the
i32 address to 16 bits, and the parser rejects modules with `memarg` offsets >
0xFFFF, then:

    max effective address = 0xFFFF + 0xFFFF = 0x1FFFE

Map 128KB per instance (64KB memory + 64KB padding). Every possible access
lands within the mapped region. No guard pages. No bounds checks. No branches.

## Comparison with Existing Modes

| Property | guarded | checked | unchecked | **memory16** |
|----------|---------|---------|-----------|-------------|
| Per-read cost | 0 | 1-2 instr | 0 | **0** |
| Per-write cost | 0 | 3 instr | 0 | **0** |
| VA per instance | 8GB | max_pages×64KB | max_pages×64KB | **128KB** |
| Kernel VMAs | 1 per instance | 1 per arena | 1 per arena | **1 total** |
| mprotect calls | per-grow | 0 | 0 | **0** |
| OOB behavior | trap (SIGSEGV) | trap (software) | undefined | **wrap** |
| Requires guard pages | yes (huge) | no (arena) | no | **no** |

## Pool Allocator Design

Single contiguous `mmap` for all instances. Zero per-instance kernel state.

```
pool = mmap(NULL, num_instances * 128KB, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0)

Instance layout (128KB stride):
  [0, 64KB)     = linear memory
  [64KB, 128KB) = padding (absorbs offset overflow, never read back)
```

- One VMA for the entire pool, regardless of instance count
- Instance reset: `memcpy` 64KB from snapshot (~1μs)
- Dirty detection: `memcmp` 64KB (~1μs)
- No `madvise`, no `mprotect`, no page faults in steady state

## JIT Changes per Backend

The address truncation replaces the existing i32→i64 zero-extend. Net new
instructions: zero. Net instruction changes: one opcode swap.

### x86_64 (jit, jit2, LLVM)

Before (wasm32):
```asm
pop rax
mov eax, eax           ; zero-extend i32 → i64
mov edx, [rsi + rax + offset]
```

After (memory16):
```asm
pop rax
movzwl %ax, %eax       ; zero-extend i16 → i64 (same cost)
mov edx, [rsi + rax + offset]
```

### aarch64

Before: `MOV W0, W0` (32-bit truncate)
After:  `UXTH W0, W0` (16-bit truncate, same single-cycle instruction)

### Interpreter

`compute_effective()`: `return static_cast<uint64_t>(op.offset) + (ptr.to_ui32() & 0xFFFF);`

## Bulk Memory Operations

`memory.fill`, `memory.copy`, `memory.init` are runtime helper function calls,
not JIT-emitted loads/stores. The `movzwl` does not apply. These already
bounds-check via `dest + count ≤ mem_size`. One comparison per bulk call,
negligible vs call overhead.

## Wrapping vs Trapping

Standard WASM requires OOB access to trap. With `movzwl`, address `0x50000`
silently wraps to `0x0000`. This is a semantic deviation.

**Why it's acceptable for smart contracts:**
- Wrapping is deterministic — all validators compute identical results
- Sandbox isolation is preserved — can't escape 128KB stride
- Correct programs never trigger wrapping (addresses provably ≤ 0xFFFF)
- Buggy contracts compute wrong results, same as any logic error
- Chain consensus is unaffected

**Optional: deploy-time static analysis.** Scan WASM bytecode at contract
deployment. If the module was compiled with 16-bit pointers, all pointer loads
use `i32.load16_u` / `i32.store16`. A dataflow pass can prove no address
exceeds 16 bits — reject modules that can't be statically proven safe. Zero
runtime cost, formal safety guarantee.

## Optional: OR-Accumulator Trap Detection

If trap-equivalent semantics are desired without guard pages, add an OR
accumulator (same pattern as checked-mode relaxed watermark):

```asm
pop rax
or r15, rax              ; accumulate high bits (1 cycle, pipelined)
movzwl %ax, %eax         ; truncate
mov edx, [rsi + rax + offset]
```

Check at host call boundaries and function return:
```asm
test r15d, 0xFFFF0000
jnz wrap_trap
```

Cost: one `or` per access + one `test+jnz` per host call. Same overhead as
checked-mode relaxed reads. Could be offered as a premium tier between
memory16 (wrapping) and checked (full bounds).

## 16-bit Pointer Compiler (optional, future)

Standard wasm32 with 32-bit pointers works with memory16 today. A 16-bit
pointer compiler is an optional optimization for storage density:

| | 32-bit ptrs | 16-bit ptrs |
|---|---|---|
| `sizeof(void*)` | 4 | 2 |
| `struct node { int val; node* next; }` | 8 bytes | 6 bytes |
| Max nodes in 64KB | ~8,000 | ~10,900 (+36%) |

**Toolchain options (increasing effort):**
1. wasm32-to-wasm16 binary translator (~2-4 weeks)
2. Fork wasi-libc for 16-bit `size_t` (~2-4 weeks)
3. LLVM wasm16 backend (16-bit `DataLayout`) (~2-4 months)

LLVM already supports 16-bit pointers for AVR/MSP430 targets
(`getArchPointerBitWidth() = 16` in `Triple.cpp`). Raw C compiles cleanly
with 16-bit pointers to WASM — `i32.load16_u`/`i32.store16` for pointer
ops, full i32 arithmetic on the operand stack. C++ STL has issues with
`sizeof(int) > sizeof(void*)` but smart contracts don't use STL.

## Implementation Cost

### Engine changes (~300 lines across ~12 files)

| Component | File(s) | Estimate |
|-----------|---------|----------|
| Parser: `is_memory16` flag, offset validation | `parser.hpp` | ~40 lines |
| Types: `is_memory16` in `memory_type` | `types.hpp` | ~10 lines |
| Constants: `max_memory_16`, `max_pages_16` | `constants.hpp` | ~5 lines |
| Pool allocator: contiguous flat allocation | `allocator.hpp` | ~60 lines |
| Interpreter: `& 0xFFFF` in `compute_effective()` | `interpret_visitor.hpp` | ~15 lines |
| JIT x86_64: `movzwl` | `x86_64.hpp` | ~25 lines |
| JIT aarch64: `UXTH` | `jit_codegen_a64.hpp` | ~25 lines |
| JIT2 codegen: `movzwl` | `jit_codegen.hpp` | ~25 lines |
| LLVM translator: `and i32 %addr, 0xFFFF` | `llvm_ir_translator.hpp` | ~25 lines |
| Host functions: skip validation for memory16 | `host_function.hpp`, `execution_interface.hpp` | ~20 lines |
| Runtime helpers: truncate bulk op args | `jit_runtime_helpers.hpp` | ~20 lines |
| Interpreter IR writer | `ir_writer.hpp` | ~15 lines |

### Backends affected: 5

Each backend (interpreter, jit, jit2, jit_llvm, jit_profile) needs the
address truncation change. The change is mechanical (swap one instruction)
but must be tested per-backend.

### Estimated effort: 2-3 weeks for engine, plus optional toolchain work.

## Open Questions

- Should memory16 be a distinct `mem_safety` enum value or a separate axis
  (`address_width { bits32, bits16 }`) orthogonal to safety mode?
- Should the pool allocator use 128KB stride (absorb offset overflow) or 64KB
  stride with `max_memory_offset = 0` (require compiler to emit explicit adds)?
- Is deploy-time static analysis sufficient to guarantee safety, or should
  the OR-accumulator always be on as a defense-in-depth measure?
- Should `memory.grow` always return -1, or allow 0→1 growth (lazy allocation)?
