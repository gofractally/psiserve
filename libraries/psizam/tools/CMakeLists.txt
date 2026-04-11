# ##################################################################################################
# Tools for psizam
# ##################################################################################################

if(CMAKE_SYSTEM_NAME STREQUAL "WASI")
   # WASI build: only pzam-compile, no runtime library needed
   add_executable(pzam-compile pzam_compile.cpp ../src/wasi_cxa_stubs.cpp
      ${CMAKE_SOURCE_DIR}/cmake/llvm-wasi/wasi_llvm_stubs.c)
   target_compile_definitions(pzam-compile PRIVATE PSIZAM_COMPILE_ONLY)
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
target_link_libraries(psizam-interp psizam psizam-runtime)

add_executable(bench-interp ${CMAKE_CURRENT_SOURCE_DIR}/bench_interp.cpp)
target_link_libraries(bench-interp psizam psizam-runtime)

add_executable(bench-host-calls ${CMAKE_CURRENT_SOURCE_DIR}/bench_host_calls.cpp)
target_link_libraries(bench-host-calls psizam psizam-runtime)

add_executable(bench-psibase-host ${CMAKE_CURRENT_SOURCE_DIR}/bench_psibase_host.cpp)
target_link_libraries(bench-psibase-host psizam psizam-runtime)

add_executable(hello-driver ${CMAKE_CURRENT_SOURCE_DIR}/hello_driver.cpp)
target_link_libraries(hello-driver psizam psizam-runtime)

add_executable(psizam-addr2line addr2line.cpp)
target_link_libraries(psizam-addr2line psizam psizam-runtime)

add_executable(bench-compile ${CMAKE_CURRENT_SOURCE_DIR}/bench_compile.cpp)
target_compile_definitions(bench-compile PRIVATE PSIZAM_COMPILE_ONLY)
target_link_libraries(bench-compile psizam psizam-runtime)

if(TARGET psizam-llvm)
   target_link_libraries(bench-compile psizam-llvm)
   target_compile_definitions(bench-compile PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()

add_executable(pzam-compile pzam_compile.cpp)
target_compile_definitions(pzam-compile PRIVATE PSIZAM_COMPILE_ONLY)
target_link_libraries(pzam-compile psizam psizam-runtime)

if(TARGET psizam-llvm)
   target_link_libraries(pzam-compile psizam-llvm)
   target_compile_definitions(pzam-compile PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()

add_executable(psizam-wasi ${CMAKE_CURRENT_SOURCE_DIR}/psizam_wasi.cpp)
target_link_libraries(psizam-wasi psizam psizam-runtime)

if(TARGET psizam-llvm)
   add_executable(pzam-run ${CMAKE_CURRENT_SOURCE_DIR}/pzam_run.cpp)
   target_link_libraries(pzam-run psizam psizam-runtime psizam-llvm)
endif()
