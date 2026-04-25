# psio3 benchmarks — cross-library perf + wire-size harness.
#
# Shape library (shapes.hpp) defines parallel PSIO_REFLECT / PSIO3_REFLECT
# structs at each size tier; harness.hpp is the timing + report utility
# layer; each bench_*.cpp links psio (v1) + psio3 into the same binary
# so encodings can be compared byte-for-byte and timed in one pass.
#
# Competitor adapters (libcapnp, libflatbuffers, msgpack-c, protobuf,
# sszpp) are optional find_package-gated additions in follow-ups.

add_executable(psio3_bench_perf_report bench_perf_report.cpp)
target_link_libraries(psio3_bench_perf_report PRIVATE psio3 psio)
target_include_directories(psio3_bench_perf_report PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio3_bench_perf_report PRIVATE cxx_std_23)
