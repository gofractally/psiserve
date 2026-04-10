# ── Build utilities for psizam ─────────────────────────────────────────────────
include(CMakeDependentOption)

# ── Compiler support check ─────────────────────────────────────────────────────
if("${CMAKE_CXX_COMPILER_ID}" STREQUAL "Intel" OR "${CMAKE_CXX_COMPILER_ID}" STREQUAL "MSVC")
   message(FATAL "Compiler currently not supported.")
endif()

# ── Profile builds ─────────────────────────────────────────────────────────────
option(PSIZAM_ENABLE_PROFILE "Enable profile build" OFF)
option(PSIZAM_ENABLE_GPERFTOOLS "Enable gperftools" OFF)

if(PSIZAM_ENABLE_PROFILE)
   message(STATUS "Building with profiling information.")
   add_compile_options("-pg")
endif()

# ── Sanitizers ─────────────────────────────────────────────────────────────────
cmake_dependent_option(PSIZAM_ENABLE_ASAN "Build with address sanitization" OFF
                       "NOT PSIZAM_ENABLE_PROFILE;NOT PSIZAM_ENABLE_GPERFTOOLS" OFF)
cmake_dependent_option(PSIZAM_ENABLE_UBSAN "Build with undefined behavior sanitization" OFF
                       "NOT PSIZAM_ENABLE_PROFILE;NOT PSIZAM_ENABLE_GPERFTOOLS" OFF)

if(PSIZAM_ENABLE_ASAN)
   message(STATUS "Building with address sanitization.")
   add_compile_options("-fsanitize=address")
   add_link_options("-fsanitize=address")
endif()

if(PSIZAM_ENABLE_UBSAN)
   message(STATUS "Building with undefined behavior sanitization.")
   add_compile_options("-fsanitize=undefined")
   add_link_options("-fsanitize=undefined")
endif()
