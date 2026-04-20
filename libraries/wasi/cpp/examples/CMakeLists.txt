add_executable(wasi_tcp_echo tcp_echo.cpp)
target_link_libraries(wasi_tcp_echo PRIVATE wasi-host)
target_compile_features(wasi_tcp_echo PRIVATE cxx_std_23)
