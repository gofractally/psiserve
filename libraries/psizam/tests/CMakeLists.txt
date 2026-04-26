# ── psizam tests ───────────────────────────────────────────────────────────────

configure_file(wasm_config.hpp.in ${CMAKE_CURRENT_BINARY_DIR}/wasm_config.hpp)
include_directories(${CMAKE_CURRENT_BINARY_DIR})

if(PSIZAM_ENABLE_FUZZ_TESTS)
   add_subdirectory(fuzz)
endif()

enable_testing()

# ── Spec tests ─────────────────────────────────────────────────────────────────

set(WASM_TESTSUITE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/spec/testsuite)
find_program(WAST2JSON wast2json)

if(PSIZAM_ENABLE_SPEC_TESTS AND NOT WAST2JSON)
   message(FATAL_ERROR "wast2json not found. Install wabt: https://github.com/WebAssembly/wabt")
endif()

set(SPEC_TESTS)

# wast2json features: match --enable-all *except* --enable-compact-imports, a
# WABT-specific non-standard multi-import encoding that psizam's parser does
# not accept. Newer testsuite .wast files rely on most of the other features.
#
# Probe each flag against the installed wast2json — older wabt builds reject
# unknown flags entirely (whole invocation fails), which used to make the
# "missing wast2json features" failure cascade through the whole spec-test
# build and block everything else. Drop unsupported flags so the build only
# loses the .wast files that genuinely need them.
execute_process(COMMAND ${WAST2JSON} --help OUTPUT_VARIABLE _wast2json_help ERROR_QUIET)
set(WAST2JSON_FEATURES "")
foreach(_feat
      enable-exceptions
      enable-threads
      enable-function-references
      enable-tail-call
      enable-annotations
      enable-code-metadata
      enable-gc
      enable-memory64
      enable-multi-memory
      enable-extended-const
      enable-relaxed-simd
      enable-custom-page-sizes
      enable-wide-arithmetic)
   if(_wast2json_help MATCHES "--${_feat}( |\n)")
      list(APPEND WAST2JSON_FEATURES "--${_feat}")
   else()
      message(STATUS "wast2json: skipping unsupported flag --${_feat}")
   endif()
endforeach()
set(SPEC_TEST_HELPER ${CMAKE_CURRENT_SOURCE_DIR}/spec_test_helpers.sh)

macro(add_spec_test_impl name source)
   list(APPEND SPEC_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/spec/${name}_tests.cpp)
   # Generated .json. wast2json can fail on .wast files using features the
   # installed wabt doesn't support (e.g. memory64 alignment values too large
   # for older wabts). The helper script tolerates failure by emitting an
   # empty .json so downstream targets can detect "regen unavailable" and
   # fall back to the existing checked-in *_tests.cpp without taking the
   # whole build down.
   add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json
      COMMAND ${SPEC_TEST_HELPER} wast2json ${WAST2JSON} ${WAST2JSON_FEATURES} --no-check ${source} ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/wasms
      DEPENDS ${source} ${SPEC_TEST_HELPER}
   )
   # _tests.cpp regen: re-runs only when .json is non-empty (wast2json
   # succeeded). Empty json means we keep the checked-in cpp.
   add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/spec/${name}_tests.cpp
      COMMAND ${SPEC_TEST_HELPER} gen_tests $<TARGET_FILE:spec_test_generator> ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json ${CMAKE_BINARY_DIR}/wasms ${CMAKE_CURRENT_SOURCE_DIR}/spec/${name}_tests.cpp
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json spec_test_generator ${SPEC_TEST_HELPER})
endmacro()

macro(add_spec_test name)
   add_spec_test_impl(${name} ${WASM_TESTSUITE_DIR}/${name}.wast)
endmacro()

if(PSIZAM_ENABLE_SPEC_TESTS)

foreach(testcase address align binary binary-leb128 block br br_if br_table call_indirect
                 call comments const conversions custom data
                 elem bulk memory_init memory_fill memory_copy table_init table_copy
                 endianness f32 f32_bitwise f32_cmp f64 f64_bitwise f64_cmp
                 fac float_exprs float_literals float_memory float_misc forward
                 func func_ptrs global i32 i64 if int_exprs int_literals labels left-to-right
                 load local_get local_set local_tee loop memory
                 memory_redundancy memory_grow memory_size memory_trap names nop return
                 select stack start store switch
                 table_get table_set table_grow table_size table_fill
                 token traps type
                 unreachable unreached-invalid unwind
                 utf8-custom-section-id utf8-import-field utf8-import-module
                 )
   add_spec_test(${testcase})
endforeach()


# Tail-call tests — pre-generated .cpp, generate .wasm from proposals/tail-call/
set(TAIL_CALL_WAST_DIR ${WASM_TESTSUITE_DIR}/proposals/tail-call)
foreach(testcase return_call return_call_indirect)
   list(APPEND SPEC_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/spec/${testcase}_tests.cpp)
   if(EXISTS ${TAIL_CALL_WAST_DIR}/${testcase}.wast)
      add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/wasms/${testcase}.wasm_marker
         COMMAND ${WAST2JSON} ${WAST2JSON_FEATURES} --no-check ${TAIL_CALL_WAST_DIR}/${testcase}.wast -o ${testcase}.json
         COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/wasms/${testcase}.wasm_marker
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/wasms
         DEPENDS ${TAIL_CALL_WAST_DIR}/${testcase}.wast
         COMMENT "Generating tail-call wasms: ${testcase}"
      )
      list(APPEND TAIL_CALL_WASM_MARKERS ${CMAKE_BINARY_DIR}/wasms/${testcase}.wasm_marker)
   endif()
endforeach()
if(TAIL_CALL_WASM_MARKERS)
   add_custom_target(tail_call_test_wasms DEPENDS ${TAIL_CALL_WASM_MARKERS})
endif()

add_spec_test_impl(e_block ${CMAKE_CURRENT_SOURCE_DIR}/spec/extra/block.wast)
add_spec_test_impl(e_function ${CMAKE_CURRENT_SOURCE_DIR}/spec/extra/function.wast)
add_spec_test_impl(e_globals ${CMAKE_CURRENT_SOURCE_DIR}/spec/extra/globals.wast)
add_spec_test_impl(e_locals ${CMAKE_CURRENT_SOURCE_DIR}/spec/extra/locals.wast)
add_spec_test_impl(e_module ${CMAKE_CURRENT_SOURCE_DIR}/spec/extra/module.wast)
add_spec_test_impl(e_table ${CMAKE_CURRENT_SOURCE_DIR}/spec/extra/table.wast)
add_spec_test_impl(e_simd ${CMAKE_CURRENT_SOURCE_DIR}/spec/extra/simd.wast)

add_executable(psizam_spec_tests main.cpp ${SPEC_TESTS})
target_link_libraries(psizam_spec_tests psizam psizam-exec Catch2::Catch2)
target_include_directories(psizam_spec_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
   ${CMAKE_CURRENT_BINARY_DIR}/spec
)
target_compile_definitions(psizam_spec_tests PUBLIC CATCH_CONFIG_NO_POSIX_SIGNALS)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(psizam_spec_tests psizam-llvm)
endif()
if(TARGET tail_call_test_wasms)
   add_dependencies(psizam_spec_tests tail_call_test_wasms)
endif()
catch_discover_tests(psizam_spec_tests)

set(SPEC_TESTS)
set(SIMD_TESTS
    simd_address simd_align simd_bit_shift simd_bitwise simd_boolean simd_const
    simd_conversions
    simd_f32x4 simd_f32x4_arith simd_f32x4_cmp simd_f32x4_pmin_pmax simd_f32x4_rounding
    simd_f64x2 simd_f64x2_arith simd_f64x2_cmp simd_f64x2_pmin_pmax simd_f64x2_rounding
    simd_i8x16_arith simd_i8x16_arith2 simd_i8x16_cmp simd_i8x16_sat_arith
    simd_i16x8_arith simd_i16x8_arith2 simd_i16x8_cmp simd_i16x8_extadd_pairwise_i8x16
    simd_i16x8_extmul_i8x16 simd_i16x8_q15mulr_sat_s simd_i16x8_sat_arith
    simd_i32x4_arith simd_i32x4_arith2 simd_i32x4_cmp simd_i32x4_dot_i16x8
    simd_i32x4_extadd_pairwise_i16x8 simd_i32x4_extmul_i16x8
    simd_i32x4_trunc_sat_f32x4 simd_i32x4_trunc_sat_f64x2
    simd_i64x2_arith simd_i64x2_arith2 simd_i64x2_cmp simd_i64x2_extmul_i32x4
    simd_int_to_int_extend simd_lane simd_load
    simd_load8_lane simd_load16_lane simd_load32_lane simd_load64_lane
    simd_load_extend simd_load_splat simd_load_zero
    simd_splat simd_store
    simd_store8_lane simd_store16_lane simd_store32_lane simd_store64_lane
    simd_select simd_linking
    i16x8_relaxed_q15mulr_s i32x4_relaxed_trunc i8x16_relaxed_swizzle
    relaxed_dot_product relaxed_laneselect relaxed_madd_nmadd relaxed_min_max
)
# SIMD tests — regenerate both .wasm and _tests.cpp from the testsuite .wast.
# Previously the _tests.cpp was checked-in-only; a submodule bump produced
# .wasm files whose bytes no longer matched the stale .cpp assertions,
# manifesting as ~784 bogus failures. Treat SIMD the same as normal spec
# tests so the generated sources stay in lockstep with the submodule.
foreach(testcase ${SIMD_TESTS})
   list(APPEND SPEC_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/spec/${testcase}_tests.cpp)
   # Locate .wast — testsuite root or proposals/relaxed-simd/
   set(_simd_wast "")
   set(_simd_extra_flags "")
   if(EXISTS ${WASM_TESTSUITE_DIR}/${testcase}.wast)
      set(_simd_wast ${WASM_TESTSUITE_DIR}/${testcase}.wast)
   elseif(EXISTS ${WASM_TESTSUITE_DIR}/proposals/relaxed-simd/${testcase}.wast)
      set(_simd_wast ${WASM_TESTSUITE_DIR}/proposals/relaxed-simd/${testcase}.wast)
      set(_simd_extra_flags --enable-relaxed-simd)
   endif()
   if(_simd_wast)
      # wast2json produces .json + side-effect .wasm files in wasms/.
      # Helper tolerates wabt feature-unsupport by emitting an empty .json.
      add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/spec/${testcase}.json
         COMMAND ${SPEC_TEST_HELPER} wast2json ${WAST2JSON} ${WAST2JSON_FEATURES} ${_simd_extra_flags} --no-check ${_simd_wast} ${CMAKE_CURRENT_BINARY_DIR}/spec/${testcase}.json
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/wasms
         DEPENDS ${_simd_wast} ${SPEC_TEST_HELPER}
         COMMENT "Generating SIMD wasms: ${testcase}"
      )
      # Regenerate _tests.cpp from .json. Empty json → leave checked-in cpp.
      add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/spec/${testcase}_tests.cpp
         COMMAND ${SPEC_TEST_HELPER} gen_tests $<TARGET_FILE:spec_test_generator> ${CMAKE_CURRENT_BINARY_DIR}/spec/${testcase}.json ${CMAKE_BINARY_DIR}/wasms ${CMAKE_CURRENT_SOURCE_DIR}/spec/${testcase}_tests.cpp
         DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/spec/${testcase}.json spec_test_generator ${SPEC_TEST_HELPER}
      )
      list(APPEND SIMD_WASM_MARKERS ${CMAKE_CURRENT_BINARY_DIR}/spec/${testcase}.json)
   endif()
endforeach()

if(SIMD_WASM_MARKERS)
   add_custom_target(simd_test_wasms DEPENDS ${SIMD_WASM_MARKERS})
endif()

add_executable(psizam_simd_tests main.cpp ${SPEC_TESTS})
target_link_libraries(psizam_simd_tests psizam psizam-exec Catch2::Catch2)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(psizam_simd_tests psizam-llvm)
endif()
target_include_directories(psizam_simd_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
   ${CMAKE_CURRENT_BINARY_DIR}/spec
)
target_compile_definitions(psizam_simd_tests PUBLIC CATCH_CONFIG_NO_POSIX_SIGNALS)
if(TARGET simd_test_wasms)
   add_dependencies(psizam_simd_tests simd_test_wasms)
endif()
catch_discover_tests(psizam_simd_tests)

endif(PSIZAM_ENABLE_SPEC_TESTS)

# ── Unit tests ─────────────────────────────────────────────────────────────────

add_executable(unit_tests main.cpp
   unity_batch_1.cpp
   unity_batch_2.cpp
   unity_batch_3.cpp
   unity_batch_4.cpp
   unity_batch_5.cpp
   unity_batch_6.cpp
   unity_batch_7.cpp
)
target_link_libraries(unit_tests psizam psizam-exec Catch2::Catch2)
target_compile_definitions(unit_tests PUBLIC CATCH_CONFIG_NO_POSIX_SIGNALS)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(unit_tests psizam-llvm)
endif()

# ── JIT2 tests ─────────────────────────────────────────────────────────────────

add_executable(jit2_tests main.cpp
   jit2_basic_tests.cpp
   backend_tests.cpp
   instantiation_tests.cpp
   psizam_max_nested_structures_tests.cpp
   host_functions_tests.cpp
   implementation_limits_tests.cpp
)
target_link_libraries(jit2_tests psizam psizam-exec Catch2::Catch2)
target_include_directories(jit2_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_definitions(jit2_tests PUBLIC CATCH_CONFIG_NO_POSIX_SIGNALS)

add_executable(jit2_load_test ${CMAKE_CURRENT_SOURCE_DIR}/../tests/jit2_load_test.cpp)
target_link_libraries(jit2_load_test psizam psizam-exec)

# ── fp_mode differential tests (interpreter-only) ─────────────────────────────

add_executable(fp_mode_differential_tests main.cpp
   fp_mode_differential_tests.cpp
)
target_link_libraries(fp_mode_differential_tests psizam psizam-exec Catch2::Catch2)
target_include_directories(fp_mode_differential_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_definitions(fp_mode_differential_tests PUBLIC CATCH_CONFIG_NO_POSIX_SIGNALS)
catch_discover_tests(fp_mode_differential_tests)

add_executable(compile_time ${CMAKE_CURRENT_SOURCE_DIR}/../tests/compile_time.cpp)
target_link_libraries(compile_time psizam psizam-exec)

add_executable(sha_verify ${CMAKE_CURRENT_SOURCE_DIR}/../tests/sha_verify.cpp)
target_link_libraries(sha_verify psizam psizam-exec)

add_executable(spec_test_generator ${CMAKE_CURRENT_SOURCE_DIR}/spec_test_generator/spec_test_generator.cpp)

catch_discover_tests(unit_tests)

# ── Compliance / differential tests ───────────────────────────────────────────

add_executable(psizam_compliance_tests main.cpp
   compliance/differential_tests.cpp
   compliance/pzam_cache_tests.cpp
   compliance/pzam_metadata_tests.cpp
   compliance/pzam_typed_tests.cpp
   compliance/pzam_attestation_tests.cpp
   compliance/wit_tests.cpp
   compliance/memory64_tests.cpp
   exception_handling_tests.cpp
)
target_link_libraries(psizam_compliance_tests psizam psizam-exec Catch2::Catch2)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(psizam_compliance_tests psizam-llvm)
endif()
target_include_directories(psizam_compliance_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_definitions(psizam_compliance_tests PUBLIC CATCH_CONFIG_NO_POSIX_SIGNALS)
catch_discover_tests(psizam_compliance_tests)

# ── Component / CType tests ───────────────────────────────────────────────────

add_executable(component_tests main.cpp
   component_tests.cpp
   golden_gen.cpp
)
target_link_libraries(component_tests psizam psizam-exec Catch2::Catch2)
target_include_directories(component_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
target_compile_definitions(component_tests PUBLIC
   CATCH_CONFIG_NO_POSIX_SIGNALS
   GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../psio1/golden"
)
catch_discover_tests(component_tests)

# ── Composition tests ────────────────────────────────────────────────────────

add_executable(composition_tests main.cpp
   composition_tests.cpp
)
target_link_libraries(composition_tests psizam psizam-exec Catch2::Catch2)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(composition_tests psizam-llvm)
endif()
target_include_directories(composition_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
   ${CMAKE_CURRENT_SOURCE_DIR}/../examples/composition
)
target_compile_definitions(composition_tests PUBLIC
   CATCH_CONFIG_NO_POSIX_SIGNALS
   COMPOSITION_WASM_DIR="${CMAKE_BINARY_DIR}/libraries/psizam/examples/composition"
)
if(NOT TARGET composition_provider_guest OR NOT TARGET composition_consumer_guest)
   message(FATAL_ERROR
      "composition_tests and runtime_parity_tests require WASM fixtures from examples/composition. "
      "Install wasi-sdk or set -DPSIZAM_WASI_SDK=/path/to/wasi-sdk.")
endif()
add_dependencies(composition_tests composition_provider_guest composition_consumer_guest)
catch_discover_tests(composition_tests)

# ── Runtime parity tests ─────────────────────────────────────────────────────
# Reproduces composition_tests's scenarios through psizam::runtime's public
# API. Acceptance gate for psizam-runtime-api-maturation Track A.

add_executable(runtime_parity_tests main.cpp
   runtime_parity_tests.cpp
   ${CMAKE_SOURCE_DIR}/libraries/psizam/src/runtime.cpp
)
target_link_libraries(runtime_parity_tests psizam psizam-exec Catch2::Catch2)
if(PSIZAM_ENABLE_LLVM)
   target_link_libraries(runtime_parity_tests psizam-llvm)
endif()
target_include_directories(runtime_parity_tests PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
   ${CMAKE_CURRENT_SOURCE_DIR}/../examples/composition
)
target_compile_definitions(runtime_parity_tests PUBLIC
   CATCH_CONFIG_NO_POSIX_SIGNALS
   COMPOSITION_WASM_DIR="${CMAKE_BINARY_DIR}/libraries/psizam/examples/composition"
)
add_dependencies(runtime_parity_tests composition_provider_guest composition_consumer_guest)
catch_discover_tests(runtime_parity_tests)

add_executable(mem64_debug compliance/mem64_debug.cpp)
target_link_libraries(mem64_debug psizam psizam-exec)
target_include_directories(mem64_debug PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)

# Composition benchmark
add_executable(bench_composition bench_composition.cpp)
target_link_libraries(bench_composition psizam psizam-exec)
target_include_directories(bench_composition PRIVATE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/examples/composition)
target_compile_definitions(bench_composition PRIVATE
   COMPOSITION_WASM_DIR="${CMAKE_BINARY_DIR}/libraries/psizam/examples/composition")
if(TARGET psizam-llvm)
   target_link_libraries(bench_composition psizam-llvm)
   target_compile_definitions(bench_composition PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()
if(TARGET psizam-llvm-jit)
   target_link_libraries(bench_composition psizam-llvm-jit)
endif()
