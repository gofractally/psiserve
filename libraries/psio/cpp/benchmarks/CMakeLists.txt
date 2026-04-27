# psio benchmarks — cross-library perf + wire-size harness.
#
# Shape library (shapes.hpp) defines reflected struct types at each
# size tier (Point, NameRecord, FlatRecord, Record, Validator, Order,
# ValidatorList, plus *Bounded variants).  harness.hpp is the timing
# + report utility layer.
#
# Competitor adapters (libcapnp, libflatbuffers, msgpack-c, protobuf,
# sszpp) are optional find_package-gated additions wired into the
# new psio_vs_externals bench (in progress).

# bench_perf_report (legacy): v1 ↔ v3 markdown+CSV report.  shapes.hpp
# was rewritten to drop the V1Foo/V3Foo pairs, which made this
# obsolete.  Kept on disk as reference for the new bench's loop
# structure, but no longer built by default — re-enable manually if
# you need the old comparison columns.
#
# add_executable(psio3_bench_perf_report bench_perf_report.cpp)
# target_link_libraries(psio3_bench_perf_report PRIVATE psio psio)
# target_include_directories(psio3_bench_perf_report PRIVATE
#    ${CMAKE_CURRENT_SOURCE_DIR})
# target_compile_features(psio3_bench_perf_report PRIVATE cxx_std_23)

# Nested-encode baseline — per-format encode timing on balanced
# binary trees of progressively greater depth.  Surfaces the
# fractal O(D × N) recursive-size-walk cost that the protobuf
# stack-cache eliminated; identifies which other formats need
# the same treatment.
add_executable(psio3_bench_nested_encode bench_nested_encode.cpp)
target_link_libraries(psio3_bench_nested_encode PRIVATE psio)
target_include_directories(psio3_bench_nested_encode PRIVATE
   ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(psio3_bench_nested_encode PRIVATE cxx_std_23)
target_compile_options(psio3_bench_nested_encode PRIVATE -O3 -DNDEBUG)

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
