// Tests for the psizam public API (compiled_module + instance)

#include <psizam/psizam.hpp>
#include "utils.hpp"
#include <catch2/catch.hpp>

using namespace psizam;
using namespace psizam::detail;

namespace {

struct test_host {
   int counter = 0;
   int32_t inc_counter() { return ++counter; }
};

// Simple WASM module that exports "add" function: (i32, i32) -> i32
wasm_code add_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic + version
   0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f, // type: (i32,i32)->i32
   0x03, 0x02, 0x01, 0x00,                               // function: type 0
   0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00, // export: "add" = func 0
   0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b // code: local.get 0 + local.get 1 + i32.add
};

// Simple WASM module that exports "const42" function: () -> i32
wasm_code const42_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic + version
   0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,       // type: ()->i32
   0x03, 0x02, 0x01, 0x00,                           // function: type 0
   0x07, 0x0b, 0x01, 0x07, 0x63, 0x6f, 0x6e, 0x73, 0x74, 0x34, 0x32, 0x00, 0x00, // export: "const42"
   0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b  // code: i32.const 42
};

} // namespace

TEST_CASE("Public API: host_function_table add and call", "[public_api]") {
   host_function_table table;
   table.add<&test_host::inc_counter>("env", "inc");

   CHECK(table.size() == 1);
}

TEST_CASE("Public API: compiled_module with interpreter", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   compiled_module mod(const42_wasm, std::move(table), &wa, {.eng = engine::interpreter});
   auto inst = mod.create_instance();
   auto fn = inst.get_function<uint32_t()>("const42");
   CHECK(fn() == 42u);
}

TEST_CASE("Public API: compiled_module with add function", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   compiled_module mod(add_wasm, std::move(table), &wa, {.eng = engine::interpreter});
   auto inst = mod.create_instance();
   auto fn = inst.get_function<uint32_t(uint32_t, uint32_t)>("add");
   CHECK(fn(10, 32) == 42u);
}

#if defined(__x86_64__) || defined(__aarch64__)
TEST_CASE("Public API: compiled_module with jit", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   compiled_module mod(const42_wasm, std::move(table), &wa, {.eng = engine::jit});
   auto inst = mod.create_instance();
   auto fn = inst.get_function<uint32_t()>("const42");
   CHECK(fn() == 42u);
}

TEST_CASE("Public API: compiled_module with jit2", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   compiled_module mod(const42_wasm, std::move(table), &wa, {.eng = engine::jit2});
   auto inst = mod.create_instance();
   auto fn = inst.get_function<uint32_t()>("const42");
   CHECK(fn() == 42u);
}
#endif

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)

TEST_CASE("LLVM: standalone IR translate + compile", "[llvm]") {
   wasm_allocator wa;
   host_function_table table;

   compiled_module cm(const42_wasm, std::move(table), &wa, {.eng = engine::jit_llvm});

   // Verify the jit_code_offset was set (non-zero = function was compiled)
   auto& llvm_mod = cm.get_module();
   REQUIRE(llvm_mod.code.size() == 1);
   INFO("jit_code_offset = " << llvm_mod.code[0].jit_code_offset);
   CHECK(llvm_mod.code[0].jit_code_offset != 0);

   // Call through the public API (provides proper execution context)
   auto inst = cm.create_instance();
   auto fn = inst.get_function<uint32_t()>("const42");
   CHECK(fn() == 42u);
}

TEST_CASE("Public API: compiled_module with jit_llvm const42", "[public_api][llvm]") {
   wasm_allocator wa;
   host_function_table table;

   compiled_module mod(const42_wasm, std::move(table), &wa, {.eng = engine::jit_llvm});
   auto inst = mod.create_instance();
   auto fn = inst.get_function<uint32_t()>("const42");
   CHECK(fn() == 42u);
}

TEST_CASE("Public API: compiled_module with jit_llvm add", "[public_api][llvm]") {
   wasm_allocator wa;
   host_function_table table;

   compiled_module mod(add_wasm, std::move(table), &wa, {.eng = engine::jit_llvm});
   auto inst = mod.create_instance();
   auto fn = inst.get_function<uint32_t(uint32_t, uint32_t)>("add");
   CHECK(fn(10, 32) == 42u);
}
#endif
