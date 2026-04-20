#pragma once

// LLVM IR Translator — converts psizam IR to LLVM IR.
//
// Pipeline: WASM → parser → ir_writer (Pass 1) → llvm_ir_translator → LLVM → native
//
// The translator takes the IR instructions produced by ir_writer and emits
// LLVM IR using the LLVM C++ API. LLVM then handles register allocation,
// instruction selection, and platform-specific optimization.
//
// Function signature convention:
//   i64 @wasm_func_N(ptr %ctx, ptr %mem, i32 %p0, ...)
//   - %ctx = pointer to jit_execution_context (first arg always)
//   - %mem = linear memory base pointer (second arg always)
//   - Remaining args are WASM function parameters
//   - Return type matches WASM function return type (void, i32, i64, f32, f64)

#include <psizam/config.hpp>
#include <psizam/detail/jit_ir.hpp>
#include <psizam/options.hpp>
#include <psizam/types.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for LLVM types (avoid pulling in LLVM headers here)
namespace llvm {
   class LLVMContext;
   class Module;
   class Function;
   class BasicBlock;
   class Value;
   class Type;
   class FunctionType;
   namespace orc {
      class LLJIT;
   }
}

namespace psizam::detail {

   /// Options for the LLVM IR translator.
   struct llvm_translate_options {
      // Floating-point execution mode (see psizam/config.hpp).
      // fast             : native FP, no determinism guarantees.
      // hw_deterministic : native FP, NaN-canonicalized, bit-matches softfloat.
      // softfloat        : lane-wise softfloat (reference oracle, slowest).
      fp_mode fp             = fp_mode::fast;

      bool enable_backtrace  = false; // Enable async backtrace support
      int  opt_level         = 2;     // LLVM optimization level (0-3)
      bool per_function      = false; // Per-function compilation: external linkage for all decls
      bool nothrow_host_calls = false; // Use nothrow host call helpers (JIT path with .eh_frame)

      // Memory safety mode: guarded (OS guard pages), checked (IR-level bounds
      // checks with deferred read watermark + immediate write check), or
      // unchecked (no protection).
      mem_safety   mem_mode      = mem_safety::guarded;
      // Checked sub-mode: strict uses max(watermark, end), relaxed uses OR.
      checked_mode checked_kind  = checked_mode::strict;
   };

   /// Translates psizam IR functions to LLVM IR.
   /// One translator instance handles a single WASM module.
   class llvm_ir_translator {
   public:
      llvm_ir_translator(const module& mod, const llvm_translate_options& opts = {});
      ~llvm_ir_translator();

      // Non-copyable, moveable
      llvm_ir_translator(const llvm_ir_translator&) = delete;
      llvm_ir_translator& operator=(const llvm_ir_translator&) = delete;
      llvm_ir_translator(llvm_ir_translator&&) noexcept;
      llvm_ir_translator& operator=(llvm_ir_translator&&) noexcept;

      /// Translate a single IR function to LLVM IR.
      /// Must be called for each function in module order.
      void translate_function(const ir_function& func);

      /// Finalize the LLVM module — run optimization passes and verify.
      /// Must be called after all functions are translated.
      void finalize();

      /// Access the LLVM module (after finalize).
      llvm::Module* get_module() const;

      /// Take ownership of the LLVM module (after finalize).
      /// The translator becomes invalid after this call.
      std::unique_ptr<llvm::Module> take_module();

      /// Take ownership of the LLVMContext (must be called with take_module).
      std::unique_ptr<llvm::LLVMContext> take_context();

      /// Dump LLVM IR to string (for debugging).
      std::string dump_ir() const;

   private:
      struct impl;
      std::unique_ptr<impl> _impl;
   };

} // namespace psizam::detail
