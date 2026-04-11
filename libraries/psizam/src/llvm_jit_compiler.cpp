// LLVM JIT Compiler implementation
//
// Uses LLVM ORC JIT v2 to compile LLVM IR modules to native code.
// Compiled function pointers are registered in the psizam module for execution.

#include <psizam/llvm_jit_compiler.hpp>
#include <psizam/llvm_runtime_helpers.hpp>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include <stdexcept>

namespace psizam {

   // Initialize LLVM targets (once)
   static bool init_llvm_targets() {
      llvm::InitializeNativeTarget();
      llvm::InitializeNativeTargetAsmPrinter();
      llvm::InitializeNativeTargetAsmParser();
      return true;
   }
   static bool llvm_targets_initialized = init_llvm_targets();

   struct llvm_jit_compiler::impl {
      llvm_jit_options opts;
      std::unique_ptr<llvm::orc::LLJIT> jit;

      impl(const llvm_jit_options& options) : opts(options) {
         auto builder = llvm::orc::LLJITBuilder();

         auto jit_or_err = builder.create();
         if (!jit_or_err) {
            std::string err;
            llvm::raw_string_ostream os(err);
            os << jit_or_err.takeError();
            throw std::runtime_error("Failed to create LLVM JIT: " + err);
         }
         jit = std::move(*jit_or_err);
      }
   };

   llvm_jit_compiler::llvm_jit_compiler(const llvm_jit_options& opts)
      : _impl(std::make_unique<impl>(opts)) {}

   llvm_jit_compiler::~llvm_jit_compiler() = default;
   llvm_jit_compiler::llvm_jit_compiler(llvm_jit_compiler&&) noexcept = default;
   llvm_jit_compiler& llvm_jit_compiler::operator=(llvm_jit_compiler&&) noexcept = default;

   void llvm_jit_compiler::compile(std::unique_ptr<llvm::Module> llvm_mod,
                                    std::unique_ptr<llvm::LLVMContext> llvm_ctx,
                                    module& mod,
                                    growable_allocator& alloc) {
      // Wrap the module and its owning context into a ThreadSafeModule
      auto tsm = llvm::orc::ThreadSafeModule(
         std::move(llvm_mod),
         std::move(llvm_ctx));

      // Register runtime helper symbols so LLVM-generated code can call them
      auto& es = _impl->jit->getExecutionSession();
      auto& dl = _impl->jit->getDataLayout();
      auto& jd = _impl->jit->getMainJITDylib();

      llvm::orc::SymbolMap runtime_syms;
      auto add_sym = [&](const char* name, void* addr) {
         runtime_syms[es.intern(name)] = {
            llvm::orc::ExecutorAddr::fromPtr(addr),
            llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable
         };
      };

      add_sym("__psizam_global_get",       reinterpret_cast<void*>(&__psizam_global_get));
      add_sym("__psizam_global_set",       reinterpret_cast<void*>(&__psizam_global_set));
      add_sym("__psizam_global_get_v128",  reinterpret_cast<void*>(&__psizam_global_get_v128));
      add_sym("__psizam_global_set_v128",  reinterpret_cast<void*>(&__psizam_global_set_v128));
      add_sym("__psizam_memory_size",    reinterpret_cast<void*>(&__psizam_memory_size));
      add_sym("__psizam_memory_grow",    reinterpret_cast<void*>(&__psizam_memory_grow));
      add_sym("__psizam_call_host",      reinterpret_cast<void*>(&__psizam_call_host));
      add_sym("__psizam_memory_init",    reinterpret_cast<void*>(&__psizam_memory_init));
      add_sym("__psizam_data_drop",      reinterpret_cast<void*>(&__psizam_data_drop));
      add_sym("__psizam_memory_copy",    reinterpret_cast<void*>(&__psizam_memory_copy));
      add_sym("__psizam_memory_fill",    reinterpret_cast<void*>(&__psizam_memory_fill));
      add_sym("__psizam_table_init",     reinterpret_cast<void*>(&__psizam_table_init));
      add_sym("__psizam_elem_drop",      reinterpret_cast<void*>(&__psizam_elem_drop));
      add_sym("__psizam_table_copy",     reinterpret_cast<void*>(&__psizam_table_copy));
      add_sym("__psizam_call_indirect",  reinterpret_cast<void*>(&__psizam_call_indirect));
      add_sym("__psizam_call_depth_dec", reinterpret_cast<void*>(&__psizam_call_depth_dec));
      add_sym("__psizam_call_depth_inc", reinterpret_cast<void*>(&__psizam_call_depth_inc));
      add_sym("__psizam_trap",           reinterpret_cast<void*>(&__psizam_trap));

      if (auto err = jd.define(llvm::orc::absoluteSymbols(std::move(runtime_syms)))) {
         std::string msg;
         llvm::raw_string_ostream os(msg);
         os << err;
         throw std::runtime_error("Failed to register runtime symbols: " + msg);
      }

      if (auto err = _impl->jit->addIRModule(std::move(tsm))) {
         std::string msg;
         llvm::raw_string_ostream os(msg);
         os << err;
         throw std::runtime_error("Failed to add module to JIT: " + msg);
      }

      // Look up each entry wrapper and register in the module.
      // Entry wrappers have uniform signature: i64(*)(ptr, ptr, ptr)
      uint32_t num_imports = mod.get_imported_functions_size();
      for (uint32_t i = 0; i < mod.code.size(); i++) {
         std::string name = "wasm_entry_" + std::to_string(num_imports + i);
         auto sym = _impl->jit->lookup(name);
         if (!sym) {
            // Function wasn't compiled (e.g., unreachable)
            continue;
         }
         auto addr = sym->toPtr<void*>();
         // Store the function pointer as the jit_code_offset
         // (for LLVM backend, this is an absolute address, not a relative offset)
         mod.code[i].jit_code_offset = reinterpret_cast<std::size_t>(addr);
      }
   }

   void* llvm_jit_compiler::lookup(const std::string& name) {
      auto sym = _impl->jit->lookup(name);
      if (!sym) return nullptr;
      return sym->toPtr<void*>();
   }

} // namespace psizam
