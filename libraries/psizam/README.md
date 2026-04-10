# psizam — A High-Performance WebAssembly Engine

- Extremely Fast Execution (interpreter + JIT backends)
- Extremely Fast Parsing/Loading
- Efficient Time-Bounded Execution
- Deterministic Execution (software float + hardware float options)
- Standards Compliant (WebAssembly 1.0)
- Designed for Parallel Execution
- C++23 / Header Only
- Simple API for integrating Native Calls

## Overview

psizam is a high-performance, deterministic WebAssembly engine designed for embedding in server applications. It provides multiple execution backends:

- **interpreter** — portable, all platforms
- **jit** — native code generation for x86_64 and aarch64
- **jit2** — two-pass JIT with IR optimization (x86_64)
- **jit_profile** — JIT with instruction profiling (x86_64)
- **null_backend** — validation only

Originally forked from [EOS VM](https://github.com/AntelopeIO/eos-vm), psizam has been rebranded and extended for use in the [psiserve](https://github.com/gofractally/psiserve) WebAssembly application server.

## Deterministic Execution

Software floating point ("softfloat") provides deterministic IEEE-754 arithmetic across all platforms, critical for consensus applications. Hardware floating point is available when determinism is not required.

Resource limits (stack size, call depth, memory pages, etc.) are fully configurable at compile time or runtime.

## Time-Bounded Execution

Two mechanisms bound execution time:

1. **Instruction counting** — simple but incurs a performance penalty
2. **Watchdog timer** — signal-based, near-zero overhead during execution

## Secure by Design

- Guard-page memory sandboxing eliminates per-access bounds checks
- Non-owning data structures with allocator-managed lifetimes
- No unbounded recursion or loops during parsing or execution
- Invariant-maintaining data types prevent invalid states by construction

## Integration

psizam is a header-only library (except for the softfloat dependency). Integration via CMake:

```cmake
add_subdirectory(path/to/psizam)
target_link_libraries(your_target psizam)
```

See [docs/OVERVIEW.md](./docs/OVERVIEW.md) for a quick start guide and [tools/hello_driver.cpp](./tools/hello_driver.cpp) for a working example.

## Building

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPSIZAM_ENABLE_TESTS=ON
cmake --build build
cd build && ctest -j$(nproc)
```

## License

See [LICENSE](./LICENSE) for terms. Originally released under the EOS VM License.
