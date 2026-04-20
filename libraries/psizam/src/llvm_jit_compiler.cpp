// LLVM JIT Compiler implementation
//
// Uses LLVM ORC JIT v2 to compile LLVM IR modules to native code.
// Compiled function pointers are registered in the psizam module for execution.

#include <psizam/detail/llvm_jit_compiler.hpp>
#include <psizam/detail/llvm_runtime_helpers.hpp>
#include <psizam/detail/softfloat.hpp>

#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/ThreadSafeModule.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/TargetSelect.h>

#include <cstring>
#include <stdexcept>
#include <string>

namespace psizam::detail {

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
         auto jit_or_err = llvm::orc::LLJITBuilder().create();
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

      // On macOS, LLJIT's symbol lookup applies the target's global prefix
      // (a leading '_') to every external reference emitted by the generated
      // module. For registered absolute symbols to resolve, they must be
      // interned with the same prefix. Linux/ELF has an empty prefix so this
      // is a no-op there, but getting this wrong produces
      //   "Symbols not found: [ ___psizam_sf_* ]"
      // (triple underscore = double-underscore helper + leading '_').
      const char prefix = dl.getGlobalPrefix();
      llvm::orc::SymbolMap runtime_syms;
      auto add_sym = [&](const char* name, void* addr) {
         std::string mangled;
         if (prefix) {
            mangled.reserve(std::strlen(name) + 1);
            mangled.push_back(prefix);
            mangled.append(name);
         } else {
            mangled = name;
         }
         runtime_syms[es.intern(mangled)] = {
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
      add_sym("__psizam_call_host",          reinterpret_cast<void*>(&__psizam_call_host));
      add_sym("__psizam_call_host_nothrow",  reinterpret_cast<void*>(&__psizam_call_host_nothrow));
      add_sym("__psizam_call_host_full",     reinterpret_cast<void*>(&__psizam_call_host_full));
      add_sym("__psizam_memory_init",    reinterpret_cast<void*>(&__psizam_memory_init));
      add_sym("__psizam_data_drop",      reinterpret_cast<void*>(&__psizam_data_drop));
      add_sym("__psizam_memory_copy",    reinterpret_cast<void*>(&__psizam_memory_copy));
      add_sym("__psizam_memory_fill",    reinterpret_cast<void*>(&__psizam_memory_fill));
      add_sym("__psizam_table_init",     reinterpret_cast<void*>(&__psizam_table_init));
      add_sym("__psizam_elem_drop",      reinterpret_cast<void*>(&__psizam_elem_drop));
      add_sym("__psizam_table_copy",     reinterpret_cast<void*>(&__psizam_table_copy));
      add_sym("__psizam_call_indirect",          reinterpret_cast<void*>(&__psizam_call_indirect));
      add_sym("__psizam_call_indirect_nothrow",  reinterpret_cast<void*>(&__psizam_call_indirect_nothrow));
      add_sym("__psizam_get_memory",    reinterpret_cast<void*>(&__psizam_get_memory));
      add_sym("__psizam_table_get",    reinterpret_cast<void*>(&__psizam_table_get));
      add_sym("__psizam_table_set",    reinterpret_cast<void*>(&__psizam_table_set));
      add_sym("__psizam_table_grow",   reinterpret_cast<void*>(&__psizam_table_grow));
      add_sym("__psizam_table_size",   reinterpret_cast<void*>(&__psizam_table_size));
      add_sym("__psizam_table_fill",   reinterpret_cast<void*>(&__psizam_table_fill));
      add_sym("__psizam_call_depth_dec", reinterpret_cast<void*>(&__psizam_call_depth_dec));
      add_sym("__psizam_call_depth_inc", reinterpret_cast<void*>(&__psizam_call_depth_inc));
      add_sym("__psizam_gas_charge",     reinterpret_cast<void*>(&__psizam_gas_charge));
      add_sym("__psizam_gas_exhausted_check", reinterpret_cast<void*>(&__psizam_gas_exhausted_check));
      add_sym("__psizam_trap",           reinterpret_cast<void*>(&__psizam_trap));

      // WASM EH runtime helpers (try_table, throw, catch)
      add_sym("__psizam_eh_enter",             reinterpret_cast<void*>(&__psizam_eh_enter));
      add_sym("__psizam_eh_leave",             reinterpret_cast<void*>(&__psizam_eh_leave));
      add_sym("__psizam_eh_throw",             reinterpret_cast<void*>(&__psizam_eh_throw));
      add_sym("__psizam_eh_throw_ref",         reinterpret_cast<void*>(&__psizam_eh_throw_ref));
      add_sym("__psizam_eh_get_match",         reinterpret_cast<void*>(&__psizam_eh_get_match));
      add_sym("__psizam_eh_get_payload",       reinterpret_cast<void*>(&__psizam_eh_get_payload));
      add_sym("__psizam_eh_get_payload_count", reinterpret_cast<void*>(&__psizam_eh_get_payload_count));
      add_sym("__psizam_eh_get_exnref",        reinterpret_cast<void*>(&__psizam_eh_get_exnref));
      add_sym("__psizam_setjmp",               reinterpret_cast<void*>(&__psizam_setjmp));

      // Softfloat helpers — map __psizam_sf_* symbols to inline _psizam_* functions
      add_sym("__psizam_sf_f32_add",       reinterpret_cast<void*>(&_psizam_f32_add));
      add_sym("__psizam_sf_f32_sub",       reinterpret_cast<void*>(&_psizam_f32_sub));
      add_sym("__psizam_sf_f32_mul",       reinterpret_cast<void*>(&_psizam_f32_mul));
      add_sym("__psizam_sf_f32_div",       reinterpret_cast<void*>(&_psizam_f32_div));
      add_sym("__psizam_sf_f32_min",       reinterpret_cast<void*>(&_psizam_f32_min<true>));
      add_sym("__psizam_sf_f32_max",       reinterpret_cast<void*>(&_psizam_f32_max<true>));
      add_sym("__psizam_sf_f32_copysign",  reinterpret_cast<void*>(&_psizam_f32_copysign));
      add_sym("__psizam_sf_f32_abs",       reinterpret_cast<void*>(&_psizam_f32_abs));
      add_sym("__psizam_sf_f32_neg",       reinterpret_cast<void*>(&_psizam_f32_neg));
      add_sym("__psizam_sf_f32_sqrt",      reinterpret_cast<void*>(&_psizam_f32_sqrt));
      add_sym("__psizam_sf_f32_ceil",      reinterpret_cast<void*>(&_psizam_f32_ceil<true>));
      add_sym("__psizam_sf_f32_floor",     reinterpret_cast<void*>(&_psizam_f32_floor<true>));
      add_sym("__psizam_sf_f32_trunc",     reinterpret_cast<void*>(&_psizam_f32_trunc<true>));
      add_sym("__psizam_sf_f32_nearest",   reinterpret_cast<void*>(&_psizam_f32_nearest<true>));
      add_sym("__psizam_sf_f64_add",       reinterpret_cast<void*>(&_psizam_f64_add));
      add_sym("__psizam_sf_f64_sub",       reinterpret_cast<void*>(&_psizam_f64_sub));
      add_sym("__psizam_sf_f64_mul",       reinterpret_cast<void*>(&_psizam_f64_mul));
      add_sym("__psizam_sf_f64_div",       reinterpret_cast<void*>(&_psizam_f64_div));
      add_sym("__psizam_sf_f64_min",       reinterpret_cast<void*>(&_psizam_f64_min<true>));
      add_sym("__psizam_sf_f64_max",       reinterpret_cast<void*>(&_psizam_f64_max<true>));
      add_sym("__psizam_sf_f64_copysign",  reinterpret_cast<void*>(&_psizam_f64_copysign));
      add_sym("__psizam_sf_f64_abs",       reinterpret_cast<void*>(&_psizam_f64_abs));
      add_sym("__psizam_sf_f64_neg",       reinterpret_cast<void*>(&_psizam_f64_neg));
      add_sym("__psizam_sf_f64_sqrt",      reinterpret_cast<void*>(&_psizam_f64_sqrt));
      add_sym("__psizam_sf_f64_ceil",      reinterpret_cast<void*>(&_psizam_f64_ceil<true>));
      add_sym("__psizam_sf_f64_floor",     reinterpret_cast<void*>(&_psizam_f64_floor<true>));
      add_sym("__psizam_sf_f64_trunc",     reinterpret_cast<void*>(&_psizam_f64_trunc<true>));
      add_sym("__psizam_sf_f64_nearest",   reinterpret_cast<void*>(&_psizam_f64_nearest<true>));
      add_sym("__psizam_sf_f32_convert_i32s", reinterpret_cast<void*>(&_psizam_i32_to_f32));
      add_sym("__psizam_sf_f32_convert_i32u", reinterpret_cast<void*>(&_psizam_ui32_to_f32));
      add_sym("__psizam_sf_f32_convert_i64s", reinterpret_cast<void*>(&_psizam_i64_to_f32));
      add_sym("__psizam_sf_f32_convert_i64u", reinterpret_cast<void*>(&_psizam_ui64_to_f32));
      add_sym("__psizam_sf_f64_convert_i32s", reinterpret_cast<void*>(&_psizam_i32_to_f64));
      add_sym("__psizam_sf_f64_convert_i32u", reinterpret_cast<void*>(&_psizam_ui32_to_f64));
      add_sym("__psizam_sf_f64_convert_i64s", reinterpret_cast<void*>(&_psizam_i64_to_f64));
      add_sym("__psizam_sf_f64_convert_i64u", reinterpret_cast<void*>(&_psizam_ui64_to_f64));
      add_sym("__psizam_sf_f32_demote_f64",   reinterpret_cast<void*>(&_psizam_f64_demote));
      add_sym("__psizam_sf_f64_promote_f32",  reinterpret_cast<void*>(&_psizam_f32_promote));

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

} // namespace psizam::detail
