// pzam-compile: Cross-compile WASM modules to native .pzam files.
//
// Usage: pzam-compile --target=x86_64|aarch64 [--backend=jit2|llvm] [-o output.pzam] input.wasm
//
// Reads a .wasm file, compiles it to native code for the specified target
// architecture, and writes a .pzam file containing the relocatable code blob.
// The .pzam file can later be loaded and relocated at runtime, skipping
// the compilation step.

#include <psizam/parser.hpp>
#include <psizam/ir_writer.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/utils.hpp>

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
#include <psizam/ir_writer_llvm_aot.hpp>
#endif

#include <cstring>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace psizam;

static void usage(const char* prog) {
   std::cerr << "Usage: " << prog << " --target=x86_64|aarch64 [--backend=jit2|llvm] [-o output.pzam] input.wasm\n";
}

static std::vector<char> write_pzam(
      const module& mod,
      const growable_allocator& alloc,
      const pzam_compile_result& result,
      std::span<const uint8_t> wasm_bytes,
      pzam_arch target_arch) {

   pzam_file file;
   file.arch = static_cast<uint8_t>(target_arch);
   file.opts.softfloat =
#ifdef PSIZAM_SOFTFLOAT
      1;
#else
      0;
#endif
   file.opts.async_backtrace = 0;
   file.opts.stack_limit_is_bytes = mod.stack_limit_is_bytes ? 1 : 0;
   file.max_stack = static_cast<uint32_t>(mod.maximum_stack);
   file.input_hash = pzam_cache::hash_wasm(wasm_bytes);
   file.compiler_hash = pzam_cache::compiler_identity(target_arch);

   bool llvm_aot = !result.code_blob.empty();

   // Function table
   file.functions.resize(mod.code.size());
   for (size_t i = 0; i < mod.code.size(); i++) {
      if (llvm_aot && i < result.function_offsets.size()) {
         file.functions[i].code_offset = result.function_offsets[i].first;
         file.functions[i].code_size   = result.function_offsets[i].second;
      } else {
         file.functions[i].code_offset = static_cast<uint32_t>(mod.code[i].jit_code_offset);
         file.functions[i].code_size   = mod.code[i].jit_code_size;
      }
      file.functions[i].stack_size = mod.code[i].stack_size;
   }

   // Relocations
   file.relocations.resize(result.relocs.size());
   for (size_t i = 0; i < result.relocs.size(); i++) {
      file.relocations[i].code_offset = result.relocs[i].code_offset;
      file.relocations[i].symbol      = static_cast<uint16_t>(result.relocs[i].symbol);
      file.relocations[i].type        = static_cast<uint8_t>(result.relocs[i].type);
      file.relocations[i].addend      = result.relocs[i].addend;
   }

   // Code blob
   if (llvm_aot) {
      file.code_blob = result.code_blob;
   } else {
      auto code_span = alloc.get_code_span();
      file.code_blob.assign(
         reinterpret_cast<const uint8_t*>(code_span.data()),
         reinterpret_cast<const uint8_t*>(code_span.data()) + code_span.size());
   }

   return pzam_save(file);
}

template<typename IrWriter>
static bool compile_wasm(
      std::vector<uint8_t>& wasm_bytes,
      pzam_arch target_arch,
      const std::string& output_file,
      const std::string& target_triple = {}) {

   module mod;
   mod.allocator.use_default_memory();

   pzam_compile_result compile_result;
   compile_result.target_triple = target_triple;
   null_debug_info debug;

   using parser_t = binary_parser<IrWriter, default_options, null_debug_info>;
   parser_t parser(mod.allocator, default_options{}, false, false);
   parser.set_compile_result(&compile_result);

   try {
      parser.parse_module(wasm_bytes, mod, debug);
   } catch (const psizam::exception& ex) {
      std::cerr << "Error: " << ex.what() << " : " << ex.detail() << "\n";
      return false;
   }

   mod.finalize();

   auto pzam_bytes = write_pzam(mod, mod.allocator, compile_result,
                                 wasm_bytes, target_arch);

   std::ofstream out(output_file, std::ios::binary);
   if (!out.is_open()) {
      std::cerr << "Error: cannot open output file: " << output_file << "\n";
      return false;
   }
   out.write(pzam_bytes.data(), pzam_bytes.size());
   return true;
}

int main(int argc, char** argv) {
   std::string target_str;
   std::string backend_str = "jit2";
   std::string output_file;
   std::string input_file;

   for (int i = 1; i < argc; i++) {
      std::string arg = argv[i];
      if (arg.starts_with("--target=")) {
         target_str = arg.substr(9);
      } else if (arg.starts_with("--backend=")) {
         backend_str = arg.substr(10);
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

   auto wasm_bytes = read_wasm(input_file);

   bool ok;
   if (backend_str == "jit2") {
      if (target_arch == pzam_arch::x86_64) {
         ok = compile_wasm<ir_writer_x64>(wasm_bytes, target_arch, output_file);
      } else {
         ok = compile_wasm<ir_writer_a64>(wasm_bytes, target_arch, output_file);
      }
   } else if (backend_str == "llvm") {
#ifdef PSIZAM_ENABLE_LLVM_BACKEND
      ok = compile_wasm<ir_writer_llvm_aot>(wasm_bytes, target_arch, output_file, target_triple);
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
             << " (target: " << target_str << ", backend: " << backend_str << ")\n";
   return 0;
}
