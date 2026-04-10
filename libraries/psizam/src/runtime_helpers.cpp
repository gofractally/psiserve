// Runtime helper functions for JIT backends.
// These are called from JIT-generated native code (both custom JIT and LLVM backends).
// They do NOT depend on LLVM — they only use psizam core types.

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

const void* __psizam_resolve_indirect(void* ctx, uint32_t type_idx,
                                       uint32_t table_idx, uint32_t elem_idx) {
   auto& c = as_ctx(ctx);
   auto& mod = c.get_module();

   if (table_idx >= mod.tables.size())
      return nullptr; // triggers trap in caller
   uint32_t table_size = c.get_table_size(table_idx);
   if (elem_idx >= table_size)
      return nullptr; // triggers trap in caller

   auto* table_base = c.get_table_base(table_idx);
   auto& entry = table_base[elem_idx];

   // Null/uninitialized check (0xFFFFFFFF is the sentinel)
   if (entry.type == 0xFFFFFFFF)
      return nullptr; // triggers trap in caller

   // Type check
   const auto& expected_type = mod.types[type_idx];
   const auto& actual_type = mod.get_function_type(entry.index);
   if (!(actual_type == expected_type))
      return nullptr; // triggers trap in caller

   // For imported functions, code_ptr points to the JIT-generated import thunk.
   // For WASM functions, code_ptr points to the JIT-compiled function body.
   return entry.code_ptr;
}

void __psizam_table_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n,
                          uint32_t dst_table, uint32_t src_table) {
   auto& c = as_ctx(ctx);
   auto* s = c.get_table_ptr(src, n, src_table);
   auto* d = c.get_table_ptr(dest, n, dst_table);
   if (n > 0)
      std::memmove(d, s, n * sizeof(psizam::table_entry));
}

void __psizam_table_init(void* ctx, uint32_t elem_idx,
                          uint32_t dest, uint32_t src, uint32_t n,
                          uint32_t table_idx) {
   as_ctx(ctx).init_table(elem_idx, dest, src, n, table_idx);
}

} // extern "C"
