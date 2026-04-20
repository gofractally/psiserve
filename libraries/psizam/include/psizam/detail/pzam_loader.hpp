#pragma once

// psizam/detail/pzam_loader.hpp — Step 3 of psizam-runtime-api-maturation.
//
// Single source of truth for the .pzam load sequence:
//
//   1. Pick a code section matching the host architecture.
//   2. Restore the `module` from embedded metadata.
//   3. Build the per-arch + LLVM symbol table for relocations.
//   4. (aarch64) Generate veneers for out-of-range external calls.
//   5. Allocate executable memory through `jit_allocator`, copy code in,
//      apply relocations, flip RW → RX, flush icache.
//   6. Populate `module.code[*]` with code offsets / sizes / stack sizes.
//   7. Fix up element-segment `code_ptr` for JIT dispatch.
//   8. Derive trampoline-direction (`reverse_host_args`) once from
//      `cs->opt_tier` — the single source of truth that downstream
//      `_host_trampoline_ptrs` writers must consult.
//
// The host-side concerns (host_function_table.resolve, _host_trampoline_ptrs
// population, jit_func_ranges, page-size conversion) are NOT done here —
// they belong to instantiate-time and live in runtime.cpp's
// `runtime::instantiate` for the load_cached-path.
//
// Today this helper is consumed by `runtime::load_cached`. The duplicated
// loaders in `tools/pzam_run.cpp` and `pzam_typed.hpp::pzam_instance` will
// migrate onto this entry point in follow-up work
// (`psizam-unify-host-registration-under-runtime` Step 3 ports pzam-run).

#include <psizam/allocator.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/pzam_metadata.hpp>
#include <psizam/types.hpp>
#include <psizam/detail/jit_reloc.hpp>

#if defined(__aarch64__)
#include <psizam/detail/jit_codegen_a64.hpp>
#else
#include <psizam/detail/x86_64.hpp>
#endif

#include <sys/mman.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

namespace psizam::detail {

// Output of the loader. Owns the executable memory; caller must free
// `exec_code` via `jit_allocator::instance().free(exec_code)` when done.
// `mod` is movable; the caller is expected to take it by value.
struct pzam_load_result {
   module   mod;                       // restored module with code mapped
   void*    exec_code            = nullptr;
   std::size_t code_alloc_size   = 0;
   std::size_t total_code_size   = 0;
   bool     is_jit_tier          = false;  // jit/jit2 vs llvm
   bool     reverse_host_args    = true;   // single source of truth
   uint32_t compile_page_size    = 0;
   uint32_t max_stack            = 0;
   bool     stack_limit_is_bytes = false;

   pzam_load_result() = default;
   pzam_load_result(pzam_load_result&&) noexcept = default;
   pzam_load_result& operator=(pzam_load_result&&) noexcept = default;

   pzam_load_result(const pzam_load_result&)            = delete;
   pzam_load_result& operator=(const pzam_load_result&) = delete;
};

// Step (1) — pick a code section for the current architecture, or throw.
inline const pzam_code_section& pick_code_section(const pzam_file& pzam) {
   constexpr pzam_arch expected_arch =
#if defined(__x86_64__)
      pzam_arch::x86_64;
#elif defined(__aarch64__)
      pzam_arch::aarch64;
#else
      pzam_arch{};
#endif
   for (const auto& s : pzam.code_sections) {
      if (static_cast<pzam_arch>(s.arch) == expected_arch) return s;
   }
   throw std::runtime_error{
      "pzam_loader: no code section for current architecture"};
}

// Step (3) — initialize the symbol table for the current architecture.
inline void init_symbol_table(void** symbol_table) {
#if defined(__aarch64__)
   std::memset(symbol_table,
               0,
               sizeof(void*) * static_cast<std::size_t>(reloc_symbol::NUM_SYMBOLS));
   using jit_cg = jit_codegen_a64;
   symbol_table[static_cast<uint32_t>(reloc_symbol::call_host_function)]     = reinterpret_cast<void*>(&jit_cg::call_host_function);
   symbol_table[static_cast<uint32_t>(reloc_symbol::current_memory)]         = reinterpret_cast<void*>(&jit_cg::current_memory);
   symbol_table[static_cast<uint32_t>(reloc_symbol::grow_memory)]            = reinterpret_cast<void*>(&jit_cg::grow_memory);
   symbol_table[static_cast<uint32_t>(reloc_symbol::memory_fill)]            = reinterpret_cast<void*>(&jit_cg::memory_fill_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::memory_copy)]            = reinterpret_cast<void*>(&jit_cg::memory_copy_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::memory_init)]            = reinterpret_cast<void*>(&jit_cg::memory_init_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::data_drop)]              = reinterpret_cast<void*>(&jit_cg::data_drop_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::table_init)]             = reinterpret_cast<void*>(&jit_cg::table_init_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::elem_drop)]              = reinterpret_cast<void*>(&jit_cg::elem_drop_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::table_copy)]             = reinterpret_cast<void*>(&jit_cg::table_copy_impl);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_unreachable)]         = reinterpret_cast<void*>(&jit_cg::on_unreachable);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_fp_error)]            = reinterpret_cast<void*>(&jit_cg::on_fp_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_memory_error)]        = reinterpret_cast<void*>(&jit_cg::on_memory_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_call_indirect_error)] = reinterpret_cast<void*>(&jit_cg::on_call_indirect_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_type_error)]          = reinterpret_cast<void*>(&jit_cg::on_type_error);
   symbol_table[static_cast<uint32_t>(reloc_symbol::on_stack_overflow)]      = reinterpret_cast<void*>(&jit_cg::on_stack_overflow);
#else
   build_symbol_table<jit_codegen>(symbol_table);
#endif
   build_llvm_symbol_table(symbol_table);
}

// Step (8) — derive the host-call trampoline direction from `opt_tier`.
//
// jit / jit2 push WASM stack in-place so args[0] = LAST WASM param,
// requiring the reverse-order trampoline. LLVM emits a normal C-ABI call
// with args[0] = FIRST WASM param, requiring the forward trampoline.
//
// This is the SINGLE SOURCE OF TRUTH for the trampoline direction in any
// pzam-loaded module. Downstream code that writes
// `_host_trampoline_ptrs` MUST consult this flag rather than re-deriving
// it from `opt_tier` (the bug fixed by commit 960c425 was caused by
// inconsistent re-derivation at one of two write sites).
inline bool derive_reverse_host_args(uint8_t opt_tier) noexcept {
   const bool is_llvm = opt_tier == static_cast<uint8_t>(pzam_opt_tier::llvm_O1) ||
                        opt_tier == static_cast<uint8_t>(pzam_opt_tier::llvm_O2) ||
                        opt_tier == static_cast<uint8_t>(pzam_opt_tier::llvm_O3);
   return !is_llvm;
}

// Run the full loader sequence. Returns a `pzam_load_result` whose
// `exec_code` the caller takes ownership of (free via
// `jit_allocator::instance().free(...)`).
inline pzam_load_result load_pzam(const pzam_file& pzam)
{
   const pzam_code_section& cs = pick_code_section(pzam);

   pzam_load_result out;

   // (2) Restore the module from embedded metadata.
   out.mod = restore_module(pzam.metadata);
   out.mod.allocator.use_default_memory();

   if (cs.functions.size() != out.mod.code.size())
      throw std::runtime_error{
         "pzam_loader: code section function count mismatches metadata"};

   // (3) Symbol table for relocations.
   void* symbol_table[static_cast<std::size_t>(reloc_symbol::NUM_SYMBOLS)];
   init_symbol_table(symbol_table);

   // Build runtime relocations from the .pzam descriptor.
   std::vector<code_relocation> relocs(cs.relocations.size());
   for (std::size_t j = 0; j < cs.relocations.size(); ++j) {
      relocs[j].code_offset = cs.relocations[j].code_offset;
      relocs[j].symbol      = static_cast<reloc_symbol>(cs.relocations[j].symbol);
      relocs[j].type        = static_cast<reloc_type>(cs.relocations[j].type);
      relocs[j].addend      = cs.relocations[j].addend;
   }

   // (4 + 5) Veneers (aarch64) + executable memory allocation.
   std::size_t total_code_size = cs.code_blob.size();

#if defined(__aarch64__)
   std::unordered_map<uint16_t, uint32_t> veneer_offsets;
   const std::size_t veneer_start = (total_code_size + 3u) & ~std::size_t{3u};
   for (auto& r : relocs) {
      if (r.type == reloc_type::aarch64_call26 &&
          r.symbol != reloc_symbol::code_blob_self) {
         const auto sym_idx = static_cast<uint16_t>(r.symbol);
         if (veneer_offsets.find(sym_idx) == veneer_offsets.end()) {
            veneer_offsets[sym_idx] = static_cast<uint32_t>(
               veneer_start + veneer_offsets.size() * 20);
         }
      }
   }
   total_code_size = veneer_start + veneer_offsets.size() * 20;
#endif

   const std::size_t page_size      = static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
   const std::size_t code_alloc_size = (total_code_size + page_size - 1) &
                                       ~(page_size - 1);

   void* exec_code = jit_allocator::instance().alloc(code_alloc_size);
   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_WRITE) != 0) {
      jit_allocator::instance().free(exec_code);
      throw std::runtime_error{"pzam_loader: mprotect RW failed"};
   }
   std::memcpy(exec_code, cs.code_blob.data(), cs.code_blob.size());

#if defined(__aarch64__)
   // Materialize veneers: 4× MOV-imm-shifted + BR x16 = 5 insns × 4 bytes.
   for (auto& [sym_idx, veneer_off] : veneer_offsets) {
      const uint64_t target = reinterpret_cast<uint64_t>(symbol_table[sym_idx]);
      uint32_t* v = reinterpret_cast<uint32_t*>(static_cast<char*>(exec_code) + veneer_off);
      v[0] = 0xD2800010u | ((static_cast<uint32_t>(target >>  0) & 0xFFFFu) << 5);
      v[1] = 0xF2A00010u | ((static_cast<uint32_t>(target >> 16) & 0xFFFFu) << 5);
      v[2] = 0xF2C00010u | ((static_cast<uint32_t>(target >> 32) & 0xFFFFu) << 5);
      v[3] = 0xF2E00010u | ((static_cast<uint32_t>(target >> 48) & 0xFFFFu) << 5);
      v[4] = 0xD61F0200u;
   }
   for (auto& r : relocs) {
      if (r.type == reloc_type::aarch64_call26 &&
          r.symbol != reloc_symbol::code_blob_self) {
         const auto it = veneer_offsets.find(static_cast<uint16_t>(r.symbol));
         if (it != veneer_offsets.end()) {
            r.symbol = reloc_symbol::code_blob_self;
            r.addend = static_cast<int32_t>(it->second);
         }
      }
   }
#endif

   symbol_table[static_cast<uint32_t>(reloc_symbol::code_blob_self)] = exec_code;

   apply_relocations(static_cast<char*>(exec_code),
                     relocs.data(),
                     static_cast<uint32_t>(relocs.size()),
                     symbol_table);

   if (mprotect(exec_code, code_alloc_size, PROT_READ | PROT_EXEC) != 0) {
      jit_allocator::instance().free(exec_code);
      throw std::runtime_error{"pzam_loader: mprotect RX failed"};
   }
#if defined(__aarch64__)
   __builtin___clear_cache(static_cast<char*>(exec_code),
                           static_cast<char*>(exec_code) + total_code_size);
#endif

   // (6) Populate per-function code metadata.
   const bool is_jit = static_cast<pzam_opt_tier>(cs.opt_tier) == pzam_opt_tier::jit1 ||
                       static_cast<pzam_opt_tier>(cs.opt_tier) == pzam_opt_tier::jit2;
   if (is_jit) {
      out.mod.allocator._code_base = static_cast<char*>(exec_code);
      out.mod.allocator._code_size = total_code_size;
      for (std::size_t j = 0; j < cs.functions.size(); ++j) {
         out.mod.code[j].jit_code_offset = cs.functions[j].code_offset;
         out.mod.code[j].jit_code_size   = cs.functions[j].code_size;
         out.mod.code[j].stack_size      = cs.functions[j].stack_size;
      }
   } else {
      // LLVM tier — `_code_base` doubles as the LLVM-dispatch flag in
      // execution_context, so it must stay null. The signal handler still
      // needs the code range via `_exec_code_base` for fault classification.
      out.mod.allocator._exec_code_base = static_cast<char*>(exec_code);
      out.mod.allocator._exec_code_size = total_code_size;
      const auto code_base_addr = reinterpret_cast<uintptr_t>(exec_code);
      for (std::size_t j = 0; j < cs.functions.size(); ++j) {
         out.mod.code[j].jit_code_offset = code_base_addr + cs.functions[j].code_offset;
         out.mod.code[j].jit_code_size   = cs.functions[j].code_size;
         out.mod.code[j].stack_size      = cs.functions[j].stack_size;
      }
   }
   out.mod.maximum_stack        = cs.max_stack;
   out.mod.stack_limit_is_bytes = cs.stack_limit_mode != 0;

   // (7) Fix up element-segment code_ptr for JIT dispatch.
   if (is_jit) {
      const uint32_t num_imports = out.mod.get_imported_functions_size();
      for (auto& elem_seg : out.mod.elements) {
         for (auto& entry : elem_seg.elems) {
            if (entry.index < num_imports + cs.functions.size()) {
               const uint32_t code_idx = entry.index - num_imports;
               if (entry.index >= num_imports && code_idx < cs.functions.size()) {
                  entry.code_ptr = out.mod.allocator._code_base +
                                   cs.functions[code_idx].code_offset;
               }
            }
         }
      }
   }

   // (8) Derive trampoline direction once.
   out.exec_code            = exec_code;
   out.code_alloc_size      = code_alloc_size;
   out.total_code_size      = total_code_size;
   out.is_jit_tier          = is_jit;
   out.reverse_host_args    = derive_reverse_host_args(cs.opt_tier);
   out.compile_page_size    = cs.page_size;
   out.max_stack            = cs.max_stack;
   out.stack_limit_is_bytes = cs.stack_limit_mode != 0;

   return out;
}

} // namespace psizam::detail
