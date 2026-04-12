// bench_compile: Batch compilation benchmark for pzam-compile.
//
// Compiles all .wasm files in a directory within a single process,
// reporting per-file and aggregate timing. Invalid WASM files are
// noted and skipped without crashing.
//
// Usage: bench-compile --target=x86_64|aarch64 [--backend=jit2|llvm] <dir>

#include <psizam/parser.hpp>
#include <psizam/ir_writer.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/utils.hpp>

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
#include <psizam/ir_writer_llvm_aot.hpp>
#endif

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>

using namespace psizam;
namespace fs = std::filesystem;

struct bench_options : default_options {
   uint32_t compile_threads = 0;
};

static bench_options g_opts;

template<typename IrWriter>
static bool try_compile(std::vector<uint8_t>& wasm_bytes,
                        pzam_arch target_arch,
                        const std::string& target_triple = {}) {
   module mod;
   mod.allocator.use_default_memory();

   pzam_compile_result compile_result;
   compile_result.target_triple = target_triple;
   null_debug_info debug;

   using parser_t = binary_parser<IrWriter, bench_options, null_debug_info>;
   parser_t parser(mod.allocator, g_opts, false, false);
   parser.set_compile_result(&compile_result);

   parser.parse_module(wasm_bytes, mod, debug);
   mod.finalize();
   if (!compile_result.error.empty())
      throw psizam::wasm_parse_exception(compile_result.error);
   return true;
}

int main(int argc, char** argv) {
   std::string target_str;
   std::string backend_str = "jit2";
   std::string dir_path;
   uint32_t threads = 0;
   int runs = 1;

   for (int i = 1; i < argc; i++) {
      std::string arg = argv[i];
      if (arg.starts_with("--target=")) {
         target_str = arg.substr(9);
      } else if (arg.starts_with("--backend=")) {
         backend_str = arg.substr(10);
      } else if (arg.starts_with("--threads=")) {
         threads = std::stoul(arg.substr(10));
      } else if (arg.starts_with("--runs=")) {
         runs = std::stoi(arg.substr(7));
      } else if (arg[0] != '-') {
         dir_path = arg;
      }
   }

   g_opts.compile_threads = threads;

   if (dir_path.empty() || target_str.empty()) {
      std::cerr << "Usage: bench-compile --target=x86_64|aarch64 [--backend=jit2|llvm] [--threads=N] [--runs=N] <wasm-dir-or-file>\n";
      return 1;
   }

   pzam_arch target_arch;
   std::string target_triple;
   if (target_str == "x86_64" || target_str == "x64") {
      target_arch = pzam_arch::x86_64;
      target_triple = "x86_64-unknown-linux-gnu";
   } else if (target_str == "aarch64" || target_str == "arm64") {
      target_arch = pzam_arch::aarch64;
      target_triple = "aarch64-unknown-linux-gnu";
   } else {
      std::cerr << "Error: unknown target: " << target_str << "\n";
      return 1;
   }

   // Collect .wasm files (single file or directory)
   std::vector<fs::path> wasm_files;
   if (fs::is_regular_file(dir_path)) {
      wasm_files.push_back(dir_path);
   } else {
      for (const auto& entry : fs::directory_iterator(dir_path)) {
         if (entry.path().extension() == ".wasm")
            wasm_files.push_back(entry.path());
      }
   }
   std::sort(wasm_files.begin(), wasm_files.end());

   std::cerr << "Found " << wasm_files.size() << " .wasm files\n";
   std::cerr << "Target: " << target_str << "  Backend: " << backend_str
             << "  Threads: " << threads << "  Runs: " << runs << "\n\n";

   struct Result {
      std::string name;
      size_t      wasm_size;
      double      compile_ms; // best of all runs
      bool        ok;
      std::string error;
   };

   // Pre-read all WASM files
   struct WasmFile {
      std::string name;
      std::vector<uint8_t> bytes;
   };
   std::vector<WasmFile> wasm_data;
   for (const auto& path : wasm_files) {
      std::ifstream ifs(path, std::ios::binary);
      wasm_data.push_back({
         path.filename().string(),
         std::vector<uint8_t>((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>())
      });
   }

   std::vector<Result> best_results;
   double best_total_ms = std::numeric_limits<double>::max();

   for (int run = 0; run < runs; ++run) {
      uint32_t ok_count = 0, fail_count = 0;
      double run_total_ms = 0;
      std::vector<Result> run_results;

      for (auto& wf : wasm_data) {
         Result r;
         r.name = wf.name;
         r.wasm_size = wf.bytes.size();

         auto t0 = std::chrono::high_resolution_clock::now();

         try {
            if (backend_str == "jit2") {
               if (target_arch == pzam_arch::x86_64)
                  try_compile<ir_writer_x64>(wf.bytes, target_arch);
               else
                  try_compile<ir_writer_a64>(wf.bytes, target_arch);
            }
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
            else if (backend_str == "llvm") {
               try_compile<ir_writer_llvm_aot>(wf.bytes, target_arch, target_triple);
            }
#endif
            r.ok = true;
         } catch (const psizam::exception& ex) {
            r.ok = false;
            r.error = ex.detail();
         } catch (const std::exception& ex) {
            r.ok = false;
            r.error = std::string("CRASH: ") + ex.what();
         }

         auto t1 = std::chrono::high_resolution_clock::now();
         r.compile_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

         if (r.ok) {
            ok_count++;
            run_total_ms += r.compile_ms;
         } else {
            fail_count++;
            std::cerr << "  FAIL: " << r.name << ": " << r.error << "\n";
         }

         run_results.push_back(std::move(r));
      }

      if (runs > 1)
         std::cerr << "Run " << (run+1) << "/" << runs << ": " << std::fixed << std::setprecision(3) << run_total_ms << " ms\n";

      if (run_total_ms < best_total_ms) {
         best_total_ms = run_total_ms;
         best_results = std::move(run_results);
      }
   }

   auto& results = best_results;
   uint32_t ok_count = 0, fail_count = 0, crash_count = 0;
   uint64_t total_wasm_bytes = 0;
   double total_compile_ms = 0;
   for (auto& r : results) {
      if (r.ok) {
         ok_count++;
         total_wasm_bytes += r.wasm_size;
         total_compile_ms += r.compile_ms;
      } else {
         fail_count++;
         if (r.error.starts_with("CRASH:")) crash_count++;
      }
   }

   // Print results — successful compilations sorted by time (slowest first)
   std::vector<const Result*> sorted_ok;
   for (const auto& r : results)
      if (r.ok) sorted_ok.push_back(&r);

   std::sort(sorted_ok.begin(), sorted_ok.end(),
      [](const Result* a, const Result* b) { return a->compile_ms > b->compile_ms; });

   std::cerr << "=== Top 20 slowest compilations ===\n";
   std::cerr << std::left << std::setw(40) << "File"
             << std::right << std::setw(10) << "WASM bytes"
             << std::setw(12) << "Time (ms)" << "\n";
   std::cerr << std::string(62, '-') << "\n";

   for (size_t i = 0; i < std::min<size_t>(20, sorted_ok.size()); i++) {
      auto* r = sorted_ok[i];
      std::cerr << std::left << std::setw(40) << r->name
                << std::right << std::setw(10) << r->wasm_size
                << std::setw(12) << std::fixed << std::setprecision(3) << r->compile_ms
                << "\n";
   }

   // Print crashes if any
   if (crash_count > 0) {
      std::cerr << "\n=== CRASHES (" << crash_count << ") ===\n";
      for (const auto& r : results) {
         if (!r.ok && r.error.starts_with("CRASH:"))
            std::cerr << "  " << r.name << ": " << r.error << "\n";
      }
   }

   // Summary
   std::cerr << "\n=== Summary ===\n";
   std::cerr << "Compiled:    " << ok_count << " / " << wasm_files.size() << " modules\n";
   std::cerr << "Rejected:    " << fail_count << " (invalid WASM)\n";
   std::cerr << "Crashed:     " << crash_count << "\n";
   std::cerr << "Total WASM:  " << total_wasm_bytes << " bytes\n";
   std::cerr << "Total time:  " << std::fixed << std::setprecision(3) << total_compile_ms << " ms\n";
   if (ok_count > 0) {
      std::cerr << "Avg time:    " << std::fixed << std::setprecision(3) << (total_compile_ms / ok_count) << " ms/module\n";
      double mb = total_wasm_bytes / (1024.0 * 1024.0);
      double sec = total_compile_ms / 1000.0;
      if (sec > 0)
         std::cerr << "Throughput:  " << std::fixed << std::setprecision(2) << (mb / sec) << " MB/s WASM input\n";
   }

   return crash_count > 0 ? 1 : 0;
}
