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
if(NOT PSISERVE_WASI)
   add_subdirectory(external/psitri/libraries/ucc)
   add_subdirectory(external/quill)
endif()
add_subdirectory(libraries/wasi)

add_subdirectory(libraries/psizam)

add_subdirectory(libraries/psiber)

option(PSISERVE_ENABLE "Build psiserve library and CLI" OFF)
if(PSISERVE_ENABLE AND NOT PSISERVE_WASI)
   add_subdirectory(libraries/psiserve)
   add_subdirectory(libraries/psi-sdk)
   add_subdirectory(programs/psiserve-cli)
   add_subdirectory(programs/chat-bench)
endif()
