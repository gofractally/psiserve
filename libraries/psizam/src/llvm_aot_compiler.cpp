// LLVM AOT Compiler implementation
//
// Compiles LLVM IR to a relocatable object file using LLVM's TargetMachine,
// then parses the object to extract the .text section and relocations.
// The result is packaged for .pzam serialization.

#include <psizam/detail/llvm_aot_compiler.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/TargetSelect.h>

// Explicit target initialization (avoid linking all targets).
// Guarded by PSIZAM_LLVM_TARGET_* defines for single-target builds.
extern "C" {
#if !defined(PSIZAM_LLVM_TARGET_X86) && !defined(PSIZAM_LLVM_TARGET_AARCH64)
   // Default: both targets
   #define PSIZAM_LLVM_TARGET_X86
   #define PSIZAM_LLVM_TARGET_AARCH64
#endif
#ifdef PSIZAM_LLVM_TARGET_X86
   void LLVMInitializeX86TargetInfo();
   void LLVMInitializeX86Target();
   void LLVMInitializeX86TargetMC();
   void LLVMInitializeX86AsmPrinter();
#endif
#ifdef PSIZAM_LLVM_TARGET_AARCH64
   void LLVMInitializeAArch64TargetInfo();
   void LLVMInitializeAArch64Target();
   void LLVMInitializeAArch64TargetMC();
   void LLVMInitializeAArch64AsmPrinter();
#endif
}
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>

#include <iostream>
#include <optional>
#include <unordered_map>

namespace psizam::detail {

   // Initialize X86 and AArch64 targets for cross-compilation.
   // Only these two are linked, so we can't use InitializeAllTargets().
   static bool ensure_llvm_targets() {
      static bool done = [] {
#ifdef PSIZAM_LLVM_TARGET_X86
         LLVMInitializeX86TargetInfo();
         LLVMInitializeX86Target();
         LLVMInitializeX86TargetMC();
         LLVMInitializeX86AsmPrinter();
#endif
#ifdef PSIZAM_LLVM_TARGET_AARCH64
         LLVMInitializeAArch64TargetInfo();
         LLVMInitializeAArch64Target();
         LLVMInitializeAArch64TargetMC();
         LLVMInitializeAArch64AsmPrinter();
#endif
         return true;
      }();
      return done;
   }

   // Map LLVM symbol name to reloc_symbol
   static reloc_symbol map_llvm_symbol(llvm::StringRef name) {
      static const std::unordered_map<std::string, reloc_symbol> sym_map = {
         {"__psizam_global_get",       reloc_symbol::llvm_global_get},
         {"__psizam_global_set",       reloc_symbol::llvm_global_set},
         {"__psizam_global_get_v128",  reloc_symbol::llvm_global_get_v128},
         {"__psizam_global_set_v128",  reloc_symbol::llvm_global_set_v128},
         {"__psizam_memory_size",      reloc_symbol::llvm_memory_size},
         {"__psizam_memory_grow",      reloc_symbol::llvm_memory_grow},
         {"__psizam_call_host",        reloc_symbol::llvm_call_host},
         {"__psizam_memory_init",      reloc_symbol::llvm_memory_init},
         {"__psizam_data_drop",        reloc_symbol::llvm_data_drop},
         {"__psizam_memory_copy",      reloc_symbol::llvm_memory_copy},
         {"__psizam_memory_fill",      reloc_symbol::llvm_memory_fill},
         {"__psizam_table_init",       reloc_symbol::llvm_table_init},
         {"__psizam_elem_drop",        reloc_symbol::llvm_elem_drop},
         {"__psizam_table_copy",       reloc_symbol::llvm_table_copy},
         {"__psizam_call_indirect",    reloc_symbol::llvm_call_indirect},
         {"__psizam_table_get",        reloc_symbol::llvm_table_get},
         {"__psizam_table_set",        reloc_symbol::llvm_table_set},
         {"__psizam_table_grow",       reloc_symbol::llvm_table_grow},
         {"__psizam_table_size",       reloc_symbol::llvm_table_size},
         {"__psizam_table_fill",       reloc_symbol::llvm_table_fill},
         {"__psizam_resolve_indirect", reloc_symbol::llvm_resolve_indirect},
         {"__psizam_atomic_rmw",       reloc_symbol::llvm_atomic_rmw},
         {"__psizam_call_depth_dec",   reloc_symbol::llvm_call_depth_dec},
         {"__psizam_call_depth_inc",   reloc_symbol::llvm_call_depth_inc},
         {"__psizam_trap",             reloc_symbol::llvm_trap},
         {"__psizam_get_memory",       reloc_symbol::llvm_get_memory},
         // Softfloat function symbols
         {"__psizam_sf_f32_add",       reloc_symbol::sf_f32_add},
         {"__psizam_sf_f32_sub",       reloc_symbol::sf_f32_sub},
         {"__psizam_sf_f32_mul",       reloc_symbol::sf_f32_mul},
         {"__psizam_sf_f32_div",       reloc_symbol::sf_f32_div},
         {"__psizam_sf_f32_min",       reloc_symbol::sf_f32_min},
         {"__psizam_sf_f32_max",       reloc_symbol::sf_f32_max},
         {"__psizam_sf_f32_copysign",  reloc_symbol::sf_f32_copysign},
         {"__psizam_sf_f32_abs",       reloc_symbol::sf_f32_abs},
         {"__psizam_sf_f32_neg",       reloc_symbol::sf_f32_neg},
         {"__psizam_sf_f32_sqrt",      reloc_symbol::sf_f32_sqrt},
         {"__psizam_sf_f32_ceil",      reloc_symbol::sf_f32_ceil},
         {"__psizam_sf_f32_floor",     reloc_symbol::sf_f32_floor},
         {"__psizam_sf_f32_trunc",     reloc_symbol::sf_f32_trunc},
         {"__psizam_sf_f32_nearest",   reloc_symbol::sf_f32_nearest},
         {"__psizam_sf_f64_add",       reloc_symbol::sf_f64_add},
         {"__psizam_sf_f64_sub",       reloc_symbol::sf_f64_sub},
         {"__psizam_sf_f64_mul",       reloc_symbol::sf_f64_mul},
         {"__psizam_sf_f64_div",       reloc_symbol::sf_f64_div},
         {"__psizam_sf_f64_min",       reloc_symbol::sf_f64_min},
         {"__psizam_sf_f64_max",       reloc_symbol::sf_f64_max},
         {"__psizam_sf_f64_copysign",  reloc_symbol::sf_f64_copysign},
         {"__psizam_sf_f64_abs",       reloc_symbol::sf_f64_abs},
         {"__psizam_sf_f64_neg",       reloc_symbol::sf_f64_neg},
         {"__psizam_sf_f64_sqrt",      reloc_symbol::sf_f64_sqrt},
         {"__psizam_sf_f64_ceil",      reloc_symbol::sf_f64_ceil},
         {"__psizam_sf_f64_floor",     reloc_symbol::sf_f64_floor},
         {"__psizam_sf_f64_trunc",     reloc_symbol::sf_f64_trunc},
         {"__psizam_sf_f64_nearest",   reloc_symbol::sf_f64_nearest},
         {"__psizam_sf_f32_convert_i32s", reloc_symbol::sf_f32_convert_i32s},
         {"__psizam_sf_f32_convert_i32u", reloc_symbol::sf_f32_convert_i32u},
         {"__psizam_sf_f32_convert_i64s", reloc_symbol::sf_f32_convert_i64s},
         {"__psizam_sf_f32_convert_i64u", reloc_symbol::sf_f32_convert_i64u},
         {"__psizam_sf_f64_convert_i32s", reloc_symbol::sf_f64_convert_i32s},
         {"__psizam_sf_f64_convert_i32u", reloc_symbol::sf_f64_convert_i32u},
         {"__psizam_sf_f64_convert_i64s", reloc_symbol::sf_f64_convert_i64s},
         {"__psizam_sf_f64_convert_i64u", reloc_symbol::sf_f64_convert_i64u},
         {"__psizam_sf_f32_demote_f64",  reloc_symbol::sf_f32_demote_f64},
         {"__psizam_sf_f64_promote_f32", reloc_symbol::sf_f64_promote_f32},
      };
      auto it = sym_map.find(name.str());
      if (it != sym_map.end()) return it->second;
      return reloc_symbol::unknown;
   }

   // Map ELF relocation type to pzam reloc_type
   static reloc_type map_elf_reloc_type(uint64_t elf_type, bool is_x86_64) {
      if (is_x86_64) {
         // ELF x86_64 relocation types (from elf.h)
         switch (elf_type) {
            case 1:  return reloc_type::abs64;           // R_X86_64_64
            case 2:  return reloc_type::x86_64_pc32;     // R_X86_64_PC32
            case 4:  return reloc_type::x86_64_pc32;     // R_X86_64_PLT32 (same encoding)
            default:
               std::cerr << "Fatal: Unsupported x86_64 ELF relocation type: " << elf_type << "\n";
               std::abort();
         }
      } else {
         // ELF aarch64 relocation types (from AArch64.def)
         switch (elf_type) {
            case 257: return reloc_type::abs64;                     // R_AARCH64_ABS64
            case 282: return reloc_type::aarch64_call26;            // R_AARCH64_JUMP26
            case 283: return reloc_type::aarch64_call26;            // R_AARCH64_CALL26
            case 263:                                                // R_AARCH64_MOVW_UABS_G0
            case 264: return reloc_type::aarch64_movw_uabs_g0_nc;  // R_AARCH64_MOVW_UABS_G0_NC
            case 265:                                                // R_AARCH64_MOVW_UABS_G1
            case 266: return reloc_type::aarch64_movw_uabs_g1_nc;  // R_AARCH64_MOVW_UABS_G1_NC
            case 267:                                                // R_AARCH64_MOVW_UABS_G2
            case 268: return reloc_type::aarch64_movw_uabs_g2_nc;  // R_AARCH64_MOVW_UABS_G2_NC
            case 269: return reloc_type::aarch64_movw_uabs_g3;     // R_AARCH64_MOVW_UABS_G3
            case 260: return reloc_type::aarch64_adr_prel_pg_hi21; // R_AARCH64_ADR_PREL_PG_HI21
            case 261: return reloc_type::aarch64_adr_prel_pg_hi21; // R_AARCH64_ADR_PREL_PG_HI21_NC
            case 275: return reloc_type::aarch64_add_abs_lo12_nc;  // R_AARCH64_ADD_ABS_LO12_NC
            case 278: return reloc_type::aarch64_ldst8_abs_lo12_nc;  // R_AARCH64_LDST8_ABS_LO12_NC
            case 284: return reloc_type::aarch64_ldst32_abs_lo12_nc; // R_AARCH64_LDST32_ABS_LO12_NC
            case 286: return reloc_type::aarch64_ldst64_abs_lo12_nc; // R_AARCH64_LDST64_ABS_LO12_NC
            default:
               std::cerr << "Fatal: Unsupported aarch64 ELF relocation type: " << elf_type << "\n";
               std::abort();
         }
      }
   }

   llvm_aot_result llvm_aot_compile(
         std::unique_ptr<llvm::Module> llvm_mod,
         std::unique_ptr<llvm::LLVMContext> llvm_ctx,
         const module& mod,
         const std::string& target_triple) {

      ensure_llvm_targets();

      llvm::Triple triple(target_triple);
      bool is_x86_64 = triple.getArch() == llvm::Triple::x86_64;

      // Look up the target
      std::string error;
#if LLVM_VERSION_MAJOR >= 22
      auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
#else
      auto* target = llvm::TargetRegistry::lookupTarget(triple.str(), error);
#endif
      if (!target) {
         return llvm_aot_result{.error = "LLVM target lookup failed: " + error};
      }

      // Create target machine with large code model for absolute addressing
      llvm::TargetOptions opts;
      auto rm = llvm::Reloc::Static;
      // Use large code model so all external references use abs64 on x86_64
      // or MOVZ+MOVK sequences on aarch64 (Small would use ADRP which is harder to relocate)
      auto cm = llvm::CodeModel::Large;
      // x86-64-v2 includes SSE4.1/4.2 — avoids libc calls for ceil/floor/etc.
      std::string cpu = is_x86_64 ? "x86-64-v2" : "";
      // On aarch64, reserve x18 — macOS uses it as a platform register.
      // We keep the linux-gnu triple for ELF output but must not clobber x18.
      std::string features = is_x86_64 ? "" : "+reserve-x18";
      auto tm = std::unique_ptr<llvm::TargetMachine>(
#if LLVM_VERSION_MAJOR >= 22
         target->createTargetMachine(triple, cpu, features, opts, rm, cm,
                                     llvm::CodeGenOptLevel::Default));
#else
         target->createTargetMachine(triple.str(), cpu, features, opts, rm, cm,
                                     llvm::CodeGenOptLevel::Default));
#endif
      if (!tm) {
         return llvm_aot_result{.error = "Failed to create LLVM TargetMachine"};
      }

      llvm_mod->setDataLayout(tm->createDataLayout());
#if LLVM_VERSION_MAJOR >= 22
      llvm_mod->setTargetTriple(triple);
#else
      llvm_mod->setTargetTriple(target_triple);
#endif

      // Emit object code to memory buffer
      llvm::SmallVector<char, 0> obj_buf;
      llvm::raw_svector_ostream obj_stream(obj_buf);
      llvm::legacy::PassManager pm;

      if (tm->addPassesToEmitFile(pm, obj_stream, nullptr,
                                   llvm::CodeGenFileType::ObjectFile)) {
         return llvm_aot_result{.error = "TargetMachine cannot emit object file"};
      }
      pm.run(*llvm_mod);

      // Parse the object file
      auto buf = std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(obj_buf), false);
      auto obj_or_err = llvm::object::ObjectFile::createObjectFile(*buf);
      if (!obj_or_err) {
         std::string msg;
         llvm::raw_string_ostream os(msg);
         os << obj_or_err.takeError();
         return llvm_aot_result{.error = "Failed to parse emitted object: " + msg};
      }
      auto& obj = **obj_or_err;

      // Find the .text section and merge any .rodata sections into the code blob.
      // LLVM may place jump tables and constant pools in .rodata, which must be
      // accessible from the code at runtime. We append .rodata after .text with
      // alignment, and emit code_blob_self relocations for cross-section references.
      llvm_aot_result result;
      std::optional<llvm::object::SectionRef> text_section;
      uint64_t rodata_blob_offset = 0;  // offset of .rodata within the merged code blob

      // Track section index → blob offset for resolving internal relocations
      std::unordered_map<uint64_t, uint64_t> section_blob_offsets;

      for (const auto& section : obj.sections()) {
         auto name_or_err = section.getName();
         if (!name_or_err) continue;
         // Large code model on x86_64 puts code in .ltext instead of .text
         if (*name_or_err == ".text" || *name_or_err == ".ltext") {
            auto contents_or_err = section.getContents();
            if (!contents_or_err) {
               return llvm_aot_result{.error = "Failed to read code section"};
            }
            if (!contents_or_err->empty()) {
               result.code.assign(contents_or_err->begin(), contents_or_err->end());
               text_section = section;
               section_blob_offsets[section.getIndex()] = 0;
               break;
            }
         }
      }

      // Append .rodata sections to the code blob
      for (const auto& section : obj.sections()) {
         auto name_or_err = section.getName();
         if (!name_or_err) continue;
         auto name = *name_or_err;
         if (name == ".rodata" || name.starts_with(".rodata.")) {
            auto contents_or_err = section.getContents();
            if (!contents_or_err || contents_or_err->empty()) continue;

            // Align to 16 bytes for data sections
            size_t align_pad = (16 - (result.code.size() % 16)) % 16;
            result.code.resize(result.code.size() + align_pad, 0);
            uint64_t offset = result.code.size();
            result.code.insert(result.code.end(), contents_or_err->begin(), contents_or_err->end());
            section_blob_offsets[section.getIndex()] = offset;
            if (name == ".rodata")
               rodata_blob_offset = offset;
         }
      }

      if (!text_section || result.code.empty()) {
         return llvm_aot_result{.error = "No .text section found in emitted object"};
      }

      // Build symbol name → index map from the object's symbol table
      std::unordered_map<uint64_t, std::string> sym_index_to_name;
      for (const auto& sym : obj.symbols()) {
         auto name_or_err = sym.getName();
         if (!name_or_err) continue;
         // Use the symbol's raw index
         sym_index_to_name[sym.getRawDataRefImpl().p] = name_or_err->str();
      }

      // Extract relocations from .text section
      for (const auto& section : obj.sections()) {
         auto relocated_or_err = section.getRelocatedSection();
         if (!relocated_or_err || *relocated_or_err != *text_section) {
            // Also check if this section IS the text section (some formats)
            auto name_or_err = section.getName();
            if (name_or_err && (*name_or_err == ".rela.text" || *name_or_err == ".rel.text" ||
                                *name_or_err == ".rela.ltext" || *name_or_err == ".rel.ltext")) {
               // This is a relocation section for .text
            } else {
               continue;
            }
         }

         for (const auto& reloc : section.relocations()) {
            auto sym_it = reloc.getSymbol();
            if (sym_it == obj.symbol_end()) continue;

            auto sym_name_or_err = sym_it->getName();
            if (!sym_name_or_err) continue;
            std::string sym_name = sym_name_or_err->str();

            auto psizam_sym = map_llvm_symbol(sym_name);

            // Handle unknown symbols — either internal references or data section refs
            if (psizam_sym == reloc_symbol::unknown) {
               // Check which section the symbol belongs to
               auto sec_or_err = sym_it->getSection();
               bool is_internal_code = false;
               bool is_data_section = false;
               uint64_t data_section_blob_offset = 0;

               if (sec_or_err && *sec_or_err != obj.section_end()) {
                  auto sn = (*sec_or_err)->getName();
                  if (sn) {
                     // Check if this symbol is in a data section we merged into the blob
                     auto blob_it = section_blob_offsets.find((*sec_or_err)->getIndex());
                     if (blob_it != section_blob_offsets.end()) {
                        auto text_name = text_section ? text_section->getName() : llvm::Expected<llvm::StringRef>(llvm::StringRef(""));
                        if (*sec_or_err == *text_section) {
                           is_internal_code = true; // reference within .text
                        } else {
                           is_data_section = true;
                           data_section_blob_offset = blob_it->second;
                        }
                     }
                  }
               }

               if (is_data_section) {
                  // Cross-section reference to merged data (e.g., .rodata jump tables).
                  // Emit a code_blob_self relocation so the loader resolves it to
                  // code_base + data_offset + symbol_value_within_section + addend.
                  auto val_or_err = llvm::object::ELFRelocationRef(reloc).getAddend();
                  int32_t elf_addend = val_or_err ? static_cast<int32_t>(*val_or_err) : 0;
                  auto sym_val_or_err = sym_it->getValue();
                  uint64_t sym_val = sym_val_or_err ? *sym_val_or_err : 0;

                  code_relocation cr;
                  cr.code_offset = static_cast<uint32_t>(reloc.getOffset());
                  cr.symbol = reloc_symbol::code_blob_self;
                  cr.type = map_elf_reloc_type(reloc.getType(), is_x86_64);
                  cr.addend = static_cast<int32_t>(data_section_blob_offset + sym_val) + elf_addend;
                  result.relocations.push_back(cr);
                  continue;
               }

               // WASM function references (per-function compilation).
               // Must check before is_internal_code: with ExternalLinkage, LLVM
               // emits relocations for calls between symbols in the same .text
               // section (e.g., wasm_entry_N → wasm_func_N within one compilation
               // unit). These are NOT pre-resolved by the assembler.
               if (sym_name.starts_with("wasm_entry_") || sym_name.starts_with("wasm_func_")) {
                  bool is_entry = sym_name.starts_with("wasm_entry_");
                  std::string idx_str = is_entry ? sym_name.substr(11) : sym_name.substr(10);
                  uint32_t func_idx = std::stoul(idx_str);
                  uint32_t num_imports_val = mod.get_imported_functions_size();
                  if (func_idx >= num_imports_val) {
                     code_relocation cr;
                     cr.code_offset = static_cast<uint32_t>(reloc.getOffset());
                     cr.symbol = reloc_symbol::code_blob_self;
                     cr.type = map_elf_reloc_type(reloc.getType(), is_x86_64);
                     uint32_t code_idx = func_idx - num_imports_val;
                     // Negative addend = pending function ref
                     // Body refs:  -(code_idx + 1)             [range -1..-N]
                     // Entry refs: -(code_idx + 1 + code_count) [range -(N+1)..-(2N)]
                     int32_t encoded = -static_cast<int32_t>(code_idx + 1);
                     if (is_entry) {
                        encoded -= static_cast<int32_t>(mod.code.size());
                     }
                     cr.addend = encoded;
                     result.relocations.push_back(cr);
                  }
                  continue;
               }

               // Internal code reference (local labels within .text)
               // The assembler already resolves intra-section relocations in .text,
               // so the instructions have correct relative offsets. Skip them.
               if (is_internal_code ||
                   sym_name.starts_with(".L") || sym_name.starts_with(".")) {
                  continue;
               }

               // Map libc/compiler-rt symbols to known reloc_symbols
               reloc_symbol mapped = reloc_symbol::unknown;
               if (sym_name == "memset")
                  mapped = reloc_symbol::libc_memset;
               else if (sym_name == "memmove" || sym_name == "memcpy")
                  mapped = reloc_symbol::libc_memmove;

               if (mapped != reloc_symbol::unknown) {
                  code_relocation cr;
                  cr.code_offset = static_cast<uint32_t>(reloc.getOffset());
                  cr.symbol = mapped;
                  cr.type = map_elf_reloc_type(reloc.getType(), is_x86_64);
                  auto val_or_err = llvm::object::ELFRelocationRef(reloc).getAddend();
                  cr.addend = val_or_err ? static_cast<int32_t>(*val_or_err) : 0;
                  result.relocations.push_back(cr);
                  continue;
               }

               return llvm_aot_result{.error = "Unknown external symbol in LLVM object: " + sym_name};
            }

            code_relocation cr;
            cr.code_offset = static_cast<uint32_t>(reloc.getOffset());
            cr.symbol = psizam_sym;
            cr.type = map_elf_reloc_type(reloc.getType(), is_x86_64);

            // Get addend (RELA-style; for REL, addend is in the instruction)
            auto val_or_err = llvm::object::ELFRelocationRef(reloc).getAddend();
            cr.addend = val_or_err ? static_cast<int32_t>(*val_or_err) : 0;

            result.relocations.push_back(cr);
         }
      }

      // Find function offsets by looking up wasm_entry_* and wasm_func_* symbols
      uint32_t num_imports = mod.get_imported_functions_size();
      result.function_offsets.resize(mod.code.size(), {0, 0});
      result.body_offsets.resize(mod.code.size(), {0, 0});

      for (const auto& sym : obj.symbols()) {
         auto name_or_err = sym.getName();
         if (!name_or_err) continue;
         std::string name = name_or_err->str();

         if (name.starts_with("wasm_entry_")) {
            uint32_t func_idx = std::stoul(name.substr(11));
            if (func_idx >= num_imports) {
               uint32_t code_idx = func_idx - num_imports;
               if (code_idx < result.function_offsets.size()) {
                  auto addr_or_err = sym.getAddress();
                  if (addr_or_err) {
                     result.function_offsets[code_idx].first = static_cast<uint32_t>(*addr_or_err);
                  }
               }
            }
         } else if (name.starts_with("wasm_func_")) {
            uint32_t func_idx = std::stoul(name.substr(10));
            if (func_idx >= num_imports) {
               uint32_t code_idx = func_idx - num_imports;
               if (code_idx < result.body_offsets.size()) {
                  auto addr_or_err = sym.getAddress();
                  if (addr_or_err) {
                     result.body_offsets[code_idx].first = static_cast<uint32_t>(*addr_or_err);
                  }
               }
            }
         }
      }

      // Module must be destroyed before Context (Module references its Context)
      llvm_mod.reset();
      llvm_ctx.reset();

      return result;
   }

} // namespace psizam::detail
