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
#include <psizam/detail/jit_reloc.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/types.hpp>
#ifndef PSIZAM_COMPILE_ONLY
#include <psizam/detail/llvm_runtime_helpers.hpp>
#endif
#ifdef PSIZAM_SOFTFLOAT
#include <psizam/detail/softfloat.hpp>
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
   /// Always available in exec contexts — a .pzam compiled elsewhere with LLVM
   /// can be loaded and run without LLVM installed locally.
#ifndef PSIZAM_COMPILE_ONLY
   inline void build_llvm_symbol_table(void** table) {
      // LLVM runtime helpers (does not zero-init — caller or build_symbol_table handles that)
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
      table[static_cast<uint32_t>(reloc_symbol::llvm_get_memory)]       = reinterpret_cast<void*>(&__psizam_get_memory);

#ifdef PSIZAM_SOFTFLOAT
      // Softfloat functions (used when .pzam was compiled with softfloat mode)
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_add)]     = reinterpret_cast<void*>(&_psizam_f32_add);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_sub)]     = reinterpret_cast<void*>(&_psizam_f32_sub);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_mul)]     = reinterpret_cast<void*>(&_psizam_f32_mul);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_div)]     = reinterpret_cast<void*>(&_psizam_f32_div);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_min)]     = reinterpret_cast<void*>(&_psizam_f32_min<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_max)]     = reinterpret_cast<void*>(&_psizam_f32_max<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_copysign)] = reinterpret_cast<void*>(&_psizam_f32_copysign);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_abs)]     = reinterpret_cast<void*>(&_psizam_f32_abs);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_neg)]     = reinterpret_cast<void*>(&_psizam_f32_neg);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_sqrt)]    = reinterpret_cast<void*>(&_psizam_f32_sqrt);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_ceil)]    = reinterpret_cast<void*>(&_psizam_f32_ceil<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_floor)]   = reinterpret_cast<void*>(&_psizam_f32_floor<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_trunc)]   = reinterpret_cast<void*>(&_psizam_f32_trunc<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_nearest)] = reinterpret_cast<void*>(&_psizam_f32_nearest<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_add)]     = reinterpret_cast<void*>(&_psizam_f64_add);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_sub)]     = reinterpret_cast<void*>(&_psizam_f64_sub);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_mul)]     = reinterpret_cast<void*>(&_psizam_f64_mul);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_div)]     = reinterpret_cast<void*>(&_psizam_f64_div);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_min)]     = reinterpret_cast<void*>(&_psizam_f64_min<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_max)]     = reinterpret_cast<void*>(&_psizam_f64_max<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_copysign)] = reinterpret_cast<void*>(&_psizam_f64_copysign);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_abs)]     = reinterpret_cast<void*>(&_psizam_f64_abs);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_neg)]     = reinterpret_cast<void*>(&_psizam_f64_neg);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_sqrt)]    = reinterpret_cast<void*>(&_psizam_f64_sqrt);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_ceil)]    = reinterpret_cast<void*>(&_psizam_f64_ceil<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_floor)]   = reinterpret_cast<void*>(&_psizam_f64_floor<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_trunc)]   = reinterpret_cast<void*>(&_psizam_f64_trunc<true>);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_nearest)] = reinterpret_cast<void*>(&_psizam_f64_nearest<true>);
      // Conversions
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_convert_i32s)] = reinterpret_cast<void*>(&_psizam_i32_to_f32);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_convert_i32u)] = reinterpret_cast<void*>(&_psizam_ui32_to_f32);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_convert_i64s)] = reinterpret_cast<void*>(&_psizam_i64_to_f32);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_convert_i64u)] = reinterpret_cast<void*>(&_psizam_ui64_to_f32);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_convert_i32s)] = reinterpret_cast<void*>(&_psizam_i32_to_f64);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_convert_i32u)] = reinterpret_cast<void*>(&_psizam_ui32_to_f64);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_convert_i64s)] = reinterpret_cast<void*>(&_psizam_i64_to_f64);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_convert_i64u)] = reinterpret_cast<void*>(&_psizam_ui64_to_f64);
      table[static_cast<uint32_t>(reloc_symbol::sf_f32_demote_f64)]   = reinterpret_cast<void*>(&_psizam_f64_demote);
      table[static_cast<uint32_t>(reloc_symbol::sf_f64_promote_f32)]  = reinterpret_cast<void*>(&_psizam_f32_promote);
#endif // PSIZAM_SOFTFLOAT
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
      /// Incorporates format version and arch. Deterministic across builds
      /// (no __DATE__/__TIME__) so native and WASI compilers produce identical
      /// .pzam files. Bump PZAM_VERSION when the compiler changes.
      inline std::array<uint8_t, 32> compiler_identity(pzam_arch arch) {
         std::array<uint8_t, 32> result = {};
         uint64_t h = PZAM_VERSION;
         h ^= static_cast<uint64_t>(arch);
         h *= 0x100000001b3ULL;
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
         file.input_hash = hash_wasm(wasm_bytes);

         // Build a single code section
         pzam_code_section cs;
         cs.arch = static_cast<uint8_t>(target_arch);
         cs.instrumentation.softfloat = softfloat_enabled ? 1 : 0;
         cs.instrumentation.async_backtrace = backtrace_enabled ? 1 : 0;
         cs.stack_limit_mode = mod.stack_limit_is_bytes ? 1 : 0;
         cs.max_stack = static_cast<uint32_t>(mod.maximum_stack);
         cs.compiler.compiler_hash = compiler_identity(target_arch);

         // Build function table
         cs.functions.resize(mod.code.size());
         for (size_t i = 0; i < mod.code.size(); i++) {
            cs.functions[i].code_offset = static_cast<uint32_t>(mod.code[i].jit_code_offset);
            cs.functions[i].code_size = mod.code[i].jit_code_size;
            cs.functions[i].stack_size = mod.code[i].stack_size;
         }

         // Convert relocations
         const auto& reloc_entries = relocs.entries();
         cs.relocations.resize(reloc_entries.size());
         for (size_t i = 0; i < reloc_entries.size(); i++) {
            cs.relocations[i].code_offset = reloc_entries[i].code_offset;
            cs.relocations[i].symbol = static_cast<uint16_t>(reloc_entries[i].symbol);
            cs.relocations[i].type = static_cast<uint8_t>(reloc_entries[i].type);
            cs.relocations[i].addend = reloc_entries[i].addend;
         }

         // Copy code blob
         cs.code_blob.assign(
            static_cast<const uint8_t*>(code_base),
            static_cast<const uint8_t*>(code_base) + code_size);

         file.code_sections.push_back(std::move(cs));

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
#ifdef __EXCEPTIONS
         try {
            file = pzam_load(pzam_data);
         } catch (...) {
            return false;
         }
#else
         file = pzam_load(pzam_data);
#endif

         // Validate magic and version
         if (file.magic != PZAM_MAGIC) return false;

         // Find a code section matching current platform
         auto expected_arch =
#if defined(__x86_64__)
            pzam_arch::x86_64;
#elif defined(__aarch64__)
            pzam_arch::aarch64;
#else
            pzam_arch{};
#endif

         // Validate input hash
         auto expected_input = hash_wasm(wasm_bytes);
         if (file.input_hash != expected_input) return false;

         // Find matching code section
         const pzam_code_section* cs = nullptr;
         for (const auto& section : file.code_sections) {
            if (static_cast<pzam_arch>(section.arch) == expected_arch) {
               cs = &section;
               break;
            }
         }
         if (!cs) return false;

         // Validate compiler hash
         auto expected_compiler = compiler_identity(expected_arch);
         if (cs->compiler.compiler_hash != expected_compiler) return false;

         // Verify function count matches
         if (cs->functions.size() != mod.code.size()) return false;

         // Allocate executable memory and copy code
         void* code_dest = alloc.start_code();
         std::memcpy(code_dest, cs->code_blob.data(), cs->code_blob.size());

         // Convert relocations back to code_relocation and apply
         std::vector<code_relocation> relocs(cs->relocations.size());
         for (size_t i = 0; i < cs->relocations.size(); i++) {
            relocs[i].code_offset = cs->relocations[i].code_offset;
            relocs[i].symbol = static_cast<reloc_symbol>(cs->relocations[i].symbol);
            relocs[i].type = static_cast<reloc_type>(cs->relocations[i].type);
            relocs[i].addend = cs->relocations[i].addend;
         }

         apply_relocations(static_cast<char*>(code_dest),
                           relocs.data(),
                           static_cast<uint32_t>(relocs.size()),
                           symbol_table);

         // Update module function entries
         for (size_t i = 0; i < cs->functions.size(); i++) {
            mod.code[i].jit_code_offset = cs->functions[i].code_offset;
            mod.code[i].jit_code_size = cs->functions[i].code_size;
            mod.code[i].stack_size = cs->functions[i].stack_size;
         }

         mod.maximum_stack = cs->max_stack;
         mod.stack_limit_is_bytes = cs->stack_limit_mode != 0;

         return true;
      }

   } // namespace pzam_cache

} // namespace psizam
