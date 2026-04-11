#pragma once

// .pzam cache — save and load compiled WASM native code using fracpack.
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
#if defined(PSIZAM_ENABLE_LLVM_BACKEND) && !defined(PSIZAM_COMPILE_ONLY)
#include <psizam/llvm_runtime_helpers.hpp>
#endif

#include <cstring>
#include <span>
#include <vector>

#ifndef PSIZAM_COMPILE_ONLY
#if defined(__APPLE__)
#include <sys/mman.h>
#elif defined(__linux__)
#include <sys/mman.h>
#endif
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

   /// Build the reloc_symbol -> function address mapping table for LLVM runtime helpers.
   /// Used when loading .pzam files compiled with the LLVM backend.
#if defined(PSIZAM_ENABLE_LLVM_BACKEND) && !defined(PSIZAM_COMPILE_ONLY)
   inline void build_llvm_symbol_table(void** table) {
      // Zero-initialize
      std::memset(table, 0, static_cast<size_t>(reloc_symbol::NUM_SYMBOLS) * sizeof(void*));

      // LLVM runtime helpers
      table[static_cast<uint32_t>(reloc_symbol::llvm_global_get)]       = reinterpret_cast<void*>(&__psizam_global_get);
      table[static_cast<uint32_t>(reloc_symbol::llvm_global_set)]       = reinterpret_cast<void*>(&__psizam_global_set);
      table[static_cast<uint32_t>(reloc_symbol::llvm_global_get_v128)]  = reinterpret_cast<void*>(&__psizam_global_get_v128);
      table[static_cast<uint32_t>(reloc_symbol::llvm_global_set_v128)]  = reinterpret_cast<void*>(&__psizam_global_set_v128);
      table[static_cast<uint32_t>(reloc_symbol::llvm_memory_size)]      = reinterpret_cast<void*>(&__psizam_memory_size);
      table[static_cast<uint32_t>(reloc_symbol::llvm_memory_grow)]      = reinterpret_cast<void*>(&__psizam_memory_grow);
      table[static_cast<uint32_t>(reloc_symbol::llvm_call_host)]        = reinterpret_cast<void*>(&__psizam_call_host);
      table[static_cast<uint32_t>(reloc_symbol::llvm_memory_init)]      = reinterpret_cast<void*>(&__psizam_memory_init);
      table[static_cast<uint32_t>(reloc_symbol::llvm_data_drop)]        = reinterpret_cast<void*>(&__psizam_data_drop);
      table[static_cast<uint32_t>(reloc_symbol::llvm_memory_copy)]      = reinterpret_cast<void*>(&__psizam_memory_copy);
      table[static_cast<uint32_t>(reloc_symbol::llvm_memory_fill)]      = reinterpret_cast<void*>(&__psizam_memory_fill);
      table[static_cast<uint32_t>(reloc_symbol::llvm_table_init)]       = reinterpret_cast<void*>(&__psizam_table_init);
      table[static_cast<uint32_t>(reloc_symbol::llvm_elem_drop)]        = reinterpret_cast<void*>(&__psizam_elem_drop);
      table[static_cast<uint32_t>(reloc_symbol::llvm_table_copy)]       = reinterpret_cast<void*>(&__psizam_table_copy);
      table[static_cast<uint32_t>(reloc_symbol::llvm_call_indirect)]    = reinterpret_cast<void*>(&__psizam_call_indirect);
      table[static_cast<uint32_t>(reloc_symbol::llvm_table_get)]        = reinterpret_cast<void*>(&__psizam_table_get);
      table[static_cast<uint32_t>(reloc_symbol::llvm_table_set)]        = reinterpret_cast<void*>(&__psizam_table_set);
      table[static_cast<uint32_t>(reloc_symbol::llvm_table_grow)]       = reinterpret_cast<void*>(&__psizam_table_grow);
      table[static_cast<uint32_t>(reloc_symbol::llvm_table_size)]       = reinterpret_cast<void*>(&__psizam_table_size);
      table[static_cast<uint32_t>(reloc_symbol::llvm_table_fill)]       = reinterpret_cast<void*>(&__psizam_table_fill);
      table[static_cast<uint32_t>(reloc_symbol::llvm_resolve_indirect)] = reinterpret_cast<void*>(&__psizam_resolve_indirect);
      table[static_cast<uint32_t>(reloc_symbol::llvm_atomic_rmw)]       = reinterpret_cast<void*>(&__psizam_atomic_rmw);
      table[static_cast<uint32_t>(reloc_symbol::llvm_call_depth_dec)]   = reinterpret_cast<void*>(&__psizam_call_depth_dec);
      table[static_cast<uint32_t>(reloc_symbol::llvm_call_depth_inc)]   = reinterpret_cast<void*>(&__psizam_call_depth_inc);
      table[static_cast<uint32_t>(reloc_symbol::llvm_trap)]             = reinterpret_cast<void*>(&__psizam_trap);
   }
#endif

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
      inline std::array<uint8_t, 32> compiler_identity(pzam_arch arch) {
         std::array<uint8_t, 32> result = {};
         uint64_t h = PZAM_VERSION;
         h ^= static_cast<uint64_t>(arch);
         // Include __DATE__ and __TIME__ to invalidate when recompiled
         const char* date = __DATE__ " " __TIME__;
         while (*date) { h ^= *date++; h *= 0x100000001b3ULL; }
         std::memcpy(result.data(), &h, 8);
         return result;
      }

      /// Save compiled module to fracpack-serialized .pzam byte vector.
      /// code_base: start of the JIT code segment
      /// code_size: total size of the code segment
      inline std::vector<char> save(
            const module& mod,
            const void* code_base,
            uint32_t code_size,
            const relocation_recorder& relocs,
            std::span<const uint8_t> wasm_bytes,
            pzam_arch target_arch,
            bool softfloat_enabled = false,
            bool backtrace_enabled = false) {

         pzam_file file;
         file.arch = static_cast<uint8_t>(target_arch);
         file.opts.softfloat = softfloat_enabled ? 1 : 0;
         file.opts.async_backtrace = backtrace_enabled ? 1 : 0;
         file.opts.stack_limit_is_bytes = mod.stack_limit_is_bytes ? 1 : 0;
         file.max_stack = static_cast<uint32_t>(mod.maximum_stack);
         file.input_hash = hash_wasm(wasm_bytes);
         file.compiler_hash = compiler_identity(target_arch);

         // Build function table
         file.functions.resize(mod.code.size());
         for (size_t i = 0; i < mod.code.size(); i++) {
            file.functions[i].code_offset = static_cast<uint32_t>(mod.code[i].jit_code_offset);
            file.functions[i].code_size = mod.code[i].jit_code_size;
            file.functions[i].stack_size = mod.code[i].stack_size;
         }

         // Convert relocations
         const auto& reloc_entries = relocs.entries();
         file.relocations.resize(reloc_entries.size());
         for (size_t i = 0; i < reloc_entries.size(); i++) {
            file.relocations[i].code_offset = reloc_entries[i].code_offset;
            file.relocations[i].symbol = static_cast<uint16_t>(reloc_entries[i].symbol);
            file.relocations[i].type = static_cast<uint8_t>(reloc_entries[i].type);
            file.relocations[i].addend = reloc_entries[i].addend;
         }

         // Copy code blob
         file.code_blob.assign(
            static_cast<const uint8_t*>(code_base),
            static_cast<const uint8_t*>(code_base) + code_size);

         return pzam_save(file);
      }

      /// Load cached code from fracpack .pzam data into a module.
      /// Returns true on success.
      inline bool load(
            std::span<const char> pzam_data,
            module& mod,
            growable_allocator& alloc,
            std::span<const uint8_t> wasm_bytes,
            void* const* symbol_table) {

         if (!pzam_validate(pzam_data)) return false;

         pzam_file file;
         try {
            file = pzam_load(pzam_data);
         } catch (...) {
            return false;
         }

         // Validate magic and version
         if (file.magic != PZAM_MAGIC) return false;

         // Validate architecture matches current platform
         auto expected_arch =
#if defined(__x86_64__)
            pzam_arch::x86_64;
#elif defined(__aarch64__)
            pzam_arch::aarch64;
#else
            pzam_arch{};
#endif
         if (static_cast<pzam_arch>(file.arch) != expected_arch) return false;

         // Validate hashes
         auto expected_input = hash_wasm(wasm_bytes);
         auto expected_compiler = compiler_identity(expected_arch);
         if (file.input_hash != expected_input) return false;
         if (file.compiler_hash != expected_compiler) return false;

         // Verify function count matches
         if (file.functions.size() != mod.code.size()) return false;

         // Allocate executable memory and copy code
         void* code_dest = alloc.start_code();
         std::memcpy(code_dest, file.code_blob.data(), file.code_blob.size());

         // Convert relocations back to code_relocation and apply
         std::vector<code_relocation> relocs(file.relocations.size());
         for (size_t i = 0; i < file.relocations.size(); i++) {
            relocs[i].code_offset = file.relocations[i].code_offset;
            relocs[i].symbol = static_cast<reloc_symbol>(file.relocations[i].symbol);
            relocs[i].type = static_cast<reloc_type>(file.relocations[i].type);
            relocs[i].addend = file.relocations[i].addend;
         }

         apply_relocations(static_cast<char*>(code_dest),
                           relocs.data(),
                           static_cast<uint32_t>(relocs.size()),
                           symbol_table);

         // Update module function entries
         for (size_t i = 0; i < file.functions.size(); i++) {
            mod.code[i].jit_code_offset = file.functions[i].code_offset;
            mod.code[i].jit_code_size = file.functions[i].code_size;
            mod.code[i].stack_size = file.functions[i].stack_size;
         }

         mod.maximum_stack = file.max_stack;
         mod.stack_limit_is_bytes = file.opts.stack_limit_is_bytes;

         return true;
      }

   } // namespace pzam_cache

} // namespace psizam
