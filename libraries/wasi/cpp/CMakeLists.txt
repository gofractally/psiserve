# ── wasi-cpp: PSIO-reflected C++ bindings for WASI interfaces ─────────────────
#
# Header-only INTERFACE library. Each header under include/wasi/<version>/
# declares one WASI interface (or related group) via PSIO1_PACKAGE,
# PSIO1_INTERFACE, and PSIO1_REFLECT so psio::emit_wit projects it back to
# the vendored .wit in ../wit/<version>/. The round-trip test is what
# enforces the C++ ↔ WIT contract.

add_library(wasi-cpp INTERFACE)

target_include_directories(wasi-cpp INTERFACE
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(wasi-cpp INTERFACE psio)

target_compile_features(wasi-cpp INTERFACE cxx_std_23)

# ── wasi-host: host-side implementations (psiber + psizam) ────────────────────
# Header-only interface library for the WASI host implementations.
# Links against wasi-cpp (interface types), psiber (scheduler/IO), and
# psizam (handle_table, host_function, hosted).

add_library(wasi-host INTERFACE)

target_include_directories(wasi-host INTERFACE
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(wasi-host INTERFACE wasi-cpp psiber psizam-exec)

option(WASI_CPP_ENABLE_TESTS "Build wasi-cpp tests" OFF)
if(WASI_CPP_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(tests)
endif()

# ── examples ──────────────────────────────────────────────────────────────────
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(examples)
endif()
