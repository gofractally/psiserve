# ── wasi-cpp tests ────────────────────────────────────────────────────────────
#
# Round-trip golden diff: reflect the C++ bindings into a psio::Schema,
# run emit_wit, and compare against the vendored canonical WIT in
# ../../../wit/. Any divergence is a contract failure.

add_executable(wasi_cpp_tests
   main.cpp
   round_trip.cpp
   string_tests.cpp
)

target_link_libraries(wasi_cpp_tests PRIVATE wasi-cpp psio)

target_include_directories(wasi_cpp_tests PRIVATE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include
)

target_compile_features(wasi_cpp_tests PRIVATE cxx_std_23)

# Tests need to locate the vendored .wit at runtime; export the path.
target_compile_definitions(wasi_cpp_tests PRIVATE
   WASI_WIT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../wit"
)

add_test(NAME wasi_cpp_tests COMMAND wasi_cpp_tests)
