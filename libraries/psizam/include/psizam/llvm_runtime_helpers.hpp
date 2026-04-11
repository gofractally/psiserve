#pragma once

// Runtime helper functions callable from LLVM-generated WASM code.
// These are resolved as absolute symbols by the ORC JIT.
// Each receives the jit_execution_context* as the first argument.

#include <cstdint>
#include <exception>

extern "C" {
   // Global variable access
   int64_t  __psizam_global_get(void* ctx, uint32_t idx);
   void     __psizam_global_set(void* ctx, uint32_t idx, int64_t value);
   void     __psizam_global_get_v128(void* ctx, uint32_t idx, void* out);
   void     __psizam_global_set_v128(void* ctx, uint32_t idx, const void* in);

   // Memory management
   void*    __psizam_get_memory(void* ctx);
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
                                 uint32_t dest, uint32_t src, uint32_t n,
                                 uint32_t table_idx);
   void     __psizam_elem_drop(void* ctx, uint32_t seg_idx);
   void     __psizam_table_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n,
                                uint32_t dst_table, uint32_t src_table);

   // Indirect call — returns result as i64
   int64_t  __psizam_call_indirect(void* ctx, void* mem, uint32_t type_idx,
                                    uint32_t table_elem, void* args_buf, uint32_t nargs);

   // Table element operations
   uint32_t __psizam_table_get(void* ctx, uint32_t table_idx, uint32_t elem_idx);
   void     __psizam_table_set(void* ctx, uint32_t table_idx, uint32_t elem_idx, uint32_t val);
   uint32_t __psizam_table_grow(void* ctx, uint32_t table_idx, uint32_t delta, uint32_t init_val);
   uint32_t __psizam_table_size(void* ctx, uint32_t table_idx);
   void     __psizam_table_fill(void* ctx, uint32_t table_idx, uint32_t i, uint32_t val, uint32_t n);

   // Resolve indirect call for JIT backends — performs bounds check, null check,
   // and type check. Returns the code_ptr from the table entry.
   // Throws wasm_interpreter_exception on error.
   const void* __psizam_resolve_indirect(void* ctx, uint32_t type_idx,
                                          uint32_t table_idx, uint32_t elem_idx);

   // Atomic RMW helper for single-threaded mode
   uint64_t __psizam_atomic_rmw(void* ctx, uint8_t sub, uint32_t addr, uint32_t offset,
                                 uint64_t val1, uint64_t val2);

   // Call depth tracking
   // Decrements remaining call depth by 1 and traps on overflow (stack overflow).
   void __psizam_call_depth_dec(void* ctx);
   // Increments remaining call depth by 1 (called on function return).
   void __psizam_call_depth_inc(void* ctx);

   // Trap — throws a WASM trap exception. Does not return.
   [[noreturn]] void __psizam_trap(void* ctx, uint32_t trap_code);
   // trap codes: 0=unreachable, 1=div_by_zero, 2=int_overflow,
   //             3=invalid_conversion, 4=undefined_element
}

namespace psizam {
   // Call an LLVM entry function on a dedicated stack.
   // The try/catch runs on the alternate stack, so C++ exceptions from
   // LLVM-generated code (runtime helpers like __psizam_trap) unwind correctly.
   // Stack overflow hits the guard page → SIGSEGV → signal handler → siglongjmp.
   //
   // stack_top: top of the mmap'd stack (stack grows down)
   // fn: LLVM entry wrapper: int64_t(*)(void* ctx, void* mem, native_value* args)
   // ctx, mem, args: forwarded to fn
   // exc_out: if fn throws, the exception is stored here and 0 is returned
   using llvm_entry_fn_t = int64_t(*)(void*, void*, void*);
   int64_t call_on_stack(void* stack_top, llvm_entry_fn_t fn,
                         void* ctx, void* mem, void* args,
                         std::exception_ptr* exc_out);
}
