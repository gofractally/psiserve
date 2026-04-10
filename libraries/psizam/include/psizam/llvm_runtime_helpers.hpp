#pragma once

// Runtime helper functions callable from LLVM-generated WASM code.
// These are resolved as absolute symbols by the ORC JIT.
// Each receives the jit_execution_context* as the first argument.

#include <cstdint>

extern "C" {
   // Global variable access
   int64_t  __psizam_global_get(void* ctx, uint32_t idx);
   void     __psizam_global_set(void* ctx, uint32_t idx, int64_t value);

   // Memory management
   int32_t  __psizam_memory_size(void* ctx);
   int32_t  __psizam_memory_grow(void* ctx, int32_t pages, void** new_mem_out);

   // Host function calls (imported functions)
   // Returns the result as i64. args_buf points to a native_value array.
   int64_t  __psizam_call_host(void* ctx, uint32_t func_idx,
                                void* args_buf, uint32_t nargs);

   // Bulk memory operations
   void     __psizam_memory_init(void* ctx, uint32_t seg_idx,
                                  uint32_t dest, uint32_t src, uint32_t n);
   void     __psizam_data_drop(void* ctx, uint32_t seg_idx);
   void     __psizam_memory_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n);
   void     __psizam_memory_fill(void* ctx, uint32_t dest, uint32_t val, uint32_t n);

   // Table operations
   void     __psizam_table_init(void* ctx, uint32_t elem_idx,
                                 uint32_t dest, uint32_t src, uint32_t n);
   void     __psizam_elem_drop(void* ctx, uint32_t seg_idx);
   void     __psizam_table_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n);

   // Indirect call — returns result as i64
   int64_t  __psizam_call_indirect(void* ctx, void* mem, uint32_t type_idx,
                                    uint32_t table_elem, void* args_buf, uint32_t nargs);

   // Trap — throws a WASM trap exception. Does not return.
   [[noreturn]] void __psizam_trap(void* ctx, uint32_t trap_code);
   // trap codes: 0=unreachable, 1=div_by_zero, 2=int_overflow,
   //             3=invalid_conversion, 4=undefined_element
}
