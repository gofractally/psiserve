cmake_minimum_required(VERSION 3.16)
project(psiserve VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Place all build artifacts under the build directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ── Platform gate ──────────────────────────────────────────────────────────────
if(WIN32)
   message(FATAL_ERROR "psiserve currently only builds on Unix-like OSes (Linux, macOS)")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   set(PSISERVE_WASI ON)
else()
   set(PSISERVE_WASI OFF)
endif()

# ── ccache / sccache auto-detection ────────────────────────────────────────────
if(NOT PSISERVE_WASI)
   find_program(CCACHE_PROGRAM ccache)
   if(NOT CCACHE_PROGRAM)
      find_program(CCACHE_PROGRAM sccache)
   endif()
   if(CCACHE_PROGRAM)
      message(STATUS "Using compiler cache: ${CCACHE_PROGRAM}")
      set(CMAKE_C_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
      set(CMAKE_CXX_COMPILER_LAUNCHER ${CCACHE_PROGRAM})
   endif()
endif()

# ── macOS: link against Homebrew LLVM's libc++ to match compiler ABI ──────────
if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
   execute_process(COMMAND brew --prefix llvm
      OUTPUT_VARIABLE BREW_LLVM_PREFIX
      OUTPUT_STRIP_TRAILING_WHITESPACE
      ERROR_QUIET)
   if(EXISTS "${BREW_LLVM_PREFIX}/lib/c++/libc++.dylib")
      add_compile_options(-stdlib=libc++)
      add_link_options(-stdlib=libc++
         -L${BREW_LLVM_PREFIX}/lib/c++
         -Wl,-rpath,${BREW_LLVM_PREFIX}/lib/c++)
   endif()
endif()

# ── Project-wide dependencies ─────────────────────────────────────────────────
if(NOT PSISERVE_WASI)
   set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
   find_package(Threads REQUIRED)
endif()

# Boost::headers is promoted to root scope so psi_add_wasm_module can
# walk `psio`'s PUBLIC Boost dep and resolve the include path in any
# subdirectory — imported targets created by find_package() are
# otherwise scoped to the directory that called find_package().
find_package(Boost REQUIRED)

# CMake helpers (e.g. psi_add_wasm_module for wasi-sdk guest builds).
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(PsiWasmModule)

# ── Testing ────────────────────────────────────────────────────────────────────
if(NOT PSISERVE_WASI)
   enable_testing()
endif()

# ── Libraries ──────────────────────────────────────────────────────────────────
add_subdirectory(libraries/psio)
add_subdirectory(libraries/psio3)
if(NOT PSISERVE_WASI)
   # Embed psitri as a subdirectory so its headers / libraries (sal, psitri,
   # hash, ucc, art) are available to psiserve. Psitri's own test suite and
   # benchmark wrappers are off in this mode; use its standalone build for that.
   set(BUILD_TESTS OFF CACHE BOOL "psitri tests stay in its own build" FORCE)
   add_subdirectory(external/psitri)
   add_subdirectory(external/quill)
endif()
add_subdirectory(libraries/wasi)
if(NOT PSISERVE_WASI)
   add_subdirectory(libraries/pcrypt)
   add_subdirectory(libraries/pfs)
endif()

add_subdirectory(libraries/psizam)

add_subdirectory(libraries/psiber)

add_subdirectory(libraries/psi-api)

option(PSISERVE_ENABLE "Build psiserve library and CLI" OFF)
if(PSISERVE_ENABLE AND NOT PSISERVE_WASI)
   add_subdirectory(libraries/psiserve)
   add_subdirectory(libraries/psi-sdk)
   add_subdirectory(programs/psiserve-cli)
   add_subdirectory(programs/chat-bench)
endif()

# source-to-pzam driver: host CLI that runs clang.pzam + wasm-ld.pzam under
# pzam-run (which itself runs inside psizam), then pzam-compile native on the
# resulting .wasm. Only relevant for native host builds.
if(NOT PSISERVE_WASI)
   add_subdirectory(programs/source-to-pzam)
endif()
