#!/bin/bash
# Build LLVM static libraries cross-compiled for wasm32-wasip1.
#
# Usage:
#   scripts/build-llvm-wasi.sh [build-dir]
#
# Environment:
#   BUILD_CLANG=1  Also build clang.wasm + wasm-ld.wasm for source-to-pzam
#                  (much longer: adds several hours and builds clang-host tblgen)
#
# The build is idempotent: if libraries already exist, it's a no-op.
# The first build downloads LLVM source (~200MB) and compiles (~1-4 hours).

set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$SCRIPT_DIR")
BUILD_DIR="${1:-$ROOT/build/llvm-wasi}"
INSTALL_DIR="$BUILD_DIR/install"
BUILD_CLANG="${BUILD_CLANG:-0}"

# What "done" looks like depends on whether we asked for clang
if [ "$BUILD_CLANG" = "1" ]; then
    CLANG_WASM="$BUILD_DIR/clang-wasi-build/bin/clang.wasm"
    LD_WASM="$BUILD_DIR/clang-wasi-build/bin/wasm-ld.wasm"
    if [ -f "$INSTALL_DIR/lib/libLLVMCore.a" ] && [ -f "$CLANG_WASM" ] && [ -f "$LD_WASM" ]; then
        echo "LLVM WASI libraries + clang.wasm already built at $BUILD_DIR"
        echo "Delete $BUILD_DIR to force rebuild."
        exit 0
    fi
else
    if [ -f "$INSTALL_DIR/lib/libLLVMCore.a" ]; then
        echo "LLVM WASI libraries already built at $INSTALL_DIR"
        echo "Delete $INSTALL_DIR to force rebuild."
        exit 0
    fi
fi

echo "Building LLVM for WASI (this will take a while)..."
echo "  Source:      cmake/llvm-wasi/"
echo "  Build:       $BUILD_DIR"
echo "  Install:     $INSTALL_DIR"
echo "  Build clang: $BUILD_CLANG"

# Detect parallelism
if command -v nproc &>/dev/null; then
    JOBS=$(nproc)
elif command -v sysctl &>/dev/null; then
    JOBS=$(sysctl -n hw.ncpu)
else
    JOBS=4
fi

# Try to find system llvm-tblgen / clang-tblgen for faster builds
TBLGEN_ARGS=()
if command -v llvm-tblgen &>/dev/null; then
    TBLGEN_ARGS+=("-DLLVM_HOST_TBLGEN=$(command -v llvm-tblgen)")
elif [ -x "$(brew --prefix llvm 2>/dev/null)/bin/llvm-tblgen" ]; then
    TBLGEN_ARGS+=("-DLLVM_HOST_TBLGEN=$(brew --prefix llvm)/bin/llvm-tblgen")
fi
if [ "$BUILD_CLANG" = "1" ]; then
    if command -v clang-tblgen &>/dev/null; then
        TBLGEN_ARGS+=("-DCLANG_HOST_TBLGEN=$(command -v clang-tblgen)")
    elif [ -x "$(brew --prefix llvm 2>/dev/null)/bin/clang-tblgen" ]; then
        TBLGEN_ARGS+=("-DCLANG_HOST_TBLGEN=$(brew --prefix llvm)/bin/clang-tblgen")
    fi
fi

CLANG_ARGS=()
if [ "$BUILD_CLANG" = "1" ]; then
    CLANG_ARGS+=("-DLLVM_WASI_BUILD_CLANG=ON")
fi

cmake -B "$BUILD_DIR" -S "$ROOT/cmake/llvm-wasi" \
    -DWASI_TOOLCHAIN_FILE="$ROOT/cmake/llvm-wasi/wasi-llvm-toolchain.cmake" \
    "${TBLGEN_ARGS[@]}" \
    "${CLANG_ARGS[@]}"

cmake --build "$BUILD_DIR" -j"$JOBS"

echo ""
echo "LLVM WASI libraries installed to $INSTALL_DIR"
echo "Use with: -DLLVM_WASI_PREFIX=$INSTALL_DIR"
if [ "$BUILD_CLANG" = "1" ]; then
    echo ""
    echo "clang.wasm:   $BUILD_DIR/clang-wasi-build/bin/clang.wasm"
    echo "wasm-ld.wasm: $BUILD_DIR/clang-wasi-build/bin/wasm-ld.wasm"
fi
