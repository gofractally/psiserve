#pragma once

// LLVM AOT codegen writer — subclass of ir_writer that translates IR to LLVM IR,
// then compiles to a relocatable object file via LLVM TargetMachine (AOT).
//
// Unlike ir_writer_llvm (which uses ORC JIT for in-process execution), this
// writer produces a code blob + relocations suitable for .pzam serialization.
// The output is stored in the pzam_compile_result passed via set_compile_result().

#include <psizam/ir_writer.hpp>
#include <psizam/llvm_ir_translator.hpp>
#include <psizam/llvm_aot_compiler.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <exception>
#include <stdexcept>

namespace psizam {

   class ir_writer_llvm_aot : public ir_writer {
    public:
      ir_writer_llvm_aot(growable_allocator& alloc, std::size_t source_bytes, module& mod,
                         bool enable_backtrace = false, bool stack_limit_is_bytes = false)
         : ir_writer_impl(alloc, source_bytes, mod, enable_backtrace, stack_limit_is_bytes)
      {
         _skip_codegen = true;
      }

      ~ir_writer_llvm_aot() noexcept {
         if (std::uncaught_exceptions() > 0) return;
         if (!_compile_result) return;

         try {
            if (_compile_result->target_triple.empty()) {
               _compile_result->error = "ir_writer_llvm_aot: target_triple not set on compile_result";
               return;
            }

            if (get_num_functions() == 0) {
               _compile_result->error = "no functions to compile";
               return;
            }

            // Step 1: Translate psizam IR -> LLVM IR
            llvm_translate_options topts;
            topts.opt_level     = 2;
            topts.deterministic = true;
#ifdef PSIZAM_SOFTFLOAT
            topts.softfloat     = true;
#endif
            llvm_ir_translator translator(get_ir_module(), topts);

            for (uint32_t i = 0; i < get_num_functions(); ++i) {
               translator.translate_function(get_functions()[i]);
            }
            translator.finalize();

            // Step 2: AOT compile LLVM IR -> relocatable code blob
            auto llvm_mod = translator.take_module();
            auto llvm_ctx = translator.take_context();
            auto aot_result = llvm_aot_compile(std::move(llvm_mod), std::move(llvm_ctx),
                                                get_ir_module(), _compile_result->target_triple);

            // Step 3: Store results
            _compile_result->relocs = std::move(aot_result.relocations);
            _compile_result->code_blob = std::move(aot_result.code);
            _compile_result->function_offsets = std::move(aot_result.function_offsets);
         } catch (const std::exception& ex) {
            _compile_result->error = ex.what();
         } catch (...) {
            _compile_result->error = "unknown error during LLVM AOT compilation";
         }
      }
   };

} // namespace psizam
