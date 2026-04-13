# psio: fracpack serialization + JSON library (from psibase)
add_library(psio
   src/fpconv.c
   src/from_json.cpp
   src/chrono.cpp
   src/schema.cpp
)
target_include_directories(psio PUBLIC include)

# rapidjson (header-only, used by from_json)
target_include_directories(psio PUBLIC external/rapidjson/include)

# Boost headers (Preprocessor + core::demangle)
find_package(Boost REQUIRED)
target_link_libraries(psio PUBLIC Boost::headers)

target_compile_features(psio PUBLIC cxx_std_23)

option(PSIO_ENABLE_TESTS "Build psio tests" OFF)
if(PSIO_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(tests)

   add_executable(gen_test_vectors tools/gen_test_vectors.cpp)
   target_link_libraries(gen_test_vectors PRIVATE psio)
endif()
