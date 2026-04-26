# ── wasi-cpp tests ────────────────────────────────────────────────────────────
#
# Round-trip golden diff: reflect the C++ bindings into a psio1::Schema,
# run emit_wit, and compare against the vendored canonical WIT in
# ../../../wit/. Any divergence is a contract failure.

add_executable(wasi_cpp_tests
   main.cpp
   round_trip.cpp
   string_tests.cpp
)

target_link_libraries(wasi_cpp_tests PRIVATE wasi-cpp psio1)

target_include_directories(wasi_cpp_tests PRIVATE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include
)

target_compile_features(wasi_cpp_tests PRIVATE cxx_std_23)

# Tests need to locate the vendored .wit at runtime; export the path.
target_compile_definitions(wasi_cpp_tests PRIVATE
   WASI_WIT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../wit"
)

add_test(NAME wasi_cpp_tests COMMAND wasi_cpp_tests)

# ── wasi-host compile test ────────────────────────────────────────────────────
# Verifies that the host implementation headers compile correctly.

add_executable(wasi_host_tests
   main.cpp
   io_host_tests.cpp
   clocks_random_host_tests.cpp
   sockets_host_tests.cpp
   filesystem_host_tests.cpp
)

target_link_libraries(wasi_host_tests PRIVATE wasi-host psio1)

target_include_directories(wasi_host_tests PRIVATE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include
)

target_compile_features(wasi_host_tests PRIVATE cxx_std_23)

add_test(NAME wasi_host_tests COMMAND wasi_host_tests)
