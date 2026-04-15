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

option(PSIO_ENABLE_BENCHMARKS "Build psio benchmarks" OFF)
if(PSIO_ENABLE_BENCHMARKS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_executable(psio_bench benchmarks/bench_fracpack.cpp)
   target_link_libraries(psio_bench PRIVATE psio)
   target_compile_options(psio_bench PRIVATE -O2)

   add_executable(psio_bench_cp benchmarks/bench_cp_view.cpp)
   target_link_libraries(psio_bench_cp PRIVATE psio)
   target_compile_options(psio_bench_cp PRIVATE -O2)

   find_package(CapnProto QUIET)
   if(CapnProto_FOUND)
      capnp_generate_cpp(CAPNP_SRCS CAPNP_HDRS benchmarks/bench_schemas.capnp)
      add_executable(psio_bench_capnp benchmarks/bench_capnp.cpp ${CAPNP_SRCS})
      target_link_libraries(psio_bench_capnp PRIVATE CapnProto::capnp)
      target_include_directories(psio_bench_capnp PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
      target_compile_options(psio_bench_capnp PRIVATE -O2)
      message(STATUS "Cap'n Proto found — building psio_bench_capnp")
   else()
      message(STATUS "Cap'n Proto not found — skipping psio_bench_capnp")
   endif()

   find_package(flatbuffers QUIET)
   if(flatbuffers_FOUND)
      add_executable(psio_bench_flatbuf benchmarks/bench_flatbuf.cpp)
      target_link_libraries(psio_bench_flatbuf PRIVATE flatbuffers::flatbuffers)
      target_include_directories(psio_bench_flatbuf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_compile_options(psio_bench_flatbuf PRIVATE -O2)
      message(STATUS "FlatBuffers found — building psio_bench_flatbuf")

      add_executable(psio_bench_reflect_flatbuf benchmarks/bench_reflect_flatbuf.cpp)
      target_link_libraries(psio_bench_reflect_flatbuf PRIVATE psio flatbuffers::flatbuffers)
      target_include_directories(psio_bench_reflect_flatbuf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_compile_options(psio_bench_reflect_flatbuf PRIVATE -O2)
      message(STATUS "FlatBuffers found — building psio_bench_reflect_flatbuf")

      add_executable(psio_bench_standalone_flatbuf benchmarks/bench_standalone_flatbuf.cpp)
      target_link_libraries(psio_bench_standalone_flatbuf PRIVATE psio flatbuffers::flatbuffers)
      target_include_directories(psio_bench_standalone_flatbuf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_compile_options(psio_bench_standalone_flatbuf PRIVATE -O2)
      message(STATUS "FlatBuffers found — building psio_bench_standalone_flatbuf")
   else()
      message(STATUS "FlatBuffers not found — skipping psio_bench_flatbuf")
   endif()

   find_package(Protobuf QUIET)
   if(Protobuf_FOUND)
      add_executable(psio_bench_protobuf
         benchmarks/bench_protobuf.cpp
         benchmarks/bench_schemas.pb.cc)
      target_link_libraries(psio_bench_protobuf PRIVATE protobuf::libprotobuf)
      target_include_directories(psio_bench_protobuf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_compile_options(psio_bench_protobuf PRIVATE -O2)
      message(STATUS "Protobuf found — building psio_bench_protobuf")
   else()
      message(STATUS "Protobuf not found — skipping psio_bench_protobuf")
   endif()

   find_package(msgpack-cxx QUIET)
   if(msgpack-cxx_FOUND)
      add_executable(psio_bench_msgpack benchmarks/bench_msgpack.cpp)
      target_link_libraries(psio_bench_msgpack PRIVATE msgpack-cxx)
      target_compile_options(psio_bench_msgpack PRIVATE -O2)
      message(STATUS "msgpack-cxx found — building psio_bench_msgpack")
   else()
      message(STATUS "msgpack-cxx not found — skipping psio_bench_msgpack")
   endif()
endif()
