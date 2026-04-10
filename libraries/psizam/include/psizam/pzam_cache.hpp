#pragma once

// .pzam cache — save and load compiled WASM native code.
//
// Usage:
//   // Save after JIT compilation:
//   auto pzam_bytes = pzam_cache::save(module, code_base, code_size, relocations, wasm_bytes);
//   write_file("module.pzam", pzam_bytes);
//
//   // Load cached code:
//   auto pzam_data = read_file("module.pzam");
//   if (pzam_cache::load(pzam_data, module, allocator, wasm_bytes)) {
//       // module.code[i].jit_code_offset is now valid — execute via JIT
//   }

#include <psizam/allocator.hpp>
#include <psizam/jit_reloc.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/types.hpp>

#include <cstring>
#include <span>
#include <vector>

#if defined(__APPLE__)
#include <sys/mman.h>
#elif defined(__linux__)
#include <sys/mman.h>
#endif

namespace psizam {

   /// Build the reloc_symbol -> function address mapping table.
   /// This is the "linker" that resolves symbols at load time.
   /// The table must be kept in sync with the reloc_symbol enum.
   ///
   /// Note: This function must be defined per-backend since different code
   /// generators have different static functions. This version is for jit2 (jit_codegen).
   template <typename CodeGen>
   void build_symbol_table(void** table) {
      // Zero-initialize
      std::memset(table, 0, static_cast<size_t>(reloc_symbol::NUM_SYMBOLS) * sizeof(void*));

      // Core runtime
      table[static_cast<uint32_t>(reloc_symbol::call_host_function)] = reinterpret_cast<void*>(&CodeGen::call_host_function);
      table[static_cast<uint32_t>(reloc_symbol::current_memory)]     = reinterpret_cast<void*>(&CodeGen::current_memory);
      table[static_cast<uint32_t>(reloc_symbol::grow_memory)]        = reinterpret_cast<void*>(&CodeGen::grow_memory);

      // Bulk memory
      table[static_cast<uint32_t>(reloc_symbol::memory_fill)]  = reinterpret_cast<void*>(&CodeGen::memory_fill_impl);
      table[static_cast<uint32_t>(reloc_symbol::memory_copy)]  = reinterpret_cast<void*>(&CodeGen::memory_copy_impl);
      table[static_cast<uint32_t>(reloc_symbol::memory_init)]  = reinterpret_cast<void*>(&CodeGen::memory_init_impl);
      table[static_cast<uint32_t>(reloc_symbol::data_drop)]    = reinterpret_cast<void*>(&CodeGen::data_drop_impl);
      table[static_cast<uint32_t>(reloc_symbol::table_init)]   = reinterpret_cast<void*>(&CodeGen::table_init_impl);
      table[static_cast<uint32_t>(reloc_symbol::elem_drop)]    = reinterpret_cast<void*>(&CodeGen::elem_drop_impl);
      table[static_cast<uint32_t>(reloc_symbol::table_copy)]   = reinterpret_cast<void*>(&CodeGen::table_copy_impl);

      // Error handlers
      table[static_cast<uint32_t>(reloc_symbol::on_unreachable)]        = reinterpret_cast<void*>(&CodeGen::on_unreachable);
      table[static_cast<uint32_t>(reloc_symbol::on_fp_error)]           = reinterpret_cast<void*>(&CodeGen::on_fp_error);
      table[static_cast<uint32_t>(reloc_symbol::on_memory_error)]       = reinterpret_cast<void*>(&CodeGen::on_memory_error);
      table[static_cast<uint32_t>(reloc_symbol::on_call_indirect_error)] = reinterpret_cast<void*>(&CodeGen::on_call_indirect_error);
      table[static_cast<uint32_t>(reloc_symbol::on_type_error)]         = reinterpret_cast<void*>(&CodeGen::on_type_error);
      table[static_cast<uint32_t>(reloc_symbol::on_stack_overflow)]     = reinterpret_cast<void*>(&CodeGen::on_stack_overflow);

      // Trunc (trapping)
      table[static_cast<uint32_t>(reloc_symbol::trunc_f32_i32s)] = reinterpret_cast<void*>(&CodeGen::trunc_f32_i32s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_f32_i32u)] = reinterpret_cast<void*>(&CodeGen::trunc_f32_i32u);
      table[static_cast<uint32_t>(reloc_symbol::trunc_f64_i32s)] = reinterpret_cast<void*>(&CodeGen::trunc_f64_i32s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_f64_i32u)] = reinterpret_cast<void*>(&CodeGen::trunc_f64_i32u);
      table[static_cast<uint32_t>(reloc_symbol::trunc_f32_i64s)] = reinterpret_cast<void*>(&CodeGen::trunc_f32_i64s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_f32_i64u)] = reinterpret_cast<void*>(&CodeGen::trunc_f32_i64u);
      table[static_cast<uint32_t>(reloc_symbol::trunc_f64_i64s)] = reinterpret_cast<void*>(&CodeGen::trunc_f64_i64s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_f64_i64u)] = reinterpret_cast<void*>(&CodeGen::trunc_f64_i64u);

      // Trunc_sat (non-trapping)
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f32_i32s)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f32_i32s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f32_i32u)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f32_i32u);
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f64_i32s)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f64_i32s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f64_i32u)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f64_i32u);
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f32_i64s)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f32_i64s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f32_i64u)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f32_i64u);
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f64_i64s)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f64_i64s);
      table[static_cast<uint32_t>(reloc_symbol::trunc_sat_f64_i64u)] = reinterpret_cast<void*>(&CodeGen::trunc_sat_f64_i64u);

      // SIMD data
      // popcnt4 table is static data in the code generator — address resolved at template instantiation
   }

   namespace pzam_cache {

      /// Compute a simple hash of WASM input bytes.
      /// Uses FNV-1a (not cryptographic — for cache invalidation only).
      inline std::array<uint8_t, 32> hash_wasm(std::span<const uint8_t> wasm) {
         std::array<uint8_t, 32> result = {};
         uint64_t h = 0xcbf29ce484222325ULL;
         for (auto b : wasm) {
            h ^= b;
            h *= 0x100000001b3ULL;
         }
         std::memcpy(result.data(), &h, 8);
         // Fill remaining bytes with rotated versions for uniqueness
         for (int i = 1; i < 4; i++) {
            h = (h >> 13) ^ (h << 51) ^ (h * 0x9e3779b97f4a7c15ULL);
            std::memcpy(result.data() + i * 8, &h, 8);
         }
         return result;
      }

      /// Compiler identity hash — changes when the compiler itself changes.
      /// Incorporates format version, arch, and a compile-time salt.
      inline std::array<uint8_t, 32> compiler_identity() {
         std::array<uint8_t, 32> result = {};
         uint64_t h = PZAM_VERSION;
#if defined(__x86_64__)
         h ^= 0x8664; // x86_64 marker
#elif defined(__aarch64__)
         h ^= 0xAA64; // aarch64 marker
#endif
         // Include __DATE__ and __TIME__ to invalidate when recompiled
         const char* date = __DATE__ " " __TIME__;
         while (*date) { h ^= *date++; h *= 0x100000001b3ULL; }
         std::memcpy(result.data(), &h, 8);
         return result;
      }

      /// Save compiled module to .pzam byte vector.
      /// code_base: start of the JIT code segment
      /// code_size: total size of the code segment
      inline std::vector<uint8_t> save(
            const module& mod,
            const void* code_base,
            uint32_t code_size,
            const relocation_recorder& relocs,
            std::span<const uint8_t> wasm_bytes,
            bool softfloat_enabled = false,
            bool backtrace_enabled = false) {

         pzam_header hdr;
#if defined(__x86_64__)
         hdr.arch = pzam_arch::x86_64;
#elif defined(__aarch64__)
         hdr.arch = pzam_arch::aarch64;
#endif
         hdr.opts.softfloat = softfloat_enabled ? 1 : 0;
         hdr.opts.async_backtrace = backtrace_enabled ? 1 : 0;
         hdr.opts.stack_limit_is_bytes = mod.stack_limit_is_bytes ? 1 : 0;
         hdr.num_functions = static_cast<uint32_t>(mod.code.size());
         hdr.num_relocations = relocs.size();
         hdr.code_size = code_size;
         hdr.max_stack = static_cast<uint32_t>(mod.maximum_stack);
         hdr.input_hash = hash_wasm(wasm_bytes);
         hdr.compiler_hash = compiler_identity();

         // Build function table
         std::vector<pzam_func_entry> funcs(mod.code.size());
         for (size_t i = 0; i < mod.code.size(); i++) {
            funcs[i].code_offset = static_cast<uint32_t>(mod.code[i].jit_code_offset);
            funcs[i].code_size = mod.code[i].jit_code_size;
            funcs[i].stack_size = mod.code[i].stack_size;
         }

         auto code_blob = std::span<const uint8_t>(
            static_cast<const uint8_t*>(code_base), code_size);

         return pzam_save(hdr, funcs, relocs.entries(), code_blob);
      }

      /// Load cached code from .pzam data into a module.
      /// Returns true on success.
      /// On success, module.code[i].jit_code_offset is updated and
      /// code is allocated in the provided allocator with execute permissions.
      inline bool load(
            std::span<const uint8_t> pzam_data,
            module& mod,
            growable_allocator& alloc,
            std::span<const uint8_t> wasm_bytes,
            void* const* symbol_table) {

         pzam_parsed parsed;
         if (!pzam_parse(pzam_data, parsed)) return false;

         auto expected_input = hash_wasm(wasm_bytes);
         auto expected_compiler = compiler_identity();
         if (!pzam_validate_header(*parsed.header, expected_input, expected_compiler))
            return false;

         // Verify function count matches
         if (parsed.header->num_functions != mod.code.size())
            return false;

         // Allocate executable memory and copy code
         void* code_dest = alloc.start_code();
         std::memcpy(code_dest, parsed.code_blob.data(), parsed.code_blob.size());

         // Apply relocations
         apply_relocations(static_cast<char*>(code_dest),
                           parsed.relocs.data(),
                           parsed.header->num_relocations,
                           symbol_table);

         // Update module function entries
         for (uint32_t i = 0; i < parsed.header->num_functions; i++) {
            mod.code[i].jit_code_offset = parsed.funcs[i].code_offset;
            mod.code[i].jit_code_size = parsed.funcs[i].code_size;
            mod.code[i].stack_size = parsed.funcs[i].stack_size;
         }

         mod.maximum_stack = parsed.header->max_stack;
         mod.stack_limit_is_bytes = parsed.header->opts.stack_limit_is_bytes;

         return true;
      }

   } // namespace pzam_cache

} // namespace psizam
