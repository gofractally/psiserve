// Runtime helper functions for the LLVM JIT backend.
// These are called from LLVM-generated native code via absolute symbol resolution.
//
// Exception safety: These helpers are called FROM LLVM-generated code. When running
// pre-compiled .pzam code, the LLVM frames lack .eh_frame data, so C++ exceptions
// cannot unwind through them. Instead, we catch exceptions at this boundary and use
// longjmp via trap_jmp_buf (set up by invoke_with_signal_handler / setjmp)
// to bypass the LLVM frames entirely.

#include <psizam/detail/llvm_runtime_helpers.hpp>
#include <psizam/detail/execution_context.hpp>
#include <psizam/types.hpp>

#include <cstring>

namespace {
   using namespace psizam;
   using namespace psizam::detail;
   using ctx_t = jit_execution_context<false>;

   ctx_t& as_ctx(void* ctx) {
      return *static_cast<ctx_t*>(ctx);
   }

   // Escape via longjmp with an existing exception_ptr.
   // Used when propagating exceptions from host calls or recursive WASM calls.
   [[noreturn]] void escape_exception(std::exception_ptr eptr) {
      if (trap_jmp_ptr) {
         saved_exception = std::move(eptr);
         longjmp(*trap_jmp_ptr, -1);
      }
      std::rethrow_exception(eptr);
   }

   // Escape via longjmp with a new exception, or throw if no escape target.
   template<typename E>
   [[noreturn]] void escape_or_throw(const char* msg) {
      if (trap_jmp_ptr) {
         saved_exception = std::make_exception_ptr(E{msg});
         longjmp(*trap_jmp_ptr, -1);
      }
      throw E{msg};
   }
}

extern "C" {

int64_t __psizam_global_get(void* ctx, uint32_t idx) {
   return as_ctx(ctx).get_global(idx).value.i64;
}

void __psizam_global_set(void* ctx, uint32_t idx, int64_t value) {
   as_ctx(ctx).get_global(idx).value.i64 = value;
}

void __psizam_global_get_v128(void* ctx, uint32_t idx, void* out) {
   auto& v = as_ctx(ctx).get_global(idx).value.v128;
   std::memcpy(out, &v, 16);
}

void __psizam_global_set_v128(void* ctx, uint32_t idx, const void* in) {
   auto& v = as_ctx(ctx).get_global(idx).value.v128;
   std::memcpy(&v, in, 16);
}

void* __psizam_get_memory(void* ctx) {
   return as_ctx(ctx).linear_memory();
}

int32_t __psizam_memory_size(void* ctx) {
   return as_ctx(ctx).current_linear_memory();
}

int32_t __psizam_memory_grow(void* ctx, int32_t pages, void** new_mem_out) {
   auto& c = as_ctx(ctx);
   int32_t result = c.grow_linear_memory(pages);
   // Return the (possibly new) memory base pointer so the caller can update %mem
   if (new_mem_out)
      *new_mem_out = c.linear_memory();
   return result;
}

int64_t __psizam_call_host(void* ctx, uint32_t func_idx,
                            void* args_buf, uint32_t nargs) {
   auto& c = as_ctx(ctx);
   auto* args = static_cast<native_value*>(args_buf);

   // Fast path: call the trampoline directly if available.
   // The trampoline_ptrs array is indexed by import number and handles
   // all type conversion internally — no table lookup needed.
   if (c._host_trampoline_ptrs) {
      auto trampoline = c._host_trampoline_ptrs[func_idx];
      if (trampoline) {
         try {
            auto result = trampoline(c.get_host_ptr(), args, c.linear_memory());
            return result.i64;
         } catch (...) {
            escape_exception(std::current_exception());
         }
      }
   }

   // Fallback: use the table dispatch
   auto* table = c.get_host_table();
   uint32_t mapped_index = c.get_module().import_functions[func_idx];
   try {
      auto result = table->call(c.get_host_ptr(), mapped_index, args, c.linear_memory());
      return result.i64;
   } catch (...) {
      escape_exception(std::current_exception());
   }
}

// Nothrow variant for LLVM JIT path where .eh_frame data is registered.
// Exceptions propagate naturally through LLVM frames without siglongjmp.
int64_t __psizam_call_host_nothrow(void* ctx, uint32_t func_idx,
                                    void* args_buf, uint32_t nargs) {
   auto& c = as_ctx(ctx);
   auto* args = static_cast<native_value*>(args_buf);

   if (c._host_trampoline_ptrs) {
      auto trampoline = c._host_trampoline_ptrs[func_idx];
      if (trampoline) {
         return trampoline(c.get_host_ptr(), args, c.linear_memory()).i64;
      }
   }

   auto* table = c.get_host_table();
   uint32_t mapped_index = c.get_module().import_functions[func_idx];
   return table->call(c.get_host_ptr(), mapped_index, args, c.linear_memory()).i64;
}

// Combined host call: call_depth_dec + call_host + call_depth_inc + get_memory in one.
// Reduces 4 extern "C" calls to 1, eliminating ~15ns of function call overhead.
int64_t __psizam_call_host_full(void* ctx, uint32_t func_idx,
                                 void* args_buf, uint32_t nargs,
                                 void** mem_out) {
   auto& c = as_ctx(ctx);
   auto* args = static_cast<native_value*>(args_buf);

   // Inline call_depth_dec
   uint32_t depth = c.get_remaining_call_depth();
   if (depth <= 1)
      escape_or_throw<wasm_interpreter_exception>("stack overflow");
   c.set_max_call_depth(depth - 1);

   // Call host function
   int64_t result;
   if (c._host_trampoline_ptrs) {
      auto trampoline = c._host_trampoline_ptrs[func_idx];
      if (trampoline) {
         result = trampoline(c.get_host_ptr(), args, c.linear_memory()).i64;
         goto done;
      }
   }
   {
      auto* table = c.get_host_table();
      uint32_t mapped_index = c.get_module().import_functions[func_idx];
      result = table->call(c.get_host_ptr(), mapped_index, args, c.linear_memory()).i64;
   }

done:
   // Inline call_depth_inc
   c.set_max_call_depth(c.get_remaining_call_depth() + 1);

   // Inline get_memory
   *mem_out = c.linear_memory();

   return result;
}

void __psizam_memory_init(void* ctx, uint32_t seg_idx,
                           uint32_t dest, uint32_t src, uint32_t n) {
   as_ctx(ctx).init_linear_memory(seg_idx, dest, src, n);
}

void __psizam_data_drop(void* ctx, uint32_t seg_idx) {
   as_ctx(ctx).drop_data(seg_idx);
}

void __psizam_memory_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n) {
   auto& c = as_ctx(ctx);
   char* mem = c.linear_memory();
   uint32_t mem_size = static_cast<uint32_t>(c.current_linear_memory()) * 65536u;
   // Spec: trap if dest + n > |mem| or src + n > |mem| (even when n=0)
   if (uint64_t(dest) + n > mem_size || uint64_t(src) + n > mem_size)
      escape_or_throw<wasm_memory_exception>("out of bounds memory access");
   if (n > 0) {
      std::memmove(mem + dest, mem + src, n);
   }
}

void __psizam_memory_fill(void* ctx, uint32_t dest, uint32_t val, uint32_t n) {
   auto& c = as_ctx(ctx);
   char* mem = c.linear_memory();
   uint32_t mem_size = static_cast<uint32_t>(c.current_linear_memory()) * 65536u;
   // Spec: trap if dest + n > |mem| (even when n=0)
   if (uint64_t(dest) + n > mem_size)
      escape_or_throw<wasm_memory_exception>("out of bounds memory access");
   if (n > 0) {
      std::memset(mem + dest, static_cast<uint8_t>(val), n);
   }
}

void __psizam_elem_drop(void* ctx, uint32_t seg_idx) {
   as_ctx(ctx).drop_elem(seg_idx);
}

int64_t __psizam_call_indirect(void* ctx, void* mem, uint32_t type_idx,
                                uint32_t table_elem, void* args_buf, uint32_t nargs) {
   // type_idx packs: type index in lower 16 bits, table index in upper 16 bits
   uint32_t table_idx = type_idx >> 16;
   type_idx &= 0xFFFF;
   auto& c = as_ctx(ctx);
   auto& mod = c.get_module();

   if (table_idx >= mod.tables.size())
      escape_or_throw<wasm_interpreter_exception>("no table");
   uint32_t table_size = c.get_table_size(table_idx);

   if (table_elem >= table_size)
      escape_or_throw<wasm_interpreter_exception>("undefined element");

   // Get table entry
   auto* table_base = c.get_table_base(table_idx);
   auto& entry = table_base[table_elem];

   // Null/uninitialized check (0xFFFFFFFF is the sentinel)
   if (entry.type == 0xFFFFFFFF)
      escape_or_throw<wasm_interpreter_exception>("undefined element");

   // Type check
   const auto& expected_type = mod.types[type_idx];
   const auto& actual_type = mod.get_function_type(entry.index);
   if (!(actual_type == expected_type))
      escape_or_throw<wasm_interpreter_exception>("indirect call type mismatch");

   // Call the function
   uint32_t num_imports = mod.get_imported_functions_size();
   if (entry.index < num_imports) {
      // Host function call — __psizam_call_host already handles escape
      return __psizam_call_host(ctx, entry.index, args_buf, nargs);
   } else {
      // WASM function — call through the entry wrapper
      // Wrap in try/catch to escape if the callee throws via a runtime helper
      uint32_t code_idx = entry.index - num_imports;
      auto offset = mod.code[code_idx].jit_code_offset;
      using llvm_entry_fn_t = int64_t(*)(void*, void*, native_value*);
      auto fn = reinterpret_cast<llvm_entry_fn_t>(offset);
      try {
         return fn(ctx, mem, static_cast<native_value*>(args_buf));
      } catch (...) {
         escape_exception(std::current_exception());
      }
   }
}

// Nothrow variant of call_indirect for LLVM JIT path.
int64_t __psizam_call_indirect_nothrow(void* ctx, void* mem, uint32_t type_idx,
                                        uint32_t table_elem, void* args_buf, uint32_t nargs) {
   uint32_t table_idx = type_idx >> 16;
   type_idx &= 0xFFFF;
   auto& c = as_ctx(ctx);
   auto& mod = c.get_module();

   if (table_idx >= mod.tables.size())
      throw wasm_interpreter_exception{"no table"};
   uint32_t table_size = c.get_table_size(table_idx);

   if (table_elem >= table_size)
      throw wasm_interpreter_exception{"undefined element"};

   auto* table_base = c.get_table_base(table_idx);
   auto& entry = table_base[table_elem];

   if (entry.type == 0xFFFFFFFF)
      throw wasm_interpreter_exception{"undefined element"};

   const auto& expected_type = mod.types[type_idx];
   const auto& actual_type = mod.get_function_type(entry.index);
   if (!(actual_type == expected_type))
      throw wasm_interpreter_exception{"indirect call type mismatch"};

   uint32_t num_imports = mod.get_imported_functions_size();
   if (entry.index < num_imports) {
      return __psizam_call_host_nothrow(ctx, entry.index, args_buf, nargs);
   } else {
      uint32_t code_idx = entry.index - num_imports;
      auto offset = mod.code[code_idx].jit_code_offset;
      using llvm_entry_fn_t = int64_t(*)(void*, void*, native_value*);
      auto fn = reinterpret_cast<llvm_entry_fn_t>(offset);
      return fn(ctx, mem, static_cast<native_value*>(args_buf));
   }
}

void __psizam_call_depth_dec(void* ctx) {
   auto& c = as_ctx(ctx);
   uint32_t depth = c.get_remaining_call_depth();
   if (depth == 0) {
      escape_or_throw<wasm_interpreter_exception>("stack overflow");
   }
   uint32_t new_depth = depth - 1;
   c.set_max_call_depth(new_depth);
   if (new_depth == 0) {
      escape_or_throw<wasm_interpreter_exception>("stack overflow");
   }
}

void __psizam_call_depth_inc(void* ctx) {
   auto& c = as_ctx(ctx);
   c.set_max_call_depth(c.get_remaining_call_depth() + 1);
}

void __psizam_trap(void* ctx, uint32_t trap_code) {
   const char* msg;
   switch (trap_code) {
      case 0:  msg = "unreachable"; break;
      case 1:  msg = "integer divide by zero"; break;
      case 2:  msg = "integer overflow"; break;
      case 3:  msg = "integer overflow"; break;
      case 4:  msg = "undefined element"; break;
      case 5:  // memory OOB — use wasm_memory_exception
         escape_or_throw<wasm_memory_exception>("out of bounds memory access");
      default: msg = "unknown trap"; break;
   }
   escape_or_throw<wasm_interpreter_exception>(msg);
}

} // extern "C"
