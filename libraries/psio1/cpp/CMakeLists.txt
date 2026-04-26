# psio1: fracpack serialization + JSON library (from psibase)
add_library(psio1
   src/fpconv.c
   src/from_json.cpp
   src/chrono.cpp
   src/emit_wit.cpp
   src/schema.cpp
)
target_include_directories(psio1 PUBLIC include)

# rapidjson (header-only, used by from_json)
target_include_directories(psio1 PUBLIC external/rapidjson/include)

# Boost headers (Preprocessor + core::demangle)
find_package(Boost REQUIRED)
target_link_libraries(psio1 PUBLIC Boost::headers)

target_compile_features(psio1 PUBLIC cxx_std_23)

add_executable(psio_tool tools/psio_tool.cpp)
target_link_libraries(psio_tool PRIVATE psio1)

option(PSIO1_ENABLE_TESTS "Build psio1 tests" OFF)
if(PSIO1_ENABLE_TESTS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_subdirectory(tests)

   add_executable(gen_test_vectors tools/gen_test_vectors.cpp)
   target_link_libraries(gen_test_vectors PRIVATE psio1)

   # Cross-validation fixture emitters — each prints hex on stdout
   # for the Rust side to verify against. Keeps C++ and Rust wire
   # formats synchronized for SSZ and pSSZ.
   foreach(xval_tool IN ITEMS
       emit_xval_fixtures
       emit_xval_extint
       emit_xval_arrays
       emit_xval_bits
       emit_xval_validator)
      add_executable(${xval_tool} tools/${xval_tool}.cpp)
      target_link_libraries(${xval_tool} PRIVATE psio1)
      target_compile_features(${xval_tool} PRIVATE cxx_std_23)
   endforeach()
endif()

option(PSIO1_ENABLE_BENCHMARKS "Build psio1 benchmarks" OFF)
if(PSIO1_ENABLE_BENCHMARKS AND NOT CMAKE_SYSTEM_NAME STREQUAL "WASI")
   add_executable(psio_bench benchmarks/bench_fracpack.cpp)
   target_link_libraries(psio_bench PRIVATE psio1)
   target_compile_options(psio_bench PRIVATE -O2)

   # frac16 size/speed comparison against frac32 baseline.
   add_executable(psio_bench16 benchmarks/bench_fracpack16.cpp)
   target_link_libraries(psio_bench16 PRIVATE psio1)
   target_compile_options(psio_bench16 PRIVATE -O2)

   # Shadow-index over pack_bin: vs fracpack view vs full unpack (read-all).
   add_executable(psio_bench_shadow benchmarks/bench_shadow_index.cpp)
   target_link_libraries(psio_bench_shadow PRIVATE psio1)
   target_compile_options(psio_bench_shadow PRIVATE -O2)

   # Aggregate perf on 200 fuzz-generated random struct types.
   add_executable(psio_bench_fuzz benchmarks/bench_fuzz_generated.cpp)
   target_link_libraries(psio_bench_fuzz PRIVATE psio1)
   target_compile_options(psio_bench_fuzz PRIVATE -O2)

   add_executable(psio_bench_zerocomp benchmarks/bench_zero_compress.cpp)
   target_link_libraries(psio_bench_zerocomp PRIVATE psio1)
   target_compile_options(psio_bench_zerocomp PRIVATE -O2)
   find_library(SNAPPY_LIB snappy)
   find_path(SNAPPY_INC snappy.h)
   if(SNAPPY_LIB AND SNAPPY_INC)
      target_link_libraries(psio_bench_zerocomp PRIVATE ${SNAPPY_LIB})
      target_include_directories(psio_bench_zerocomp PRIVATE ${SNAPPY_INC})
      target_compile_definitions(psio_bench_zerocomp PRIVATE HAS_SNAPPY)
   endif()
   find_package(PkgConfig QUIET)
   if(PkgConfig_FOUND)
      pkg_check_modules(LZ4 QUIET IMPORTED_TARGET liblz4)
      if(LZ4_FOUND)
         target_link_libraries(psio_bench_zerocomp PRIVATE PkgConfig::LZ4)
         target_compile_definitions(psio_bench_zerocomp PRIVATE HAS_LZ4)
      endif()
      pkg_check_modules(ZSTD QUIET IMPORTED_TARGET libzstd)
      if(ZSTD_FOUND)
         target_link_libraries(psio_bench_zerocomp PRIVATE PkgConfig::ZSTD)
         target_compile_definitions(psio_bench_zerocomp PRIVATE HAS_ZSTD)
      endif()
   endif()

   add_executable(psio_bench_cp benchmarks/bench_cp_view.cpp)
   target_link_libraries(psio_bench_cp PRIVATE psio1)
   target_compile_options(psio_bench_cp PRIVATE -O2)

   # Canonical ABI (WIT) serialization benchmark
   add_executable(psio_bench_wit benchmarks/bench_wit.cpp)
   target_link_libraries(psio_bench_wit PRIVATE psio1)
   target_compile_options(psio_bench_wit PRIVATE -O2)

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
      target_link_libraries(psio_bench_reflect_flatbuf PRIVATE psio1 flatbuffers::flatbuffers)
      target_include_directories(psio_bench_reflect_flatbuf PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
      target_compile_options(psio_bench_reflect_flatbuf PRIVATE -O2)
      message(STATUS "FlatBuffers found — building psio_bench_reflect_flatbuf")

      add_executable(psio_bench_standalone_flatbuf benchmarks/bench_standalone_flatbuf.cpp)
      target_link_libraries(psio_bench_standalone_flatbuf PRIVATE psio1 flatbuffers::flatbuffers)
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
      find_package(absl QUIET)
      if(absl_FOUND)
         target_link_libraries(psio_bench_protobuf PRIVATE
            absl::log_internal_check_op absl::strings absl::status)
      endif()
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

   # ── SSZ BeaconState benchmark ────────────────────────────────────────────
   # psio1-only build is always present. Optional comparison against sszpp
   # (OffchainLabs) is gated on PSIO1_BENCH_SSZPP + PSIO1_SSZPP_DIR.

   add_executable(psio_bench_ssz_beacon benchmarks/bench_ssz_beacon.cpp)
   target_link_libraries(psio_bench_ssz_beacon PRIVATE psio1)
   target_compile_features(psio_bench_ssz_beacon PRIVATE cxx_std_23)
   target_compile_options(psio_bench_ssz_beacon PRIVATE -O3)

   # Real current-mainnet (Fulu) BeaconState — 310 MiB load + round-trip.
   add_executable(psio_bench_modern_state benchmarks/bench_modern_state.cpp)
   target_link_libraries(psio_bench_modern_state PRIVATE psio1)
   target_compile_features(psio_bench_modern_state PRIVATE cxx_std_23)
   target_compile_options(psio_bench_modern_state PRIVATE -O3)

   option(PSIO1_BENCH_SSZPP "Enable sszpp comparison in psio_bench_ssz_beacon" OFF)
   set(PSIO1_SSZPP_DIR "" CACHE PATH
       "Path to OffchainLabs/sszpp source tree (headers under lib/)")
   set(PSIO1_INTX_DIR ""  CACHE PATH
       "Path to chfast/intx source tree (sszpp dependency, headers under include/)")
   set(PSIO1_HASHTREE_DIR "" CACHE PATH
       "Path to prysmaticlabs/hashtree source tree (sszpp dependency, built under build/)")

   if(PSIO1_BENCH_SSZPP)
      if(NOT PSIO1_SSZPP_DIR)
         message(FATAL_ERROR
                 "PSIO1_BENCH_SSZPP=ON requires -DPSIO_SSZPP_DIR=/path/to/sszpp")
      endif()
      if(NOT EXISTS "${PSIO1_SSZPP_DIR}/lib/ssz++.hpp")
         message(WARNING
                 "PSIO1_SSZPP_DIR=${PSIO1_SSZPP_DIR} does not contain lib/ssz++.hpp. "
                 "Verify the sszpp checkout path.")
      endif()
      target_include_directories(psio_bench_ssz_beacon PRIVATE
          "${PSIO1_SSZPP_DIR}/lib"
          "${PSIO1_SSZPP_DIR}")
      if(PSIO1_INTX_DIR)
         target_include_directories(psio_bench_ssz_beacon PRIVATE
             "${PSIO1_INTX_DIR}/include")
      else()
         message(FATAL_ERROR
                 "sszpp depends on chfast/intx; set -DPSIO_INTX_DIR=/path/to/intx")
      endif()
      if(PSIO1_HASHTREE_DIR)
         target_include_directories(psio_bench_ssz_beacon PRIVATE
             "${PSIO1_HASHTREE_DIR}/src")
         target_link_libraries(psio_bench_ssz_beacon PRIVATE
             "${PSIO1_HASHTREE_DIR}/build/lib/libhashtree.a")
      else()
         message(FATAL_ERROR
                 "sszpp depends on prysmaticlabs/hashtree; "
                 "set -DPSIO_HASHTREE_DIR=/path/to/hashtree (run `make` inside it first)")
      endif()
      target_compile_definitions(psio_bench_ssz_beacon PRIVATE PSIO1_HAVE_SSZPP=1)
      message(STATUS "sszpp enabled: ${PSIO1_SSZPP_DIR}")
   else()
      message(STATUS
              "sszpp disabled (use -DPSIO_BENCH_SSZPP=ON -DPSIO_SSZPP_DIR=... "
              "-DPSIO_INTX_DIR=... -DPSIO_HASHTREE_DIR=... to enable)")
   endif()
endif()
