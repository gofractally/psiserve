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

# ── libFuzzer driver (optional) ──────────────────────────────────────────────
# Requires clang with libFuzzer support. Known ASan issues on macOS ARM64.

option(PSIZAM_FUZZ_LIBFUZZER "Build libFuzzer-based fuzz target" OFF)
if(PSIZAM_FUZZ_LIBFUZZER)
   add_executable(psizam-fuzz fuzz_driver.cpp)
   target_link_libraries(psizam-fuzz PRIVATE psizam psizam-exec)
   target_compile_options(psizam-fuzz PRIVATE -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer)
   target_link_options(psizam-fuzz PRIVATE -fsanitize=fuzzer,address,undefined)

   set(FUZZ_CORPUS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/corpus")
   add_test(NAME psizam_fuzz_smoke
      COMMAND psizam-fuzz -max_total_time=10 -jobs=4 ${FUZZ_CORPUS_DIR})
endif()
