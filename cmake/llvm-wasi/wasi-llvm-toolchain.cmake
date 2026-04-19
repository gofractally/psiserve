# CMake toolchain file for cross-compiling LLVM to wasm32-wasip1.
#
# Extends the base wasi-toolchain.cmake with LLVM-specific settings.
# Used by cmake/llvm-wasi/CMakeLists.txt.

cmake_minimum_required(VERSION 3.20)

set(CMAKE_SYSTEM_NAME WASI)
set(CMAKE_SYSTEM_PROCESSOR wasm32)

# ---------- locate toolchain components ----------

if(DEFINED ENV{LLVM_PREFIX})
   set(_LLVM "$ENV{LLVM_PREFIX}")
else()
   execute_process(COMMAND brew --prefix llvm
      OUTPUT_VARIABLE _LLVM OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _brew_result)
   if(NOT _brew_result EQUAL 0)
      # Linux: try system clang
      find_program(_CLANG clang)
      if(_CLANG)
         get_filename_component(_LLVM "${_CLANG}" DIRECTORY)
         get_filename_component(_LLVM "${_LLVM}" DIRECTORY)
      else()
         message(FATAL_ERROR "Cannot find LLVM. Set LLVM_PREFIX environment variable.")
      endif()
   endif()
endif()

if(DEFINED ENV{WASI_LIBC_PREFIX})
   set(_WASI_LIBC "$ENV{WASI_LIBC_PREFIX}")
else()
   execute_process(COMMAND brew --prefix wasi-libc
      OUTPUT_VARIABLE _WASI_LIBC OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _brew_result)
   if(NOT _brew_result EQUAL 0)
      message(FATAL_ERROR "Cannot find wasi-libc. Set WASI_LIBC_PREFIX environment variable.")
   endif()
endif()

if(DEFINED ENV{WASI_RUNTIMES_PREFIX})
   set(_WASI_RUNTIMES "$ENV{WASI_RUNTIMES_PREFIX}")
else()
   execute_process(COMMAND brew --prefix wasi-runtimes
      OUTPUT_VARIABLE _WASI_RUNTIMES OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET RESULT_VARIABLE _brew_result)
   if(NOT _brew_result EQUAL 0)
      message(FATAL_ERROR "Cannot find wasi-runtimes. Set WASI_RUNTIMES_PREFIX environment variable.")
   endif()
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

set(_CXX_INCLUDE "${_WASI_RT_SYSROOT}/include/${_TARGET}/c++/v1")
set(_CXX_LIB "${_WASI_RT_SYSROOT}/lib/${_TARGET}")
set(_RT_LIB "${_WASI_RT_CLANG}/lib/wasm32-unknown-wasip1")

# ---------- flags ----------

# Compat headers for POSIX types LLVM needs but WASI lacks (signal, mman, setjmp)
get_filename_component(_COMPAT_DIR "${CMAKE_CURRENT_LIST_DIR}/wasi_compat" ABSOLUTE)

set(_COMMON_FLAGS "--target=${_TARGET} --sysroot=${_WASI_SYSROOT}")
# Compat headers searched before sysroot
set(_COMMON_FLAGS "${_COMMON_FLAGS} -isystem ${_COMPAT_DIR}")
set(_COMMON_FLAGS "${_COMMON_FLAGS} -D_WASI_EMULATED_SIGNAL -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS")
set(_COMMON_FLAGS "${_COMMON_FLAGS} -fno-exceptions")
# LLVM needs to think it's on Unix for Support library OS-specific code paths
set(_COMMON_FLAGS "${_COMMON_FLAGS} -DLLVM_ON_UNIX=1")
# Clang 22's CLANG_ABI macro (clang/Support/Compiler.h) has a wasm branch that
# tests `__WASM__` but clang only predefines `__wasm__` — so none of the
# branches match and CLANG_ABI stays undefined, failing tblgen-generated
# Attrs.inc. We're building everything statically, so force the static branch.
set(_COMMON_FLAGS "${_COMMON_FLAGS} -DCLANG_BUILD_STATIC")
# WASM is little-endian; LLVM's bit.h needs BYTE_ORDER to avoid #include <machine/endian.h>
set(_COMMON_FLAGS "${_COMMON_FLAGS} -DBYTE_ORDER=1234 -DLITTLE_ENDIAN=1234 -DBIG_ENDIAN=4321")

set(CMAKE_C_FLAGS_INIT "${_COMMON_FLAGS}")
set(CMAKE_CXX_FLAGS_INIT "${_COMMON_FLAGS} -isystem ${_CXX_INCLUDE} -stdlib=libc++")

set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${_CXX_LIB} -L${_RT_LIB} -lc++ -lc++abi -lwasi-emulated-signal -lwasi-emulated-mman -lwasi-emulated-process-clocks")

# Opt-in link against libwasi_llvm_stubs.a (POSIX stubs for LLVM symbols that
# wasi-libc doesn't provide: sigaction, getrlimit, getpwuid_r, ...). Used by
# the clang-wasi ExternalProject; not needed for llvm-wasi (archives only).
if(WASI_LINK_STUBS AND WASI_LLVM_STUBS_DIR)
   set(CMAKE_EXE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT} -L${WASI_LLVM_STUBS_DIR} -lwasi_llvm_stubs")
endif()

# Opt-in link against mimalloc ahead of wasi-libc's dlmalloc. mimalloc
# provides strong malloc/free/realloc/calloc/aligned_alloc/posix_memalign
# symbols that the linker resolves before walking further right, so as
# long as -lmimalloc appears before -lc, mimalloc wins every malloc call.
# wasi-libc's dlmalloc then becomes dead code (DCE'd by wasm-ld).
if(WASI_LINK_MIMALLOC AND WASI_MIMALLOC_DIR)
   set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${WASI_MIMALLOC_DIR} -lmimalloc ${CMAKE_EXE_LINKER_FLAGS_INIT}")
endif()

# ---------- cmake behavior ----------

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE BOTH)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)

set(CMAKE_EXECUTABLE_SUFFIX ".wasm")
