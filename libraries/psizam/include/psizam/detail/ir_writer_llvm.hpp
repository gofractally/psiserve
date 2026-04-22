#pragma once

// LLVM codegen writer — subclass of ir_writer that replaces the native jit_codegen
// pipeline with LLVM IR translation + ORC JIT compilation.
//
// Pass 1 (parsing -> IR) is inherited from ir_writer unchanged.
// Pass 2 (codegen) is replaced: ir_writer's destructor is skipped (_skip_codegen),
// and this class's destructor calls the LLVM pipeline via llvm_compile_functions().

#include <psizam/detail/ir_writer.hpp>
#include <psizam/options.hpp>

#include <cstdint>
#include <exception>

namespace psizam::detail {

   // Forward-declared LLVM compile pipeline (defined in src/llvm_compile.cpp).
   // Takes IR functions from ir_writer and compiles them to native code via LLVM.
   void llvm_compile_functions(ir_function* funcs, uint32_t num_functions,
                               module& mod, growable_allocator& alloc,
                               bool deterministic = false,
                               mem_safety mem_mode = mem_safety::guarded,
                               checked_mode checked_kind = checked_mode::strict);

   // Set/get LLVM optimization level (0-3). Default is 2.
   void     set_llvm_opt_level(int level);
   int      get_llvm_opt_level();

   // Thread-local for transporting exceptions out of ir_writer_llvm destructor.
   // Destructors cannot throw; this stores the exception for post-destructor rethrow.
   inline thread_local std::exception_ptr llvm_deferred_exception;

   class ir_writer_llvm : public ir_writer {
    public:
      ir_writer_llvm(growable_allocator& alloc, std::size_t source_bytes, module& mod,
                     bool enable_backtrace = false, bool stack_limit_is_bytes = false,
                     uint32_t compile_threads = 0, bool deterministic = true)
         : ir_writer_impl(alloc, source_bytes, mod, enable_backtrace, stack_limit_is_bytes, compile_threads),
           _deterministic(deterministic)
      {
         _skip_codegen = true;
         llvm_deferred_exception = nullptr;
      }

      // Parser calls these via `requires` to propagate options to the
      // underlying code_writer BEFORE emission. For LLVM the emission is
      // deferred to the destructor, so we only have to stash the values.
      void set_mem_mode(mem_safety m) noexcept { _mem_mode = m; }
      void set_checked_kind(checked_mode k) noexcept { _checked_kind = k; }
      void set_fp_mode(fp_mode m) noexcept {
         _fp = m;
         _deterministic = (m != fp_mode::fast);
      }

      ~ir_writer_llvm() {
         if (std::uncaught_exceptions() > 0) return;

         // Run the LLVM pipeline: translate IR -> LLVM IR -> native code.
         // IR data in scratch allocator is still alive; base class destructor
         // (which runs after this) will clean it up.
         try {
            llvm_compile_functions(get_functions(), get_num_functions(),
                                   get_ir_module(), get_ir_allocator(),
                                   _deterministic, _mem_mode, _checked_kind);
         } catch (...) {
            // Cannot throw from destructor; defer for rethrow after parser destructs
            llvm_deferred_exception = std::current_exception();
         }
      }

    private:
      bool         _deterministic = false;
      fp_mode      _fp            = fp_mode::fast;
      mem_safety   _mem_mode      = mem_safety::guarded;
      checked_mode _checked_kind  = checked_mode::strict;
   };

} // namespace psizam::detail
