# Cross-Architecture Build Guide (macOS Apple Silicon)

psizam has four JIT backends that target different CPU architectures. On an Apple Silicon Mac, you can build and test **both** aarch64 and x86_64 backends using Rosetta 2.

## Architecture Overview

| Backend | x86_64 | aarch64 | Notes |
|---------|:------:|:-------:|-------|
| interpreter | native | native | Pure C++, no JIT |
| jit | native | native | Single-pass JIT |
| jit2 | native | native | Two-pass IR + regalloc |
| jit_profile | native | - | x86_64 only |
| jit_llvm | native | native | LLVM backend (opt-in) |

## Prerequisites

### ARM64 Toolchain (default on Apple Silicon)

```bash
# Install Homebrew (ARM, default location: /opt/homebrew)
/bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install LLVM and Ninja
brew install llvm ninja cmake ccache
```

### x86_64 Toolchain (via Rosetta 2)

```bash
# Enable Rosetta if not already installed
softwareupdate --install-rosetta --agree-to-license

# Install x86_64 Homebrew (installs to /usr/local)
arch -x86_64 /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"

# Install x86_64 LLVM and tools
arch -x86_64 /usr/local/bin/brew install llvm ninja cmake ccache
```

After setup you have two independent Homebrew prefixes:
- **ARM64**: `/opt/homebrew/` (default `brew`)
- **x86_64**: `/usr/local/` (`arch -x86_64 /usr/local/bin/brew`)

## Build Directories

Use separate build directories per architecture to avoid conflicts:

```
build/
  Debug/          # ARM64 debug (default)
  Release/        # ARM64 release
  Debug-x86/      # x86_64 debug (Rosetta)
  Release-x86/    # x86_64 release (Rosetta)
```

## ARM64 Build (native)

```bash
export CC=/opt/homebrew/opt/llvm/bin/clang
export CXX=/opt/homebrew/opt/llvm/bin/clang++

# Debug build with tests
cmake -B build/Debug -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DPSIZAM_ENABLE_TESTS=ON

cmake --build build/Debug

# Run tests
cd build/Debug && ctest -j$(sysctl -n hw.logicalcpu)
```

Backends tested: `interpreter`, `jit` (aarch64), `jit2` (aarch64)

## x86_64 Build (via Rosetta 2)

```bash
# All commands run under Rosetta
export CC=/usr/local/opt/llvm/bin/clang
export CXX=/usr/local/opt/llvm/bin/clang++

# Configure for x86_64
arch -x86_64 cmake -B build/Debug-x86 -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_OSX_ARCHITECTURES=x86_64 \
  -DPSIZAM_ENABLE_TESTS=ON

# Build under Rosetta
arch -x86_64 cmake --build build/Debug-x86

# Run tests under Rosetta
cd build/Debug-x86 && arch -x86_64 ctest -j$(sysctl -n hw.logicalcpu)
```

Backends tested: `interpreter`, `jit` (x86_64), `jit2` (x86_64), `jit_profile` (x86_64)

## LLVM Backend (either arch)

```bash
# Add to cmake configure command:
-DPSIZAM_ENABLE_LLVM=ON

# LLVM must match the target architecture.
# ARM64 build uses /opt/homebrew/opt/llvm
# x86_64 build uses /usr/local/opt/llvm
```

## Verifying Architecture

```bash
# Check a built binary's architecture
file build/Debug/bin/unit_tests
# Expected: Mach-O 64-bit executable arm64

file build/Debug-x86/bin/unit_tests
# Expected: Mach-O 64-bit executable x86_64
```

## Running Tests for a Specific Backend

```bash
# Run only interpreter tests
./bin/psizam_spec_tests "[interpreter]"

# Run only jit2 tests
./bin/psizam_spec_tests "[jit2]"

# Run specific spec test
./bin/psizam_spec_tests "[call_indirect]"

# Run multi-value tests
./bin/jit2_tests "[multi_value]"
```

## Troubleshooting

**"Bad CPU type in executable"**: You're trying to run an x86_64 binary without `arch -x86_64`, or Rosetta isn't installed.

**Wrong Homebrew**: Verify which `brew` you're using:
```bash
which brew           # Should be /opt/homebrew/bin/brew (ARM64)
arch -x86_64 which brew  # May not work; use full path /usr/local/bin/brew
```

**Mixing architectures**: Never point an ARM64 build at x86_64 libraries or vice versa. Keep `CC`/`CXX` and build directories strictly separated.

**ccache conflicts**: ccache is architecture-aware and safe to share. Both builds can use the same ccache directory without issues.
