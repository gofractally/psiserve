# psio3 benchmarks — format-by-format perf parity against v1 lands
# starting Phase 6. Phase 0 scaffold: empty subdir (nothing to build).
#
# Pattern reused by each bench:
#
#   add_executable(psio3_bench_<name> bench_<name>.cpp)
#   target_link_libraries(psio3_bench_<name> PRIVATE psio3)
#
# The v1 bench harness under libraries/psio/cpp/benchmarks/ is NOT
# linked here — v1 benches stay on v1; v3 benches run the same
# workloads through psio3. CI compares medians.
