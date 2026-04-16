add_executable(psio_tests
   main.cpp
   frac_ref_tests.cpp
   json_tests.cpp
   multiformat_tests.cpp
   view_tests.cpp
   capnp_view_tests.cpp
   capnp_schema_tests.cpp
)

target_link_libraries(psio_tests PRIVATE psio)
target_include_directories(psio_tests PRIVATE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include
)
target_compile_features(psio_tests PRIVATE cxx_std_23)

add_test(NAME psio_tests COMMAND psio_tests)
