// LLVM AOT Compiler implementation
//
// Compiles LLVM IR to a relocatable object file using LLVM's TargetMachine,
// then parses the object to extract the .text section and relocations.
// The result is packaged for .pzam serialization.

#include <psizam/llvm_aot_compiler.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Object/ELFObjectFile.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/TargetSelect.h>

// Explicit target initialization (avoid linking all targets)
extern "C" {
   void LLVMInitializeX86TargetInfo();
   void LLVMInitializeX86Target();
   void LLVMInitializeX86TargetMC();
   void LLVMInitializeX86AsmPrinter();
   void LLVMInitializeAArch64TargetInfo();
   void LLVMInitializeAArch64Target();
   void LLVMInitializeAArch64TargetMC();
   void LLVMInitializeAArch64AsmPrinter();
}
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/Support/SmallVectorMemoryBuffer.h>

#include <stdexcept>
#include <unordered_map>

namespace psizam {

   // Initialize X86 and AArch64 targets for cross-compilation.
   // Only these two are linked, so we can't use InitializeAllTargets().
   static bool ensure_llvm_targets() {
      static bool done = [] {
         LLVMInitializeX86TargetInfo();
         LLVMInitializeX86Target();
         LLVMInitializeX86TargetMC();
         LLVMInitializeX86AsmPrinter();
         LLVMInitializeAArch64TargetInfo();
         LLVMInitializeAArch64Target();
         LLVMInitializeAArch64TargetMC();
         LLVMInitializeAArch64AsmPrinter();
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
               throw std::runtime_error("Unsupported x86_64 ELF relocation type: " + std::to_string(elf_type));
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
            default:
               throw std::runtime_error("Unsupported aarch64 ELF relocation type: " + std::to_string(elf_type));
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
      auto* target = llvm::TargetRegistry::lookupTarget(triple, error);
      if (!target) {
         throw std::runtime_error("LLVM target lookup failed: " + error);
      }

      // Create target machine with large code model for absolute addressing
      llvm::TargetOptions opts;
      auto rm = llvm::Reloc::Static;
      // Use large code model so all external references use abs64 on x86_64
      // or MOVZ+MOVK sequences on aarch64 (Small would use ADRP which is harder to relocate)
      auto cm = llvm::CodeModel::Large;
      // x86-64-v2 includes SSE4.1/4.2 — avoids libc calls for ceil/floor/etc.
      std::string cpu = is_x86_64 ? "x86-64-v2" : "";
      auto tm = std::unique_ptr<llvm::TargetMachine>(
         target->createTargetMachine(triple, cpu, "", opts, rm, cm,
                                     llvm::CodeGenOptLevel::Default));
      if (!tm) {
         throw std::runtime_error("Failed to create LLVM TargetMachine");
      }

      llvm_mod->setDataLayout(tm->createDataLayout());
      llvm_mod->setTargetTriple(triple);

      // Emit object code to memory buffer
      llvm::SmallVector<char, 0> obj_buf;
      llvm::raw_svector_ostream obj_stream(obj_buf);
      llvm::legacy::PassManager pm;

      if (tm->addPassesToEmitFile(pm, obj_stream, nullptr,
                                   llvm::CodeGenFileType::ObjectFile)) {
         throw std::runtime_error("TargetMachine cannot emit object file");
      }
      pm.run(*llvm_mod);

      // Parse the object file
      auto buf = std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(obj_buf), false);
      auto obj_or_err = llvm::object::ObjectFile::createObjectFile(*buf);
      if (!obj_or_err) {
         std::string msg;
         llvm::raw_string_ostream os(msg);
         os << obj_or_err.takeError();
         throw std::runtime_error("Failed to parse emitted object: " + msg);
      }
      auto& obj = **obj_or_err;

      // Find the .text section
      llvm_aot_result result;
      const llvm::object::SectionRef* text_section = nullptr;

      for (const auto& section : obj.sections()) {
         auto name_or_err = section.getName();
         if (!name_or_err) continue;
         // Large code model on x86_64 puts code in .ltext instead of .text
         if (*name_or_err == ".text" || *name_or_err == ".ltext") {
            auto contents_or_err = section.getContents();
            if (!contents_or_err) {
               throw std::runtime_error("Failed to read code section");
            }
            if (!contents_or_err->empty()) {
               result.code.assign(contents_or_err->begin(), contents_or_err->end());
               text_section = &section;
               break;
            }
         }
      }

      if (!text_section || result.code.empty()) {
         throw std::runtime_error("No .text section found in emitted object");
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

            // Skip internal symbols (wasm function cross-references within the blob)
            if (psizam_sym == reloc_symbol::unknown) {
               // Check if it's a wasm_entry_* or wasm_func_* internal reference,
               // a local label (.L*), or a section symbol (.text, .ltext, etc.)
               if (sym_name.starts_with("wasm_entry_") || sym_name.starts_with("wasm_func_") ||
                   sym_name.starts_with(".L") || sym_name.starts_with(".") ||
                   sym_name.starts_with("_")) {
                  continue; // internal reference, resolved within the blob
               }
               throw std::runtime_error("Unknown external symbol in LLVM object: " + sym_name);
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

      // Find function offsets by looking up wasm_entry_* symbols
      uint32_t num_imports = mod.get_imported_functions_size();
      result.function_offsets.resize(mod.code.size(), {0, 0});

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
                  auto val_or_err = sym.getValue();
                  // Size is tricky from symbols; leave as 0 for now
               }
            }
         }
      }

      // Module must be destroyed before Context (Module references its Context)
      llvm_mod.reset();
      llvm_ctx.reset();

      return result;
   }

} // namespace psizam
