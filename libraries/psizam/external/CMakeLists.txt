include(softfloat.cmake)

# Suppress all warnings on softfloat — it is third-party C code
target_compile_options(softfloat PRIVATE -w)
