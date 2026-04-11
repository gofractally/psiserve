# CMake toolchain file for building WASM modules targeting wasm32-wasip1.
#
# Uses Homebrew LLVM + wasi-libc + wasi-runtimes (libc++ / compiler-rt).
#
# Usage:
#   cmake -B build-wasi -DCMAKE_TOOLCHAIN_FILE=cmake/wasi-toolchain.cmake
#   cmake --build build-wasi
#
# Override paths via environment variables if not using Homebrew defaults:
#   LLVM_PREFIX, WASI_LIBC_PREFIX, WASI_RUNTIMES_PREFIX

cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

# ---------- locate toolchain components ----------

if(DEFINED ENV{LLVM_PREFIX})
   set(_LLVM "$ENV{LLVM_PREFIX}")
else()
   execute_process(COMMAND brew --prefix llvm OUTPUT_VARIABLE _LLVM OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

if(DEFINED ENV{WASI_LIBC_PREFIX})
   set(_WASI_LIBC "$ENV{WASI_LIBC_PREFIX}")
else()
   execute_process(COMMAND brew --prefix wasi-libc OUTPUT_VARIABLE _WASI_LIBC OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

if(DEFINED ENV{WASI_RUNTIMES_PREFIX})
   set(_WASI_RUNTIMES "$ENV{WASI_RUNTIMES_PREFIX}")
else()
   execute_process(COMMAND brew --prefix wasi-runtimes OUTPUT_VARIABLE _WASI_RUNTIMES OUTPUT_STRIP_TRAILING_WHITESPACE)
endif()

set(_WASI_SYSROOT "${_WASI_LIBC}/share/wasi-sysroot")
set(_WASI_RT_SYSROOT "${_WASI_RUNTIMES}/share/wasi-sysroot")
set(_WASI_RT_CLANG "${_WASI_RUNTIMES}/share/wasi-runtimes")
set(_TARGET wasm32-wasip1)

# ---------- compiler ----------

set(CMAKE_C_COMPILER "${_LLVM}/bin/clang")
set(CMAKE_CXX_COMPILER "${_LLVM}/bin/clang++")
set(CMAKE_AR "${_LLVM}/bin/llvm-ar")
set(CMAKE_RANLIB "${_LLVM}/bin/llvm-ranlib")
set(CMAKE_C_COMPILER_TARGET ${_TARGET})
set(CMAKE_CXX_COMPILER_TARGET ${_TARGET})

# ---------- sysroot & search paths ----------

set(CMAKE_SYSROOT "${_WASI_SYSROOT}")

# C++ standard library headers from wasi-runtimes
set(_CXX_INCLUDE "${_WASI_RT_SYSROOT}/include/${_TARGET}/c++/v1")
# C++ library archives from wasi-runtimes
set(_CXX_LIB "${_WASI_RT_SYSROOT}/lib/${_TARGET}")
# compiler-rt builtins from wasi-runtimes
set(_RT_LIB "${_WASI_RT_CLANG}/lib/wasm32-unknown-wasip1")

# ---------- flags ----------

set(_COMMON_FLAGS "--target=${_TARGET} --sysroot=${_WASI_SYSROOT}")
# Enable WASI emulated signal support (provides signal constants like SIGSEGV)
set(_COMMON_FLAGS "${_COMMON_FLAGS} -D_WASI_EMULATED_SIGNAL")
# Compile-only mode: no mmap/mprotect/sigaction needed
set(_COMMON_FLAGS "${_COMMON_FLAGS} -DPSIZAM_COMPILE_ONLY")
# No C++ exception support available in WASI toolchains
# Error handling uses abort() via PSIZAM_THROW / PSIZAM_ASSERT
set(_COMMON_FLAGS "${_COMMON_FLAGS} -fno-exceptions")

set(CMAKE_C_FLAGS_INIT "${_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_COMMON_FLAGS} -isystem ${_CXX_INCLUDE} -stdlib=libc++")

# LLVM code generation requires deep recursion; default 64KB stack is too small
set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${_CXX_LIB} -L${_RT_LIB} -lc++ -lc++abi -lwasi-emulated-signal -z stack-size=8388608")

# ---------- cmake behavior ----------

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

# Suffix for WASI executables
set(CMAKE_EXECUTABLE_SUFFIX ".wasm")
