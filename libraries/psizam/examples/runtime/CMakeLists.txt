add_executable(hello_runtime hello_runtime.cpp
   ${CMAKE_SOURCE_DIR}/libraries/psizam/src/runtime.cpp)
target_link_libraries(hello_runtime PRIVATE psizam psizam-exec)
target_include_directories(hello_runtime PRIVATE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/include)
