#include <psizam/composition.hpp>
#include <chrono>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>

// Include shared interface declarations
#include "shared.hpp"

struct BenchHost {
   void log_u64(uint64_t) {}
   void log_string(std::string_view) {}
};
PSIO_HOST_MODULE(BenchHost, interface(env, log_u64, log_string))

static std::vector<uint8_t> read_file(const char* path) {
   std::ifstream f(path, std::ios::binary);
   return {std::istreambuf_iterator<char>(f), {}};
}

template <typename Backend>
struct bench_result {
   const char* backend_name;
   double scalar_i32_cps;
   double scalar_i64_cps;
   double string_short_cps;
   double string_long_cps;
   double direct_scalar_cps;
};

template <typename Backend>
bench_result<Backend> run_bench(const char* name,
   const std::vector<uint8_t>& provider_wasm,
   const std::vector<uint8_t>& consumer_wasm,
   int iters)
{
   BenchHost host;
   psizam::composition<BenchHost, Backend> comp{host};
   auto& provider = comp.add(provider_wasm);
   auto& consumer = comp.add(consumer_wasm);
   comp.template register_host<BenchHost>(provider);
   comp.template register_host<BenchHost>(consumer);
   comp.template link<greeter>(consumer, provider);
   comp.instantiate();

   auto proxy = consumer.template as<processor>();
   auto prov_proxy = provider.template as<greeter>();
   bench_result<Backend> r;
   r.backend_name = name;

   // Warm up
   for (int i = 0; i < 100; ++i) proxy.test_add(1u, 2u);

   // Scalar i32 (cross-module)
   {
      auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < iters; ++i)
         proxy.test_add(static_cast<uint32_t>(i), 1u);
      auto t1 = std::chrono::high_resolution_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();
      r.scalar_i32_cps = iters / secs;
   }

   // Scalar i64 (cross-module)
   {
      auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < iters; ++i)
         proxy.test_double(static_cast<uint64_t>(i));
      auto t1 = std::chrono::high_resolution_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();
      r.scalar_i64_cps = iters / secs;
   }

   // Short string concat (cross-module, 12 bytes total)
   {
      auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < iters; ++i)
         proxy.test_concat(std::string_view{"hello,"}, std::string_view{"world!"});
      auto t1 = std::chrono::high_resolution_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();
      r.string_short_cps = iters / secs;
   }

   // Long string concat (cross-module, 200 bytes total)
   {
      std::string a(100, 'A');
      std::string b(100, 'B');
      auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < iters; ++i)
         proxy.test_concat(std::string_view{a}, std::string_view{b});
      auto t1 = std::chrono::high_resolution_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();
      r.string_long_cps = iters / secs;
   }

   // Direct provider call (no bridge, single module)
   {
      auto t0 = std::chrono::high_resolution_clock::now();
      for (int i = 0; i < iters; ++i)
         prov_proxy.add(static_cast<uint32_t>(i), 1u, 0u);
      auto t1 = std::chrono::high_resolution_clock::now();
      double secs = std::chrono::duration<double>(t1 - t0).count();
      r.direct_scalar_cps = iters / secs;
   }

   return r;
}

static void print_row(const char* name, double cps) {
   if (cps > 1e6)
      std::cout << std::setw(22) << std::left << name
                << std::setw(12) << std::right << std::fixed << std::setprecision(1)
                << cps / 1e6 << " M/s\n";
   else
      std::cout << std::setw(22) << std::left << name
                << std::setw(12) << std::right << std::fixed << std::setprecision(0)
                << cps / 1e3 << " K/s\n";
}

int main() {
   auto provider = read_file(COMPOSITION_WASM_DIR "/provider.wasm");
   auto consumer = read_file(COMPOSITION_WASM_DIR "/consumer.wasm");

   if (provider.empty() || consumer.empty()) {
      std::cerr << "Cannot load WASM files\n";
      return 1;
   }

   constexpr int ITERS = 500000;

   std::cout << "Composition bridge benchmark (" << ITERS << " iterations)\n";
   std::cout << std::string(50, '=') << "\n\n";

   // Interpreter
   {
      auto r = run_bench<psizam::interpreter>("interpreter", provider, consumer, ITERS);
      std::cout << "Backend: " << r.backend_name << "\n";
      std::cout << std::string(40, '-') << "\n";
      print_row("scalar i32 (bridge)", r.scalar_i32_cps);
      print_row("scalar i64 (bridge)", r.scalar_i64_cps);
      print_row("string 12B (bridge)", r.string_short_cps);
      print_row("string 200B (bridge)", r.string_long_cps);
      print_row("scalar i32 (direct)", r.direct_scalar_cps);
      std::cout << "\n";
   }

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   {
      auto r = run_bench<psizam::jit_llvm>("jit_llvm", provider, consumer, ITERS);
      std::cout << "Backend: " << r.backend_name << "\n";
      std::cout << std::string(40, '-') << "\n";
      print_row("scalar i32 (bridge)", r.scalar_i32_cps);
      print_row("scalar i64 (bridge)", r.scalar_i64_cps);
      print_row("string 12B (bridge)", r.string_short_cps);
      print_row("string 200B (bridge)", r.string_long_cps);
      print_row("scalar i32 (direct)", r.direct_scalar_cps);
      std::cout << "\n";
   }
#endif

   return 0;
}
