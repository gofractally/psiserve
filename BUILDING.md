# Building psiserve

## Supported Platforms

- **macOS** (x86_64, arm64) — requires Homebrew LLVM/Clang
- **Linux** (x86_64) — GCC 13+ or Clang 17+

Windows is not supported.

## Prerequisites

### macOS (Homebrew)

```bash
brew install llvm cmake ninja boost ccache wabt
```

### Ubuntu / Debian

```bash
sudo apt-get install \
    gcc g++ cmake ninja-build \
    libboost-context-dev \
    wabt ccache
```

### Optional dependencies

| Package | Ubuntu / Debian | Homebrew | Enables |
|---------|-----------------|----------|---------|
| c-ares | `libc-ares-dev` | `c-ares` | Async DNS (`PSIBER_HAS_DNS`) |
| OpenSSL | `libssl-dev` | `openssl` | TLS sockets (`PSIBER_HAS_TLS`) |

## Submodules

After cloning, initialize submodules:

```bash
git submodule update --init --recursive
```

## Build

```bash
# macOS — use Homebrew Clang (Apple Clang lacks C++23 features)
export CC=$(brew --prefix llvm)/bin/clang
export CXX=$(brew --prefix llvm)/bin/clang++

# Configure
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build
```

On Linux, the system GCC or Clang is used automatically — no exports needed.

## Tests

```bash
# Configure with tests
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPSIZAM_ENABLE_TESTS=ON

# Build (includes wasm spec test generation via wast2json)
cmake --build build

# Run all tests
cd build && ctest -j$(nproc)

# Run specific Catch2 tests
./build/bin/psizam_unit_tests "[allocator]"     # by tag
./build/bin/psizam_unit_tests "test name"       # by name
./build/bin/psizam_spec_tests "[i32]"           # spec tests by tag
```

`wast2json` (from wabt) is **required** when tests are enabled. It converts
the WebAssembly spec `.wast` files into `.json` + `.wasm` test fixtures at
build time. Without it, the configure step will fail.

## CMake Options

| Option | Default | Purpose |
|--------|---------|---------|
| `PSIZAM_ENABLE_TESTS` | OFF | Unit tests and spec tests |
| `PSIZAM_ENABLE_SOFTFLOAT` | ON | Deterministic software floating point |
| `PSIZAM_ENABLE_TOOLS` | OFF | CLI tools (psizam-interp, etc.) |
| `PSIZAM_ENABLE_BENCHMARKS` | OFF | Performance benchmarks |
| `PSIZAM_FULL_DEBUG` | OFF | Stack dumping and instruction tracing |
| `PSIBER_ENABLE_TESTS` | OFF | Psiber fiber scheduler tests |
| `PSIBER_ENABLE_DNS` | ON | Async DNS (auto-disabled if c-ares not found) |
| `PSIBER_ENABLE_TLS` | ON | TLS sockets (auto-disabled if OpenSSL not found) |
