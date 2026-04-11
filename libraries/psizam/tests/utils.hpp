#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <psizam/allocator.hpp>
#include <psizam/backend.hpp>
#include <psizam/stack_elem.hpp>
#include <psizam/utils.hpp>

struct type_converter32 {
   union {
      uint32_t ui;
      float    f;
   } _data;
   type_converter32(uint32_t n) { _data.ui = n; }
   uint32_t to_ui() const { return _data.ui; }
   float    to_f() const { return _data.f; }
};

struct type_converter64 {
   union {
      uint64_t ui;
      double   f;
   } _data;
   type_converter64(uint64_t n) { _data.ui = n; }
   uint64_t to_ui() const { return _data.ui; }
   double   to_f() const { return _data.f; }
};

// C++20: using std::bit_cast;
template<typename T, typename U>
T bit_cast(const U& u) {
   static_assert(sizeof(T) == sizeof(U), "bitcast requires identical sizes.");
   T result;
   std::memcpy(&result, &u, sizeof(T));
   return result;
}

template<typename... T>
psizam::v128_t make_v128_i8(T... x) {
   static_assert(sizeof...(T) == 16);
   uint8_t a[16] = {static_cast<uint8_t>(x)...};
   psizam::v128_t result;
   memcpy(&result, &a, 16);
   return result;
}

template<typename... T>
psizam::v128_t make_v128_i16(T... x) {
   static_assert(sizeof...(T) == 8);
   uint16_t a[8] = {static_cast<uint16_t>(x)...};
   psizam::v128_t result;
   memcpy(&result, &a, 16);
   return result;
}

template<typename... T>
psizam::v128_t make_v128_i32(T... x) {
   static_assert(sizeof...(T) == 4);
   uint32_t a[4] = {static_cast<uint32_t>(x)...};
   psizam::v128_t result;
   memcpy(&result, &a, 16);
   return result;
}

template<typename... T>
psizam::v128_t make_v128_i64(T... x) {
   static_assert(sizeof...(T) == 2);
   uint64_t a[2] = {static_cast<uint64_t>(x)...};
   psizam::v128_t result;
   memcpy(&result, &a, 16);
   return result;
}

template<typename... T>
psizam::v128_t make_v128_f32(T... x) {
   static_assert(sizeof...(T) == 4);
   uint32_t a[4] = {static_cast<uint32_t>(x)...};
   psizam::v128_t result;
   memcpy(&result, &a, 16);
   return result;
}

template<typename... T>
psizam::v128_t make_v128_f64(T... x) {
   static_assert(sizeof...(T) == 2);
   uint64_t a[2] = {static_cast<uint64_t>(x)...};
   psizam::v128_t result;
   memcpy(&result, &a, 16);
   return result;
}

struct nan_arithmetic_t {};

inline std::ostream& operator<<(std::ostream& os, nan_arithmetic_t) {
   return os << "nan:arithmetic";
}

inline bool operator==(uint32_t arg, nan_arithmetic_t) {
   return (arg & 0x7fc00000u) == 0x7fc00000u;
}

inline bool operator==(uint64_t arg, nan_arithmetic_t) {
   return (arg & 0x7ff8000000000000u) == 0x7ff8000000000000u;
}

struct nan_canonical_t {};

inline std::ostream& operator<<(std::ostream& os, nan_canonical_t) {
   return os << "nan:canonical";
}

inline bool operator==(uint32_t arg, nan_canonical_t) {
   return (arg & 0x7fffffffu) == 0x7fc00000u;
}

inline bool operator==(uint64_t arg, nan_canonical_t) {
   return (arg & 0x7fffffffffffffffu) == 0x7ff8000000000000u;
}

template<typename... T>
struct v128_matcher {
   v128_matcher(T... t) : lanes(t...) {}
   std::tuple<T...> lanes;
};

template<typename... T>
std::ostream& operator<<(std::ostream& os, v128_matcher<T...> m) {
   os << "[";
   os << std::get<0>(m.lanes);
   os << "," << std::get<1>(m.lanes);
   if constexpr (sizeof... (T) > 2) {
      os << "," << std::get<2>(m.lanes);
      os << "," << std::get<3>(m.lanes);
   }
   os << "]";
   return os;
}

template<int N>
auto split_v128(psizam::v128_t);

template<>
inline auto split_v128<2>(psizam::v128_t arg) {
   std::uint64_t result[2];
   memcpy(&result, &arg, sizeof(arg));
   return std::tuple(result[0], result[1]);
}

template<>
inline auto split_v128<4>(psizam::v128_t arg) {
   std::uint32_t result[4];
   memcpy(&result, &arg, sizeof(arg));
   return std::tuple(result[0], result[1], result[2], result[3]);
}

template<typename... T>
bool operator==(psizam::v128_t vec, v128_matcher<T...> pattern) {
   return split_v128<sizeof...(T)>(vec) == pattern.lanes;
}

inline bool check_nan(const std::optional<psizam::operand_stack_elem>& v) {
   return visit(psizam::overloaded{[](psizam::i32_const_t){ return false; },
                                      [](psizam::i64_const_t){ return false; },
                                      [](psizam::f32_const_t f) { return std::isnan(f.data.f); },
                                      [](psizam::f64_const_t f) { return std::isnan(f.data.f); },
                                      [](psizam::v128_const_t){ return false; }}, *v);
}

extern psizam::wasm_allocator wa;

inline psizam::wasm_allocator* get_wasm_allocator() {
   static psizam::wasm_allocator alloc;
   return &alloc;
}

extern template void psizam::execution_context::execute(psizam::interpret_visitor<psizam::execution_context>& visitor);
extern template class psizam::backend<psizam::standalone_function_t, psizam::interpreter>;
#ifdef __x86_64__
extern template class psizam::backend<psizam::standalone_function_t, psizam::jit>;
extern template class psizam::backend<psizam::standalone_function_t, psizam::jit2>;
#elif defined(__aarch64__)
extern template class psizam::backend<psizam::standalone_function_t, psizam::jit>;
extern template class psizam::backend<psizam::standalone_function_t, psizam::jit2>;
#endif

// Spectest host: provides the standard spectest module imports
// (tables, memories, globals) required by WASM spec test modules.
struct spectest_host_t;
using spectest_rhf = psizam::registered_host_functions<spectest_host_t>;

inline void register_spectest_imports() {
   static bool registered = false;
   if (registered) return;
   registered = true;
   // spectest.table: funcref table with 10 elements (standard spectest module)
   spectest_rhf::add_table("spectest", "table", 10, psizam::types::funcref);
   // spectest.global_i32: i32 global, value 666
   spectest_rhf::add_global("spectest", "global_i32", psizam::types::i32, 666);
   // spectest.global_i64: i64 global, value 666
   spectest_rhf::add_global("spectest", "global_i64", psizam::types::i64, 666);
   // spectest.global_f32: f32 global (666.6f bit pattern)
   spectest_rhf::add_global("spectest", "global_f32", psizam::types::f32, 0x4426999a);
   // spectest.global_f64: f64 global (666.6 bit pattern)
   spectest_rhf::add_global("spectest", "global_f64", psizam::types::f64, 0x4084d33333333333ULL);
}

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
  #if defined(__x86_64__) || defined(__aarch64__)
    #define BACKEND_TEST_CASE(name, tags) \
      TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit2, psizam::jit_llvm)
  #else
    #define BACKEND_TEST_CASE(name, tags) \
      TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit_llvm)
  #endif
#elif defined(__x86_64__) || defined(__aarch64__)
  #define BACKEND_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit2)
#else
  #define BACKEND_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter)
#endif
