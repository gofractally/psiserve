# Use psizam's bundled Catch2 v2
set(CATCH2_INCLUDE "${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include")
if(NOT EXISTS "${CATCH2_INCLUDE}/catch2/catch.hpp")
   message(WARNING "pfs tests: Catch2 not found at ${CATCH2_INCLUDE}, skipping tests")
   return()
endif()

add_executable(pfs_tests
   pfs_tests.cpp
)
target_include_directories(pfs_tests PRIVATE "${CATCH2_INCLUDE}")
target_link_libraries(pfs_tests PRIVATE pfs)

add_test(NAME pfs_tests COMMAND pfs_tests)
