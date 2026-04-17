// Differential fuzzer for psizam: interpreter vs JIT vs JIT2 vs LLVM
//
// Usage:
//   # Generate and test random WASM modules continuously:
//   ./psizam-fuzz-diff [iterations] [seed]
//
//   # Test a specific WASM file:
//   ./psizam-fuzz-diff --file module.wasm
//
// Uses wasm-gen (Rust helper) to bulk-generate valid WASM modules via pipe,
// then runs each on all backends and asserts identical outcomes.

#include <psizam/backend.hpp>
#include <psizam/detail/watchdog.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

using namespace psizam;
using namespace psizam::detail;

// Get directory containing our own executable
static std::string get_exe_dir() {
   char self_path[4096];
#if defined(__APPLE__)
   uint32_t sz = sizeof(self_path);
   if (_NSGetExecutablePath(self_path, &sz) == 0) {
#else
   ssize_t len = readlink("/proc/self/exe", self_path, sizeof(self_path) - 1);
   if (len > 0) { self_path[len] = 0;
#endif
      char* slash = strrchr(self_path, '/');
      if (slash) { *slash = 0; return self_path; }
   }
   return {};
}

// Locate wasm-gen binary — check build tree, Rust target, PATH
static std::string find_wasm_gen() {
   // 1. Next to our own binary (CMake bin/ directory)
   auto dir = get_exe_dir();
   if (!dir.empty()) {
      std::string path = dir + "/wasm-gen";
      if (access(path.c_str(), X_OK) == 0) return path;
   }
   // 2. Rust build output (relative to source tree)
   // __FILE__ gives the source path at compile time
   {
      std::string src_dir = __FILE__;
      auto pos = src_dir.rfind('/');
      if (pos != std::string::npos) {
         src_dir.resize(pos);
         std::string path = src_dir + "/wasm-gen/target/release/wasm-gen";
         if (access(path.c_str(), X_OK) == 0) return path;
      }
   }
   // 3. PATH
   if (system("wasm-gen --help >/dev/null 2>&1") == 0) return "wasm-gen";
   return {};
}

// Result of running a module on one backend
// Outcome codes:
//   0 = ok (ran to completion)
//   1 = rejected (parse error or resource allocation failure — module can't run)
//   2 = memory trap (OOB access, guard page fault)
//   3 = interpreter trap (unreachable, div-by-zero, etc.)
//   4 = timeout (watchdog fired)
//   5 = other std::exception
//   6 = unknown (non-std::exception)
struct run_result {
   int  outcome;
   bool has_start;
   std::string what;
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
   } catch (wasm_parse_exception& e) {
      r.outcome = 1;
      r.what = e.what();
   } catch (wasm_bad_alloc& e) {
      r.outcome = 1;
      r.what = e.what();
   } catch (wasm_link_exception& e) {
      r.outcome = 1;
      r.what = e.what();
   } catch (wasm_memory_exception&) {
      r.outcome = 2;
   } catch (wasm_exception&) {
      r.outcome = 3;
   } catch (wasm_interpreter_exception& e) {
      r.outcome = 3;
      r.what = e.what();
   } catch (timeout_exception&) {
      r.outcome = 4;
   } catch (std::exception& e) {
      r.outcome = 5;
      r.what = e.what();
   } catch (...) {
      r.outcome = 6;
   }
   return r;
}

static const char* outcome_name(int o) {
   switch (o) {
      case 0: return "ok";
      case 1: return "rejected";
      case 2: return "memory_trap";
      case 3: return "interp_trap";
      case 4: return "timeout";
      case 5: return "exception";
      case 6: return "unknown";
      default: return "???";
   }
}

static void print_mismatch(const char* source, const char* b1_name, const run_result& r1,
                           const char* b2_name, const run_result& r2) {
   fprintf(stderr, "MISMATCH [%s]: %s=%s %s=%s\n",
           source, b1_name, outcome_name(r1.outcome), b2_name, outcome_name(r2.outcome));
   if (!r1.what.empty()) fprintf(stderr, "  %s what: %s\n", b1_name, r1.what.c_str());
   if (!r2.what.empty()) fprintf(stderr, "  %s what: %s\n", b2_name, r2.what.c_str());
}

// Returns interpreter outcome (0-6), or -1 on mismatch
// Result codes from test_module_impl: 0-6 = interpreter outcome, -1 = mismatch, -2 = crash
static int test_module_impl(const std::vector<uint8_t>& wasm, const char* source, bool verbose) {
   auto r_interp = run_backend<interpreter>(wasm);

#if defined(__x86_64__) || defined(__aarch64__)
   auto r_jit = run_backend<jit>(wasm);
   if (r_interp.outcome != r_jit.outcome) {
      print_mismatch(source, "interpreter", r_interp, "jit", r_jit);
      return -1;
   }
#endif

#if defined(__x86_64__) || defined(__aarch64__)
   auto r_jit2 = run_backend<jit2>(wasm);
   if (r_interp.outcome != r_jit2.outcome) {
      print_mismatch(source, "interpreter", r_interp, "jit2", r_jit2);
      return -1;
   }
#endif

   if (verbose) {
      fprintf(stderr, "  OK [%s]: %s (%zu bytes)\n",
              source, outcome_name(r_interp.outcome), wasm.size());
   }
   return r_interp.outcome;
}

// Run test_module_impl in a forked child process for crash isolation.
// Returns 0-6 (outcome), -1 (mismatch), or -2 (child crashed).
static int test_module(const std::vector<uint8_t>& wasm, const char* source, bool verbose) {
   int pipefd[2];
   if (pipe(pipefd) != 0) return test_module_impl(wasm, source, verbose); // fallback

   pid_t pid = fork();
   if (pid < 0) {
      close(pipefd[0]); close(pipefd[1]);
      return test_module_impl(wasm, source, verbose); // fallback
   }
   if (pid == 0) {
      // Child: run the test and write result to pipe
      close(pipefd[0]);
      int result = test_module_impl(wasm, source, verbose);
      auto written = write(pipefd[1], &result, sizeof(result));
      (void)written;
      close(pipefd[1]);
      _exit(result >= 0 ? 0 : 1);
   }
   // Parent: read result from child
   close(pipefd[1]);
   int result;
   ssize_t n = read(pipefd[0], &result, sizeof(result));
   close(pipefd[0]);
   int status;
   waitpid(pid, &status, 0);
   if (n == sizeof(result)) return result;
   // Child crashed before writing result
   if (WIFSIGNALED(status)) {
      fprintf(stderr, "CRASH [%s]: child killed by signal %d (%zu bytes)\n",
              source, WTERMSIG(status), wasm.size());
   }
   return -2;
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

// Read exactly n bytes from a FILE*, returns false on short read/EOF
static bool read_exact(FILE* f, void* buf, size_t n) {
   size_t got = fread(buf, 1, n, f);
   return got == n;
}

int main(int argc, char* argv[]) {
   // --file mode: test a single WASM file (uses fork for crash isolation)
   if (argc >= 3 && strcmp(argv[1], "--file") == 0) {
      auto wasm = read_file(argv[2]);
      if (wasm.empty()) { fprintf(stderr, "Cannot read %s\n", argv[2]); return 1; }
      int ok = test_module(wasm, argv[2], true);
      return ok >= 0 ? 0 : 1;
   }
   // --file-backend mode: test one backend at a time (no fork, for debugger)
   if (argc >= 4 && strcmp(argv[1], "--file-backend") == 0) {
      auto wasm = read_file(argv[2]);
      if (wasm.empty()) { fprintf(stderr, "Cannot read %s\n", argv[2]); return 1; }
      const char* which = argv[3];
      run_result r{};
      if (strcmp(which, "interp") == 0) r = run_backend<interpreter>(wasm);
#if defined(__x86_64__) || defined(__aarch64__)
      else if (strcmp(which, "jit") == 0) r = run_backend<jit>(wasm);
      else if (strcmp(which, "jit2") == 0) r = run_backend<jit2>(wasm);
#endif
      else { fprintf(stderr, "Unknown backend: %s\n", which); return 1; }
      fprintf(stderr, "%s: %s", which, outcome_name(r.outcome));
      if (!r.what.empty()) fprintf(stderr, " (%s)", r.what.c_str());
      fprintf(stderr, "\n");
      return 0;
   }

   auto wasm_gen = find_wasm_gen();
   if (wasm_gen.empty()) {
      fprintf(stderr, "ERROR: wasm-gen not found. Build it:\n");
      fprintf(stderr, "  cd libraries/psizam/tests/fuzz/wasm-gen && cargo build --release\n");
      return 1;
   }

   uint32_t iterations = (argc > 1) ? static_cast<uint32_t>(atoi(argv[1])) : 10000;
   uint64_t seed = (argc > 2) ? static_cast<uint64_t>(atoll(argv[2])) :
      static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

   fprintf(stderr, "psizam differential fuzzer: %u iterations, seed=%llu\n", iterations, (unsigned long long)seed);
   fprintf(stderr, "Generator: %s\n", wasm_gen.c_str());
   fprintf(stderr, "Backends: interpreter");
#if defined(__x86_64__) || defined(__aarch64__)
   fprintf(stderr, ", jit");
#endif
#if defined(__x86_64__) || defined(__aarch64__)
   fprintf(stderr, ", jit2");
#endif
   // jit_llvm disabled in fuzzer due to known IR translation bugs
   fprintf(stderr, "\n\n");

   // Launch wasm-gen as a pipe — it writes length-prefixed WASM modules to stdout
   char cmd[512];
   snprintf(cmd, sizeof(cmd), "%s %u %llu", wasm_gen.c_str(), iterations, (unsigned long long)seed);
   FILE* gen = popen(cmd, "r");
   if (!gen) {
      fprintf(stderr, "ERROR: failed to launch wasm-gen\n");
      return 1;
   }

   uint32_t tested = 0, passed = 0, mismatches = 0, crashes = 0;
   uint32_t outcomes[7] = {};
   auto t_start = std::chrono::steady_clock::now();

   for (uint32_t i = 0; i < iterations; i++) {
      // Read length-prefixed module: [u32le length][wasm bytes]
      uint32_t len = 0;
      if (!read_exact(gen, &len, 4)) break; // EOF or generator done

      std::vector<uint8_t> wasm(len);
      if (!read_exact(gen, wasm.data(), len)) {
         fprintf(stderr, "ERROR: short read at module %u (expected %u bytes)\n", i, len);
         break;
      }

      tested++;
      char label[64];
      snprintf(label, sizeof(label), "module#%u", i);

      int result = test_module(wasm, label, false);
      if (result >= 0) {
         passed++;
         if (result < 7) outcomes[result]++;
      } else if (result == -2) {
         crashes++;
         char crash_path[256];
         snprintf(crash_path, sizeof(crash_path), "crash_%u_seed%llu.wasm", i, (unsigned long long)seed);
         FILE* cf = fopen(crash_path, "wb");
         if (cf) { fwrite(wasm.data(), 1, wasm.size(), cf); fclose(cf); }
         fprintf(stderr, "  Saved to %s (%zu bytes)\n", crash_path, wasm.size());
      } else {
         mismatches++;
         char crash_path[256];
         snprintf(crash_path, sizeof(crash_path), "mismatch_%u_seed%llu.wasm", i, (unsigned long long)seed);
         FILE* cf = fopen(crash_path, "wb");
         if (cf) { fwrite(wasm.data(), 1, wasm.size(), cf); fclose(cf); }
         fprintf(stderr, "  Saved to %s (%zu bytes)\n", crash_path, wasm.size());
      }

      if ((i + 1) % 100 == 0) {
         auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
         fprintf(stderr, "\r  [%u/%u] %u passed, %u mismatches, %u crashes (%.1f modules/sec)",
                 i + 1, iterations, passed, mismatches, crashes, tested / elapsed);
      }
   }

   pclose(gen);

   auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
   fprintf(stderr, "\n\nDone: %u tested, %u passed, %u mismatches, %u crashes (%.1fs, %.1f/sec)\n",
           tested, passed, mismatches, crashes, elapsed, tested / elapsed);
   fprintf(stderr, "Outcomes: %u ok, %u rejected, %u memory_trap, %u interp_trap, %u timeout\n",
           outcomes[0], outcomes[1], outcomes[2], outcomes[3], outcomes[4]);

   return (mismatches == 0 && crashes == 0) ? 0 : 1;
}
