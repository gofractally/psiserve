add_executable(psio_tests
   main.cpp
   frac_ref_tests.cpp
   frac16_tests.cpp
   frac16_mutation_tests.cpp
   frac16_fuzz_generated.cpp
   bin_extensibility_tests.cpp
   json_tests.cpp
   multiformat_tests.cpp
   ext_int_tests.cpp
   bounded_tests.cpp
   schema_bounded_tests.cpp
   bitset_tests.cpp
   ssz_tests.cpp
   pssz_tests.cpp
   beacon_state_tests.cpp
   view_tests.cpp
   wit_variant_tests.cpp
   capnp_view_tests.cpp
   capnp_schema_tests.cpp
   capnp_parser_tests.cpp
   fbs_parser_tests.cpp
   wit_resource_tests.cpp
   wit_attribute_tests.cpp
   psio_attribute_tests.cpp
   psio_structural_tests.cpp
   validate_frac_tests.cpp
)

target_link_libraries(psio_tests PRIVATE psio)
target_include_directories(psio_tests PRIVATE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include
)
target_compile_features(psio_tests PRIVATE cxx_std_23)

add_test(NAME psio_tests COMMAND psio_tests)
