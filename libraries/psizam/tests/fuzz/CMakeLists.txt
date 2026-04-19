# Fuzz testing for psizam
# Two targets:
#   psizam-fuzz-diff  — standalone wasm-smith differential fuzzer (recommended)
#   psizam-fuzz       — libFuzzer driver (requires clang with libFuzzer+ASan)

# ── wasm-gen: bulk WASM module generator (Rust) ─────────────────────────────
# Build separately: cd wasm-gen && cargo build --release
# The fuzzer finds it automatically next to its own binary, in wasm-gen/target/release/,
# or in PATH.

# ── Standalone differential fuzzer (wasm-smith based) ────────────────────────
# Uses wasm-gen to bulk-generate valid WASM modules via pipe, runs on all
# backends, asserts identical outcomes.
#
# Usage:
#   ./psizam-fuzz-diff [iterations] [seed]
#   ./psizam-fuzz-diff --file module.wasm

add_executable(psizam-fuzz-diff fuzz_differential.cpp)
target_link_libraries(psizam-fuzz-diff PRIVATE psizam psizam-exec)
target_compile_definitions(psizam-fuzz-diff PRIVATE PSIZAM_JIT_SIGNAL_DIAGNOSTICS)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(psizam-fuzz-diff PRIVATE psizam-llvm)
   target_compile_definitions(psizam-fuzz-diff PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()
if(PSIZAM_ENABLE_WASM3)
   target_link_libraries(psizam-fuzz-diff PRIVATE m3)
   target_compile_definitions(psizam-fuzz-diff PRIVATE PSIZAM_ENABLE_WASM3)
endif()

# Quick smoke test: 20 iterations
add_test(NAME psizam_fuzz_diff_smoke
   COMMAND psizam-fuzz-diff 20)

# ── Regression tests: re-run all archived crash/mismatch reproducers ────────
# Each .wasm file in regression/ becomes a separate ctest entry.
# To add a new regression case, just drop a .wasm file in the directory.
file(GLOB FUZZ_REGRESSION_WASM "${CMAKE_CURRENT_SOURCE_DIR}/regression/*.wasm")
foreach(wasm_file ${FUZZ_REGRESSION_WASM})
   get_filename_component(wasm_name ${wasm_file} NAME_WE)
   add_test(NAME "fuzz_regression/${wasm_name}"
      COMMAND psizam-fuzz-diff --file ${wasm_file})
   set_tests_properties("fuzz_regression/${wasm_name}" PROPERTIES
      LABELS "fuzz_regression"
      TIMEOUT 30)
endforeach()


# ── libFuzzer driver (optional) ──────────────────────────────────────────────
# Coverage-guided differential fuzzer using libFuzzer.
# Requires Homebrew Clang with libFuzzer support.
#
# Usage:
#   cmake -B build -DPSIZAM_FUZZ_LIBFUZZER=ON -DCMAKE_BUILD_TYPE=Release
#   cmake --build build --target psizam-fuzz
#   ./build/bin/psizam-fuzz corpus/ -max_len=8192 -jobs=4

option(PSIZAM_FUZZ_LIBFUZZER "Build libFuzzer-based fuzz target" OFF)
if(PSIZAM_FUZZ_LIBFUZZER)
   add_executable(psizam-fuzz fuzz_driver.cpp)
   target_link_libraries(psizam-fuzz PRIVATE psizam psizam-exec)
   target_compile_definitions(psizam-fuzz PRIVATE PSIZAM_JIT_SIGNAL_DIAGNOSTICS)
   if(PSIZAM_ENABLE_LLVM)
      target_link_libraries(psizam-fuzz PRIVATE psizam-llvm)
      target_compile_definitions(psizam-fuzz PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
   endif()
   # libFuzzer instrumentation; skip ASan on macOS ARM64 (known issues)
   if(APPLE AND CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
      target_compile_options(psizam-fuzz PRIVATE -fsanitize=fuzzer -fno-omit-frame-pointer)
      target_link_options(psizam-fuzz PRIVATE -fsanitize=fuzzer)
   else()
      target_compile_options(psizam-fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
      target_link_options(psizam-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
   endif()

   set(FUZZ_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
   add_test(NAME psizam_fuzz_smoke
      COMMAND psizam-fuzz -max_total_time=10 ${FUZZ_CORPUS_DIR})
endif()
