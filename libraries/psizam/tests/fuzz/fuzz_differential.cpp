// Differential fuzzer for psizam: interpreter vs JIT vs JIT2 vs LLVM vs wasm3
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

#ifdef PSIZAM_ENABLE_WASM3
extern "C" {
#include <wasm3.h>
#include <m3_env.h>
}
#endif

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
// Serialized return value: type tag + raw bits for bitwise comparison
struct return_value {
   uint8_t  type;   // types::i32, i64, f32, f64, v128, or 0 = void
   uint64_t bits;   // raw value bits (i32 zero-extended, f32 bit-cast to u32 zero-extended)
   uint64_t bits_hi; // high 64 bits for v128, 0 otherwise
};

struct run_result {
   int  outcome;
   bool has_start;
   std::string what;
   std::vector<return_value> returns;  // one per exported function that completed
   std::vector<std::string> export_names;  // names of exported functions (in order called)
};

template <typename Impl>
static run_result run_backend(const std::vector<uint8_t>& wasm_bytes,
                              fp_mode fp = fp_mode::softfloat) {
   run_result r{};
   try {
      using backend_t = backend<std::nullptr_t, Impl>;
      wasm_code code(wasm_bytes.begin(), wasm_bytes.end());
      wasm_allocator wa;
      backend_t bkend(code, &wa);

      // Interpreter honors post-construction mode changes. For JIT backends
      // this is a no-op when the requested mode matches the baked mode, and
      // throws if it would require re-emission — so callers passing a
      // non-default mode to a JIT backend must reconstruct with mode wired in.
      if constexpr (!Impl::is_jit) {
         bkend.set_fp_mode(fp);
      }

      // Check if module has a start function
      r.has_start = (bkend.get_module().start != std::numeric_limits<uint32_t>::max());

      // Execute all exported functions, capturing return values
      bkend.timed_run(watchdog(std::chrono::milliseconds(500)), [&]() {
         auto& mod = bkend.get_module();
         auto& ctx = bkend.get_context();
         for (int i = 0; i < mod.exports.size(); i++) {
            if (mod.exports[i].kind == external_kind::Function) {
               std::string s{ (const char*)mod.exports[i].field_str.data(),
                              mod.exports[i].field_str.size() };
               r.export_names.push_back(s);
               auto ret = ctx.execute(nullptr, detail::interpret_visitor(ctx), s);
               return_value rv{};
               if (ret) {
                  if (ret->template is_a<i32_const_t>()) {
                     rv.type = types::i32;
                     rv.bits = ret->to_ui32();
                  } else if (ret->template is_a<i64_const_t>()) {
                     rv.type = types::i64;
                     rv.bits = ret->to_ui64();
                  } else if (ret->template is_a<f32_const_t>()) {
                     rv.type = types::f32;
                     rv.bits = ret->to_fui32();
                  } else if (ret->template is_a<f64_const_t>()) {
                     rv.type = types::f64;
                     rv.bits = ret->to_fui64();
                  } else if (ret->template is_a<v128_const_t>()) {
                     rv.type = types::v128;
                     auto v = ret->to_v128();
                     rv.bits = v.low;
                     rv.bits_hi = v.high;
                  }
               }
               r.returns.push_back(rv);
            }
         }
      });
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

#ifdef PSIZAM_ENABLE_WASM3
// Map wasm3 error strings to outcome codes matching psizam's convention
static int wasm3_map_error(M3Result res) {
   if (!res) return 0;
   if (strstr(res, "out of bounds memory") || strstr(res, "stack overflow"))
      return 2; // memory trap
   if (strstr(res, "[trap]"))
      return 3; // interpreter trap
   return 1;    // parse/link/compile error
}

static run_result run_wasm3(const std::vector<uint8_t>& wasm_bytes,
                            const std::vector<std::string>& export_names) {
   run_result r{};
   IM3Environment env = m3_NewEnvironment();
   if (!env) { r.outcome = 1; r.what = "m3_NewEnvironment failed"; return r; }

   IM3Runtime runtime = m3_NewRuntime(env, 64 * 1024, nullptr);
   if (!runtime) { m3_FreeEnvironment(env); r.outcome = 1; r.what = "m3_NewRuntime failed"; return r; }

   IM3Module module = nullptr;
   M3Result res = m3_ParseModule(env, &module, wasm_bytes.data(), static_cast<uint32_t>(wasm_bytes.size()));
   if (res) {
      r.outcome = 1;
      r.what = res;
      m3_FreeRuntime(runtime);
      m3_FreeEnvironment(env);
      return r;
   }

   res = m3_LoadModule(runtime, module);
   if (res) {
      r.outcome = 1;
      r.what = res;
      m3_FreeModule(module);
      m3_FreeRuntime(runtime);
      m3_FreeEnvironment(env);
      return r;
   }
   // module is now owned by runtime

   // Run start function if present
   r.has_start = (module->startFunction >= 0);
   if (r.has_start) {
      res = m3_RunStart(module);
      if (res) {
         r.outcome = wasm3_map_error(res);
         r.what = res;
         m3_FreeRuntime(runtime);
         m3_FreeEnvironment(env);
         return r;
      }
   }

   // Call exported functions in the same order as psizam (by export name)
   for (auto& name : export_names) {
      IM3Function fn = nullptr;
      res = m3_FindFunction(&fn, runtime, name.c_str());
      if (res) {
         r.outcome = 1;
         r.what = res;
         m3_FreeRuntime(runtime);
         m3_FreeEnvironment(env);
         return r;
      }

      // Call with zero-valued args (matching psizam behavior)
      uint32_t num_args = m3_GetArgCount(fn);
      std::vector<uint64_t> args(num_args, 0);
      std::vector<const void*> arg_ptrs(num_args);
      for (uint32_t i = 0; i < num_args; i++)
         arg_ptrs[i] = &args[i];

      res = m3_Call(fn, num_args, num_args ? arg_ptrs.data() : nullptr);
      if (res) {
         r.outcome = wasm3_map_error(res);
         r.what = res;
         m3_FreeRuntime(runtime);
         m3_FreeEnvironment(env);
         return r;
      }

      // Read return values using the array API so multi-value works.
      // m3_GetResultsV requires one variadic pointer per return; a single-pointer
      // call is UB for multi-value functions and wrote garbage into slot 0.
      return_value rv{};
      uint32_t num_rets = m3_GetRetCount(fn);
      if (num_rets > 0) {
         std::vector<uint64_t> slots(num_rets, 0);
         std::vector<const void*> ptrs(num_rets);
         for (uint32_t i = 0; i < num_rets; ++i) ptrs[i] = &slots[i];
         M3Result gr = m3_GetResults(fn, num_rets, ptrs.data());
         if (gr == m3Err_none) {
            M3ValueType ret_type = m3_GetRetType(fn, 0);
            switch (ret_type) {
               case c_m3Type_i32: rv.type = types::i32; rv.bits = (uint32_t)slots[0]; break;
               case c_m3Type_i64: rv.type = types::i64; rv.bits = slots[0]; break;
               case c_m3Type_f32: rv.type = types::f32; rv.bits = (uint32_t)slots[0]; break;
               case c_m3Type_f64: rv.type = types::f64; rv.bits = slots[0]; break;
               default: break;
            }
         }
      }
      r.returns.push_back(rv);
   }

   r.outcome = 0;
   m3_FreeRuntime(runtime);
   m3_FreeEnvironment(env);
   return r;
}
#endif

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

static const char* type_name(uint8_t t) {
   switch (t) {
      case types::i32: return "i32";
      case types::i64: return "i64";
      case types::f32: return "f32";
      case types::f64: return "f64";
      case types::v128: return "v128";
      case 0: return "void";
      default: return "?";
   }
}

// Check if a float return value is NaN (by examining raw bits)
static bool is_nan_value(const return_value& v) {
   if (v.type == types::f32) {
      uint32_t bits = static_cast<uint32_t>(v.bits);
      return (bits & 0x7F800000u) == 0x7F800000u && (bits & 0x007FFFFFu) != 0;
   }
   if (v.type == types::f64) {
      return (v.bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
             (v.bits & 0x000FFFFFFFFFFFFFull) != 0;
   }
   return false;
}

// Compare return values between two backends. Returns true if all match.
// When nan_tolerant is true, NaN bit patterns are allowed to differ (both must be NaN though).
static bool compare_returns(const char* source, const char* b1_name, const run_result& r1,
                            const char* b2_name, const run_result& r2,
                            bool nan_tolerant = false) {
   if (r1.outcome != 0 || r2.outcome != 0) return true; // only compare successful runs
   size_t n = std::min(r1.returns.size(), r2.returns.size());
   for (size_t i = 0; i < n; i++) {
      auto& a = r1.returns[i];
      auto& b = r2.returns[i];
      if (a.type != b.type || a.bits != b.bits || a.bits_hi != b.bits_hi) {
         // In NaN-tolerant mode, both being NaN is acceptable
         if (nan_tolerant && is_nan_value(a) && is_nan_value(b))
            continue;
         fprintf(stderr, "RETURN VALUE MISMATCH [%s] export#%zu: %s=%s:0x%llx %s=%s:0x%llx\n",
                 source, i,
                 b1_name, type_name(a.type), (unsigned long long)a.bits,
                 b2_name, type_name(b.type), (unsigned long long)b.bits);
         return false;
      }
   }
   if (r1.returns.size() != r2.returns.size()) {
      fprintf(stderr, "RETURN COUNT MISMATCH [%s]: %s=%zu %s=%zu\n",
              source, b1_name, r1.returns.size(), b2_name, r2.returns.size());
      return false;
   }
   return true;
}

// Returns interpreter outcome (0-6), or -1 on mismatch
// Result codes from test_module_impl: 0-6 = interpreter outcome, -1 = mismatch, -2 = crash
static int test_module_impl(const std::vector<uint8_t>& wasm, const char* source, bool verbose) {
   bool trace = getenv("PSIZAM_FUZZ_TRACE") != nullptr;
   if (trace) fprintf(stderr, "[trace] interp/softfloat\n");
   auto r_interp = run_backend<interpreter>(wasm, fp_mode::softfloat);

   // Determinism cross-check: the same interpreter run in hw_deterministic
   // must produce the same outcome as softfloat (invariant from config.hpp:
   // hw_deterministic(x) == softfloat(x) bit-for-bit).
   if (trace) fprintf(stderr, "[trace] interp/hw_det\n");
   auto r_interp_hwd = run_backend<interpreter>(wasm, fp_mode::hw_deterministic);

   bool has_mismatch = false;
   if (r_interp.outcome != r_interp_hwd.outcome) {
      print_mismatch(source, "interp/softfloat", r_interp, "interp/hw_det", r_interp_hwd);
      has_mismatch = true;
   }
   if (!compare_returns(source, "interp/softfloat", r_interp, "interp/hw_det", r_interp_hwd))
      has_mismatch = true;

#if defined(__x86_64__) || defined(__aarch64__)
   if (trace) fprintf(stderr, "[trace] jit\n");
   auto r_jit = run_backend<jit>(wasm);
   if (r_interp.outcome != r_jit.outcome) {
      print_mismatch(source, "interpreter", r_interp, "jit", r_jit);
      has_mismatch = true;
   }
   if (!compare_returns(source, "interpreter", r_interp, "jit", r_jit))
      has_mismatch = true;
#endif

#if defined(__x86_64__) || defined(__aarch64__)
   if (trace) fprintf(stderr, "[trace] jit2\n");
   auto r_jit2 = run_backend<jit2>(wasm);
   if (r_interp.outcome != r_jit2.outcome) {
      print_mismatch(source, "interpreter", r_interp, "jit2", r_jit2);
      has_mismatch = true;
   }
   if (!compare_returns(source, "interpreter", r_interp, "jit2", r_jit2))
      has_mismatch = true;
#endif

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   if (trace) fprintf(stderr, "[trace] jit_llvm\n");
   run_result r_jit_llvm;
   if (getenv("PSIZAM_FUZZ_SKIP_LLVM")) { r_jit_llvm.outcome = r_interp.outcome; } else
   r_jit_llvm = run_backend<jit_llvm>(wasm);
   if (r_interp.outcome != r_jit_llvm.outcome) {
      print_mismatch(source, "interpreter", r_interp, "jit_llvm", r_jit_llvm);
      has_mismatch = true;
   }
   if (!compare_returns(source, "interpreter", r_interp, "jit_llvm", r_jit_llvm))
      has_mismatch = true;
#endif

#ifdef PSIZAM_ENABLE_WASM3
   // Compare against wasm3 reference interpreter.
   // wasm3 doesn't support EH or v128 — skip if psizam rejected the module
   // (wasm3 may accept/reject differently for unsupported proposals).
   // NaN bit patterns may differ since wasm3 uses hardware floats.
   // Compare against wasm3 only when psizam ran to completion (outcome 0).
   // Outcome mismatches from traps/parse errors can differ legitimately:
   // psizam's execute-by-name enforces argument count (traps on functions
   // with params), while wasm3 passes zero-valued args.
   if (r_interp.outcome == 0) {
      if (trace) fprintf(stderr, "[trace] wasm3\n");
      auto r_wasm3 = run_wasm3(wasm, r_interp.export_names);
      if (trace) fprintf(stderr, "[trace] wasm3 done\n");
      if (r_wasm3.outcome == 0) {
         // Both succeeded — compare return values (NaN-tolerant)
         if (!compare_returns(source, "interpreter", r_interp, "wasm3", r_wasm3, true))
            has_mismatch = true;
      } else if (r_wasm3.outcome != 1) {
         // psizam succeeded but wasm3 trapped — potential psizam bug
         print_mismatch(source, "interpreter", r_interp, "wasm3", r_wasm3);
         has_mismatch = true;
      }
   }
#endif

   if (has_mismatch) return -1;

   if (verbose) {
      fprintf(stderr, "  OK [%s]: %s (%zu bytes)\n",
              source, outcome_name(r_interp.outcome), wasm.size());
   }
   return r_interp.outcome;
}

// Run test_module_impl in a forked child process for crash isolation.
// Returns 0-6 (outcome), -1 (mismatch), or -2 (child crashed).
static int test_module(const std::vector<uint8_t>& wasm, const char* source, bool verbose) {
   if (getenv("PSIZAM_FUZZ_NO_FORK")) return test_module_impl(wasm, source, verbose);
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
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      else if (strcmp(which, "jit_llvm") == 0) r = run_backend<jit_llvm>(wasm);
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
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
   fprintf(stderr, ", jit_llvm");
#endif
#ifdef PSIZAM_ENABLE_WASM3
   fprintf(stderr, ", wasm3");
#endif
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
