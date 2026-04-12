// pzam inspect: Show metadata and code section info from a .pzam file.
//
// Usage: pzam inspect <module.pzam>

#include <psizam/pzam_format.hpp>

#include <fstream>
#include <iostream>
#include <string>
#include <vector>

using namespace psizam;
using namespace psizam::detail;

static const char* arch_name(uint8_t arch) {
   switch (static_cast<pzam_arch>(arch)) {
      case pzam_arch::x86_64:  return "x86_64";
      case pzam_arch::aarch64: return "aarch64";
      default:                 return "unknown";
   }
}

static const char* tier_name(uint8_t tier) {
   switch (static_cast<pzam_opt_tier>(tier)) {
      case pzam_opt_tier::jit1:    return "jit1";
      case pzam_opt_tier::jit2:    return "jit2";
      case pzam_opt_tier::llvm_O1: return "llvm-O1";
      case pzam_opt_tier::llvm_O2: return "llvm-O2";
      case pzam_opt_tier::llvm_O3: return "llvm-O3";
      default:                     return "unknown";
   }
}

static const char* import_kind(uint8_t k) {
   switch (k) {
      case 0: return "function";
      case 1: return "table";
      case 2: return "memory";
      case 3: return "global";
      default: return "unknown";
   }
}

static const char* export_kind(uint8_t k) {
   return import_kind(k); // same encoding
}

int pzam_inspect_main(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: pzam inspect <module.pzam>\n";
      return 1;
   }

   std::string path = argv[1];

   std::ifstream in(path, std::ios::binary | std::ios::ate);
   if (!in.is_open()) {
      std::cerr << "Error: cannot open: " << path << "\n";
      return 1;
   }
   auto size = in.tellg();
   in.seekg(0);
   std::vector<char> data(size);
   in.read(data.data(), size);

   if (!pzam_validate(data)) {
      std::cerr << "Error: invalid .pzam file\n";
      return 1;
   }

   pzam_file pzam;
   try {
      pzam = pzam_load(data);
   } catch (const std::exception& e) {
      std::cerr << "Error: " << e.what() << "\n";
      return 1;
   }

   // Header
   std::cout << "=== .pzam File: " << path << " ===\n";
   std::cout << "Format version: " << pzam.format_version << "\n";
   std::cout << "File size: " << size << " bytes\n";
   std::cout << "Input hash: ";
   for (int i = 0; i < 8; i++) std::printf("%02x", pzam.input_hash[i]);
   std::cout << "...\n\n";

   // Metadata summary
   const auto& m = pzam.metadata;
   std::cout << "=== Module Metadata ===\n";
   std::cout << "Types:    " << m.types.size() << "\n";
   std::cout << "Imports:  " << m.imports.size()
             << " (func=" << m.num_imported_functions
             << " table=" << m.num_imported_tables
             << " mem=" << m.num_imported_memories
             << " global=" << m.num_imported_globals << ")\n";
   std::cout << "Functions: " << m.functions.size() << " (local)\n";
   std::cout << "Tables:   " << m.tables.size() << "\n";
   std::cout << "Memories: " << m.memories.size() << "\n";
   std::cout << "Globals:  " << m.globals.size() << "\n";
   std::cout << "Exports:  " << m.exports.size() << "\n";
   std::cout << "Elements: " << m.elements.size() << "\n";
   std::cout << "Data:     " << m.data.size() << "\n";
   std::cout << "Tags:     " << m.tags.size() << "\n";
   if (m.start_function != UINT32_MAX)
      std::cout << "Start:    function " << m.start_function << "\n";
   std::cout << "\n";

   // Imports
   if (!m.imports.empty()) {
      std::cout << "--- Imports ---\n";
      for (size_t i = 0; i < m.imports.size(); i++) {
         const auto& imp = m.imports[i];
         std::cout << "  [" << i << "] " << import_kind(imp.kind)
                   << " " << imp.module_name << "." << imp.field_name;
         if (imp.kind == 0)
            std::cout << " type=" << imp.func_type_idx;
         std::cout << "\n";
      }
      std::cout << "\n";
   }

   // Exports
   if (!m.exports.empty()) {
      std::cout << "--- Exports ---\n";
      for (const auto& exp : m.exports) {
         std::cout << "  " << export_kind(exp.kind) << " \"" << exp.field_name
                   << "\" -> " << exp.index << "\n";
      }
      std::cout << "\n";
   }

   // Code sections
   std::cout << "=== Code Sections: " << pzam.code_sections.size() << " ===\n";
   for (size_t i = 0; i < pzam.code_sections.size(); i++) {
      const auto& cs = pzam.code_sections[i];
      std::cout << "\n--- Section " << i << " ---\n";
      std::cout << "  Architecture: " << arch_name(cs.arch) << "\n";
      std::cout << "  Opt tier:     " << tier_name(cs.opt_tier) << "\n";
      std::cout << "  Functions:    " << cs.functions.size() << "\n";
      std::cout << "  Relocations:  " << cs.relocations.size() << "\n";
      std::cout << "  Code size:    " << cs.code_blob.size() << " bytes\n";
      std::cout << "  Max stack:    " << cs.max_stack << "\n";
      std::cout << "  Page size:    " << cs.page_size << "\n";
      std::cout << "  Stack limit:  " << (cs.stack_limit_mode ? "bytes" : "frames") << "\n";

      // Instrumentation
      const auto& inst = cs.instrumentation;
      if (inst.softfloat || inst.gas_metering || inst.yield_points ||
          inst.debug_info || inst.async_backtrace) {
         std::cout << "  Instrumentation:";
         if (inst.softfloat)       std::cout << " softfloat";
         if (inst.gas_metering)    std::cout << " gas";
         if (inst.yield_points)    std::cout << " yield";
         if (inst.debug_info)      std::cout << " debug";
         if (inst.async_backtrace) std::cout << " backtrace";
         std::cout << "\n";
      }

      // Compiler info
      if (!cs.compiler.compiler_name.empty()) {
         std::cout << "  Compiler:     " << cs.compiler.compiler_name;
         if (!cs.compiler.compiler_version.empty())
            std::cout << " " << cs.compiler.compiler_version;
         std::cout << "\n";
      }

      // Attestations
      if (!cs.attestations.empty()) {
         std::cout << "  Attestations: " << cs.attestations.size() << "\n";
         for (size_t j = 0; j < cs.attestations.size(); j++) {
            std::cout << "    [" << j << "] pubkey=";
            for (int k = 0; k < 8; k++)
               std::printf("%02x", cs.attestations[j].pubkey_hash[k]);
            std::cout << "... sig=" << cs.attestations[j].signature.size() << " bytes\n";
         }
      }
   }

   return 0;
}
