// pzam-compile: Cross-compile WASM modules to native .pzam files.
//
// Usage: pzam-compile --target=x86_64|aarch64 [--backend=jit2|llvm] [-o output.pzam] input.wasm
//
// Reads a .wasm file, compiles it to native code for the specified target
// architecture, and writes a .pzam file containing the relocatable code blob.
// The .pzam file can later be loaded and relocated at runtime, skipping
// the compilation step.

#include <psizam/detail/parser.hpp>
#include <psizam/detail/ir_writer.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/pzam_metadata.hpp>
#include <psizam/utils.hpp>

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
#include <psizam/detail/ir_writer_llvm_aot.hpp>
#endif

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace psizam;
using namespace psizam::detail;

struct compile_options : default_options {
   std::uint32_t compile_threads = 0;
};

static void usage(const char* prog) {
   std::cerr << "Usage: " << prog << " --target=x86_64|aarch64 [--backend=jit2|llvm] [--opt-level=0|1|2] [--softfloat] [--backtrace] [-jN] [--dump-layout=FILE] [-o output.pzam] input.wasm\n";
}

static std::vector<char> write_pzam(
      const module& mod,
      const growable_allocator& alloc,
      const pzam_compile_result& result,
      std::span<const uint8_t> wasm_bytes,
      pzam_arch target_arch,
      bool use_softfloat,
      bool use_backtrace) {

   pzam_file file;
   file.input_hash = pzam_cache::hash_wasm(wasm_bytes);
   file.metadata = extract_metadata(mod);

   bool llvm_aot = !result.code_blob.empty();

   // Build a single code section
   pzam_code_section cs;
   cs.arch = static_cast<uint8_t>(target_arch);
   cs.opt_tier = static_cast<uint8_t>(llvm_aot ? pzam_opt_tier::llvm_O3 : pzam_opt_tier::jit2);
   cs.instrumentation.softfloat = use_softfloat ? 1 : 0;
   cs.instrumentation.async_backtrace = use_backtrace ? 1 : 0;
   cs.stack_limit_mode = mod.stack_limit_is_bytes ? 1 : 0;
   cs.page_size = static_cast<uint32_t>(wasm_allocator::table_size());
   cs.max_stack = static_cast<uint32_t>(mod.maximum_stack);
   cs.compiler.compiler_name = llvm_aot ? "psizam-llvm" : "psizam-jit2";
   cs.compiler.compiler_version = "0.1.0";
   cs.compiler.compiler_hash = pzam_cache::compiler_identity(target_arch);

   // Function table
   cs.functions.resize(mod.code.size());
   for (size_t i = 0; i < mod.code.size(); i++) {
      if (llvm_aot && i < result.function_offsets.size()) {
         cs.functions[i].code_offset = result.function_offsets[i].first;
         cs.functions[i].code_size   = result.function_offsets[i].second;
      } else {
         cs.functions[i].code_offset = static_cast<uint32_t>(mod.code[i].jit_code_offset);
         cs.functions[i].code_size   = mod.code[i].jit_code_size;
      }
      cs.functions[i].stack_size = mod.code[i].stack_size;
   }

   // Relocations
   cs.relocations.resize(result.relocs.size());
   for (size_t i = 0; i < result.relocs.size(); i++) {
      cs.relocations[i].code_offset = result.relocs[i].code_offset;
      cs.relocations[i].symbol      = static_cast<uint16_t>(result.relocs[i].symbol);
      cs.relocations[i].type        = static_cast<uint8_t>(result.relocs[i].type);
      cs.relocations[i].addend      = result.relocs[i].addend;
   }

   // Code blob — use actual size (no page-alignment padding)
   if (llvm_aot) {
      cs.code_blob = result.code_blob;
   } else {
      auto code_start = reinterpret_cast<const uint8_t*>(alloc.get_code_start());
      size_t actual_size = alloc.get_actual_code_size();
      cs.code_blob.assign(code_start, code_start + actual_size);
   }

   // Canonicalize reloc sites so byte-level output is deterministic
   // regardless of where the code was laid out in memory at compile time.
   // The bits cleared here are overwritten by apply_relocations at load time.
   if (!cs.code_blob.empty() && !result.relocs.empty()) {
      canonicalize_reloc_sites(reinterpret_cast<char*>(cs.code_blob.data()),
                               result.relocs.data(),
                               static_cast<uint32_t>(result.relocs.size()));
   }

   file.code_sections.push_back(std::move(cs));

   return pzam_save(file);
}

template<typename IrWriter>
static bool compile_wasm(
      std::vector<uint8_t>& wasm_bytes,
      pzam_arch target_arch,
      const std::string& output_file,
      bool use_softfloat = false,
      bool use_backtrace = false,
      const std::string& target_triple = {},
      int opt_level = 2,
      std::uint32_t compile_threads = 0,
      const std::string& dump_layout = {}) {

   module mod;
   mod.allocator.use_default_memory();

   pzam_compile_result compile_result;
   compile_result.target_triple = target_triple;
   compile_result.softfloat = use_softfloat;
   compile_result.backtrace = use_backtrace;
   compile_result.opt_level = opt_level;
   null_debug_info debug;

   compile_options opts;
   opts.compile_threads = compile_threads;

   using parser_t = binary_parser<IrWriter, compile_options, null_debug_info>;
   parser_t parser(mod.allocator, opts, use_backtrace, false);
   parser.set_compile_result(&compile_result);

#ifdef __EXCEPTIONS
   try {
      parser.parse_module(wasm_bytes, mod, debug);
   } catch (const psizam::exception& ex) {
      std::cerr << "Error: " << ex.what() << " : " << ex.detail() << "\n";
      return false;
   }
#else
   parser.parse_module(wasm_bytes, mod, debug);
#endif

   mod.finalize();

   if (!compile_result.error.empty()) {
      std::cerr << "Error: " << compile_result.error << "\n";
      return false;
   }

   auto pzam_bytes = write_pzam(mod, mod.allocator, compile_result,
                                 wasm_bytes, target_arch, use_softfloat,
                                 use_backtrace);

   std::ofstream out(output_file, std::ios::binary);
   if (!out.is_open()) {
      std::cerr << "Error: cannot open output file: " << output_file << "\n";
      return false;
   }
   out.write(pzam_bytes.data(), pzam_bytes.size());

   if (const char* dump_code_env = std::getenv("PSIZAM_DUMP_CODE_BLOB")) {
      std::ofstream code(dump_code_env, std::ios::binary);
      if (code.is_open()) {
         code.write(reinterpret_cast<const char*>(compile_result.code_blob.data()),
                    compile_result.code_blob.size());
         std::cerr << "[dump-code] wrote " << compile_result.code_blob.size()
                   << " bytes to " << dump_code_env << "\n";
      }
   }

   if (!dump_layout.empty()) {
      std::ofstream layout(dump_layout);
      if (!layout.is_open()) {
         std::cerr << "Warning: cannot open layout dump file: " << dump_layout << "\n";
      } else {
         // Sort by offset so "grep + sort" isn't needed for PC lookup
         struct row { uint32_t idx; uint32_t off; uint32_t sz; int kind; };
         std::vector<row> rows;
         rows.reserve(compile_result.function_offsets.size() +
                      compile_result.body_offsets.size());
         for (size_t i = 0; i < compile_result.function_offsets.size(); ++i) {
            auto [off, sz] = compile_result.function_offsets[i];
            if (sz != 0) rows.push_back({static_cast<uint32_t>(i), off, sz, 0});
         }
         for (size_t i = 0; i < compile_result.body_offsets.size(); ++i) {
            auto [off, sz] = compile_result.body_offsets[i];
            if (sz != 0) rows.push_back({static_cast<uint32_t>(i), off, sz, 1});
         }
         std::sort(rows.begin(), rows.end(), [](const row& a, const row& b) {
            return a.off < b.off;
         });
         layout << "# kind=0 entry, kind=1 body. idx = code_idx (excludes imports)\n";
         layout << "# off_hex off_dec size kind code_idx\n";
         for (auto& r : rows) {
            layout << std::hex << "0x" << r.off << std::dec
                   << " " << r.off << " " << r.sz << " " << r.kind << " " << r.idx << "\n";
         }
      }
   }
   return true;
}

int pzam_compile_main(int argc, char** argv);

#ifdef PZAM_STANDALONE_COMPILE
int main(int argc, char** argv) { return pzam_compile_main(argc, argv); }
#endif

int pzam_compile_main(int argc, char** argv) {
   std::string target_str;
   std::string backend_str = "jit2";
   std::string output_file;
   std::string input_file;
   bool use_softfloat = false;
   bool use_backtrace = false;
   int opt_level = 2;
   std::uint32_t compile_threads = std::thread::hardware_concurrency();
   if (compile_threads == 0) compile_threads = 1;
   std::string dump_layout;

   for (int i = 1; i < argc; i++) {
      std::string arg = argv[i];
      if (arg.starts_with("--target=")) {
         target_str = arg.substr(9);
      } else if (arg.starts_with("--backend=")) {
         backend_str = arg.substr(10);
      } else if (arg.starts_with("--opt-level=")) {
         opt_level = std::stoi(arg.substr(12));
      } else if (arg.starts_with("--dump-layout=")) {
         dump_layout = arg.substr(14);
      } else if (arg == "-j") {
         if (i + 1 < argc) {
            compile_threads = static_cast<std::uint32_t>(std::stoul(argv[++i]));
         } else {
            compile_threads = std::thread::hardware_concurrency();
            if (compile_threads == 0) compile_threads = 1;
         }
      } else if (arg.starts_with("-j")) {
         compile_threads = static_cast<std::uint32_t>(std::stoul(arg.substr(2)));
      } else if (arg == "--softfloat") {
         use_softfloat = true;
      } else if (arg == "--backtrace") {
         use_backtrace = true;
      } else if (arg == "-o" && i + 1 < argc) {
         output_file = argv[++i];
      } else if (arg[0] != '-') {
         input_file = arg;
      } else {
         usage(argv[0]);
         return 1;
      }
   }

   if (input_file.empty() || target_str.empty()) {
      usage(argv[0]);
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
      std::cerr << "Error: unknown target architecture: " << target_str << "\n";
      std::cerr << "Supported targets: x86_64, aarch64\n";
      return 1;
   }

   if (output_file.empty()) {
      // Default: replace .wasm extension with .pzam
      output_file = input_file;
      auto dot = output_file.rfind('.');
      if (dot != std::string::npos) {
         output_file = output_file.substr(0, dot);
      }
      output_file += ".pzam";
   }

   if (use_softfloat && backend_str != "llvm") {
      std::cerr << "Error: --softfloat requires --backend=llvm\n";
      return 1;
   }

   auto wasm_bytes = read_wasm(input_file);

   bool ok;
   if (backend_str == "jit2") {
      // jit2 backend ignores compile_threads; pass 0 to keep it serial.
      if (target_arch == pzam_arch::x86_64) {
         ok = compile_wasm<ir_writer_x64>(wasm_bytes, target_arch, output_file,
                                           false, use_backtrace, {}, 2, 0, dump_layout);
      } else {
         ok = compile_wasm<ir_writer_a64>(wasm_bytes, target_arch, output_file,
                                           false, use_backtrace, {}, 2, 0, dump_layout);
      }
   } else if (backend_str == "llvm") {
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      ok = compile_wasm<ir_writer_llvm_aot>(wasm_bytes, target_arch, output_file,
                                             use_softfloat, use_backtrace,
                                             target_triple, opt_level,
                                             compile_threads,
                                             dump_layout);
#else
      std::cerr << "Error: LLVM backend not available (build with -DPSIZAM_ENABLE_LLVM=ON)\n";
      return 1;
#endif
   } else {
      std::cerr << "Error: unknown backend: " << backend_str << "\n";
      std::cerr << "Supported backends: jit2, llvm\n";
      return 1;
   }

   if (!ok) return 1;

   std::cerr << "Compiled " << input_file << " -> " << output_file
             << " (target: " << target_str << ", backend: " << backend_str;
   if (backend_str == "llvm") std::cerr << ", -j" << compile_threads;
   std::cerr << (use_softfloat ? ", softfloat" : "")
             << (use_backtrace ? ", backtrace" : "") << ")\n";
   return 0;
}
