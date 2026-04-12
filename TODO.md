# TODO

## Active

- [ ] Fix softfloat=OFF build (SIMD stubs referenced unconditionally) so JIT backends can run pzam-compile.wasm
- [ ] Backend stress test: benchmark pzam-compile.wasm across all engines (see `plans/backend-stress-test.md`)
- [ ] Investigate wasm-ld LTO (`-flto`) for further pzam-compile.wasm size reduction

## Pending

- [ ] LLVM softfloat integration (Phase 4c) — LLVM backend emits softfloat calls instead of native float ops
- [ ] Regenerate spec tests from official WASM test suite
- [ ] Implement multiple memories
- [ ] True tail call optimization
- [ ] Audit relaxed SIMD completeness
- [ ] Fix multi-module elem tests (elem_59/60/68)
