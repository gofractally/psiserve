// Gas-metering microbench — covers several workloads to show the
// absolute runtime cost with metering engaged. Each workload runs with
// an unlimited budget (deadline=UINT64_MAX) so the inline charge site
// fires at every function entry / loop back-edge but the handler never
// trips; we measure the total time that inline path adds.
//
// Prior to the gas_state redesign this file compared
// `gas_insertion_strategy::off` vs `prepay_max`. The Phase C universal
// memory-deadline codegen emits the check unconditionally at runtime
// — the only way to disable it is to compile the module without
// metering caps, which is a compile-time decision. We therefore report
// absolute timings instead; a no-metering baseline belongs in a
// separate bench that builds modules with `meter_cap::none`.
//
// Workloads (ordered from call-heavy to compute-heavy):
//
//   recurse(100, 50000)   — 5M calls, ~0 work per call
//                           (upper bound on gas overhead as a %age)
//   fib(1_000_000) × 1000 — 1M-iter loop × 1000 calls = 1K host→wasm
//                           entries, heavy compute per call
//   sort(10_000) × 10     — 10K bubble-sort-64 iters × 10 calls,
//                           memory-heavy
//   matmul(10_000) × 10   — 10K 8×8-matmul iters × 10 calls,
//                           arithmetic-heavy

#include <psizam/backend.hpp>

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <vector>
#include <string>

#include "../tests/implementation_limits.hpp"

namespace {

void host_call() {}

using namespace psizam;
using namespace psizam::detail;
using rhf_t = registered_host_functions<standalone_function_t>;

static std::vector<uint8_t> read_wasm(const char* path) {
   std::ifstream in(path, std::ios::binary);
   if (!in.good()) return {};
   return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

using clock_t_ = std::chrono::high_resolution_clock;

// ── Workload drivers ────────────────────────────────────────────────────────

// Call-heavy recursive WASM workload.
template <typename Impl>
double run_recurse(wasm_code code, uint32_t depth, uint32_t iterations) {
   using backend_t = backend<rhf_t, Impl>;
   wasm_allocator wa;
   backend_t bkend(code, &wa);
   rhf_t::resolve(bkend.get_module());
   bkend.get_context().set_gas_budget(gas_units{gas_budget_unlimited});
   (void)bkend.call_with_return("env", "call", depth); // warm
   const auto t0 = clock_t_::now();
   for (uint32_t i = 0; i < iterations; ++i)
      (void)bkend.call_with_return("env", "call", depth);
   return std::chrono::duration<double, std::milli>(clock_t_::now() - t0).count();
}

// Generic "call exported function N times with an i32 arg" driver.
template <typename Impl>
double run_compute(wasm_code code, const char* fn, int32_t arg,
                   uint32_t iterations) {
   using backend_t = backend<std::nullptr_t, Impl>;
   wasm_allocator wa;
   backend_t bkend(code, &wa);
   bkend.get_context().set_gas_budget(gas_units{gas_budget_unlimited});
   (void)bkend.call_with_return(static_cast<void*>(nullptr), fn, arg); // warm
   const auto t0 = clock_t_::now();
   for (uint32_t i = 0; i < iterations; ++i)
      (void)bkend.call_with_return(static_cast<void*>(nullptr), fn, arg);
   return std::chrono::duration<double, std::milli>(clock_t_::now() - t0).count();
}

// ── Reporter ────────────────────────────────────────────────────────────────

struct row {
   const char* backend;
   double      ms;
};

template <typename F>
row trial_best(const char* backend_name, uint32_t trials, F&& runner) {
   double best = 1e18;
   for (uint32_t t = 0; t < trials; ++t) {
      double v = runner();
      if (v < best) best = v;
   }
   return { backend_name, best };
}

void report(const char* workload, uint64_t approx_gas_charges,
            const std::vector<row>& rows) {
   std::printf("\n── %s (≈%llu gas_charge calls per run) ──\n",
               workload, (unsigned long long)approx_gas_charges);
   std::printf("  %-12s  %10s\n", "backend", "time (ms)");
   for (const auto& r : rows)
      std::printf("  %-12s  %10.2f\n", r.backend, r.ms);
}

} // namespace

int main() {
   rhf_t::add<&host_call>("env", "host.call");

   const uint32_t trials = 3;

   // ── Workload 1: recursive (call-heavy, gas worst case) ──
   {
      wasm_code code(implementation_limits_wasm,
                     implementation_limits_wasm + implementation_limits_wasm_len);
      constexpr uint32_t depth = 100, iterations = 50'000;
      std::vector<row> rows = {
         trial_best("interpreter", trials, [&](){ return run_recurse<interpreter>(code, depth, iterations); }),
         trial_best("jit",         trials, [&](){ return run_recurse<jit>        (code, depth, iterations); }),
         trial_best("jit2",        trials, [&](){ return run_recurse<jit2>       (code, depth, iterations); }),
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
         trial_best("jit_llvm",    trials, [&](){ return run_recurse<jit_llvm>   (code, depth, iterations); }),
#endif
      };
      uint64_t charges = uint64_t(iterations) * (uint64_t(depth) + 1);
      report("recursive depth=100 × iter=50K (call-heavy)", charges, rows);
   }

#ifdef BENCH_HAS_COMPUTE
   auto misc = read_wasm(BENCH_MISC_WASM);
   if (misc.empty()) {
      std::printf("\nbench_misc.wasm not found at %s — skipping compute workloads.\n",
                  BENCH_MISC_WASM);
      return 0;
   }
   wasm_code misc_code(misc.begin(), misc.end());

   // ── Workload 2: iterative fib (compute, low call count) ──
   {
      constexpr int32_t arg = 1'000'000;
      constexpr uint32_t iterations = 1000;
      std::vector<row> rows = {
         trial_best("jit",      trials, [&](){ return run_compute<jit>     (misc_code, "bench_fib", arg, iterations); }),
         trial_best("jit2",     trials, [&](){ return run_compute<jit2>    (misc_code, "bench_fib", arg, iterations); }),
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
         trial_best("jit_llvm", trials, [&](){ return run_compute<jit_llvm>(misc_code, "bench_fib", arg, iterations); }),
#endif
      };
      report("bench_fib(1M) × 1000 (compute, 1K host→wasm calls)", iterations, rows);
   }

   // ── Workload 3: bubble sort (memory-heavy) ──
   {
      constexpr int32_t arg = 10'000;
      constexpr uint32_t iterations = 10;
      std::vector<row> rows = {
         trial_best("jit",      trials, [&](){ return run_compute<jit>     (misc_code, "bench_sort", arg, iterations); }),
         trial_best("jit2",     trials, [&](){ return run_compute<jit2>    (misc_code, "bench_sort", arg, iterations); }),
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
         trial_best("jit_llvm", trials, [&](){ return run_compute<jit_llvm>(misc_code, "bench_sort", arg, iterations); }),
#endif
      };
      report("bench_sort(10K) × 10 (memory-heavy)", iterations, rows);
   }

   // ── Workload 4: 8×8 matmul (arithmetic-heavy) ──
   {
      constexpr int32_t arg = 10'000;
      constexpr uint32_t iterations = 10;
      std::vector<row> rows = {
         trial_best("jit",      trials, [&](){ return run_compute<jit>     (misc_code, "bench_matmul", arg, iterations); }),
         trial_best("jit2",     trials, [&](){ return run_compute<jit2>    (misc_code, "bench_matmul", arg, iterations); }),
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
         trial_best("jit_llvm", trials, [&](){ return run_compute<jit_llvm>(misc_code, "bench_matmul", arg, iterations); }),
#endif
      };
      report("bench_matmul(10K) × 10 (arithmetic-heavy)", iterations, rows);
   }
#endif

   return 0;
}
