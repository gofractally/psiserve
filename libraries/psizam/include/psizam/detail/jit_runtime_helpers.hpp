#pragma once

// Runtime helper functions for the x86_64 JIT (jit2) backend.
//
// These are called from JIT-generated native code via function-pointer
// relocations (see pzam_cache.hpp / reloc_symbol). They are the JIT
// counterpart of libraries/psizam/src/llvm_runtime_helpers.cpp for the
// LLVM backend; the two files share a role but differ in exception
// policy: jit2's emitted code has no .eh_frame, so every throwable
// helper must escape via signal_throw / longjmp_on_exception (both
// resolve to longjmp through trap_jmp_ptr) rather than a C++ throw.
//
// Historically these lived as private static members of
// `psizam::detail::jit_codegen`. That made them invisible to
// pzam_cache.hpp when it started building the reloc-symbol table from
// outside the class, producing a cascade of access-violation errors.
// Moving them here (free functions in `psizam::detail`) removes the
// visibility coupling and mirrors the LLVM backend's split.

#include <psizam/detail/execution_context.hpp>
#include <psizam/detail/signals.hpp>
#include <psizam/detail/softfloat.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/types.hpp>

#include <cstdint>
#include <cstring>

namespace psizam::detail {

// ── Host call + memory size ──────────────────────────────────────────

// Host call with fast-trampoline dispatch. .eh_frame is registered for
// JIT code, so C++ exceptions propagate naturally through JIT frames.
// Args are already in forward order (packed by the JIT stub).
// remaining_stack is synced to context for recursive host→WASM calls.
inline native_value call_host_function(void* ctx, native_value* args,
                                       uint32_t idx, uint32_t remaining_stack) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   auto saved = context->_remaining_call_depth;
   context->_remaining_call_depth = remaining_stack;

   native_value result;
   if (context->_host_trampoline_ptrs) {
      auto trampoline = context->_host_trampoline_ptrs[idx];
      if (trampoline) {
         result = trampoline(context->get_host_ptr(), args, context->linear_memory());
         context->_remaining_call_depth = saved;
         return result;
      }
   }
   uint32_t mapped_index = context->_mod->import_functions[idx];
   result = context->_table->call(context->get_host_ptr(), mapped_index, args,
                                  context->linear_memory());
   context->_remaining_call_depth = saved;
   return result;
}

// Names carry the `_impl` suffix because the plain names `current_memory`
// and `grow_memory` already name enumerators of the unscoped `enum opcodes`
// that also lives in `psizam::detail`; without the suffix the free
// functions would shadow (and clash with) the enumerators at
// unqualified lookup from any TU that includes opcodes.hpp. Matches the
// `_impl` convention on the other helpers in this file.
inline int32_t current_memory_impl(void* ctx) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   return context->current_linear_memory();
}
inline int64_t current_memory64_impl(void* ctx) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   return static_cast<int64_t>(context->current_linear_memory());
}
inline int32_t grow_memory_impl(void* ctx, int32_t pages) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   return context->grow_linear_memory(pages);
}
inline int64_t grow_memory64_impl(void* ctx, int64_t pages) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   int32_t result = context->grow_linear_memory(static_cast<int32_t>(pages));
   return (result == -1) ? int64_t(-1) : static_cast<int64_t>(result);
}

// ── Trap handlers (long-jump escape) ────────────────────────────────

inline void on_memory_error()        { signal_throw<wasm_memory_exception>("wasm memory out-of-bounds"); }
inline void on_unreachable()         { signal_throw<wasm_interpreter_exception>("unreachable"); }
inline void on_fp_error()            { signal_throw<wasm_interpreter_exception>("floating point error"); }
inline void on_call_indirect_error() { signal_throw<wasm_interpreter_exception>("call_indirect out of range"); }
inline void on_type_error()          { signal_throw<wasm_interpreter_exception>("call_indirect incorrect function type"); }
inline void on_stack_overflow()      { signal_throw<wasm_interpreter_exception>("stack overflow"); }

// ── Bulk memory / table ops (may throw → longjmp) ───────────────────
//
// Explicit bounds checking; longjmp_on_exception routes the throw
// through trap_jmp_ptr so jit2's no-.eh_frame code doesn't need to
// unwind a C++ stack it can't unwind.

inline void memory_fill_impl(void* ctx, uint32_t dest, uint32_t val, uint32_t count) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   longjmp_on_exception([&]() {
      uint64_t end = static_cast<uint64_t>(dest) + count;
      uint64_t mem_size = static_cast<uint64_t>(context->current_linear_memory()) * 65536ULL;
      if (end > mem_size)
         signal_throw<wasm_memory_exception>("memory.fill out of bounds");
      if (count > 0)
         std::memset(context->linear_memory() + dest, static_cast<uint8_t>(val), count);
   });
}

inline void memory_copy_impl(void* ctx, uint32_t dest, uint32_t src, uint32_t count) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   longjmp_on_exception([&]() {
      uint64_t src_end = static_cast<uint64_t>(src) + count;
      uint64_t dst_end = static_cast<uint64_t>(dest) + count;
      uint64_t mem_size = static_cast<uint64_t>(context->current_linear_memory()) * 65536ULL;
      if (src_end > mem_size || dst_end > mem_size)
         signal_throw<wasm_memory_exception>("memory.copy out of bounds");
      if (count > 0)
         std::memmove(context->linear_memory() + dest, context->linear_memory() + src, count);
   });
}

inline void memory_init_impl(void* ctx, uint32_t seg_idx, uint32_t dest,
                             uint32_t src, uint32_t count) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   longjmp_on_exception([&]() {
      context->init_linear_memory(seg_idx, dest, src, count);
   });
}

inline void data_drop_impl(void* ctx, uint32_t seg_idx) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   longjmp_on_exception([&]() {
      context->drop_data(seg_idx);
   });
}

inline void table_init_impl(void* ctx, uint32_t packed_idx, uint32_t dest,
                            uint32_t src, uint32_t count) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   uint32_t seg_idx   = packed_idx & 0xFFFF;
   uint32_t table_idx = packed_idx >> 16;
   longjmp_on_exception([&]() {
      context->init_table(seg_idx, dest, src, count, table_idx);
   });
}

inline void elem_drop_impl(void* ctx, uint32_t seg_idx) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   longjmp_on_exception([&]() {
      context->drop_elem(seg_idx);
   });
}

inline void table_copy_impl(void* ctx, uint32_t dest, uint32_t src,
                            uint32_t count, uint32_t packed_tables) {
   auto* context = static_cast<jit_execution_context<false>*>(ctx);
   uint32_t dst_table = packed_tables & 0xFFFF;
   uint32_t src_table = packed_tables >> 16;
   longjmp_on_exception([&]() {
      auto* s = context->get_table_ptr(src, count, src_table);
      auto* d = context->get_table_ptr(dest, count, dst_table);
      if (count > 0)
         std::memmove(d, s, count * sizeof(table_entry));
   });
}

// ── Saturating float-to-int (no trap, clamp to min/max, NaN→0) ──────

inline uint64_t trunc_sat_f32_i32s(uint64_t v) { float f; std::memcpy(&f,&v,4); if (f!=f) return 0; if (f >=  2147483648.0f) return (uint32_t)INT32_MAX; if (f <= -2147483649.0f) return (uint32_t)INT32_MIN; return (uint32_t)(int32_t)f; }
inline uint64_t trunc_sat_f32_i32u(uint64_t v) { float f; std::memcpy(&f,&v,4); if (f!=f) return 0; if (f >=  4294967296.0f) return UINT32_MAX;           if (f <= -1.0f)           return 0;                    return (uint32_t)f; }
inline uint64_t trunc_sat_f64_i32s(uint64_t v) { double f; std::memcpy(&f,&v,8); if (f!=f) return 0; if (f >=  2147483648.0)  return (uint32_t)INT32_MAX; if (f <= -2147483649.0)  return (uint32_t)INT32_MIN; return (uint32_t)(int32_t)f; }
inline uint64_t trunc_sat_f64_i32u(uint64_t v) { double f; std::memcpy(&f,&v,8); if (f!=f) return 0; if (f >=  4294967296.0)  return UINT32_MAX;           if (f <= -1.0)           return 0;                    return (uint32_t)f; }
inline uint64_t trunc_sat_f32_i64s(uint64_t v) { float f; std::memcpy(&f,&v,4); if (f!=f) return 0; if (f >=  9223372036854775808.0f) return (uint64_t)INT64_MAX; if (f <= -9223372036854775809.0f) return (uint64_t)INT64_MIN; return (uint64_t)(int64_t)f; }
inline uint64_t trunc_sat_f32_i64u(uint64_t v) { float f; std::memcpy(&f,&v,4); if (f!=f) return 0; if (f >= 18446744073709551616.0f) return UINT64_MAX;           if (f <= -1.0f)                  return 0;                    return (uint64_t)f; }
inline uint64_t trunc_sat_f64_i64s(uint64_t v) { double f; std::memcpy(&f,&v,8); if (f!=f) return 0; if (f >=  9223372036854775808.0) return (uint64_t)INT64_MAX; if (f <= -9223372036854775809.0) return (uint64_t)INT64_MIN; return (uint64_t)(int64_t)f; }
inline uint64_t trunc_sat_f64_i64u(uint64_t v) { double f; std::memcpy(&f,&v,8); if (f!=f) return 0; if (f >= 18446744073709551616.0) return UINT64_MAX;           if (f <= -1.0)                   return 0;                    return (uint64_t)f; }

// ── Trapping float-to-int conversions ───────────────────────────────
//
// Use signal_throw (longjmp-based) for NaN/overflow traps instead of
// the underlying softfloat helper's PSIZAM_ASSERT (throw-based). The
// throw path requires C++ exception unwinding through the JIT's
// host-call frame (emit_c_call), which has no .eh_frame data in jit2's
// generated code — fuzz testing showed this faulted in libgcc's
// _Unwind_RaiseException (misaligned movaps during its own frame
// setup). signal_throw sidesteps unwinding entirely by longjmping
// straight to trap_jmp_ptr.

inline uint64_t trunc_f32_i32s(uint64_t v) {
   float f; std::memcpy(&f, &v, 4);
   if (is_nan(to_softfloat32(f)))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_s/i32 unrepresentable");
   if (_psizam_f32_ge(f, 2147483648.0f) || _psizam_f32_lt(f, -2147483648.0f))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_s/i32 overflow");
   return static_cast<uint32_t>(f32_to_i32(to_softfloat32(_psizam_f32_trunc<false>(f)), 0, false));
}
inline uint64_t trunc_f32_i32u(uint64_t v) {
   float f; std::memcpy(&f, &v, 4);
   if (is_nan(to_softfloat32(f)))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_u/i32 unrepresentable");
   if (_psizam_f32_ge(f, 4294967296.0f) || _psizam_f32_le(f, -1.0f))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_u/i32 overflow");
   return f32_to_ui32(to_softfloat32(_psizam_f32_trunc<false>(f)), 0, false);
}
inline uint64_t trunc_f64_i32s(uint64_t v) {
   double f; std::memcpy(&f, &v, 8);
   if (is_nan(to_softfloat64(f)))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_s/i32 unrepresentable");
   if (_psizam_f64_ge(f, 2147483648.0) || _psizam_f64_le(f, -2147483649.0))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_s/i32 overflow");
   return static_cast<uint32_t>(f64_to_i32(to_softfloat64(_psizam_f64_trunc<false>(f)), 0, false));
}
inline uint64_t trunc_f64_i32u(uint64_t v) {
   double f; std::memcpy(&f, &v, 8);
   if (is_nan(to_softfloat64(f)))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_u/i32 unrepresentable");
   if (_psizam_f64_ge(f, 4294967296.0) || _psizam_f64_le(f, -1.0))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_u/i32 overflow");
   return f64_to_ui32(to_softfloat64(_psizam_f64_trunc<false>(f)), 0, false);
}
inline uint64_t trunc_f32_i64s(uint64_t v) {
   float f; std::memcpy(&f, &v, 4);
   if (is_nan(to_softfloat32(f)))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_s/i64 unrepresentable");
   if (_psizam_f32_ge(f, 9223372036854775808.0f) || _psizam_f32_lt(f, -9223372036854775808.0f))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_s/i64 overflow");
   return static_cast<uint64_t>(f32_to_i64(to_softfloat32(_psizam_f32_trunc<false>(f)), 0, false));
}
inline uint64_t trunc_f32_i64u(uint64_t v) {
   float f; std::memcpy(&f, &v, 4);
   if (is_nan(to_softfloat32(f)))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_u/i64 unrepresentable");
   if (_psizam_f32_ge(f, 18446744073709551616.0f) || _psizam_f32_le(f, -1.0f))
      signal_throw<wasm_interpreter_exception>("Error, f32.convert_u/i64 overflow");
   return f32_to_ui64(to_softfloat32(_psizam_f32_trunc<false>(f)), 0, false);
}
inline uint64_t trunc_f64_i64s(uint64_t v) {
   double f; std::memcpy(&f, &v, 8);
   if (is_nan(to_softfloat64(f)))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_s/i64 unrepresentable");
   if (_psizam_f64_ge(f, 9223372036854775808.0) || _psizam_f64_lt(f, -9223372036854775808.0))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_s/i64 overflow");
   return static_cast<uint64_t>(f64_to_i64(to_softfloat64(_psizam_f64_trunc<false>(f)), 0, false));
}
inline uint64_t trunc_f64_i64u(uint64_t v) {
   double f; std::memcpy(&f, &v, 8);
   if (is_nan(to_softfloat64(f)))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_u/i64 unrepresentable");
   if (_psizam_f64_ge(f, 18446744073709551616.0) || _psizam_f64_le(f, -1.0))
      signal_throw<wasm_interpreter_exception>("Error, f64.convert_u/i64 overflow");
   return f64_to_ui64(to_softfloat64(_psizam_f64_trunc<false>(f)), 0, false);
}

} // namespace psizam::detail
