# ##################################################################################################
# Create the psizam interpreter driver.
# ##################################################################################################
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

add_executable(pzam-compile pzam_compile.cpp)
target_compile_definitions(pzam-compile PRIVATE PSIZAM_COMPILE_ONLY)
target_link_libraries(pzam-compile psizam psizam-runtime)

if(TARGET psizam-llvm)
   target_link_libraries(pzam-compile psizam-llvm)
   target_compile_definitions(pzam-compile PRIVATE PSIZAM_ENABLE_LLVM_BACKEND)
endif()
