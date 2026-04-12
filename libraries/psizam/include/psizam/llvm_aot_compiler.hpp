#pragma once

// LLVM AOT Compiler — compiles LLVM IR to relocatable native code for .pzam files.
//
// Unlike llvm_jit_compiler which uses ORC JIT for in-process execution,
// this compiler uses LLVM's TargetMachine to emit an object file, then
// extracts the code section and relocations for serialization into .pzam.

#include <psizam/jit_reloc.hpp>
#include <psizam/types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
   class LLVMContext;
   class Module;
}

namespace psizam {

   /// Result of LLVM AOT compilation — code blob + relocations.
   struct llvm_aot_result {
      std::vector<uint8_t> code;                   // native code blob
      std::vector<code_relocation> relocations;     // relocations against runtime symbols
      std::vector<std::pair<uint32_t, uint32_t>> function_offsets; // (offset, size) per entry wrapper
      std::vector<std::pair<uint32_t, uint32_t>> body_offsets;     // (offset, size) per function body
      std::string error;                            // non-empty on failure
   };

   /// Compile an LLVM module to a relocatable code blob.
   /// target_triple: e.g., "x86_64-unknown-linux-gnu" or "aarch64-unknown-linux-gnu"
   llvm_aot_result llvm_aot_compile(
         std::unique_ptr<llvm::Module> llvm_mod,
         std::unique_ptr<llvm::LLVMContext> llvm_ctx,
         const module& mod,
         const std::string& target_triple);

} // namespace psizam
