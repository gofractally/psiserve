#pragma once

// High-level API for compiling .wasm to .pzam.
//
//   // Defaults: host arch, jit2 tier, no instrumentation
//   psizam::pzam_compile_file("module.wasm", "module.pzam");
//
//   // Cross-compile for both architectures with softfloat:
//   using enum psizam::pzam_arch_flags;
//   using enum psizam::pzam_tier_flags;
//   using enum psizam::pzam_compile_flags;
//   psizam::pzam_compile_file("module.wasm", "module.pzam",
//                             {.arch = x86_64 | aarch64, .tier = jit2 | llvm_O3,
//                              .flags = softfloat});

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

   // ---- Compile option bitmaps ----

   enum class pzam_arch_flags : uint32_t {
      none    = 0,
      x86_64  = 1 << 0,
      aarch64 = 1 << 1,
      all     = x86_64 | aarch64,
#if defined(__x86_64__)
      host    = x86_64,
#elif defined(__aarch64__)
      host    = aarch64,
#else
      host    = none,
#endif
   };

   enum class pzam_tier_flags : uint32_t {
      none    = 0,
      jit1    = 1 << 0,
      jit2    = 1 << 1,
      llvm_O1 = 1 << 2,
      llvm_O2 = 1 << 3,
      llvm_O3 = 1 << 4,
      all_jit  = jit1 | jit2,
      all_llvm = llvm_O1 | llvm_O2 | llvm_O3,
      all      = all_jit | all_llvm,
   };

   enum class pzam_compile_flags : uint32_t {
      none       = 0,
      softfloat  = 1 << 0,
      backtrace  = 1 << 1,
      gas_meter  = 1 << 2,
      debug_info = 1 << 3,
   };

   // Bitwise operators for flag enums
   #define PSIZAM_FLAG_OPS(E) \
      constexpr E operator|(E a, E b) { return E(uint32_t(a) | uint32_t(b)); } \
      constexpr E operator&(E a, E b) { return E(uint32_t(a) & uint32_t(b)); } \
      constexpr E operator~(E a)      { return E(~uint32_t(a)); } \
      constexpr bool operator!(E a)   { return uint32_t(a) == 0; }
   PSIZAM_FLAG_OPS(pzam_arch_flags)
   PSIZAM_FLAG_OPS(pzam_tier_flags)
   PSIZAM_FLAG_OPS(pzam_compile_flags)
   #undef PSIZAM_FLAG_OPS

   /// Options for pzam_compile.  All fields are bitmaps — compile for
   /// multiple architectures and optimization tiers in a single call,
   /// producing one code section per arch×tier combination.
   struct pzam_compile_options {
      pzam_arch_flags    arch  = pzam_arch_flags::host;
      pzam_tier_flags    tier  = pzam_tier_flags::jit2;
      pzam_compile_flags flags = pzam_compile_flags::none;
   };

   // Map flag bits to format enum values
   namespace detail {
      inline constexpr std::pair<pzam_arch_flags, pzam_arch> arch_map[] = {
         {pzam_arch_flags::x86_64,  pzam_arch::x86_64},
         {pzam_arch_flags::aarch64, pzam_arch::aarch64},
      };
      inline constexpr std::pair<pzam_tier_flags, pzam_opt_tier> tier_map[] = {
         {pzam_tier_flags::jit1,    pzam_opt_tier::jit1},
         {pzam_tier_flags::jit2,    pzam_opt_tier::jit2},
         {pzam_tier_flags::llvm_O1, pzam_opt_tier::llvm_O1},
         {pzam_tier_flags::llvm_O2, pzam_opt_tier::llvm_O2},
         {pzam_tier_flags::llvm_O3, pzam_opt_tier::llvm_O3},
      };
   }

   /// Compile WASM bytes to a .pzam byte vector.
   /// Produces one code section per arch×tier combination set in opts.
   inline std::vector<char> pzam_compile(std::span<const uint8_t> wasm_bytes,
                                         const pzam_compile_options& opts = {}) {
      using namespace detail;

      bool do_softfloat  = !!(opts.flags & pzam_compile_flags::softfloat);
      bool do_backtrace  = !!(opts.flags & pzam_compile_flags::backtrace);

      // Parse once, reuse for all code sections
      module mod;
      mod.allocator.use_default_memory();

      pzam_compile_result compile_result;
      compile_result.softfloat = do_softfloat;
      compile_result.backtrace = do_backtrace;
      null_debug_info debug;

      using parser_t = binary_parser<ir_writer, default_options, null_debug_info>;
      parser_t parser(mod.allocator, default_options{}, do_backtrace, false);
      parser.set_compile_result(&compile_result);

      std::vector<uint8_t> wasm_copy(wasm_bytes.begin(), wasm_bytes.end());
      parser.parse_module(wasm_copy, mod, debug);
      mod.finalize();

      if (!compile_result.error.empty())
         throw std::runtime_error("pzam_compile: " + compile_result.error);

      // Build pzam_file with metadata
      pzam_file file;
      file.input_hash = pzam_cache::hash_wasm(wasm_copy);
      file.metadata = extract_metadata(mod);

      bool llvm_aot = !compile_result.code_blob.empty();

      // Generate a code section for each arch×tier combination
      for (auto [arch_flag, arch_val] : arch_map) {
         if (!(opts.arch & arch_flag)) continue;
         for (auto [tier_flag, tier_val] : tier_map) {
            if (!(opts.tier & tier_flag)) continue;

            // TODO: re-compile for each arch×tier when cross-compilation
            // backends are wired up. For now, use the single compile result
            // and tag it with the requested arch/tier.

            pzam_code_section cs;
            cs.arch = static_cast<uint8_t>(arch_val);
            cs.opt_tier = static_cast<uint8_t>(llvm_aot ? pzam_opt_tier::llvm_O3 : tier_val);
            cs.instrumentation.softfloat = do_softfloat ? 1 : 0;
            cs.instrumentation.async_backtrace = do_backtrace ? 1 : 0;
            cs.instrumentation.gas_metering = !!(opts.flags & pzam_compile_flags::gas_meter) ? 1 : 0;
            cs.instrumentation.debug_info = !!(opts.flags & pzam_compile_flags::debug_info) ? 1 : 0;
            cs.stack_limit_mode = mod.stack_limit_is_bytes ? 1 : 0;
            cs.page_size = 4096;
            cs.max_stack = static_cast<uint32_t>(mod.maximum_stack);
            cs.compiler.compiler_name = llvm_aot ? "psizam-llvm" : "psizam-jit2";
            cs.compiler.compiler_version = "0.1.0";
            cs.compiler.compiler_hash = pzam_cache::compiler_identity(arch_val);

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
         }
      }

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
