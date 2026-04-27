# psio benchmarks — cross-library perf + wire-size harness.
#
# Shape library (shapes.hpp) defines parallel PSIO1_REFLECT / PSIO_REFLECT
# structs at each size tier; harness.hpp is the timing + report utility
# layer; each bench_*.cpp links psio (v1) + psio into the same binary
# so encodings can be compared byte-for-byte and timed in one pass.
#
# Competitor adapters (libcapnp, libflatbuffers, msgpack-c, protobuf,
# sszpp) are optional find_package-gated additions in follow-ups.

add_executable(psio3_bench_perf_report bench_perf_report.cpp)
target_link_libraries(psio3_bench_perf_report PRIVATE psio psio)
target_include_directories(psio3_bench_perf_report PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio3_bench_perf_report PRIVATE cxx_std_23)

# Varint codec library — leb128, prefix2 (BE + LE), prefix3.
add_executable(psio3_bench_varint bench_varint.cpp)
target_link_libraries(psio3_bench_varint PRIVATE psio)
target_include_directories(psio3_bench_varint PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio3_bench_varint PRIVATE cxx_std_23)
target_compile_options(psio3_bench_varint PRIVATE -O3 -DNDEBUG)

# pjson walk: JSON → pjson, plus every reader path on the resulting
# bytes. simdjson DOM / on-demand are included as competitive
# baselines when simdjson is found by the parent.
add_executable(psio3_bench_pjson_walk bench_pjson_walk.cpp)
target_link_libraries(psio3_bench_pjson_walk PRIVATE psio)
target_include_directories(psio3_bench_pjson_walk PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio3_bench_pjson_walk PRIVATE cxx_std_23)
target_compile_options(psio3_bench_pjson_walk PRIVATE -O3 -DNDEBUG)

# pjson per-type encode/decode/view-access timing.
add_executable(psio3_bench_pjson_types bench_pjson_types.cpp)
target_link_libraries(psio3_bench_pjson_types PRIVATE psio)
target_include_directories(psio3_bench_pjson_types PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio3_bench_pjson_types PRIVATE cxx_std_23)
target_compile_options(psio3_bench_pjson_types PRIVATE -O3 -DNDEBUG)

# psch schema-format perf: encode + view + lookup latency, plus
# size-versus-typical-alternatives.
add_executable(psio3_bench_psch bench_psch.cpp)
target_link_libraries(psio3_bench_psch PRIVATE psio)
target_include_directories(psio3_bench_psch PRIVATE
   ${CMAKE_SOURCE_DIR}/external/psitri/libraries/hash/include)
target_compile_features(psio3_bench_psch PRIVATE cxx_std_23)
target_compile_options(psio3_bench_psch PRIVATE -O3 -DNDEBUG)
