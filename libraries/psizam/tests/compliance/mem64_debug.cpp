// Standalone jit2 memory64 debug test — dumps generated machine code
#include <psizam/psizam.hpp>
#include <cstdio>
#include <cstring>

using namespace psizam;
using namespace psizam::detail;

// Minimal module: memory64, (i64, i32) → i32, stores then loads i32 at i64 addr
static const uint8_t mem64_wasm[] = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x01, 0x07, 0x01, 0x60, 0x02, 0x7e, 0x7f, 0x01, 0x7f,
   0x03, 0x02, 0x01, 0x00,
   0x05, 0x03, 0x01, 0x04, 0x01,
   0x07, 0x0e, 0x01, 0x0a, 's', 't', 'o', 'r', 'e', '_', 'l', 'o', 'a', 'd', 0x00, 0x00,
   0x0a, 0x10, 0x01,
   0x0e, 0x00,
   0x20, 0x00,
   0x20, 0x01,
   0x36, 0x02, 0x00,
   0x20, 0x00,
   0x28, 0x02, 0x00,
   0x0b,
};

// Same but memory32: (i32, i32) → i32
static const uint8_t mem32_wasm[] = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
   0x03, 0x02, 0x01, 0x00,
   0x05, 0x03, 0x01, 0x00, 0x01,
   0x07, 0x0e, 0x01, 0x0a, 's', 't', 'o', 'r', 'e', '_', 'l', 'o', 'a', 'd', 0x00, 0x00,
   0x0a, 0x10, 0x01,
   0x0e, 0x00,
   0x20, 0x00,
   0x20, 0x01,
   0x36, 0x02, 0x00,
   0x20, 0x00,
   0x28, 0x02, 0x00,
   0x0b,
};

static void dump_code(const char* label, compiled_module& mod, const char* func_name) {
   auto& m = mod.get_module();
   uint32_t func_index = m.get_exported_function(func_name);
   uint32_t local_idx = func_index - m.get_imported_functions_size();
   auto offset = m.code[local_idx].jit_code_offset;
   auto* base = m.allocator._code_base;
   auto* fn_start = base + offset;

   printf("\n=== %s: func %u, code offset=%u ===\n", label, func_index, (unsigned)offset);
   printf("code_base=%p, fn_addr=%p\n", (void*)base, (void*)fn_start);

   // Dump up to 128 bytes of machine code
   const int dump_bytes = 128;
   printf("Machine code (%d bytes):\n", dump_bytes);
   for (int i = 0; i < dump_bytes; i++) {
      printf("%02x ", (unsigned char)fn_start[i]);
      if ((i & 15) == 15) printf("\n");
   }
   printf("\n");

   // Also dump the error handler area (code_base + 512 to code_base + 768)
   printf("Error handler area (offset 512-768):\n");
   for (int i = 512; i < 768; i++) {
      printf("%02x ", (unsigned char)base[i]);
      if ((i & 15) == 15) printf("\n");
   }
   printf("\n");
}

int main() {
   printf("=== Memory64 JIT2 Debug Test ===\n");

   // Test 1: mem64 addr=1 ONLY on fresh instance (no prior addr=0 call)
   printf("\n--- Test 1: mem64 addr=1 only (fresh instance) ---\n");
   {
      wasm_allocator wa;
      host_function_table table;
      compiled_module mod(wasm_code(mem64_wasm, mem64_wasm + sizeof(mem64_wasm)),
                          std::move(table), &wa, {.eng = engine::jit2});

      auto inst = mod.create_instance();
      char* mem = inst.linear_memory();
      printf("linear_memory=%p\n", (void*)mem);

      try {
         auto r1 = inst.call_with_return("store_load", uint64_t(1), uint32_t(42));
         printf("mem64 addr=1: %s, val=%u\n", r1 ? "ok" : "void", r1 ? r1->to_ui32() : 0);
      } catch (const std::exception& e) {
         printf("mem64 addr=1: ERROR: %s\n", e.what());
      }
   }

   // Test 2: mem32 addr=1 for comparison (should work)
   printf("\n--- Test 2: mem32 addr=1 (should work) ---\n");
   {
      wasm_allocator wa;
      host_function_table table;
      compiled_module mod(wasm_code(mem32_wasm, mem32_wasm + sizeof(mem32_wasm)),
                          std::move(table), &wa, {.eng = engine::jit2});

      auto inst = mod.create_instance();
      try {
         auto r1 = inst.call_with_return("store_load", uint32_t(1), uint32_t(42));
         printf("mem32 addr=1: %s, val=%u\n", r1 ? "ok" : "void", r1 ? r1->to_ui32() : 0);
      } catch (const std::exception& e) {
         printf("mem32 addr=1: ERROR: %s\n", e.what());
      }
   }

   // Test 3: mem64 with JIT1 for comparison
   printf("\n--- Test 3: mem64 addr=1 with JIT1 ---\n");
   {
      wasm_allocator wa;
      host_function_table table;
      compiled_module mod(wasm_code(mem64_wasm, mem64_wasm + sizeof(mem64_wasm)),
                          std::move(table), &wa, {.eng = engine::jit});

      auto inst = mod.create_instance();
      try {
         auto r1 = inst.call_with_return("store_load", uint64_t(1), uint32_t(42));
         printf("jit1 mem64 addr=1: %s, val=%u\n", r1 ? "ok" : "void", r1 ? r1->to_ui32() : 0);
      } catch (const std::exception& e) {
         printf("jit1 mem64 addr=1: ERROR: %s\n", e.what());
      }
   }

   // Test 4: mem64 with interpreter for comparison
   printf("\n--- Test 4: mem64 addr=1 with interpreter ---\n");
   {
      wasm_allocator wa;
      host_function_table table;
      compiled_module mod(wasm_code(mem64_wasm, mem64_wasm + sizeof(mem64_wasm)),
                          std::move(table), &wa, {.eng = engine::interpreter});

      auto inst = mod.create_instance();
      try {
         auto r1 = inst.call_with_return("store_load", uint64_t(1), uint32_t(42));
         printf("interp mem64 addr=1: %s, val=%u\n", r1 ? "ok" : "void", r1 ? r1->to_ui32() : 0);
      } catch (const std::exception& e) {
         printf("interp mem64 addr=1: ERROR: %s\n", e.what());
      }
   }

   // Test 5: Dump generated code and try to narrow down
   printf("\n--- Test 5: Code dump + step-by-step ---\n");
   {
      wasm_allocator wa;
      host_function_table table;
      compiled_module mod(wasm_code(mem64_wasm, mem64_wasm + sizeof(mem64_wasm)),
                          std::move(table), &wa, {.eng = engine::jit2});
      dump_code("mem64 jit2", mod, "store_load");

      auto inst = mod.create_instance();
      char* mem = inst.linear_memory();
      printf("\nlinear_memory=%p\n", (void*)mem);
      printf("linear_memory + 1 = %p\n", (void*)(mem + 1));
      printf("linear_memory + 4 = %p\n", (void*)(mem + 4));

      // Manual test: can we read/write at each address?
      for (int off = 0; off < 8; off++) {
         mem[off] = (char)off;
      }
      printf("C++ manual writes OK at offsets 0-7\n");
      for (int off = 0; off < 8; off++) {
         printf("  mem[%d] = %d\n", off, (int)(unsigned char)mem[off]);
      }

      // Test addr=0
      try {
         auto r0 = inst.call_with_return("store_load", uint64_t(0), uint32_t(42));
         printf("\nmem64 addr=0: ok, val=%u\n", r0 ? r0->to_ui32() : 0);
      } catch (const std::exception& e) {
         printf("\nmem64 addr=0: ERROR: %s\n", e.what());
      }

      // Test addr=1
      try {
         auto r1 = inst.call_with_return("store_load", uint64_t(1), uint32_t(42));
         printf("mem64 addr=1: ok, val=%u\n", r1 ? r1->to_ui32() : 0);
      } catch (const std::exception& e) {
         printf("mem64 addr=1: ERROR: %s\n", e.what());
      }
   }

   return 0;
}
