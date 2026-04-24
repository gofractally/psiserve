# psio3 — v2-architecture successor to psio.
#
# Implements the design captured in .issues/psio-v2-design.md:
#   - tag_invoke CPOs as the customization mechanism
#   - format_tag_base<Derived> CRTP for format-scoped sugar
#   - Shape concepts + reflection annotations (psio3::annotate<X>)
#   - Runtime reflection (psio3::reflect<T>) with perfect-hash name lookup
#   - Dynamic / schema-driven access (psio3::schema, transcode, dynamic_view)
#   - Rich wrapper types coexisting with annotations
#   - [[nodiscard]] codec_status error model, exception-optional
#
# psio v1 (../psio/) remains as the reference implementation during
# development. psio3 never #includes psio headers — they are reference-only.

add_library(psio3
   # Compiled units land here as phases 14+ introduce them.
   # Phase 0 scaffold: single placeholder TU so CMake accepts the
   # library target; psio3 is header-only until phase 14.
   src/stub.cpp
)

target_include_directories(psio3 PUBLIC include)
target_compile_features(psio3 PUBLIC cxx_std_23)

# Boost::headers is needed by the reflection macro machinery (preprocessor
# seq/tuple helpers) once Phase 1 lands. Already on CI for psio/psio2.
find_package(Boost REQUIRED)
target_link_libraries(psio3 PUBLIC Boost::headers)

# Exception-optional build. The generic API (validate returning
# codec_status) works under -fno-exceptions; only the opt-in .or_throw()
# helper disappears.
option(PSIO3_EXCEPTIONS_ENABLED "Enable exception-throwing helpers" ON)
if(PSIO3_EXCEPTIONS_ENABLED)
   target_compile_definitions(psio3 PUBLIC PSIO3_EXCEPTIONS_ENABLED=1)
else()
   target_compile_definitions(psio3 PUBLIC PSIO3_EXCEPTIONS_ENABLED=0)
endif()

# Phase 4 will make this required for all v3 library code.
target_compile_options(psio3 INTERFACE -Werror=unused-result)

# Build-gated components ------------------------------------------------------

option(PSIO3_ENABLE_TESTS "Build psio3 tests" OFF)
option(PSIO3_ENABLE_BENCHMARKS "Build psio3 benchmarks" OFF)
option(PSIO3_ENABLE_TOOLS "Build psio3 tools (fixture emitters, psio3_tool)" OFF)

if(PSIO3_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(tests)
endif()

if(PSIO3_ENABLE_BENCHMARKS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(benchmarks)
endif()

if(PSIO3_ENABLE_TOOLS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(tools)
endif()
