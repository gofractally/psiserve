add_library(m3 STATIC
    m3_bind.c
    m3_code.c
    m3_compile.c
    m3_core.c
    m3_env.c
    m3_exec.c
    m3_function.c
    m3_info.c
    m3_module.c
    m3_parse.c
)

target_include_directories(m3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(m3 PRIVATE c_std_99)

# Suppress warnings in third-party code
target_compile_options(m3 PRIVATE -w)
