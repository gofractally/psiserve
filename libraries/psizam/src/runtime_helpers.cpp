// Runtime helper functions for JIT backends.
// These are called from JIT-generated native code (both custom JIT and LLVM backends).
// They do NOT depend on LLVM — they only use psizam core types.
//
// See llvm_runtime_helpers.cpp for the exception escape rationale.

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
      std::memmove(d, s, n * sizeof(table_entry));
}

void __psizam_table_init(void* ctx, uint32_t elem_idx,
                          uint32_t dest, uint32_t src, uint32_t n,
                          uint32_t table_idx) {
   as_ctx(ctx).init_table(elem_idx, dest, src, n, table_idx);
}

uint32_t __psizam_table_get(void* ctx, uint32_t table_idx, uint32_t elem_idx) {
   auto& c = as_ctx(ctx);
   if (elem_idx >= c.get_table_size(table_idx))
      escape_or_throw<wasm_interpreter_exception>("table index out of range");
   return c.get_table_base(table_idx)[elem_idx].index;
}

void __psizam_table_set(void* ctx, uint32_t table_idx, uint32_t elem_idx, uint32_t val) {
   auto& c = as_ctx(ctx);
   if (elem_idx >= c.get_table_size(table_idx))
      escape_or_throw<wasm_interpreter_exception>("table index out of range");
   auto& entry = c.get_table_base(table_idx)[elem_idx];
   entry.index = val;
   entry.type = UINT32_MAX;
   entry.code_ptr = nullptr;
}

uint32_t __psizam_table_grow(void* ctx, uint32_t table_idx, uint32_t delta, uint32_t init_val) {
   auto& c = as_ctx(ctx);
   table_entry te;
   te.type = UINT32_MAX;
   te.index = init_val;
   te.code_ptr = nullptr;
   return c.table_grow(table_idx, delta, te);
}

uint32_t __psizam_table_size(void* ctx, uint32_t table_idx) {
   return as_ctx(ctx).get_table_size(table_idx);
}

void __psizam_table_fill(void* ctx, uint32_t table_idx, uint32_t i, uint32_t val, uint32_t n) {
   auto& c = as_ctx(ctx);
   table_entry te;
   te.type = UINT32_MAX;
   te.index = val;
   te.code_ptr = nullptr;
   c.table_fill(table_idx, i, te, n);
}

uint64_t __psizam_atomic_rmw(void* ctx, uint8_t sub, uint32_t addr, uint32_t offset,
                              uint64_t val1, uint64_t val2) {
   auto& c = as_ctx(ctx);
   char* ptr = c.linear_memory() + addr + offset;
   auto asub = static_cast<atomic_sub>(sub);

   auto rmw = [&](auto load_fn, auto store_fn, auto op) -> uint64_t {
      auto old = load_fn(ptr);
      store_fn(ptr, op(old, val1));
      return static_cast<uint64_t>(old);
   };

   switch(asub) {
   // Add
   case atomic_sub::i32_atomic_rmw_add:     { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o+uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   case atomic_sub::i64_atomic_rmw_add:     { uint64_t o; std::memcpy(&o,ptr,8); uint64_t n=o+val1; std::memcpy(ptr,&n,8); return o; }
   case atomic_sub::i32_atomic_rmw8_add_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o+uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i32_atomic_rmw16_add_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o+uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw8_add_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o+uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i64_atomic_rmw16_add_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o+uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw32_add_u: { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o+uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   // Sub
   case atomic_sub::i32_atomic_rmw_sub:     { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o-uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   case atomic_sub::i64_atomic_rmw_sub:     { uint64_t o; std::memcpy(&o,ptr,8); uint64_t n=o-val1; std::memcpy(ptr,&n,8); return o; }
   case atomic_sub::i32_atomic_rmw8_sub_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o-uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i32_atomic_rmw16_sub_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o-uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw8_sub_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o-uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i64_atomic_rmw16_sub_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o-uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw32_sub_u: { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o-uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   // And
   case atomic_sub::i32_atomic_rmw_and:     { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o&uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   case atomic_sub::i64_atomic_rmw_and:     { uint64_t o; std::memcpy(&o,ptr,8); uint64_t n=o&val1; std::memcpy(ptr,&n,8); return o; }
   case atomic_sub::i32_atomic_rmw8_and_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o&uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i32_atomic_rmw16_and_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o&uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw8_and_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o&uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i64_atomic_rmw16_and_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o&uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw32_and_u: { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o&uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   // Or
   case atomic_sub::i32_atomic_rmw_or:      { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o|uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   case atomic_sub::i64_atomic_rmw_or:      { uint64_t o; std::memcpy(&o,ptr,8); uint64_t n=o|val1; std::memcpy(ptr,&n,8); return o; }
   case atomic_sub::i32_atomic_rmw8_or_u:   { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o|uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i32_atomic_rmw16_or_u:  { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o|uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw8_or_u:   { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o|uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i64_atomic_rmw16_or_u:  { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o|uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw32_or_u:  { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o|uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   // Xor
   case atomic_sub::i32_atomic_rmw_xor:     { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o^uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   case atomic_sub::i64_atomic_rmw_xor:     { uint64_t o; std::memcpy(&o,ptr,8); uint64_t n=o^val1; std::memcpy(ptr,&n,8); return o; }
   case atomic_sub::i32_atomic_rmw8_xor_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o^uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i32_atomic_rmw16_xor_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o^uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw8_xor_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=o^uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i64_atomic_rmw16_xor_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=o^uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw32_xor_u: { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=o^uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   // Xchg
   case atomic_sub::i32_atomic_rmw_xchg:     { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   case atomic_sub::i64_atomic_rmw_xchg:     { uint64_t o; std::memcpy(&o,ptr,8); std::memcpy(ptr,&val1,8); return o; }
   case atomic_sub::i32_atomic_rmw8_xchg_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i32_atomic_rmw16_xchg_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw8_xchg_u:  { uint8_t o; std::memcpy(&o,ptr,1); uint8_t n=uint8_t(val1); std::memcpy(ptr,&n,1); return o; }
   case atomic_sub::i64_atomic_rmw16_xchg_u: { uint16_t o; std::memcpy(&o,ptr,2); uint16_t n=uint16_t(val1); std::memcpy(ptr,&n,2); return o; }
   case atomic_sub::i64_atomic_rmw32_xchg_u: { uint32_t o; std::memcpy(&o,ptr,4); uint32_t n=uint32_t(val1); std::memcpy(ptr,&n,4); return o; }
   // Cmpxchg: val1=expected, val2=replacement
   case atomic_sub::i32_atomic_rmw_cmpxchg:     { uint32_t o; std::memcpy(&o,ptr,4); if(o==uint32_t(val1)){uint32_t r=uint32_t(val2);std::memcpy(ptr,&r,4);} return o; }
   case atomic_sub::i64_atomic_rmw_cmpxchg:     { uint64_t o; std::memcpy(&o,ptr,8); if(o==val1)std::memcpy(ptr,&val2,8); return o; }
   case atomic_sub::i32_atomic_rmw8_cmpxchg_u:  { uint8_t o; std::memcpy(&o,ptr,1); if(o==uint8_t(val1)){uint8_t r=uint8_t(val2);std::memcpy(ptr,&r,1);} return o; }
   case atomic_sub::i32_atomic_rmw16_cmpxchg_u: { uint16_t o; std::memcpy(&o,ptr,2); if(o==uint16_t(val1)){uint16_t r=uint16_t(val2);std::memcpy(ptr,&r,2);} return o; }
   case atomic_sub::i64_atomic_rmw8_cmpxchg_u:  { uint8_t o; std::memcpy(&o,ptr,1); if(o==uint8_t(val1)){uint8_t r=uint8_t(val2);std::memcpy(ptr,&r,1);} return o; }
   case atomic_sub::i64_atomic_rmw16_cmpxchg_u: { uint16_t o; std::memcpy(&o,ptr,2); if(o==uint16_t(val1)){uint16_t r=uint16_t(val2);std::memcpy(ptr,&r,2);} return o; }
   case atomic_sub::i64_atomic_rmw32_cmpxchg_u: { uint32_t o; std::memcpy(&o,ptr,4); if(o==uint32_t(val1)){uint32_t r=uint32_t(val2);std::memcpy(ptr,&r,4);} return o; }
   default: return 0;
   }
}

void __psizam_gas_charge(void* ctx, int64_t cost) {
   // All three primary JIT backends (jit, jit2, jit_llvm) use
   // jit_execution_context<false>. jit_profile uses <true> but doesn't
   // opt into gas metering in the current scope.
   as_ctx(ctx).gas_charge(cost);
}

} // extern "C"
