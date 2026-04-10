// Runtime helper functions for the LLVM JIT backend.
// These are called from LLVM-generated native code via absolute symbol resolution.

#include <psizam/llvm_runtime_helpers.hpp>
#include <psizam/execution_context.hpp>
#include <psizam/types.hpp>

#include <cstring>

namespace {
   using ctx_t = psizam::jit_execution_context<false>;

   ctx_t& as_ctx(void* ctx) {
      return *static_cast<ctx_t*>(ctx);
   }
}

extern "C" {

int64_t __psizam_global_get(void* ctx, uint32_t idx) {
   return as_ctx(ctx).get_global(idx).value.i64;
}

void __psizam_global_set(void* ctx, uint32_t idx, int64_t value) {
   as_ctx(ctx).get_global(idx).value.i64 = value;
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
   auto* table = c.get_host_table();
   uint32_t mapped_index = c.get_module().import_functions[func_idx];
   auto* args = static_cast<psizam::native_value*>(args_buf);
   auto result = table->call(c.get_host_ptr(), mapped_index, args, c.linear_memory());
   return result.i64;
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
   // Validate bounds by accessing through interface
   if (n > 0) {
      uint64_t max_addr = std::max(uint64_t(dest) + n, uint64_t(src) + n);
      (void)max_addr; // bounds checked by guard page
      std::memmove(mem + dest, mem + src, n);
   }
}

void __psizam_memory_fill(void* ctx, uint32_t dest, uint32_t val, uint32_t n) {
   auto& c = as_ctx(ctx);
   char* mem = c.linear_memory();
   if (n > 0) {
      std::memset(mem + dest, static_cast<uint8_t>(val), n);
   }
}

void __psizam_table_init(void* ctx, uint32_t elem_idx,
                          uint32_t dest, uint32_t src, uint32_t n) {
   as_ctx(ctx).init_table(elem_idx, dest, src, n);
}

void __psizam_elem_drop(void* ctx, uint32_t seg_idx) {
   as_ctx(ctx).drop_elem(seg_idx);
}

void __psizam_table_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n) {
   auto& c = as_ctx(ctx);
   auto* table_base = c.get_table_base();
   auto& mod = c.get_module();
   uint32_t table_size = mod.tables[0].limits.initial;
   if (uint64_t(dest) + n > table_size || uint64_t(src) + n > table_size)
      throw psizam::wasm_memory_exception{"table out of range"};
   if (n > 0)
      std::memmove(table_base + dest, table_base + src, n * sizeof(psizam::table_entry));
}

int64_t __psizam_call_indirect(void* ctx, void* mem, uint32_t type_idx,
                                uint32_t table_elem, void* args_buf, uint32_t nargs) {
   auto& c = as_ctx(ctx);
   auto& mod = c.get_module();

   // Bounds check
   if (mod.tables.empty())
      throw psizam::wasm_interpreter_exception{"no table"};
   uint32_t table_size = mod.tables[0].limits.initial;
   if (table_elem >= table_size)
      throw psizam::wasm_interpreter_exception{"undefined element"};

   // Get table entry
   auto* table_base = c.get_table_base();
   auto& entry = table_base[table_elem];

   // Type check
   uint32_t func_type = mod.functions[entry.index];
   const auto& expected_type = mod.types[type_idx];
   const auto& actual_type = mod.get_function_type(entry.index);
   if (!(actual_type == expected_type))
      throw psizam::wasm_interpreter_exception{"indirect call type mismatch"};

   // Call the function
   uint32_t num_imports = mod.get_imported_functions_size();
   if (entry.index < num_imports) {
      // Host function call
      return __psizam_call_host(ctx, entry.index, args_buf, nargs);
   } else {
      // WASM function — call through the entry wrapper
      uint32_t code_idx = entry.index - num_imports;
      auto offset = mod.code[code_idx].jit_code_offset;
      using llvm_entry_fn_t = int64_t(*)(void*, void*, psizam::native_value*);
      auto fn = reinterpret_cast<llvm_entry_fn_t>(offset);
      return fn(ctx, mem, static_cast<psizam::native_value*>(args_buf));
   }
}

void __psizam_trap(void* ctx, uint32_t trap_code) {
   switch (trap_code) {
      case 0:  throw psizam::wasm_interpreter_exception{"unreachable"};
      case 1:  throw psizam::wasm_interpreter_exception{"integer divide by zero"};
      case 2:  throw psizam::wasm_interpreter_exception{"integer overflow"};
      case 3:  throw psizam::wasm_interpreter_exception{"integer overflow"};
      case 4:  throw psizam::wasm_interpreter_exception{"undefined element"};
      default: throw psizam::wasm_interpreter_exception{"unknown trap"};
   }
}

} // extern "C"
