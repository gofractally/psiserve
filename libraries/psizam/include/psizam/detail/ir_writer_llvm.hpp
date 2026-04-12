#pragma once

// LLVM codegen writer — subclass of ir_writer that replaces the native jit_codegen
// pipeline with LLVM IR translation + ORC JIT compilation.
//
// Pass 1 (parsing -> IR) is inherited from ir_writer unchanged.
// Pass 2 (codegen) is replaced: ir_writer's destructor is skipped (_skip_codegen),
// and this class's destructor calls the LLVM pipeline via llvm_compile_functions().

#include <psizam/detail/ir_writer.hpp>

#include <cstdint>
#include <exception>

namespace psizam::detail {

   // Forward-declared LLVM compile pipeline (defined in src/llvm_compile.cpp).
   // Takes IR functions from ir_writer and compiles them to native code via LLVM.
   void llvm_compile_functions(ir_function* funcs, uint32_t num_functions,
                               module& mod, growable_allocator& alloc,
                               bool deterministic = false);

   // Set/get LLVM optimization level (0-3). Default is 2.
   void     set_llvm_opt_level(int level);
   int      get_llvm_opt_level();

   class ir_writer_llvm : public ir_writer {
    public:
      ir_writer_llvm(growable_allocator& alloc, std::size_t source_bytes, module& mod,
                     bool enable_backtrace = false, bool stack_limit_is_bytes = false,
                     uint32_t compile_threads = 0, bool deterministic = true)
         : ir_writer_impl(alloc, source_bytes, mod, enable_backtrace, stack_limit_is_bytes, compile_threads),
           _deterministic(deterministic)
      {
         _skip_codegen = true;
      }

      ~ir_writer_llvm() {
         if (std::uncaught_exceptions() > 0) return;

         // Run the LLVM pipeline: translate IR -> LLVM IR -> native code.
         // IR data in scratch allocator is still alive; base class destructor
         // (which runs after this) will clean it up.
         llvm_compile_functions(get_functions(), get_num_functions(),
                                get_ir_module(), get_ir_allocator(),
                                _deterministic);
      }

    private:
      bool _deterministic = false;
   };

} // namespace psizam::detail
