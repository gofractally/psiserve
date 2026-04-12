# TODO

## Active

- [ ] Fix JIT2 runtime crash on complex modules (compiles OK but SEGV at ~5.7MB offset; pre-existing codegen bug, not branch range)
- [ ] Investigate wasm-ld LTO (`-flto`) for further pzam-compile.wasm size reduction

## Pending

- [ ] LLVM softfloat integration (Phase 4c) — LLVM backend emits softfloat calls instead of native float ops
- [ ] Regenerate spec tests from official WASM test suite
- [ ] Implement multiple memories
- [ ] True tail call optimization
- [ ] Audit relaxed SIMD completeness
- [ ] Fix multi-module elem tests (elem_59/60/68)

## Done

- [x] Fix JIT1 branch-out-of-range on large modules — veneer islands + long-form conditional branches
- [x] Fix JIT2 branch-out-of-range on large modules — veneer islands + long-form conditional branches + separate scratch allocator
- [x] Backend stress test: benchmark pzam-compile.wasm across all engines (see `plans/backend-stress-test.md`)
