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
