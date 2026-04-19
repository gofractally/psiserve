// Phase 2a gas-metering microbench — interpreter only.
//
// Measures the overhead of the per-call gas_charge() hook. The hot path
// is a recursive WASM function that performs N self-calls; we time it
// with gas_insertion_strategy::off (baseline) vs prepay_max (metered
// with an unlimited budget so the handler never fires — we isolate the
// fetch_sub + branch cost from any trap path).
//
// The WASM module is the same embedded binary used by
// tests/implementation_limits.hpp: its `call` export is a recursive
// function that takes an i32 depth and recurses N+1 times before
// returning. That's ideal for a call-heavy bench.

#include <psizam/backend.hpp>

#include <cstdio>
#include <cstdint>
#include <chrono>
#include <vector>

#include "../tests/implementation_limits.hpp"

namespace {

void host_call() {}

using namespace psizam;
using namespace psizam::detail;
using rhf_t     = registered_host_functions<standalone_function_t>;
using backend_t = backend<rhf_t, interpreter>;

double run(backend_t& bkend, uint32_t depth, uint32_t iterations) {
   const auto t0 = std::chrono::high_resolution_clock::now();
   for (uint32_t i = 0; i < iterations; ++i) {
      (void)bkend.call_with_return("env", "call", depth);
   }
   const auto t1 = std::chrono::high_resolution_clock::now();
   return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

} // namespace

int main() {
   rhf_t::add<&host_call>("env", "host.call");

   wasm_code code(
      implementation_limits_wasm,
      implementation_limits_wasm + implementation_limits_wasm_len);
   wasm_allocator wa;
   backend_t bkend(code, &wa);
   rhf_t::resolve(bkend.get_module());

   constexpr uint32_t depth      = 100;    // WASM self-calls per top-level call
   constexpr uint32_t iterations = 50'000; // top-level calls per timing run
   constexpr uint32_t trials     = 5;

   // Each iteration does one top-level call_with_return which enters
   // the WASM `call` export that then recurses `depth` more times —
   // so total gas_charge() invocations per iteration ≈ depth + 1.
   const uint64_t gas_calls_per_run = uint64_t(iterations) * (depth + 1);

   std::printf("bench_gas — interpreter — depth=%u, iterations=%u, trials=%u\n",
               depth, iterations, trials);
   std::printf("gas_charge invocations per run: %llu\n",
               (unsigned long long)gas_calls_per_run);
   std::printf("\n  strategy     trial  ms         ns/call\n");

   double best_off = 1e18, best_on = 1e18;
   for (uint32_t trial = 0; trial < trials; ++trial) {
      // Baseline: strategy::off → gas_charge is a single compare + return.
      bkend.get_context().set_gas_strategy(gas_insertion_strategy::off);
      double ms_off = run(bkend, depth, iterations);
      double ns_off = ms_off * 1'000'000.0 / double(gas_calls_per_run);
      std::printf("  off          %5u  %-10.2f %.2f\n", trial, ms_off, ns_off);
      if (ms_off < best_off) best_off = ms_off;

      // Metered: strategy::prepay_max → atomic fetch_sub + negative test.
      // Unlimited budget (the default INT64_MAX) so the branch never
      // triggers — measuring pure instrumentation overhead.
      bkend.get_context().set_gas_strategy(gas_insertion_strategy::prepay_max);
      bkend.get_context().set_gas_budget(gas_budget_unlimited);
      double ms_on  = run(bkend, depth, iterations);
      double ns_on  = ms_on * 1'000'000.0 / double(gas_calls_per_run);
      std::printf("  prepay_max   %5u  %-10.2f %.2f\n", trial, ms_on, ns_on);
      if (ms_on < best_on) best_on = ms_on;
   }

   double delta_ms = best_on - best_off;
   double overhead_ns = delta_ms * 1'000'000.0 / double(gas_calls_per_run);
   double pct = (best_on - best_off) / best_off * 100.0;
   std::printf("\nBest-of-%u summary:\n", trials);
   std::printf("  off:        %.2f ms\n", best_off);
   std::printf("  prepay_max: %.2f ms\n", best_on);
   std::printf("  delta:      %.2f ms (%.2f%% overhead)\n", delta_ms, pct);
   std::printf("  ≈ %.2f ns per gas_charge() call\n", overhead_ns);
   return 0;
}
