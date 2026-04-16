#pragma once
#include <cstdint>
#include <vector>

// Shared host functions used by all runtime benchmarks
int32_t bench_host_identity(int32_t x);
void bench_host_accumulate(int64_t val);
int64_t bench_host_mix(int64_t a, int64_t b);
void bench_host_mem_op(int32_t ptr, int32_t len);
int64_t bench_host_multi(int64_t a, int64_t b, int32_t c, int32_t d);

// --- Canonical ABI / resource handle host functions ---
// These simulate canonical ABI patterns with varying type complexity.
// g_guest_memory must be set to the guest's linear memory base before calling
// benchmarks that access memory (string_pass, record_mem).

extern const uint8_t* g_guest_memory;
extern uint64_t g_guest_memory_size;

int32_t bench_host_noop(void);
int64_t bench_host_string_pass(int32_t ptr, int32_t len);
int64_t bench_host_record_flat(int32_t x, int32_t y, int32_t z, int64_t w);
int64_t bench_host_record_mem(int32_t ptr);
int64_t bench_host_resource_call(int32_t handle, int64_t arg);
int32_t bench_host_resource_create(int64_t initial);
void    bench_host_resource_drop(int32_t handle);

// Rich type patterns — canonical ABI lowerings of complex types
int64_t bench_host_struct_2str(int32_t a_ptr, int32_t a_len, int32_t b_ptr, int32_t b_len);
int64_t bench_host_struct_mixed(int32_t id, int32_t name_ptr, int32_t name_len,
                                int64_t value, int32_t desc_ptr, int32_t desc_len);
int64_t bench_host_nested(int32_t id, int32_t inner_x, int32_t inner_y,
                          int32_t inner_str_ptr, int32_t inner_str_len,
                          int64_t val, int32_t outer_str_ptr, int32_t outer_str_len);
int64_t bench_host_vec_records(int32_t ptr, int32_t count);
int64_t bench_host_many_params(int32_t a, int32_t b, int32_t c, int32_t d,
                               int64_t e, int64_t f,
                               int32_t g, int32_t h, int32_t i, int32_t j,
                               int64_t k, int64_t l);

// Resource handle table state (shared between psizam and wasmtime runners)
extern int32_t g_resource_next;
extern int32_t g_resource_free_count;

// Build the benchmark WASM module
std::vector<uint8_t> build_bench_wasm();

// Runner function type: (wasm_bytes, func_name, iteration_count) -> milliseconds
using bench_runner_t = double (*)(const std::vector<uint8_t>&, const char*, uint32_t);

// Optional runners provided by separate TUs (host-call benchmarks)
#ifdef BENCH_HAS_WASMTIME
double run_wasmtime(const std::vector<uint8_t>& wasm, const char* func, uint32_t n);
double run_wasmtime_abi(const std::vector<uint8_t>& wasm, const char* func, uint32_t n);
double run_wasmtime_guest(const std::vector<uint8_t>& wasm, const char* func, int num_params, uint32_t n);
#endif

#ifdef BENCH_HAS_WASMER
double run_wasmer(const std::vector<uint8_t>& wasm, const char* func, uint32_t n);
#endif

// Compute benchmark runners (zero-import WASM modules)
#ifdef BENCH_HAS_WASMTIME
double run_wasmtime_compute(const std::vector<uint8_t>& wasm, const char* func, uint32_t n);
#endif

#ifdef BENCH_HAS_WASMER
double run_wasmer_compute(const std::vector<uint8_t>& wasm, const char* func, uint32_t n);
#endif

// Compile-time measurement (returns milliseconds)
#ifdef BENCH_HAS_WASMTIME
double compile_wasmtime(const std::vector<uint8_t>& wasm);
#endif

#ifdef BENCH_HAS_WASMER
double compile_wasmer(const std::vector<uint8_t>& wasm);
#endif
