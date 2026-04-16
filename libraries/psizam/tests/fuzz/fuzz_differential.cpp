// Differential fuzzer for psizam: interpreter vs JIT vs JIT2
//
// Usage:
//   # Generate and test random WASM modules continuously:
//   ./psizam-fuzz-diff [iterations] [seed]
//
//   # Test a specific WASM file:
//   ./psizam-fuzz-diff --file module.wasm
//
// Each iteration: wasm-smith generates a valid module, we run it on all
// backends and assert identical results/trap behavior.

#include <psizam/backend.hpp>
#include <psizam/detail/watchdog.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <unistd.h>
#include <vector>

using namespace psizam;
using namespace psizam::detail;

// Locate wasm-tools binary — check common locations, then PATH
static std::string find_wasm_tools() {
   // Check explicit locations first
   const char* home = getenv("HOME");
   if (home) {
      std::string cargo_path = std::string(home) + "/.cargo/bin/wasm-tools";
      if (access(cargo_path.c_str(), X_OK) == 0) return cargo_path;
   }
   // Fall back to PATH
   if (system("wasm-tools --version >/dev/null 2>&1") == 0) return "wasm-tools";
   return {};
}

// Result of running a module on one backend
struct run_result {
   int  outcome;       // 0=ok, 1=parse, 2=memory, 3=interp, 4=timeout, 5=other
   bool has_start;     // module had a start function
};

template <typename Impl>
static run_result run_backend(const std::vector<uint8_t>& wasm_bytes) {
   run_result r{};
   try {
      using backend_t = backend<std::nullptr_t, Impl>;
      wasm_code code(wasm_bytes.begin(), wasm_bytes.end());
      wasm_allocator wa;
      backend_t bkend(code, &wa);

      // Check if module has a start function
      r.has_start = (bkend.get_module().start != std::numeric_limits<uint32_t>::max());

      // Execute all exported functions with a tight watchdog
      bkend.execute_all(watchdog(std::chrono::milliseconds(500)));
      r.outcome = 0;
   } catch (wasm_parse_exception&) {
      r.outcome = 1;
   } catch (wasm_memory_exception&) {
      r.outcome = 2;
   } catch (wasm_interpreter_exception&) {
      r.outcome = 3;
   } catch (timeout_exception&) {
      r.outcome = 4;
   } catch (std::exception& e) {
      r.outcome = 5;
   } catch (...) {
      r.outcome = 6;
   }
   return r;
}

static const char* outcome_name(int o) {
   switch (o) {
      case 0: return "ok";
      case 1: return "parse_error";
      case 2: return "memory_trap";
      case 3: return "interp_trap";
      case 4: return "timeout";
      case 5: return "exception";
      case 6: return "unknown";
      default: return "???";
   }
}

static bool test_module(const std::vector<uint8_t>& wasm, const char* source, bool verbose) {
   auto r_interp = run_backend<interpreter>(wasm);

#if defined(__x86_64__) || defined(__aarch64__)
   auto r_jit = run_backend<jit>(wasm);
   if (r_interp.outcome != r_jit.outcome) {
      fprintf(stderr, "MISMATCH [%s]: interpreter=%s jit=%s\n",
              source, outcome_name(r_interp.outcome), outcome_name(r_jit.outcome));
      return false;
   }
#endif

#if defined(__x86_64__) || defined(__aarch64__)
   auto r_jit2 = run_backend<jit2>(wasm);
   if (r_interp.outcome != r_jit2.outcome) {
      fprintf(stderr, "MISMATCH [%s]: interpreter=%s jit2=%s\n",
              source, outcome_name(r_interp.outcome), outcome_name(r_jit2.outcome));
      return false;
   }
#endif

   if (verbose) {
      fprintf(stderr, "  OK [%s]: %s (%zu bytes)\n",
              source, outcome_name(r_interp.outcome), wasm.size());
   }
   return true;
}

static std::vector<uint8_t> read_file(const char* path) {
   std::ifstream f(path, std::ios::binary | std::ios::ate);
   if (!f) return {};
   auto sz = f.tellg();
   f.seekg(0);
   std::vector<uint8_t> buf(static_cast<size_t>(sz));
   f.read(reinterpret_cast<char*>(buf.data()), sz);
   return buf;
}

static std::vector<uint8_t> generate_wasm(const std::string& wasm_tools, uint64_t seed) {
   // Write seed bytes to a temp file, pipe through wasm-smith
   char cmd[512];
   char seed_file[128], wasm_file[128];
   snprintf(seed_file, sizeof(seed_file), "/tmp/psizam_fuzz_seed_%d.bin", getpid());
   snprintf(wasm_file, sizeof(wasm_file), "/tmp/psizam_fuzz_out_%d.wasm", getpid());

   // Write 32 bytes of PRNG output as seed material
   std::mt19937_64 rng(seed);
   FILE* sf = fopen(seed_file, "wb");
   for (int i = 0; i < 4; i++) {
      uint64_t v = rng();
      fwrite(&v, 8, 1, sf);
   }
   fclose(sf);

   snprintf(cmd, sizeof(cmd),
            "%s smith --ensure-termination "
            "--min-memories=0 --max-memories=1 "
            "--min-tables=0 --max-tables=2 "
            "--max-instructions=1000 "
            "--max-memory32-bytes=655360 "
            "-o %s %s 2>/dev/null",
            wasm_tools.c_str(), wasm_file, seed_file);

   int rc = system(cmd);
   unlink(seed_file);

   if (rc != 0) return {};

   auto wasm = read_file(wasm_file);
   unlink(wasm_file);
   return wasm;
}

int main(int argc, char* argv[]) {
   // --file mode: test a single WASM file
   if (argc >= 3 && strcmp(argv[1], "--file") == 0) {
      auto wasm = read_file(argv[2]);
      if (wasm.empty()) { fprintf(stderr, "Cannot read %s\n", argv[2]); return 1; }
      bool ok = test_module(wasm, argv[2], true);
      return ok ? 0 : 1;
   }

   auto wasm_tools = find_wasm_tools();
   if (wasm_tools.empty()) {
      fprintf(stderr, "ERROR: wasm-tools not found. Install: cargo install wasm-tools\n");
      return 1;
   }

   uint32_t iterations = (argc > 1) ? static_cast<uint32_t>(atoi(argv[1])) : 10000;
   uint64_t seed = (argc > 2) ? static_cast<uint64_t>(atoll(argv[2])) :
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

   fprintf(stderr, "psizam differential fuzzer: %u iterations, seed=%llu\n", iterations, (unsigned long long)seed);
   fprintf(stderr, "Using: %s\n", wasm_tools.c_str());
   fprintf(stderr, "Backends: interpreter");
#if defined(__x86_64__) || defined(__aarch64__)
   fprintf(stderr, ", jit");
#endif
#if defined(__x86_64__) || defined(__aarch64__)
   fprintf(stderr, ", jit2");
#endif
   fprintf(stderr, "\n\n");

   uint32_t tested = 0, passed = 0, gen_failed = 0;
   auto t_start = std::chrono::steady_clock::now();

   for (uint32_t i = 0; i < iterations; i++) {
      auto wasm = generate_wasm(wasm_tools, seed + i);
      if (wasm.empty()) { gen_failed++; continue; }

      tested++;
      char label[64];
      snprintf(label, sizeof(label), "seed=%llu", (unsigned long long)(seed + i));

      if (test_module(wasm, label, false)) {
         passed++;
      } else {
         // Save failing module for reproduction
         char crash_path[256];
         snprintf(crash_path, sizeof(crash_path), "crash_%llu.wasm", (unsigned long long)(seed + i));
         FILE* cf = fopen(crash_path, "wb");
         if (cf) { fwrite(wasm.data(), 1, wasm.size(), cf); fclose(cf); }
         fprintf(stderr, "  Saved to %s (%zu bytes)\n", crash_path, wasm.size());
      }

      if ((i + 1) % 100 == 0) {
         auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
         fprintf(stderr, "\r  [%u/%u] %u passed, %u mismatches, %u gen_failed (%.1f modules/sec)",
                 i + 1, iterations, passed, tested - passed, gen_failed, tested / elapsed);
      }
   }

   auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
   fprintf(stderr, "\n\nDone: %u tested, %u passed, %u MISMATCHES, %u gen_failed (%.1fs, %.1f/sec)\n",
           tested, passed, tested - passed, gen_failed, elapsed, tested / elapsed);

   return (passed == tested) ? 0 : 1;
}
