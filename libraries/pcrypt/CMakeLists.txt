add_library(pcrypt
   src/sha256.cpp
)
target_include_directories(pcrypt PUBLIC include)
target_link_libraries(pcrypt PUBLIC psio1)
target_compile_features(pcrypt PUBLIC cxx_std_23)

if(APPLE)
   target_link_libraries(pcrypt PRIVATE "-framework Security")
else()
   find_package(OpenSSL REQUIRED)
   target_link_libraries(pcrypt PRIVATE OpenSSL::Crypto)
endif()
