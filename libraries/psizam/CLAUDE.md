# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

psizam is a high-performance, deterministic WebAssembly engine (forked from EOS VM). It's a C++23 header-only library (except for the softfloat dependency). It provides five backends: **interpreter** (all platforms), **jit** (x86_64 + aarch64), **jit_profile** (x86_64), **jit2** (x86_64 only, two-pass with IR optimization), and **null_backend** (validation only).

## Build Commands

psizam is built as part of the psiserve project from the repository root:

```bash
# macOS (Homebrew Clang required)
export CC=$(brew --prefix llvm)/bin/clang
export CXX=$(brew --prefix llvm)/bin/clang++

# Configure and build
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/Debug

# Build with tests
cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPSIZAM_ENABLE_TESTS=ON
cmake --build build/Debug

# Run all tests
cd build/Debug && ctest -j$(nproc)

# Run a single test (Catch2-based)
./build/Debug/bin/psizam_unit_tests "[allocator]"     # by tag
./build/Debug/bin/psizam_unit_tests "test name"       # by name
./build/Debug/bin/psizam_spec_tests "[i32]"           # spec tests
```

### Key CMake Options

All options are prefixed with `PSIZAM_`:

| Option | Default | Purpose |
|--------|---------|---------|
| `PSIZAM_ENABLE_SOFTFLOAT` | ON | Deterministic software floating point |
| `PSIZAM_ENABLE_TESTS` | OFF | Unit tests and spec tests |
| `PSIZAM_ENABLE_SPEC_TESTS` | ON (if tests) | WebAssembly spec compliance tests |
| `PSIZAM_ENABLE_FUZZ_TESTS` | OFF | Fuzz testing |
| `PSIZAM_ENABLE_TOOLS` | OFF (as subdirectory) | CLI tools (psizam-interp, etc.) |
| `PSIZAM_ENABLE_BENCHMARKS` | OFF | Comparative benchmark suite |
| `PSIZAM_FULL_DEBUG` | OFF | Stack dumping and instruction tracing |

## Architecture

### Core Components (all in `include/psizam/`)

- **`backend.hpp`** -- Main entry point. Templated on host function class and backend type (`interpreter`, `jit`, `jit2`, `null_backend`). Owns module parsing, instantiation, and execution.
- **`parser.hpp`** -- Binary WASM parser (~2700 lines). Parses all standard sections into the `module` struct defined in `types.hpp`.
- **`execution_context.hpp`** -- Manages operand stack, call stack, linear memory, globals, and control flow during execution.
- **`interpret_visitor.hpp`** -- Interpreter implementation (~3000 lines). Visitor pattern handler for every WASM opcode.
- **`x86_64.hpp`** -- JIT code generator for x86_64. Emits native machine code from parsed WASM.
- **`jit_codegen_a64.hpp`** -- JIT code generator for aarch64.
- **`jit_ir.hpp`** -- Two-pass JIT (jit2) intermediate representation and optimizer (x86_64 only).
- **`bitcode_writer.hpp`** -- Converts parsed WASM into an intermediate bitcode representation used by the interpreter.

### Host Function Integration

- **`host_function.hpp`** -- Type-safe binding of native C++ functions as WASM imports via template metaprogramming.
- **`type_converter.hpp`** -- Automatic conversion between WASM types and native C++ types.
- See `tools/hello_driver.cpp` for a working integration example.

### Memory & Safety

- **`allocator.hpp`** -- Multiple allocator strategies using OS guard pages for memory sandboxing.
- **`guarded_ptr.hpp`** -- Pointer validation via guard pages (no per-access runtime checks).
- **`softfloat.hpp`** -- Deterministic IEEE-754 wrapper over berkeley-softfloat-3.
- **`watchdog.hpp`** -- Time-bounded execution via signal-based timer.

### Key Design Patterns

- **Static dispatch visitor**: `variant.hpp` implements a discriminating union with compile-time dispatch.
- **Non-owning data structures**: Core types don't own memory -- allocators manage lifetimes.
- **Guard paging over bounds checks**: Memory safety via OS page protection.
- **No unbounded recursion or loops**: All parsing and execution paths are tightly bounded.

### Macro Conventions

- `PSIZAM_ASSERT(cond, exception_type, msg)` -- assertion macro used throughout
- `PSIZAM_SOFTFLOAT` -- defined when softfloat is enabled
- `PSIZAM_FULL_DEBUG` -- defined for debug builds with tracing
- `PSIZAM_HAS_MEMBER`, `PSIZAM_HAS_TEMPLATE_MEMBER` -- SFINAE helpers

### Test Structure

- **Unit tests** (`tests/`): Catch2-based. Cover allocators, host functions, parsing, implementation limits.
- **Spec tests** (`tests/spec/`): Auto-generated from WebAssembly spec `.wast` files via `spec_test_generator`.
- **SIMD tests**: Separate executable for v128/SIMD spec compliance.
- **Known issues**: See `KNOWN_ISSUES.md` for pre-existing test failures (31 of 8203).

### External Dependencies

- **softfloat** (`external/`): Berkeley SoftFloat-3 -- deterministic float ops.
- **Catch2** (`external/Catch2`): Test framework v2.7.2 (single-header).
