#pragma once

// LLVM JIT Compiler — compiles LLVM IR to native code via ORC JIT v2.
//
// Takes the LLVM module produced by llvm_ir_translator and JIT-compiles it
// to native code. The compiled function pointers are written into the
// psizam module's function_body entries for execution.
//
// Uses a custom memory manager to allocate code into the psizam
// growable_allocator, keeping compiled code alongside existing JIT code
// in the same memory region.

#include <psizam/allocator.hpp>
#include <psizam/types.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace llvm {
   class LLVMContext;
   class Module;
   namespace orc {
      class LLJIT;
   }
}

namespace psizam { class host_function_table; }

namespace psizam::detail {

   /// LLVM JIT compiler options.
   struct llvm_jit_options {
      int opt_level = 2; // LLVM optimization level (0-3)
   };

   /// Compiles LLVM IR to native code and registers function pointers in the module.
   class llvm_jit_compiler {
   public:
      llvm_jit_compiler(const llvm_jit_options& opts = {});
      ~llvm_jit_compiler();

      // Non-copyable, moveable
      llvm_jit_compiler(const llvm_jit_compiler&) = delete;
      llvm_jit_compiler& operator=(const llvm_jit_compiler&) = delete;
      llvm_jit_compiler(llvm_jit_compiler&&) noexcept;
      llvm_jit_compiler& operator=(llvm_jit_compiler&&) noexcept;

      /// Compile an LLVM module and register the compiled functions.
      /// On success, mod.code[i].jit_code_offset is updated for each function.
      /// The LLVM module and context ownership are transferred to the JIT.
      void compile(std::unique_ptr<llvm::Module> llvm_mod,
                   std::unique_ptr<llvm::LLVMContext> llvm_ctx,
                   module& mod,
                   growable_allocator& alloc);

      /// Look up a compiled function by name.
      /// Returns null if not found.
      void* lookup(const std::string& name);

   private:
      struct impl;
      std::unique_ptr<impl> _impl;
   };

} // namespace psizam::detail
