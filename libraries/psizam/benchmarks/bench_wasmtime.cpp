#ifdef BENCH_HAS_WASMTIME
#include "bench_hosts.hpp"
#include <wasmtime.h>
#include <chrono>
#include <cstdio>
#include <cstring>

// Helper: print and delete a wasmtime error
static void print_wt_error(const char* prefix, wasmtime_error_t* err) {
   wasm_message_t msg;
   wasmtime_error_message(err, &msg);
   fprintf(stderr, "%s: %.*s\n", prefix, (int)msg.size, msg.data);
   wasm_byte_vec_delete(&msg);
   wasmtime_error_delete(err);
}

// Helper: create a wasm_functype_t from param/result kinds
static wasm_functype_t* make_ft(std::initializer_list<wasm_valkind_t> params,
                                std::initializer_list<wasm_valkind_t> results) {
   wasm_valtype_vec_t pv, rv;
   wasm_valtype_vec_new_uninitialized(&pv, params.size());
   wasm_valtype_vec_new_uninitialized(&rv, results.size());
   size_t i = 0;
   for (auto k : params) pv.data[i++] = wasm_valtype_new(k);
   i = 0;
   for (auto k : results) rv.data[i++] = wasm_valtype_new(k);
   return wasm_functype_new(&pv, &rv);
}

// ============================================================================
// Host-call benchmark callbacks
// ============================================================================

static wasm_trap_t* wt_identity(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I32;
   results[0].of.i32 = bench_host_identity(args[0].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_accumulate(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t) {
   bench_host_accumulate(args[0].of.i64);
   return nullptr;
}
static wasm_trap_t* wt_mix(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_mix(args[0].of.i64, args[1].of.i64);
   return nullptr;
}
static wasm_trap_t* wt_mem_op(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t) {
   bench_host_mem_op(args[0].of.i32, args[1].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_multi(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_multi(args[0].of.i64, args[1].of.i64, args[2].of.i32, args[3].of.i32);
   return nullptr;
}

double run_wasmtime(const std::vector<uint8_t>& wasm, const char* func, uint32_t n) {
   wasm_engine_t* engine = wasm_engine_new();
   wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
   wasmtime_context_t* ctx = wasmtime_store_context(store);
   wasmtime_linker_t* linker = wasmtime_linker_new(engine);

   wasmtime_module_t* module = nullptr;
   wasmtime_error_t* err = wasmtime_module_new(engine, wasm.data(), wasm.size(), &module);
   if (err) { print_wt_error("wasmtime module", err); wasmtime_linker_delete(linker); wasmtime_store_delete(store); wasm_engine_delete(engine); return -1; }

   // Define host functions via linker (matches by module+name)
   struct { const char* name; wasmtime_func_callback_t cb; wasm_functype_t* ft; } defs[] = {
      {"identity",   wt_identity,   make_ft({WASM_I32}, {WASM_I32})},
      {"accumulate", wt_accumulate, make_ft({WASM_I64}, {})},
      {"mix",        wt_mix,        make_ft({WASM_I64, WASM_I64}, {WASM_I64})},
      {"mem_op",     wt_mem_op,     make_ft({WASM_I32, WASM_I32}, {})},
      {"multi",      wt_multi,      make_ft({WASM_I64, WASM_I64, WASM_I32, WASM_I32}, {WASM_I64})},
   };
   for (auto& d : defs) {
      err = wasmtime_linker_define_func(linker, "env", 3, d.name, strlen(d.name), d.ft, d.cb, nullptr, nullptr);
      wasm_functype_delete(d.ft);
      if (err) { print_wt_error("wasmtime linker define", err); wasmtime_linker_delete(linker); wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine); return -1; }
   }

   wasmtime_instance_t instance;
   wasm_trap_t* trap = nullptr;
   err = wasmtime_linker_instantiate(linker, ctx, module, &instance, &trap);
   if (err || trap) {
      if (err) print_wt_error("wasmtime instantiate", err);
      if (trap) { fprintf(stderr, "wasmtime instantiate trap\n"); wasm_trap_delete(trap); }
      wasmtime_linker_delete(linker); wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
      return -1;
   }

   wasmtime_extern_t exp;
   bool found = wasmtime_instance_export_get(ctx, &instance, func, strlen(func), &exp);
   if (!found || exp.kind != WASMTIME_EXTERN_FUNC) {
      fprintf(stderr, "wasmtime: export %s not found\n", func);
      wasmtime_linker_delete(linker); wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
      return -1;
   }

   wasmtime_val_t arg = {.kind = WASMTIME_I32, .of = {.i32 = static_cast<int32_t>(n)}};
   wasmtime_val_t result;

   auto t1 = std::chrono::high_resolution_clock::now();
   err = wasmtime_func_call(ctx, &exp.of.func, &arg, 1, &result, 1, &trap);
   auto t2 = std::chrono::high_resolution_clock::now();
   if (err || trap) { fprintf(stderr, "wasmtime call error\n"); if (err) wasmtime_error_delete(err); if (trap) wasm_trap_delete(trap); }

   wasmtime_linker_delete(linker);
   wasmtime_module_delete(module);
   wasmtime_store_delete(store);
   wasm_engine_delete(engine);
   return std::chrono::duration<double, std::milli>(t2 - t1).count();
}

// ============================================================================
// ABI benchmark callbacks
// ============================================================================

static wasm_trap_t* wt_noop(void*, wasmtime_caller_t*, const wasmtime_val_t*, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I32;
   results[0].of.i32 = bench_host_noop();
   return nullptr;
}
static wasm_trap_t* wt_string_pass(void*, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   wasmtime_extern_t mem_ext;
   bool found = wasmtime_caller_export_get(caller, "memory", 6, &mem_ext);
   if (found && mem_ext.kind == WASMTIME_EXTERN_MEMORY) {
      wasmtime_context_t* ctx = wasmtime_caller_context(caller);
      g_guest_memory = wasmtime_memory_data(ctx, &mem_ext.of.memory);
      g_guest_memory_size = wasmtime_memory_data_size(ctx, &mem_ext.of.memory);
   }
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_string_pass(args[0].of.i32, args[1].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_record_flat(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_record_flat(args[0].of.i32, args[1].of.i32, args[2].of.i32, args[3].of.i64);
   return nullptr;
}
static wasm_trap_t* wt_record_mem(void*, wasmtime_caller_t* caller, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   wasmtime_extern_t mem_ext;
   bool found = wasmtime_caller_export_get(caller, "memory", 6, &mem_ext);
   if (found && mem_ext.kind == WASMTIME_EXTERN_MEMORY) {
      wasmtime_context_t* ctx = wasmtime_caller_context(caller);
      g_guest_memory = wasmtime_memory_data(ctx, &mem_ext.of.memory);
      g_guest_memory_size = wasmtime_memory_data_size(ctx, &mem_ext.of.memory);
   }
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_record_mem(args[0].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_resource_call(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_resource_call(args[0].of.i32, args[1].of.i64);
   return nullptr;
}
static wasm_trap_t* wt_resource_create(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I32;
   results[0].of.i32 = bench_host_resource_create(args[0].of.i64);
   return nullptr;
}
static wasm_trap_t* wt_resource_drop(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t*, size_t) {
   bench_host_resource_drop(args[0].of.i32);
   return nullptr;
}

// Rich type callbacks
static wasm_trap_t* wt_struct_2str(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_struct_2str(args[0].of.i32, args[1].of.i32, args[2].of.i32, args[3].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_struct_mixed(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_struct_mixed(args[0].of.i32, args[1].of.i32, args[2].of.i32,
                                               args[3].of.i64, args[4].of.i32, args[5].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_nested(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_nested(args[0].of.i32, args[1].of.i32, args[2].of.i32,
                                         args[3].of.i32, args[4].of.i32,
                                         args[5].of.i64, args[6].of.i32, args[7].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_vec_records(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_vec_records(args[0].of.i32, args[1].of.i32);
   return nullptr;
}
static wasm_trap_t* wt_many_params(void*, wasmtime_caller_t*, const wasmtime_val_t* args, size_t, wasmtime_val_t* results, size_t) {
   results[0].kind = WASMTIME_I64;
   results[0].of.i64 = bench_host_many_params(args[0].of.i32, args[1].of.i32, args[2].of.i32, args[3].of.i32,
                                              args[4].of.i64, args[5].of.i64,
                                              args[6].of.i32, args[7].of.i32, args[8].of.i32, args[9].of.i32,
                                              args[10].of.i64, args[11].of.i64);
   return nullptr;
}

double run_wasmtime_abi(const std::vector<uint8_t>& wasm, const char* func, uint32_t n) {
   wasm_engine_t* engine = wasm_engine_new();
   wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
   wasmtime_context_t* ctx = wasmtime_store_context(store);
   wasmtime_linker_t* linker = wasmtime_linker_new(engine);

   wasmtime_module_t* module = nullptr;
   wasmtime_error_t* err = wasmtime_module_new(engine, wasm.data(), wasm.size(), &module);
   if (err) { print_wt_error("wasmtime abi module", err); wasmtime_linker_delete(linker); wasmtime_store_delete(store); wasm_engine_delete(engine); return -1; }

   // ABI host functions matching bench_abi.c imports (order in WASM: noop, string_pass, record_flat, record_mem, resource_create, resource_call, resource_drop)
   struct { const char* name; wasmtime_func_callback_t cb; wasm_functype_t* ft; } defs[] = {
      {"host_noop",            wt_noop,            make_ft({}, {WASM_I32})},
      {"host_string_pass",     wt_string_pass,     make_ft({WASM_I32, WASM_I32}, {WASM_I64})},
      {"host_record_flat",     wt_record_flat,     make_ft({WASM_I32, WASM_I32, WASM_I32, WASM_I64}, {WASM_I64})},
      {"host_record_mem",      wt_record_mem,      make_ft({WASM_I32}, {WASM_I64})},
      {"host_resource_call",   wt_resource_call,   make_ft({WASM_I32, WASM_I64}, {WASM_I64})},
      {"host_resource_create", wt_resource_create, make_ft({WASM_I64}, {WASM_I32})},
      {"host_resource_drop",   wt_resource_drop,   make_ft({WASM_I32}, {})},
      // Rich type patterns
      {"host_struct_2str",     wt_struct_2str,     make_ft({WASM_I32, WASM_I32, WASM_I32, WASM_I32}, {WASM_I64})},
      {"host_struct_mixed",    wt_struct_mixed,    make_ft({WASM_I32, WASM_I32, WASM_I32, WASM_I64, WASM_I32, WASM_I32}, {WASM_I64})},
      {"host_nested",          wt_nested,          make_ft({WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I64, WASM_I32, WASM_I32}, {WASM_I64})},
      {"host_vec_records",     wt_vec_records,     make_ft({WASM_I32, WASM_I32}, {WASM_I64})},
      {"host_many_params",     wt_many_params,     make_ft({WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I64, WASM_I64, WASM_I32, WASM_I32, WASM_I32, WASM_I32, WASM_I64, WASM_I64}, {WASM_I64})},
   };
   for (auto& d : defs) {
      err = wasmtime_linker_define_func(linker, "env", 3, d.name, strlen(d.name), d.ft, d.cb, nullptr, nullptr);
      wasm_functype_delete(d.ft);
      if (err) { print_wt_error("wasmtime abi linker define", err); wasmtime_linker_delete(linker); wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine); return -1; }
   }

   wasmtime_instance_t instance;
   wasm_trap_t* trap = nullptr;
   err = wasmtime_linker_instantiate(linker, ctx, module, &instance, &trap);
   if (err || trap) {
      if (err) print_wt_error("wasmtime abi instantiate", err);
      if (trap) { fprintf(stderr, "wasmtime abi instantiate trap\n"); wasm_trap_delete(trap); }
      wasmtime_linker_delete(linker); wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
      return -1;
   }

   wasmtime_extern_t exp;
   bool found = wasmtime_instance_export_get(ctx, &instance, func, strlen(func), &exp);
   if (!found || exp.kind != WASMTIME_EXTERN_FUNC) {
      fprintf(stderr, "wasmtime abi: export %s not found\n", func);
      wasmtime_linker_delete(linker); wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
      return -1;
   }

   // Reset resource handle table for ABI benchmarks
   g_resource_next = 0;
   g_resource_free_count = 0;

   wasmtime_val_t arg = {.kind = WASMTIME_I32, .of = {.i32 = static_cast<int32_t>(n)}};
   wasmtime_val_t result;

   auto t1 = std::chrono::high_resolution_clock::now();
   err = wasmtime_func_call(ctx, &exp.of.func, &arg, 1, &result, 1, &trap);
   auto t2 = std::chrono::high_resolution_clock::now();
   if (err || trap) { fprintf(stderr, "wasmtime abi call error\n"); if (err) wasmtime_error_delete(err); if (trap) wasm_trap_delete(trap); }

   wasmtime_linker_delete(linker);
   wasmtime_module_delete(module);
   wasmtime_store_delete(store);
   wasm_engine_delete(engine);
   return std::chrono::duration<double, std::milli>(t2 - t1).count();
}

double compile_wasmtime(const std::vector<uint8_t>& wasm) {
   wasm_engine_t* engine = wasm_engine_new();

   auto t1 = std::chrono::high_resolution_clock::now();
   wasmtime_module_t* module = nullptr;
   wasmtime_error_t* err = wasmtime_module_new(engine, wasm.data(), wasm.size(), &module);
   auto t2 = std::chrono::high_resolution_clock::now();

   if (err) { print_wt_error("wasmtime compile", err); wasm_engine_delete(engine); return -1; }
   wasmtime_module_delete(module);
   wasm_engine_delete(engine);
   return std::chrono::duration<double, std::milli>(t2 - t1).count();
}

double run_wasmtime_guest(const std::vector<uint8_t>& wasm, const char* func, int num_params, uint32_t n) {
   wasm_engine_t* engine = wasm_engine_new();
   wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
   wasmtime_context_t* ctx = wasmtime_store_context(store);

   wasmtime_module_t* module = nullptr;
   wasmtime_error_t* err = wasmtime_module_new(engine, wasm.data(), wasm.size(), &module);
   if (err) { print_wt_error("wasmtime guest module", err); wasmtime_store_delete(store); wasm_engine_delete(engine); return -1; }

   // No imports needed — nop module is self-contained
   wasmtime_instance_t instance;
   wasm_trap_t* trap = nullptr;
   err = wasmtime_instance_new(ctx, module, nullptr, 0, &instance, &trap);
   if (err || trap) {
      if (err) print_wt_error("wasmtime guest instantiate", err);
      if (trap) wasm_trap_delete(trap);
      wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
      return -1;
   }

   wasmtime_extern_t exp;
   bool found = wasmtime_instance_export_get(ctx, &instance, func, strlen(func), &exp);
   if (!found || exp.kind != WASMTIME_EXTERN_FUNC) {
      fprintf(stderr, "wasmtime guest: export %s not found\n", func);
      wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
      return -1;
   }

   // Use unchecked call path (wasmtime_func_call_unchecked) for a fair
   // comparison against psizam's typed_function — both resolve types once,
   // not per-call.  wasmtime_val_raw_t is a plain 16-byte union without the
   // kind discriminator; results overwrite args starting at index 0.
   size_t buf_len = static_cast<size_t>(num_params > 1 ? num_params : 1);
   wasmtime_val_raw_t buf[8] = {};
   for (int i = 0; i < num_params && i < 8; i++)
      buf[i].i64 = static_cast<int64_t>(i + 1);

   // Warm-up
   err = wasmtime_func_call_unchecked(ctx, &exp.of.func, buf, buf_len, &trap);
   if (err) { wasmtime_error_delete(err); }
   if (trap) { wasm_trap_delete(trap); trap = nullptr; }

   auto t1 = std::chrono::high_resolution_clock::now();
   for (uint32_t i = 0; i < n; i++) {
      buf[0].i64 = static_cast<int64_t>(i);
      trap = nullptr;
      err = wasmtime_func_call_unchecked(ctx, &exp.of.func, buf, buf_len, &trap);
      if (err) { wasmtime_error_delete(err); break; }
      if (trap) { wasm_trap_delete(trap); break; }
   }
   auto t2 = std::chrono::high_resolution_clock::now();

   wasmtime_module_delete(module);
   wasmtime_store_delete(store);
   wasm_engine_delete(engine);
   return std::chrono::duration<double, std::milli>(t2 - t1).count();
}

double run_wasmtime_compute(const std::vector<uint8_t>& wasm, const char* func, uint32_t n) {
   wasm_engine_t* engine = wasm_engine_new();
   wasmtime_store_t* store = wasmtime_store_new(engine, nullptr, nullptr);
   wasmtime_context_t* ctx = wasmtime_store_context(store);

   wasmtime_module_t* module = nullptr;
   wasmtime_error_t* err = wasmtime_module_new(engine, wasm.data(), wasm.size(), &module);
   if (err) { print_wt_error("wasmtime compute module", err); wasmtime_store_delete(store); wasm_engine_delete(engine); return -1; }

   wasmtime_instance_t instance;
   wasm_trap_t* trap = nullptr;
   err = wasmtime_linker_instantiate(wasmtime_linker_new(engine), ctx, module, &instance, &trap);
   if (err || trap) {
      // Fallback: try direct instantiation with no imports
      err = wasmtime_instance_new(ctx, module, nullptr, 0, &instance, &trap);
      if (err || trap) {
         if (err) print_wt_error("wasmtime compute instantiate", err);
         if (trap) wasm_trap_delete(trap);
         wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
         return -1;
      }
   }

   wasmtime_extern_t exp;
   bool found = wasmtime_instance_export_get(ctx, &instance, func, strlen(func), &exp);
   if (!found || exp.kind != WASMTIME_EXTERN_FUNC) {
      fprintf(stderr, "wasmtime compute: export %s not found\n", func);
      wasmtime_module_delete(module); wasmtime_store_delete(store); wasm_engine_delete(engine);
      return -1;
   }

   wasmtime_val_t arg = {.kind = WASMTIME_I32, .of = {.i32 = static_cast<int32_t>(n)}};
   wasmtime_val_t result;

   auto t1 = std::chrono::high_resolution_clock::now();
   err = wasmtime_func_call(ctx, &exp.of.func, &arg, 1, &result, 1, &trap);
   auto t2 = std::chrono::high_resolution_clock::now();
   if (err || trap) { fprintf(stderr, "wasmtime compute call error\n"); if (err) wasmtime_error_delete(err); if (trap) wasm_trap_delete(trap); }

   wasmtime_module_delete(module);
   wasmtime_store_delete(store);
   wasm_engine_delete(engine);
   return std::chrono::duration<double, std::milli>(t2 - t1).count();
}
#endif
