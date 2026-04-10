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
   $<$<COMPILE_LANG_AND_ID:CXX,Clang,AppleClang>:-fbracket-depth=512>
)

target_link_libraries(psizam INTERFACE Threads::Threads)

if(PSIZAM_ENABLE_SOFTFLOAT)
   target_compile_definitions(psizam INTERFACE PSIZAM_SOFTFLOAT)
   target_link_libraries(psizam INTERFACE softfloat)
endif()

if(PSIZAM_FULL_DEBUG)
   target_compile_definitions(psizam INTERFACE PSIZAM_FULL_DEBUG)
endif()

# ── Tools ──────────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_TOOLS)
   add_subdirectory(tools)
endif()

# ── LLVM Backend (opt-in) ──────────────────────────────────────────────────────
option(PSIZAM_ENABLE_LLVM "Build LLVM JIT backend" OFF)

if(PSIZAM_ENABLE_LLVM)
   find_package(LLVM REQUIRED CONFIG)
   message(STATUS "Found LLVM ${LLVM_PACKAGE_VERSION} at ${LLVM_DIR}")

   llvm_map_components_to_libnames(LLVM_LIBS
      core support orcjit native
      passes instcombine scalaropts
   )

   add_library(psizam-llvm STATIC
      src/llvm_ir_translator.cpp
      src/llvm_jit_compiler.cpp
      src/llvm_compile.cpp
      src/llvm_runtime_helpers.cpp
   )
   target_link_libraries(psizam-llvm PUBLIC psizam ${LLVM_LIBS})
   target_include_directories(psizam-llvm PUBLIC ${LLVM_INCLUDE_DIRS})
   target_compile_definitions(psizam-llvm PUBLIC PSIZAM_ENABLE_LLVM_BACKEND)

   # LLVM requires consistent RTTI settings
   if(NOT LLVM_ENABLE_RTTI)
      target_compile_options(psizam-llvm PRIVATE -fno-rtti)
   endif()
endif()

# ── Benchmarks ─────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_BENCHMARKS)
   add_subdirectory(benchmarks)
endif()

# ── Tests ──────────────────────────────────────────────────────────────────────
if(PSIZAM_ENABLE_TESTS)
   include(CTest)
   set(CATCH_BUILD_TESTING OFF CACHE BOOL "disable building Catch2 tests")
   set(CATCH_INSTALL_DOCS OFF CACHE BOOL "disable installing Catch2 docs")
   set(CATCH_INSTALL_HELPERS OFF CACHE BOOL "disable installing Catch2 helpers")
   add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/external/Catch2)
   include(external/Catch2/contrib/Catch.cmake)
   add_subdirectory(tests)
   if(PSIZAM_ENABLE_SPEC_TESTS AND NOT EXISTS ${CMAKE_BINARY_DIR}/wasms)
      include(ExternalProject)
      ExternalProject_Add(
         psizam_test_wasms
         GIT_REPOSITORY "https://github.com/gofractally/eos-vm-test-wasms.git"
         SOURCE_DIR ${CMAKE_BINARY_DIR}/wasms
         BINARY_DIR ${CMAKE_BINARY_DIR}/wasms
         BUILD_ALWAYS 0
         BUILD_COMMAND ""
         TEST_COMMAND ""
         INSTALL_COMMAND ""
         UPDATE_COMMAND ""
         PATCH_COMMAND ""
         CONFIGURE_COMMAND ""
      )
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
