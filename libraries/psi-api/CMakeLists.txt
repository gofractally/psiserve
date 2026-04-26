add_library(psi-api INTERFACE)
target_include_directories(psi-api INTERFACE
   ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(psi-api INTERFACE psio1)
