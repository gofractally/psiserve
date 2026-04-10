// LLVM compile pipeline — bridges ir_writer_llvm to llvm_ir_translator + llvm_jit_compiler.
//
// Called from ir_writer_llvm's destructor to translate psizam IR functions
// to LLVM IR and JIT-compile them to native code.

#include <psizam/llvm_ir_translator.hpp>
#include <psizam/llvm_jit_compiler.hpp>
#include <psizam/jit_ir.hpp>
#include <psizam/types.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace psizam {

   static int g_llvm_opt_level = 2;

   void set_llvm_opt_level(int level) { g_llvm_opt_level = (level < 0) ? 0 : (level > 5) ? 5 : level; }
   int  get_llvm_opt_level()          { return g_llvm_opt_level; }

   void llvm_compile_functions(ir_function* funcs, uint32_t num_functions,
                               module& mod, growable_allocator& alloc,
                               bool deterministic) {
      // Step 1: Translate all IR functions to LLVM IR
      llvm_translate_options topts;
      topts.opt_level     = g_llvm_opt_level;
      topts.deterministic = deterministic;
      llvm_ir_translator translator(mod, topts);

      for (uint32_t i = 0; i < num_functions; ++i) {
         translator.translate_function(funcs[i]);
      }

      translator.finalize();

      // Step 2: JIT-compile the LLVM module to native code
      auto llvm_mod = translator.take_module();
      auto llvm_ctx = translator.take_context();

      auto compiler = std::make_shared<llvm_jit_compiler>();
      compiler->compile(std::move(llvm_mod), std::move(llvm_ctx), mod, alloc);

      // Keep the JIT compiler alive — it owns the compiled code memory
      mod.jit_engine = compiler;

   }

} // namespace psizam
