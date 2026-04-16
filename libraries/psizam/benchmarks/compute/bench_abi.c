// Canonical ABI and resource handle benchmark guest module.
//
// Compiled to WASM with wasi-sdk. Each bench_* function runs a tight loop
// calling imported host functions with different type complexities.
// The host measures wall time for each exported function.
//
// Call patterns tested (from cheapest to most expensive):
//
//   noop           — zero args, zero return. Pure dispatch overhead.
//   string_pass    — (ptr, len) pair. Simulates canonical ABI string passing.
//   record_flat    — 4 scalar args (x, y, z, w). Canonical ABI flattened record.
//   record_mem     — single ptr arg, host reads struct from guest memory.
//   resource_call  — (handle, arg). Host does handle table lookup + method.
//   resource_life  — create + N method calls + drop. Full resource lifecycle.

#include <stdint.h>

// ============================================================================
// Imported host functions (resolved at instantiation time)
// ============================================================================

extern int32_t host_noop(void);
extern int64_t host_string_pass(int32_t ptr, int32_t len);
extern int64_t host_record_flat(int32_t x, int32_t y, int32_t z, int64_t w);
extern int64_t host_record_mem(int32_t ptr);
extern int64_t host_resource_call(int32_t handle, int64_t arg);
extern int32_t host_resource_create(int64_t initial);
extern void    host_resource_drop(int32_t handle);

// Rich type patterns — canonical ABI lowerings of complex types
// struct_2str: record { a: string, b: string }
extern int64_t host_struct_2str(int32_t a_ptr, int32_t a_len, int32_t b_ptr, int32_t b_len);
// struct_mixed: record { id: u32, name: string, value: u64, desc: string }
extern int64_t host_struct_mixed(int32_t id, int32_t name_ptr, int32_t name_len,
                                 int64_t value, int32_t desc_ptr, int32_t desc_len);
// nested: record { id: u32, inner: record { x: u32, y: u32, name: string }, val: u64 }
extern int64_t host_nested(int32_t id, int32_t inner_x, int32_t inner_y,
                           int32_t inner_str_ptr, int32_t inner_str_len,
                           int64_t val, int32_t outer_str_ptr, int32_t outer_str_len);
// vec_records: list<record { x: u32, y: u32, z: u32 }> — passed as (ptr, count)
extern int64_t host_vec_records(int32_t ptr, int32_t count);
// many_params: large flattened struct, 12 scalar params (near MAX_FLAT_PARAMS=16)
extern int64_t host_many_params(int32_t a, int32_t b, int32_t c, int32_t d,
                                int64_t e, int64_t f,
                                int32_t g, int32_t h, int32_t i, int32_t j,
                                int64_t k, int64_t l);

// ============================================================================
// Test data in linear memory
// ============================================================================

// A 48-byte string sitting in the data segment.
static const char test_string[] = "hello world benchmark test canonical ABI string";
#define TEST_STRING_LEN 48

// A record struct in memory (matches the flat layout: x, y, z padding w).
struct test_record {
   int32_t x;
   int32_t y;
   int32_t z;
   int32_t _pad;
   int64_t w;
};
static struct test_record test_rec = { 1, 2, 3, 0, 0x100000004LL };

// Second string for struct_2str benchmark
static const char test_string2[] = "second field description value";
#define TEST_STRING2_LEN 30

// Array of records for vec_records benchmark
struct vec_record { int32_t x; int32_t y; int32_t z; };
static struct vec_record test_vec[] = {
   {1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {10, 11, 12},
   {13, 14, 15}, {16, 17, 18}, {19, 20, 21}, {22, 23, 24},
};
#define TEST_VEC_COUNT 8

// ============================================================================
// Exported benchmark functions
// ============================================================================

// Pure host call dispatch overhead — no arguments, no meaningful return.
int64_t bench_noop(int32_t n) {
   int64_t sum = 0;
   for (int32_t i = 0; i < n; i++) {
      sum += host_noop();
   }
   return sum;
}

// Pass a string as (ptr, len) — simulates canonical ABI string parameter.
// The host receives the pointer and length, validates/reads from guest memory.
int64_t bench_string_pass(int32_t n) {
   int64_t sum = 0;
   int32_t ptr = (int32_t)(uintptr_t)test_string;
   for (int32_t i = 0; i < n; i++) {
      sum += host_string_pass(ptr, TEST_STRING_LEN);
   }
   return sum;
}

// Pass a record as flattened scalar arguments — canonical ABI "core representation"
// for a small struct. Each field becomes a separate WASM argument.
int64_t bench_record_flat(int32_t n) {
   int64_t sum = 0;
   for (int32_t i = 0; i < n; i++) {
      sum += host_record_flat(1, 2, 3, 0x100000004LL);
   }
   return sum;
}

// Pass a record via memory pointer — host reads the struct from guest linear memory.
// This is how larger structs or non-flat types are passed.
int64_t bench_record_mem(int32_t n) {
   int64_t sum = 0;
   int32_t ptr = (int32_t)(uintptr_t)&test_rec;
   for (int32_t i = 0; i < n; i++) {
      sum += host_record_mem(ptr);
   }
   return sum;
}

// Call a method on a resource handle — host does table lookup + dispatch.
// This measures the per-call overhead of the handle table indirection.
int64_t bench_resource_call(int32_t n) {
   int32_t handle = host_resource_create(42);
   int64_t sum = 0;
   for (int32_t i = 0; i < n; i++) {
      sum += host_resource_call(handle, (int64_t)i);
   }
   host_resource_drop(handle);
   return sum;
}

// Full resource lifecycle: create, call N times, drop.
// Measures amortized cost of create/drop over many method calls.
int64_t bench_resource_lifecycle(int32_t n) {
   int64_t sum = 0;
   for (int32_t i = 0; i < n; i++) {
      int32_t h = host_resource_create((int64_t)i);
      sum += host_resource_call(h, (int64_t)i);
      host_resource_drop(h);
   }
   return sum;
}

// ============================================================================
// Rich type benchmarks — canonical ABI lowerings of complex structures
// ============================================================================

// record { a: string, b: string } — two strings, 4 i32 params
int64_t bench_struct_2str(int32_t n) {
   int64_t sum = 0;
   int32_t ptr1 = (int32_t)(uintptr_t)test_string;
   int32_t ptr2 = (int32_t)(uintptr_t)test_string2;
   for (int32_t i = 0; i < n; i++) {
      sum += host_struct_2str(ptr1, TEST_STRING_LEN, ptr2, TEST_STRING2_LEN);
   }
   return sum;
}

// record { id: u32, name: string, value: u64, desc: string } — mixed scalars + strings
int64_t bench_struct_mixed(int32_t n) {
   int64_t sum = 0;
   int32_t name_ptr = (int32_t)(uintptr_t)test_string;
   int32_t desc_ptr = (int32_t)(uintptr_t)test_string2;
   for (int32_t i = 0; i < n; i++) {
      sum += host_struct_mixed(42, name_ptr, TEST_STRING_LEN,
                               0x100000004LL, desc_ptr, TEST_STRING2_LEN);
   }
   return sum;
}

// record { id: u32, inner: record { x: u32, y: u32, name: string }, val: u64 }
// — nested struct flattened, 6 i32 + 2 i64 = 8 params
int64_t bench_nested(int32_t n) {
   int64_t sum = 0;
   int32_t inner_str = (int32_t)(uintptr_t)test_string;
   int32_t outer_str = (int32_t)(uintptr_t)test_string2;
   for (int32_t i = 0; i < n; i++) {
      sum += host_nested(1, 10, 20, inner_str, TEST_STRING_LEN,
                         0x200000005LL, outer_str, TEST_STRING2_LEN);
   }
   return sum;
}

// list<record { x: u32, y: u32, z: u32 }> — vector of structs, (ptr, count)
int64_t bench_vec_records(int32_t n) {
   int64_t sum = 0;
   int32_t ptr = (int32_t)(uintptr_t)test_vec;
   for (int32_t i = 0; i < n; i++) {
      sum += host_vec_records(ptr, TEST_VEC_COUNT);
   }
   return sum;
}

// Large flattened struct — 12 params (8 i32 + 4 i64), near MAX_FLAT_PARAMS=16
int64_t bench_many_params(int32_t n) {
   int64_t sum = 0;
   for (int32_t i = 0; i < n; i++) {
      sum += host_many_params(1, 2, 3, 4,
                              0x100000001LL, 0x200000002LL,
                              5, 6, 7, 8,
                              0x300000003LL, 0x400000004LL);
   }
   return sum;
}
