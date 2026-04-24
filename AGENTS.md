# AGENTS.md

This file provides guidance to Codex (Codex.ai/code) when working with code in this repository.

## Project Overview

psiserve is a high-performance WebAssembly TCP application server library. It minimizes data copies (targeting 2 vs. 8-10 in current psibase HTTP path) by giving WASM modules direct ownership of raw TCP byte streams via I/O ring buffers, COW instance forking, and stackful fiber concurrency.

The core WASM engine is **psizam** (rebranded from eos-vm), a C++23 header-only library in `libraries/psizam/`. The psiserve library itself is in `libraries/psiserve/` (early stage). The implementation plan is in `plans/master-plan.md` with 10 layers building up from scaffold to work-stealing.

## Build Commands

```bash
# macOS requires Homebrew Clang
export CC=$(brew --prefix llvm)/bin/clang
export CXX=$(brew --prefix llvm)/bin/clang++

# Configure and build
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug

# Build with tests enabled
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPSIZAM_ENABLE_TESTS=ON
cmake --build build/Debug

# Run all tests
cd build/Debug && ctest -j$(nproc)

# Run specific Catch2 tests
./build/Debug/bin/psizam_unit_tests "[allocator]"     # by tag
./build/Debug/bin/psizam_unit_tests "test name"       # by name
./build/Debug/bin/psizam_spec_tests "[i32]"           # spec tests by tag
```

### Key CMake Options

| Option | Default | Purpose |
|--------|---------|---------|
| `PSIZAM_ENABLE_TESTS` | OFF | Unit tests and spec tests |
| `PSIZAM_ENABLE_SOFTFLOAT` | ON | Deterministic software floating point |
| `PSIZAM_ENABLE_TOOLS` | OFF | CLI tools (psizam-interp, etc.) |
| `PSIZAM_ENABLE_BENCHMARKS` | OFF | Performance benchmarks |
| `PSIZAM_FULL_DEBUG` | OFF | Stack dumping and instruction tracing |

## Architecture

### psizam (WASM Engine) — `libraries/psizam/`

Header-only library (all in `include/psizam/`) with five backends:

- **interpreter** — pure interpreted execution (all platforms)
- **jit** — native x86_64 & aarch64 JIT
- **jit2** — two-pass JIT with IR optimization (x86_64 only)
- **jit_profile** — JIT with instruction profiling (x86_64)
- **null_backend** — validation only

Key source files:
- `backend.hpp` — main entry point, templated on host class & backend type
- `parser.hpp` — binary WASM parser
- `execution_context.hpp` — operand stack, call stack, linear memory, globals
- `interpret_visitor.hpp` — interpreter visitor for all WASM opcodes
- `x86_64.hpp` / `jit_codegen_a64.hpp` — JIT code generators
- `host_function.hpp` — type-safe C++ ↔ WASM function binding via templates
- `allocator.hpp` — guard-page-based memory sandboxing (no per-access checks)
- `softfloat.hpp` — deterministic IEEE-754 wrapper over berkeley-softfloat-3

### psiserve (Server Library) — `libraries/psiserve/`

Not yet implemented. Will provide COW snapshots, fiber scheduler, I/O ring buffers, TCP listener, and module dispatch per the layered plan in `plans/`.

## Conventions

- **C++23** with extensions, Unix-only (Linux/macOS), Windows blocked
- Assertion macro: `PSIZAM_ASSERT(cond, exception_type, msg)`
- SFINAE helpers: `PSIZAM_HAS_MEMBER`, `PSIZAM_HAS_TEMPLATE_MEMBER`
- All psizam macros prefixed with `PSIZAM_`
- Non-owning data structures — allocators manage lifetimes
- Guard paging for memory safety over runtime bounds checks
- ccache/sccache auto-detected for compiler caching
- Tests use Catch2 v2 (single-header, in `libraries/psizam/external/Catch2/`)
- External dep: softfloat in `libraries/psizam/external/softfloat/`
- See `libraries/psizam/KNOWN_ISSUES.md` for 31 pre-existing spec test failures (99% pass rate)
