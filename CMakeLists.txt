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
add_subdirectory(libraries/psizam)

option(PSISERVE_ENABLE "Build psiserve library and CLI" OFF)
if(PSISERVE_ENABLE AND NOT PSISERVE_WASI)
   add_subdirectory(libraries/psiserve)
   add_subdirectory(programs/psiserve-cli)
   add_subdirectory(programs/chat-bench)
endif()
