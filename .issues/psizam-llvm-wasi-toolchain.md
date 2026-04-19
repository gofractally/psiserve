---
id: psizam-llvm-wasi-toolchain
title: psizam LLVM-WASI toolchain (ThinLTO, mimalloc, web-IDE)
status: in-progress
priority: medium
area: psizam
agent: psiserve-agent-psio
branch: main
created: 2026-04-19
depends_on: []
blocks: []
---

## Description
Upgrade the psizam LLVM/WASM compilation pipeline: add ThinLTO support,
optional mimalloc allocator override, and an in-browser web IDE for
C/C++ → .wasm authoring and running.

## Acceptance Criteria
- [x] ThinLTO + mimalloc override in llvm-wasi CMakeLists
- [x] Web IDE (in-browser C/C++ → .wasm editor + runner)
- [ ] Dirty files committed: `cmake/llvm-wasi/CMakeLists.txt`, `libraries/psizam/` headers and `llvm_aot_compiler.cpp`, `tools/pzam_compile.cpp`, `tools/pzam_run.cpp`

## Dirty Files (agent-psio working set)
- `cmake/llvm-wasi/CMakeLists.txt`
- `libraries/psizam/include/psizam/allocator.hpp`
- `libraries/psizam/include/psizam/detail/ir_writer.hpp`
- `libraries/psizam/include/psizam/detail/ir_writer_llvm_aot.hpp`
- `libraries/psizam/include/psizam/detail/jit_reloc.hpp`
- `libraries/psizam/include/psizam/detail/llvm_aot_compiler.hpp`
- `libraries/psizam/include/psizam/pzam_typed.hpp`
- `libraries/psizam/src/llvm_aot_compiler.cpp`
- `libraries/psizam/tools/CMakeLists.txt`
- `libraries/psizam/tools/pzam_compile.cpp`
- `libraries/psizam/tools/pzam_run.cpp`
