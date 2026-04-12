# TODO

## Active

- [ ] Fix JIT2 branch-out-of-range on large modules (23k+ funcs exceed +-128MB aarch64 B/BL displacement; needs veneer islands or code partitioning)
- [ ] Fix JIT1 softfloat: wire up float JIT codegen trampolines for softfloat mode, or fix softfloat=OFF build (SIMD stubs referenced unconditionally)
- [ ] Backend stress test: benchmark pzam-compile.wasm across all engines including wasmer (see `plans/backend-stress-test.md`)
- [ ] Investigate wasm-ld LTO (`-flto`) for further pzam-compile.wasm size reduction

## Pending

- [ ] LLVM softfloat integration (Phase 4c) — LLVM backend emits softfloat calls instead of native float ops
- [ ] Regenerate spec tests from official WASM test suite
- [ ] Implement multiple memories
- [ ] True tail call optimization
- [ ] Audit relaxed SIMD completeness
- [ ] Fix multi-module elem tests (elem_59/60/68)
