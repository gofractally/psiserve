#pragma once

// High-level API for compiling .wasm to .pzam.
//
//   psizam::pzam_compile_file("module.wasm", "module.pzam");
//
// Or with options:
//
//   psizam::pzam_compile_file("module.wasm", "module.pzam",
//                             {.arch = pzam_arch::aarch64, .softfloat = true});

#include <psizam/detail/parser.hpp>
#include <psizam/detail/ir_writer.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/pzam_metadata.hpp>
#include <psizam/utils.hpp>

#ifdef PSIZAM_ENABLE_LLVM_BACKEND
#include <psizam/detail/ir_writer_llvm_aot.hpp>
#endif

#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace psizam {

   /// Options for pzam_compile.
   struct pzam_compile_options {
      pzam_arch arch =
#if defined(__x86_64__)
         pzam_arch::x86_64;
#elif defined(__aarch64__)
         pzam_arch::aarch64;
#else
         pzam_arch{};
#endif
      bool softfloat = false;
      bool backtrace = false;
   };

   /// Compile WASM bytes to a .pzam byte vector.
   inline std::vector<char> pzam_compile(std::span<const uint8_t> wasm_bytes,
                                         const pzam_compile_options& opts = {}) {
      using namespace detail;

      module mod;
      mod.allocator.use_default_memory();

      pzam_compile_result compile_result;
      compile_result.softfloat = opts.softfloat;
      compile_result.backtrace = opts.backtrace;
      null_debug_info debug;

      // Use JIT2 backend (cross-compilation capable)
      using parser_t = binary_parser<ir_writer, default_options, null_debug_info>;
      parser_t parser(mod.allocator, default_options{}, opts.backtrace, false);
      parser.set_compile_result(&compile_result);

      // Need a mutable copy for parsing
      std::vector<uint8_t> wasm_copy(wasm_bytes.begin(), wasm_bytes.end());
      parser.parse_module(wasm_copy, mod, debug);
      mod.finalize();

      if (!compile_result.error.empty())
         throw std::runtime_error("pzam_compile: " + compile_result.error);

      // Build pzam_file
      pzam_file file;
      file.input_hash = pzam_cache::hash_wasm(wasm_copy);
      file.metadata = extract_metadata(mod);

      bool llvm_aot = !compile_result.code_blob.empty();

      pzam_code_section cs;
      cs.arch = static_cast<uint8_t>(opts.arch);
      cs.opt_tier = static_cast<uint8_t>(llvm_aot ? pzam_opt_tier::llvm_O3 : pzam_opt_tier::jit2);
      cs.instrumentation.softfloat = opts.softfloat ? 1 : 0;
      cs.instrumentation.async_backtrace = opts.backtrace ? 1 : 0;
      cs.stack_limit_mode = mod.stack_limit_is_bytes ? 1 : 0;
      cs.page_size = 4096;
      cs.max_stack = static_cast<uint32_t>(mod.maximum_stack);
      cs.compiler.compiler_name = llvm_aot ? "psizam-llvm" : "psizam-jit2";
      cs.compiler.compiler_version = "0.1.0";
      cs.compiler.compiler_hash = pzam_cache::compiler_identity(opts.arch);

      cs.functions.resize(mod.code.size());
      for (size_t i = 0; i < mod.code.size(); i++) {
         if (llvm_aot && i < compile_result.function_offsets.size()) {
            cs.functions[i].code_offset = compile_result.function_offsets[i].first;
            cs.functions[i].code_size   = compile_result.function_offsets[i].second;
         } else {
            cs.functions[i].code_offset = static_cast<uint32_t>(mod.code[i].jit_code_offset);
            cs.functions[i].code_size   = mod.code[i].jit_code_size;
         }
         cs.functions[i].stack_size = mod.code[i].stack_size;
      }

      cs.relocations.resize(compile_result.relocs.size());
      for (size_t i = 0; i < compile_result.relocs.size(); i++) {
         cs.relocations[i].code_offset = compile_result.relocs[i].code_offset;
         cs.relocations[i].symbol      = static_cast<uint16_t>(compile_result.relocs[i].symbol);
         cs.relocations[i].type        = static_cast<uint8_t>(compile_result.relocs[i].type);
         cs.relocations[i].addend      = compile_result.relocs[i].addend;
      }

      if (llvm_aot) {
         cs.code_blob = compile_result.code_blob;
      } else {
         auto code_start = reinterpret_cast<const uint8_t*>(mod.allocator.get_code_start());
         size_t actual_size = mod.allocator.get_actual_code_size();
         cs.code_blob.assign(code_start, code_start + actual_size);
      }

      file.code_sections.push_back(std::move(cs));
      return pzam_save(file);
   }

   /// Compile a .wasm file and write the result to a .pzam file.
   inline void pzam_compile_file(const std::string& wasm_path, const std::string& pzam_path,
                                 const pzam_compile_options& opts = {}) {
      auto wasm = read_wasm(wasm_path);
      auto pzam = pzam_compile(wasm, opts);

      std::ofstream out(pzam_path, std::ios::binary);
      if (!out.is_open())
         throw std::runtime_error("pzam_compile_file: cannot open " + pzam_path);
      out.write(pzam.data(), static_cast<std::streamsize>(pzam.size()));
   }

} // namespace psizam
