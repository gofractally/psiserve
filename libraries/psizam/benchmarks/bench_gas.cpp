// Phase 2a gas-metering microbench — runs the same workload twice
// (gas_insertion_strategy::off vs prepay_max) across every compiled-in
// backend: interpreter, jit (jit1), jit2, jit_llvm. The metered run
// uses an unlimited budget so the trap path never fires; we isolate
// the per-call fetch_sub + branch cost from any handler overhead.
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
#include <string>

#include "../tests/implementation_limits.hpp"

namespace {

void host_call() {}

using namespace psizam;
using namespace psizam::detail;
using rhf_t = registered_host_functions<standalone_function_t>;

struct result {
   double ms;
   double ns_per_call;
};

template <typename Impl>
result run_once(uint32_t depth, uint32_t iterations,
                gas_insertion_strategy strategy) {
   using backend_t = backend<rhf_t, Impl>;
   wasm_code code(
      implementation_limits_wasm,
      implementation_limits_wasm + implementation_limits_wasm_len);
   wasm_allocator wa;
   backend_t bkend(code, &wa);
   rhf_t::resolve(bkend.get_module());

   bkend.get_context().set_gas_strategy(strategy);
   bkend.get_context().set_gas_budget(gas_budget_unlimited);

   // Warm-up
   (void)bkend.call_with_return("env", "call", depth);

   const auto t0 = std::chrono::high_resolution_clock::now();
   for (uint32_t i = 0; i < iterations; ++i) {
      (void)bkend.call_with_return("env", "call", depth);
   }
   const auto t1 = std::chrono::high_resolution_clock::now();
   double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
   uint64_t calls = uint64_t(iterations) * (uint64_t(depth) + 1);
   return { ms, ms * 1'000'000.0 / double(calls) };
}

template <typename Impl>
void run_backend(const char* name, uint32_t depth, uint32_t iterations,
                 uint32_t trials) {
   double best_off = 1e18, best_on = 1e18;
   for (uint32_t t = 0; t < trials; ++t) {
      auto off = run_once<Impl>(depth, iterations, gas_insertion_strategy::off);
      auto on  = run_once<Impl>(depth, iterations, gas_insertion_strategy::prepay_max);
      if (off.ms < best_off) best_off = off.ms;
      if (on.ms  < best_on)  best_on  = on.ms;
   }
   double delta = best_on - best_off;
   uint64_t calls = uint64_t(iterations) * (uint64_t(depth) + 1);
   double overhead_ns = delta * 1'000'000.0 / double(calls);
   double pct = (best_on - best_off) / best_off * 100.0;
   std::printf("  %-14s off %8.2f ms (%6.2f ns)  "
               "on %8.2f ms (%6.2f ns)  "
               "Δ %+7.2f ms (%+.2f%%, ~%.2f ns/call)\n",
               name,
               best_off, best_off * 1'000'000.0 / double(calls),
               best_on,  best_on  * 1'000'000.0 / double(calls),
               delta, pct, overhead_ns);
}

} // namespace

int main() {
   rhf_t::add<&host_call>("env", "host.call");

   constexpr uint32_t depth      = 100;
   constexpr uint32_t iterations = 50'000;
   constexpr uint32_t trials     = 5;
   const uint64_t calls = uint64_t(iterations) * (uint64_t(depth) + 1);

   std::printf("bench_gas — depth=%u, iterations=%u, trials=%u (best-of)\n",
               depth, iterations, trials);
   std::printf("gas_charge invocations per run: %llu\n\n",
               (unsigned long long)calls);

   run_backend<interpreter>("interpreter", depth, iterations, trials);
#if defined(__x86_64__) || defined(__aarch64__)
   run_backend<jit>        ("jit",         depth, iterations, trials);
   run_backend<jit2>       ("jit2",        depth, iterations, trials);
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
   run_backend<jit_llvm>   ("jit_llvm",    depth, iterations, trials);
#endif

   return 0;
}
