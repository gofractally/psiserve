---
name: Per-function allocator restructuring needed
description: growable_allocator must reset per-function IR to keep memory bounded, especially critical for WASM-hosted compilation
type: project
---

The growable_allocator is a bump allocator that never frees per-function IR nodes (104 bytes each). For a 30MB WASM module (33,761 functions), this accumulates 4GB+ of IR that's only needed one function at a time.

**Why:** When pzam-compile runs inside WASM (the bootstrap loop), WASM linear memory is bounded. Cannot rely on 16GB virtual memory. Must keep peak memory proportional to the largest single function, not all functions combined.

**How to apply:** Restructure into three lifetime tiers:
1. Module allocator — types, imports, function signatures (small, lives entire compilation)
2. Per-function temp — psizam IR nodes (reset after each function → LLVM IR translation)
3. Output accumulator — LLVM Module, native code blob, relocations (grows but compact)

In `ir_writer_llvm_aot`, translate each function's psizam IR to LLVM IR immediately after building it, then discard. Peak memory drops from "all functions' IR" to "one function's IR + LLVM Module."
