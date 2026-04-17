include(softfloat.cmake)

# Suppress all warnings on softfloat — it is third-party C code
target_compile_options(softfloat PRIVATE -w)

if(PSIZAM_ENABLE_WASM3)
   add_subdirectory(wasm3)
endif()
