# ##################################################################################################
# Create the psizam interpreter driver.
# ##################################################################################################
add_executable(psizam-interp ${CMAKE_CURRENT_SOURCE_DIR}/interp.cpp)
target_link_libraries(psizam-interp psizam)

add_executable(bench-interp ${CMAKE_CURRENT_SOURCE_DIR}/bench_interp.cpp)
target_link_libraries(bench-interp psizam)

add_executable(bench-host-calls ${CMAKE_CURRENT_SOURCE_DIR}/bench_host_calls.cpp)
target_link_libraries(bench-host-calls psizam)

add_executable(bench-psibase-host ${CMAKE_CURRENT_SOURCE_DIR}/bench_psibase_host.cpp)
target_link_libraries(bench-psibase-host psizam)

add_executable(hello-driver ${CMAKE_CURRENT_SOURCE_DIR}/hello_driver.cpp)
target_link_libraries(hello-driver psizam)

add_executable(psizam-addr2line addr2line.cpp)
target_link_libraries(psizam-addr2line psizam)
