#pragma once

// JIT relocation infrastructure for position-independent code caching (.pzam).
//
// When JIT code generators embed absolute C++ function addresses into native code
// (via emit_operand_ptr), the relocation system records the code offset and symbol.
// At load time, these recorded relocations are patched to the current process's
// function addresses, allowing cached code to be loaded at any base address.

#include <cstdint>
#include <cstring>
#include <vector>

namespace psizam::detail {

   /// Symbols that may be embedded as absolute addresses in JIT-generated code.
   /// Each symbol maps to a specific C++ function or data address that must be
   /// resolved at load time.
   enum class reloc_symbol : uint16_t {
      // Core runtime
      call_host_function,
      current_memory,
      grow_memory,

      // Bulk memory operations
      memory_fill,
      memory_copy,
      memory_init,
      data_drop,
      table_init,
      elem_drop,
      table_copy,

      // Error handlers
      on_unreachable,
      on_fp_error,
      on_memory_error,
      on_call_indirect_error,
      on_type_error,
      on_stack_overflow,

      // Softfloat arithmetic (f32)
      sf_f32_add, sf_f32_sub, sf_f32_mul, sf_f32_div,
      sf_f32_min, sf_f32_max, sf_f32_sqrt, sf_f32_ceil,
      sf_f32_floor, sf_f32_trunc, sf_f32_nearest, sf_f32_abs, sf_f32_neg,
      sf_f32_copysign,

      // Softfloat arithmetic (f64)
      sf_f64_add, sf_f64_sub, sf_f64_mul, sf_f64_div,
      sf_f64_min, sf_f64_max, sf_f64_sqrt, sf_f64_ceil,
      sf_f64_floor, sf_f64_trunc, sf_f64_nearest, sf_f64_abs, sf_f64_neg,
      sf_f64_copysign,

      // Softfloat conversions
      sf_f32_convert_i32s, sf_f32_convert_i32u,
      sf_f32_convert_i64s, sf_f32_convert_i64u,
      sf_f64_convert_i32s, sf_f64_convert_i32u,
      sf_f64_convert_i64s, sf_f64_convert_i64u,
      sf_f32_demote_f64, sf_f64_promote_f32,

      // Softfloat comparisons
      sf_f32_eq, sf_f32_ne, sf_f32_lt, sf_f32_gt, sf_f32_le, sf_f32_ge,
      sf_f64_eq, sf_f64_ne, sf_f64_lt, sf_f64_gt, sf_f64_le, sf_f64_ge,

      // Softfloat reinterpret
      sf_f32_reinterpret_i32, sf_f64_reinterpret_i64,
      sf_i32_reinterpret_f32, sf_i64_reinterpret_f64,

      // Trunc (trapping)
      trunc_f32_i32s, trunc_f32_i32u, trunc_f64_i32s, trunc_f64_i32u,
      trunc_f32_i64s, trunc_f32_i64u, trunc_f64_i64s, trunc_f64_i64u,

      // Trunc_sat (non-trapping)
      trunc_sat_f32_i32s, trunc_sat_f32_i32u, trunc_sat_f64_i32s, trunc_sat_f64_i32u,
      trunc_sat_f32_i64s, trunc_sat_f32_i64u, trunc_sat_f64_i64s, trunc_sat_f64_i64u,

      // SIMD helpers
      simd_popcnt4_table,

      // Standard library
      libc_memset,
      libc_memmove,

      // LLVM runtime helpers (extern "C" __psizam_* functions)
      llvm_global_get,
      llvm_global_set,
      llvm_global_get_v128,
      llvm_global_set_v128,
      llvm_memory_size,
      llvm_memory_grow,
      llvm_call_host,
      llvm_memory_init,
      llvm_data_drop,
      llvm_memory_copy,
      llvm_memory_fill,
      llvm_table_init,
      llvm_elem_drop,
      llvm_table_copy,
      llvm_call_indirect,
      llvm_table_get,
      llvm_table_set,
      llvm_table_grow,
      llvm_table_size,
      llvm_table_fill,
      llvm_resolve_indirect,
      llvm_atomic_rmw,
      llvm_call_depth_dec,
      llvm_call_depth_inc,
      llvm_trap,
      llvm_get_memory,

      // Self-reference: resolved to the code blob's own base address.
      // Used for internal data references (e.g., .rodata merged into the blob).
      // The addend encodes the offset within the blob.
      code_blob_self,

      // Generic/unknown (for addresses not yet categorized)
      unknown,

      NUM_SYMBOLS
   };

   /// Relocation types — how the address should be patched into code.
   enum class reloc_type : uint8_t {
      abs64 = 0,                    // 8-byte absolute address (native JIT, x86_64 LLVM large model)
      x86_64_pc32 = 1,             // 4-byte PC-relative (x86_64 call rel32)
      aarch64_call26 = 2,          // 26-bit PC-relative branch (aarch64 BL)
      aarch64_movw_uabs_g0_nc = 3, // MOVZ/MOVK lower 16 bits (bits 0-15)
      aarch64_movw_uabs_g1_nc = 4, // MOVK bits 16-31
      aarch64_movw_uabs_g2_nc = 5, // MOVK bits 32-47
      aarch64_movw_uabs_g3 = 6,    // MOVK bits 48-63
      aarch64_adr_prel_pg_hi21 = 7,// ADRP page-relative (bits 5-23, 29-30)
      aarch64_add_abs_lo12_nc = 8, // ADD immediate low 12 bits (bits 10-21)
      aarch64_ldst8_abs_lo12_nc = 9,  // LDR/STR 8-bit low 12 (bits 10-21, no scale)
      aarch64_ldst32_abs_lo12_nc = 10, // LDR/STR 32-bit low 12 (bits 10-21, scale 4)
      aarch64_ldst64_abs_lo12_nc = 11, // LDR/STR 64-bit low 12 (bits 10-21, scale 8)
   };

   /// A single relocation entry: records where an absolute address was embedded.
   struct code_relocation {
      uint32_t     code_offset;  // byte offset within the code blob
      reloc_symbol symbol;       // which function/data this points to
      reloc_type   type = reloc_type::abs64;
      uint8_t      reserved = 0;
      int32_t      addend = 0;   // relocation addend (for PC-relative)
   };
   static_assert(sizeof(code_relocation) == 12);

   /// Relocation recorder — attached to code generators during compilation.
   /// Records each absolute address embedding for later .pzam serialization.
   class relocation_recorder {
   public:
      void record(uint32_t code_offset, reloc_symbol sym,
                  reloc_type type = reloc_type::abs64, int32_t addend = 0) {
         _relocs.push_back({code_offset, sym, type, 0, addend});
      }

      const std::vector<code_relocation>& entries() const { return _relocs; }
      std::vector<code_relocation>& entries() { return _relocs; }
      uint32_t size() const { return static_cast<uint32_t>(_relocs.size()); }
      void clear() { _relocs.clear(); }

   private:
      std::vector<code_relocation> _relocs;
   };

   /// Apply relocations to a loaded code blob.
   /// symbol_table must have NUM_SYMBOLS entries (void* per symbol).
   inline void apply_relocations(char* code_base,
                                  const code_relocation* relocs,
                                  uint32_t num_relocs,
                                  void* const* symbol_table) {
      for (uint32_t i = 0; i < num_relocs; i++) {
         const auto& r = relocs[i];
         void* addr = symbol_table[static_cast<uint32_t>(r.symbol)];
         uint64_t target = reinterpret_cast<uint64_t>(addr);
         char* patch_site = code_base + r.code_offset;

         switch (r.type) {
            case reloc_type::abs64: {
               uint64_t val = target + r.addend;
               std::memcpy(patch_site, &val, 8);
               break;
            }

            case reloc_type::x86_64_pc32: {
               // PC-relative 32-bit: value = target - (patch_site + 4) + addend
               int64_t pc = reinterpret_cast<int64_t>(patch_site);
               int32_t val = static_cast<int32_t>(static_cast<int64_t>(target) - pc - 4 + r.addend);
               std::memcpy(patch_site, &val, 4);
               break;
            }

            case reloc_type::aarch64_call26: {
               // 26-bit PC-relative branch offset (BL/B instruction)
               int64_t pc = reinterpret_cast<int64_t>(patch_site);
               int64_t offset = (static_cast<int64_t>(target) - pc + r.addend) >> 2;
               uint32_t insn;
               std::memcpy(&insn, patch_site, 4);
               insn = (insn & 0xFC000000u) | (static_cast<uint32_t>(offset) & 0x03FFFFFFu);
               std::memcpy(patch_site, &insn, 4);
               break;
            }

            case reloc_type::aarch64_movw_uabs_g0_nc:
            case reloc_type::aarch64_movw_uabs_g1_nc:
            case reloc_type::aarch64_movw_uabs_g2_nc:
            case reloc_type::aarch64_movw_uabs_g3: {
               // Patch 16-bit immediate in MOVZ/MOVK instruction (bits 5-20)
               uint64_t resolved = target + r.addend;
               int shift = (static_cast<int>(r.type) - static_cast<int>(reloc_type::aarch64_movw_uabs_g0_nc)) * 16;
               uint16_t imm16 = static_cast<uint16_t>((resolved >> shift) & 0xFFFF);
               uint32_t insn;
               std::memcpy(&insn, patch_site, 4);
               insn = (insn & ~(0xFFFFu << 5)) | (static_cast<uint32_t>(imm16) << 5);
               std::memcpy(patch_site, &insn, 4);
               break;
            }

            case reloc_type::aarch64_adr_prel_pg_hi21: {
               // ADRP: Page(S+A) - Page(P), encoded in bits 5-23 (immlo) and 29-30 (immhi)
               uint64_t resolved = target + r.addend;
               int64_t page_delta = static_cast<int64_t>((resolved & ~0xFFFULL) -
                                    (reinterpret_cast<uint64_t>(patch_site) & ~0xFFFULL));
               int32_t imm21 = static_cast<int32_t>(page_delta >> 12);
               uint32_t insn;
               std::memcpy(&insn, patch_site, 4);
               uint32_t immlo = static_cast<uint32_t>(imm21) & 0x3u;
               uint32_t immhi = (static_cast<uint32_t>(imm21) >> 2) & 0x7FFFFu;
               insn = (insn & 0x9F00001Fu) | (immlo << 29) | (immhi << 5);
               std::memcpy(patch_site, &insn, 4);
               break;
            }

            case reloc_type::aarch64_add_abs_lo12_nc:
            case reloc_type::aarch64_ldst8_abs_lo12_nc: {
               // ADD/LDR8: low 12 bits of (S+A), in bits 10-21
               uint64_t resolved = target + r.addend;
               uint32_t imm12 = static_cast<uint32_t>(resolved) & 0xFFFu;
               uint32_t insn;
               std::memcpy(&insn, patch_site, 4);
               insn = (insn & ~(0xFFFu << 10)) | (imm12 << 10);
               std::memcpy(patch_site, &insn, 4);
               break;
            }

            case reloc_type::aarch64_ldst32_abs_lo12_nc: {
               // LDR/STR 32-bit: low 12 bits of (S+A) >> 2, in bits 10-21
               uint64_t resolved = target + r.addend;
               uint32_t imm12 = (static_cast<uint32_t>(resolved) & 0xFFFu) >> 2;
               uint32_t insn;
               std::memcpy(&insn, patch_site, 4);
               insn = (insn & ~(0xFFFu << 10)) | (imm12 << 10);
               std::memcpy(patch_site, &insn, 4);
               break;
            }

            case reloc_type::aarch64_ldst64_abs_lo12_nc: {
               // LDR/STR 64-bit: low 12 bits of (S+A) >> 3, in bits 10-21
               uint64_t resolved = target + r.addend;
               uint32_t imm12 = (static_cast<uint32_t>(resolved) & 0xFFFu) >> 3;
               uint32_t insn;
               std::memcpy(&insn, patch_site, 4);
               insn = (insn & ~(0xFFFu << 10)) | (imm12 << 10);
               std::memcpy(patch_site, &insn, 4);
               break;
            }
         }
      }
   }

} // namespace psizam::detail
