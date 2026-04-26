add_library(pfs
   src/cid.cpp
   src/unixfs.cpp
   src/block_store.cpp
   src/cas.cpp
   src/store.cpp
)
target_include_directories(pfs PUBLIC include)
target_link_libraries(pfs PUBLIC psio1 psitri pcrypt)
target_compile_features(pfs PUBLIC cxx_std_23)

option(PFS_ENABLE_TESTS "Build pfs tests" OFF)
if(PFS_ENABLE_TESTS)
   add_subdirectory(tests)
endif()
