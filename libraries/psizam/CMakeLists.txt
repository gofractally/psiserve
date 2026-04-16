# ── psizam: WebAssembly virtual machine library ────────────────────────────────
# Works both as a standalone project and as a subdirectory of a parent project.

# Standalone guard: only set project-level settings when building psizam alone
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   cmake_minimum_required(VERSION 3.16)
   project(psizam VERSION 1.0.0 LANGUAGES C CXX)
   set(CMAKE_CXX_STANDARD 23)
   set(CMAKE_CXX_EXTENSIONS ON)
   set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
   set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
   find_package(Threads REQUIRED)

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

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
   set(PSIZAM_IS_TOPLEVEL ON)
else()
   set(PSIZAM_IS_TOPLEVEL OFF)
endif()

# ── Options ────────────────────────────────────────────────────────────────────
include(CMakeDependentOption)

option(PSIZAM_ENABLE_SOFTFLOAT
   "Enable deterministic software floating point operations" ON)
option(PSIZAM_FULL_DEBUG
   "Enable stack dumping and instruction tracing" OFF)
option(PSIZAM_ENABLE_INSTALL
   "Enable install rules for psizam" ${PSIZAM_IS_TOPLEVEL})
option(PSIZAM_ENABLE_TOOLS
   "Build psizam CLI tools" ${PSIZAM_IS_TOPLEVEL})
option(PSIZAM_ENABLE_TESTS
   "Build psizam unit tests and spec tests" OFF)
option(PSIZAM_ENABLE_BENCHMARKS
   "Build comparative benchmark suite" OFF)

cmake_dependent_option(PSIZAM_ENABLE_SPEC_TESTS
   "Enable wasm spec tests" ON "PSIZAM_ENABLE_TESTS" ON)
cmake_dependent_option(PSIZAM_ENABLE_FUZZ_TESTS
   "Enable fuzz testing" OFF "PSIZAM_ENABLE_TESTS" ON)

# ── Build utilities (sanitizers, profiling) ────────────────────────────────────
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/modules")
include(PsiZamBuildUtils)

# ── Softfloat dependency ──────────────────────────────────────────────────────
if(PSIZAM_ENABLE_SOFTFLOAT AND NOT TARGET softfloat)
   add_subdirectory(external)
endif()

# ── The psizam INTERFACE library ──────────────────────────────────────────────
add_library(psizam INTERFACE)
add_library(psizam::psizam ALIAS psizam)

target_include_directories(psizam INTERFACE
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/external/softfloat/source/include>
)

target_compile_options(psizam INTERFACE
   $<$<COMPILE_LANGUAGE:CXX>:-Wno-register>
   $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fbracket-depth=1024>
)

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   target_link_libraries(psizam INTERFACE psio)
else()
   target_link_libraries(psizam INTERFACE Threads::Threads psio)
endif()

if(PSIZAM_ENABLE_SOFTFLOAT)
   target_compile_definitions(psizam INTERFACE PSIZAM_SOFTFLOAT)
   target_link_libraries(psizam INTERFACE softfloat)
endif()

if(PSIZAM_FULL_DEBUG)
   target_compile_definitions(psizam INTERFACE PSIZAM_FULL_DEBUG)
endif()

# ── Per-mode per-arch compile targets (INTERFACE, header-only JIT backends) ───
# These targets document the compile dependency for each backend × architecture.
# JIT backends are entirely header-only so no source files are compiled.

add_library(psizam-jit1-x86 INTERFACE)
target_link_libraries(psizam-jit1-x86 INTERFACE psizam)

add_library(psizam-jit1-arm INTERFACE)
target_link_libraries(psizam-jit1-arm INTERFACE psizam)

add_library(psizam-jit2-x86 INTERFACE)
target_link_libraries(psizam-jit2-x86 INTERFACE psizam)

add_library(psizam-jit2-arm INTERFACE)
target_link_libraries(psizam-jit2-arm INTERFACE psizam)

# ── Execution library (native only) ──────────────────────────────────────────
# Replaces psizam-runtime. Includes LLVM runtime helpers unconditionally so
# .pzam files compiled with LLVM can be executed without LLVM installed.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_library(psizam-exec STATIC
      src/runtime_helpers.cpp
      src/call_on_stack.cpp
      src/llvm_runtime_helpers.cpp
      src/eh_runtime.cpp
   )
   target_link_libraries(psizam-exec PUBLIC psizam)
endif()

# ── LLVM Compile Backend (opt-in) ────────────────────────────────────────────
option(PSIZAM_ENABLE_LLVM "Build LLVM AOT/JIT backend" OFF)

if(PSIZAM_ENABLE_LLVM)
   # PSIZAM_LLVM_TARGETS: semicolon-separated list, e.g. "X86;AArch64" (default: both)
   if(NOT DEFINED PSIZAM_LLVM_TARGETS)
      set(PSIZAM_LLVM_TARGETS "X86;AArch64")
   endif()

   if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
      # WASI: use pre-built LLVM WASI libraries
      if(NOT LLVM_WASI_PREFIX)
         message(FATAL_ERROR "PSIZAM_ENABLE_LLVM=ON for WASI requires -DLLVM_WASI_PREFIX=<path>")
      endif()
      cmake_path(ABSOLUTE_PATH LLVM_WASI_PREFIX BASE_DIRECTORY "${CMAKE_SOURCE_DIR}" OUTPUT_VARIABLE _LLVM_WASI_ABS)
      set(LLVM_DIR "${_LLVM_WASI_ABS}/lib/cmake/llvm")
      find_package(LLVM REQUIRED CONFIG NO_CMAKE_FIND_ROOT_PATH)
      message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} (WASI) at ${LLVM_DIR}")
   else()
      find_package(LLVM REQUIRED CONFIG)
      message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")
   endif()

   # ── Per-arch LLVM compile targets ──────────────────────────────────────────
   # Each produces a STATIC library with AOT compilation for one architecture.
   set(_LLVM_AOT_SOURCES
      src/llvm_ir_translator.cpp
      src/llvm_aot_compiler.cpp
   )

   if("X86" IN_LIST PSIZAM_LLVM_TARGETS)
      set(_LLVM_X86_COMPONENTS core support instcombine scalaropts object
         x86codegen x86asmparser x86desc x86info)
      llvm_map_components_to_libnames(_LLVM_X86_LIBS ${_LLVM_X86_COMPONENTS})

      add_library(psizam-llvm-x86 STATIC ${_LLVM_AOT_SOURCES})
      target_link_libraries(psizam-llvm-x86 PUBLIC psizam ${_LLVM_X86_LIBS})
      target_include_directories(psizam-llvm-x86 PUBLIC ${LLVM_INCLUDE_DIRS})
      target_compile_definitions(psizam-llvm-x86 PUBLIC PSIZAM_ENABLE_LLVM_BACKEND)
      target_compile_definitions(psizam-llvm-x86 PRIVATE PSIZAM_LLVM_TARGET_X86)
      if(NOT LLVM_ENABLE_RTTI)
         target_compile_options(psizam-llvm-x86 PRIVATE -fno-rtti)
      endif()
   endif()

   if("AArch64" IN_LIST PSIZAM_LLVM_TARGETS)
      set(_LLVM_ARM_COMPONENTS core support instcombine scalaropts object
         aarch64codegen aarch64asmparser aarch64desc aarch64info)
      llvm_map_components_to_libnames(_LLVM_ARM_LIBS ${_LLVM_ARM_COMPONENTS})

      add_library(psizam-llvm-arm STATIC ${_LLVM_AOT_SOURCES})
      target_link_libraries(psizam-llvm-arm PUBLIC psizam ${_LLVM_ARM_LIBS})
      target_include_directories(psizam-llvm-arm PUBLIC ${LLVM_INCLUDE_DIRS})
      target_compile_definitions(psizam-llvm-arm PUBLIC PSIZAM_ENABLE_LLVM_BACKEND)
      target_compile_definitions(psizam-llvm-arm PRIVATE PSIZAM_LLVM_TARGET_AARCH64)
      if(NOT LLVM_ENABLE_RTTI)
         target_compile_options(psizam-llvm-arm PRIVATE -fno-rtti)
      endif()
   endif()

   # ── LLVM JIT runtime (native only) ────────────────────────────────────────
   # ORC JIT compiler + llvm_compile entry point — only needed for native
   # live-JIT, not for loading pre-compiled .pzam files.
   if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
      llvm_map_components_to_libnames(_LLVM_JIT_LIBS core support orcjit native)

      add_library(psizam-llvm-jit STATIC
         src/llvm_jit_compiler.cpp
         src/llvm_compile.cpp
      )
      # llvm_compile.cpp uses llvm_ir_translator (in per-arch AOT targets).
      # Link the host-platform AOT target so the translator symbols resolve.
      if(TARGET psizam-llvm-x86 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
         target_link_libraries(psizam-llvm-jit PUBLIC psizam-llvm-x86)
      elseif(TARGET psizam-llvm-arm)
         target_link_libraries(psizam-llvm-jit PUBLIC psizam-llvm-arm)
      endif()
      target_link_libraries(psizam-llvm-jit PUBLIC psizam psizam-exec ${_LLVM_JIT_LIBS})
      target_include_directories(psizam-llvm-jit PUBLIC ${LLVM_INCLUDE_DIRS})
      target_compile_definitions(psizam-llvm-jit PUBLIC PSIZAM_ENABLE_LLVM_BACKEND)
      if(NOT LLVM_ENABLE_RTTI)
         target_compile_options(psizam-llvm-jit PRIVATE -fno-rtti)
      endif()
   endif()

   # ── psizam-llvm umbrella target ───────────────────────────────────────────
   # Links all enabled LLVM compile targets + JIT for backward compatibility
   # with existing consumers that just say target_link_libraries(... psizam-llvm).
   add_library(psizam-llvm INTERFACE)
   target_compile_definitions(psizam-llvm INTERFACE PSIZAM_ENABLE_LLVM_BACKEND)
   if(TARGET psizam-llvm-x86)
      target_link_libraries(psizam-llvm INTERFACE psizam-llvm-x86)
   endif()
   if(TARGET psizam-llvm-arm)
      target_link_libraries(psizam-llvm INTERFACE psizam-llvm-arm)
   endif()
   if(TARGET psizam-llvm-jit)
      target_link_libraries(psizam-llvm INTERFACE psizam-llvm-jit)
   endif()
endif()

# ── Tools (after LLVM so pzam-compile can link psizam-llvm) ───────────────────
if(PSIZAM_ENABLE_TOOLS)
   add_subdirectory(tools)
endif()

# ── Benchmarks ─────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_BENCHMARKS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(benchmarks)
endif()

# ── Tests ──────────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   include(CTest)
   set(CATCH_BUILD_TESTING OFF CACHE BOOL "disable building Catch2 tests")
   set(CATCH_INSTALL_DOCS OFF CACHE BOOL "disable installing Catch2 docs")
   set(CATCH_INSTALL_HELPERS OFF CACHE BOOL "disable installing Catch2 helpers")
   add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/Catch2)
   include(external/Catch2/contrib/Catch.cmake)
   add_subdirectory(tests)
   if(PSIZAM_ENABLE_SPEC_TESTS)
      file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/wasms)
   endif()
endif()

# ── Install ────────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_INSTALL)
   include(GNUInstallDirs)
   install(TARGETS psizam
      LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR} COMPONENT Libraries
      PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR} COMPONENT Headers
   )
   install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/psizam
      DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
   )
endif()
