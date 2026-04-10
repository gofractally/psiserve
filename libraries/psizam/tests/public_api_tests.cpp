// Tests for the new psizam public API (runtime + host_function_table)

#include <psizam/psizam.hpp>
#include "utils.hpp"
#include <catch2/catch.hpp>

using namespace psizam;

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

TEST_CASE("Public API: runtime with interpreter", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   runtime rt(const42_wasm, table, &wa, nullptr, {.eng = engine::interpreter});
   auto result = rt.call_with_return("env", "const42");
   REQUIRE(result.has_value());
   CHECK(result->to_ui32() == 42u);
}

TEST_CASE("Public API: runtime with add function", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   runtime rt(add_wasm, table, &wa, nullptr, {.eng = engine::interpreter});
   auto result = rt.call_with_return("env", "add", (uint32_t)10, (uint32_t)32);
   REQUIRE(result.has_value());
   CHECK(result->to_ui32() == 42u);
}

#if defined(__x86_64__) || defined(__aarch64__)
TEST_CASE("Public API: runtime with jit", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   runtime rt(const42_wasm, table, &wa, nullptr, {.eng = engine::jit});
   auto result = rt.call_with_return("env", "const42");
   REQUIRE(result.has_value());
   CHECK(result->to_ui32() == 42u);
}

TEST_CASE("Public API: runtime with jit2", "[public_api]") {
   wasm_allocator wa;
   host_function_table table;

   runtime rt(const42_wasm, table, &wa, nullptr, {.eng = engine::jit2});
   auto result = rt.call_with_return("env", "const42");
   REQUIRE(result.has_value());
   CHECK(result->to_ui32() == 42u);
}
#endif

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
#include <psizam/llvm_ir_translator.hpp>
#include <psizam/llvm_jit_compiler.hpp>
#include <psizam/jit_ir.hpp>

TEST_CASE("LLVM: standalone IR translate + compile", "[llvm]") {
   // Parse the const42 module with null_backend (validation only) to get the module
   wasm_code code = const42_wasm;
   backend<std::nullptr_t, null_backend> nb(code, nullptr);
   auto& mod = nb.get_module();

   // Now parse it with jit_llvm backend using the new ir_writer_llvm
   // to get function pointers
   wasm_allocator wa;
   host_function_table table;

   runtime rt(const42_wasm, table, &wa, nullptr, {.eng = engine::jit_llvm});

   // Verify the jit_code_offset was set (non-zero = function was compiled)
   auto& llvm_mod = rt.get_module();
   REQUIRE(llvm_mod.code.size() == 1);
   INFO("jit_code_offset = " << llvm_mod.code[0].jit_code_offset);
   CHECK(llvm_mod.code[0].jit_code_offset != 0);

   // Now try to call the entry function directly
   using entry_fn_t = int64_t(*)(void*, void*, psizam::native_value*);
   auto entry = reinterpret_cast<entry_fn_t>(llvm_mod.code[0].jit_code_offset);
   INFO("entry function pointer = " << (void*)entry);

   psizam::native_value args[1] = {};
   int64_t result = entry(nullptr, nullptr, args);
   CHECK(result == 42);
}

TEST_CASE("Public API: runtime with jit_llvm const42", "[public_api][llvm]") {
   wasm_allocator wa;
   host_function_table table;

   runtime rt(const42_wasm, table, &wa, nullptr, {.eng = engine::jit_llvm});
   auto result = rt.call_with_return("env", "const42");
   REQUIRE(result.has_value());
   CHECK(result->to_ui32() == 42u);
}

TEST_CASE("Public API: runtime with jit_llvm add", "[public_api][llvm]") {
   wasm_allocator wa;
   host_function_table table;

   runtime rt(add_wasm, table, &wa, nullptr, {.eng = engine::jit_llvm});
   auto result = rt.call_with_return("env", "add", (uint32_t)10, (uint32_t)32);
   REQUIRE(result.has_value());
   CHECK(result->to_ui32() == 42u);
}
#endif
