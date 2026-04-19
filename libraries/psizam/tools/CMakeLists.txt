# ##################################################################################################
# Tools for psizam
# ##################################################################################################

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   # WASI build: only pzam-compile, no runtime library needed
   add_executable(pzam-compile pzam_compile.cpp ../src/wasi_cxa_stubs.cpp
      ${CMAKE_SOURCE_DIR}/cmake/llvm-wasi/wasi_llvm_stubs.c)
   target_compile_definitions(pzam-compile PRIVATE
      PSIZAM_COMPILE_ONLY
      PZAM_STANDALONE_COMPILE)
   target_include_directories(pzam-compile PRIVATE ${CMAKE_SOURCE_DIR}/cmake/llvm-wasi/wasi_compat)
   target_link_libraries(pzam-compile psizam)
   if(TARGET psizam-llvm)
      target_link_libraries(pzam-compile psizam-llvm)
      target_compile_definitions(pzam-compile PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
   endif()
   if(PSIZAM_ENABLE_SOFTFLOAT)
      target_link_libraries(pzam-compile softfloat)
   endif()
   return()
endif()

# Native build: all tools
add_executable(psizam-interp ${CMAKE_CURRENT_SOURCE_DIR}/interp.cpp)
target_link_libraries(psizam-interp psizam psizam-exec)

add_executable(bench-interp ${CMAKE_CURRENT_SOURCE_DIR}/bench_interp.cpp)
target_link_libraries(bench-interp psizam psizam-exec)

add_executable(bench-host-calls ${CMAKE_CURRENT_SOURCE_DIR}/bench_host_calls.cpp)
target_link_libraries(bench-host-calls psizam psizam-exec)

add_executable(bench-psibase-host ${CMAKE_CURRENT_SOURCE_DIR}/bench_psibase_host.cpp)
target_link_libraries(bench-psibase-host psizam psizam-exec)

add_executable(hello-driver ${CMAKE_CURRENT_SOURCE_DIR}/hello_driver.cpp)
target_link_libraries(hello-driver psizam psizam-exec)

add_executable(psizam-addr2line addr2line.cpp)
target_link_libraries(psizam-addr2line psizam psizam-exec)

add_executable(bench-compile ${CMAKE_CURRENT_SOURCE_DIR}/bench_compile.cpp)
target_compile_definitions(bench-compile PRIVATE PSIZAM_COMPILE_ONLY)
target_link_libraries(bench-compile psizam psizam-exec)

if(TARGET psizam-llvm)
   target_link_libraries(bench-compile psizam-llvm)
   target_compile_definitions(bench-compile PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()

# ── Standalone tools (pzam-compile, pzam-run) ────────────────────────────────
# These use the renamed entry points (pzam_compile_main, pzam_run_main) with
# PZAM_STANDALONE_* defines to generate a main() wrapper.

add_executable(pzam-compile pzam_compile.cpp)
target_compile_definitions(pzam-compile PRIVATE PZAM_STANDALONE_COMPILE)
target_link_libraries(pzam-compile psizam psizam-exec)

if(TARGET psizam-llvm)
   target_link_libraries(pzam-compile psizam-llvm)
   target_compile_definitions(pzam-compile PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()

add_executable(pzam-run pzam_run.cpp)
target_compile_definitions(pzam-run PRIVATE PZAM_STANDALONE_RUN PSIZAM_JIT_SIGNAL_DIAGNOSTICS)
target_link_libraries(pzam-run psizam psizam-exec)
if(TARGET psizam-llvm-jit)
   target_link_libraries(pzam-run psizam-llvm-jit)
endif()

add_executable(psizam-wasi ${CMAKE_CURRENT_SOURCE_DIR}/psizam_wasi.cpp)
target_link_libraries(psizam-wasi psizam psizam-exec)
if(TARGET psizam-llvm)
   target_link_libraries(psizam-wasi psizam-llvm)
   target_compile_definitions(psizam-wasi PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()

# ── Unified pzam CLI (compile + run + inspect + validate) ────────────────────

add_executable(pzam
   pzam.cpp
   pzam_compile.cpp
   pzam_run.cpp
   pzam_inspect.cpp
   pzam_validate.cpp
   pzam_wit.cpp
)
target_link_libraries(pzam psizam psizam-exec)
if(TARGET psizam-llvm)
   target_link_libraries(pzam psizam-llvm)
   target_compile_definitions(pzam PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()
if(TARGET psizam-llvm-jit)
   target_link_libraries(pzam psizam-llvm-jit)
endif()
