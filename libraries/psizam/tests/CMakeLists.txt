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
set(WAST2JSON_FEATURES
   --enable-exceptions
   --enable-threads
   --enable-function-references
   --enable-tail-call
   --enable-annotations
   --enable-code-metadata
   --enable-gc
   --enable-memory64
   --enable-multi-memory
   --enable-extended-const
   --enable-relaxed-simd
   --enable-custom-page-sizes
   --enable-wide-arithmetic
)

macro(add_spec_test_impl name source)
   list(APPEND SPEC_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/spec/${name}_tests.cpp)
   add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json
      COMMAND ${WAST2JSON} ${WAST2JSON_FEATURES} --no-check ${source} -o ${name}.json && mv ${name}.json ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json
      WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/wasms
      DEPENDS ${source}
   )
   add_custom_command(OUTPUT ${CMAKE_CURRENT_SOURCE_DIR}/spec/${name}_tests.cpp
      COMMAND spec_test_generator ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json ${CMAKE_BINARY_DIR}/wasms > ${CMAKE_CURRENT_SOURCE_DIR}/spec/${name}_tests.cpp
      DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/spec/${name}.json spec_test_generator)
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
# SIMD tests — use pre-generated _tests.cpp but generate .wasm files from testsuite
foreach(testcase ${SIMD_TESTS})
   list(APPEND SPEC_TESTS ${CMAKE_CURRENT_SOURCE_DIR}/spec/${testcase}_tests.cpp)
   # Generate .wasm files via wast2json — check testsuite root and proposals/relaxed-simd/
   set(_simd_wast "")
   set(_simd_extra_flags "")
   if(EXISTS ${WASM_TESTSUITE_DIR}/${testcase}.wast)
      set(_simd_wast ${WASM_TESTSUITE_DIR}/${testcase}.wast)
   elseif(EXISTS ${WASM_TESTSUITE_DIR}/proposals/relaxed-simd/${testcase}.wast)
      set(_simd_wast ${WASM_TESTSUITE_DIR}/proposals/relaxed-simd/${testcase}.wast)
      set(_simd_extra_flags --enable-relaxed-simd)
   endif()
   if(_simd_wast)
      add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/wasms/${testcase}.wasm_marker
         COMMAND ${WAST2JSON} ${WAST2JSON_FEATURES} --no-check ${_simd_extra_flags} ${_simd_wast} -o ${testcase}.json
         COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/wasms/${testcase}.wasm_marker
         WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/wasms
         DEPENDS ${_simd_wast}
         COMMENT "Generating SIMD wasms: ${testcase}"
      )
      list(APPEND SIMD_WASM_MARKERS ${CMAKE_BINARY_DIR}/wasms/${testcase}.wasm_marker)
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
   GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../psio/golden"
)
catch_discover_tests(component_tests)

add_executable(mem64_debug compliance/mem64_debug.cpp)
target_link_libraries(mem64_debug psizam psizam-exec)
target_include_directories(mem64_debug PUBLIC
   ${CMAKE_CURRENT_SOURCE_DIR}
   ${CMAKE_CURRENT_SOURCE_DIR}/../include
)
