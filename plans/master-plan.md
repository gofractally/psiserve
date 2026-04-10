# psiserve Master Implementation Plan

## Context

psiserve is a new high-performance WASM TCP application server library. The current psibase HTTP path copies data 8-10 times per request through fracpack serialization, Beast buffers, and WASM boundaries. psiserve targets 2 copies (kernel DMA only) by giving WASM modules direct ownership of raw TCP byte streams via I/O ring buffers in WASM linear memory, COW instance forking, and stackful fiber concurrency.

The design is fully documented in `~/psibase/doc/psiserve-design.md`. This plan implements it layer-by-layer, each layer with unit tests before moving to the next.

eos-vm is rebranded to **psizam**. psitri is a database dependency for consumers (not psiserve itself).

## Repository Layout

```
~/psiserve/
  CMakeLists.txt              # Top-level CMake (C++20)
  external/
    psizam/                   # eos-vm fork (git submodule)
    Catch2/                   # Test framework (git submodule)
  libraries/
    psiserve/                 # Core server library
      CMakeLists.txt
      include/psiserve/
      src/
    io_ring/                  # SPSC ring buffer library
      CMakeLists.txt
      include/io_ring/
      src/
    fiber/                    # Fiber scheduler library
      CMakeLists.txt
      include/fiber/
      src/
  programs/
    psiserve-cli/             # Standalone server binary
  services/                   # WASM modules (echo, router, etc.)
  tests/                      # Integration tests + test WASM sources
  plans/                      # Implementation plans (this directory)
```

## Layers

Each layer has a detailed sub-plan in `plans/layer-NN-name.md`.

| Layer | Name | Depends On | Status |
|-------|------|-----------|--------|
| 1 | [Project Scaffold](layer-01-scaffold.md) | — | Not started |
| 2 | [COW Snapshots](layer-02-cow.md) | 1 | Not started |
| 3 | [Yield Flag](layer-03-yield.md) | 1 | Not started |
| 4 | [Fiber Scheduler](layer-04-fiber.md) | 1 | Not started |
| 5 | [I/O Ring Buffers](layer-05-io-ring.md) | 1 | Not started |
| 6 | [Host Function API](layer-06-host-api.md) | 2,3,4,5 | Not started |
| 7 | [TCP Listener + Server](layer-07-server.md) | 6 | Not started |
| 8 | [Module Dispatch](layer-08-dispatch.md) | 7 | Not started |
| 9 | [Fiber Resource Locking](layer-09-fiber-lock.md) | 4,3 | Not started |
| 10 | [Work Stealing](layer-10-work-stealing.md) | 4,7 | Not started |

## Build & Test

```bash
cd ~/psiserve
cmake -B build -G Ninja -DPSISERVE_ENABLE_TESTS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build
cd build && ctest --output-on-failure
```

## Test WASM Modules

Compiled from C using wasi-sdk with `--global-base=32768`:

- `tests/wasm/trivial.c` — empty start(), Layer 1
- `tests/wasm/infinite_loop.c` — `while(1){}`, Layer 3
- `tests/wasm/echo.c` — reads input ring, writes to output ring, Layers 6-7
- `tests/wasm/router.c` — reads first line, dispatches based on content, Layer 8
