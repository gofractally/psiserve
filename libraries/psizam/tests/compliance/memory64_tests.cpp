// Memory64 proposal compliance tests

#include "differential_runner.hpp"
#include <catch2/catch.hpp>

using namespace psizam;
using namespace psizam::detail;
using namespace psizam::compliance;

namespace {

// ── Helper to build a memory64 WASM module ─────────────────────────────────

// Minimal WASM module with memory64 (flag 0x04):
// - 1 memory (memory64, 1 page initial)
// - 1 exported function
//
// Memory section for memory64 with 1 page, no max:
//   section id=5, payload_len, count=1, flags=0x04, initial=1 (as varuint64)

// Module: export "memory_size" which returns memory.size (as i64)
// (module
//   (memory (;0;) i64 1)
//   (func (export "memory_size") (result i64)
//     memory.size)
// )
wasm_code mem64_size_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic + version
   // Type section: 1 type, () -> (i64)
   0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7e,
   // Function section: 1 function, type 0
   0x03, 0x02, 0x01, 0x00,
   // Memory section: 1 memory, flags=0x04 (memory64, no max), initial=1
   0x05, 0x03, 0x01, 0x04, 0x01,
   // Export section: "memory_size" -> func 0
   0x07, 0x0f, 0x01, 0x0b, 'm', 'e', 'm', 'o', 'r', 'y', '_', 's', 'i', 'z', 'e', 0x00, 0x00,
   // Code section: 1 body
   0x0a, 0x06, 0x01,
   0x04, 0x00,       // body size=4, local count=0
   0x3f, 0x00,       // memory.size 0
   0x0b,             // end
};

// Module: export "store_load" which stores an i32 at addr 0 then loads it back
// (module
//   (memory (;0;) i64 1)
//   (func (export "store_load") (param i64 i32) (result i32)
//     local.get 0    ;; addr (i64)
//     local.get 1    ;; value (i32)
//     i32.store offset=0
//     local.get 0    ;; addr (i64)
//     i32.load offset=0)
// )
wasm_code mem64_store_load_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, // magic + version
   // Type section: (i64, i32) -> (i32)
   0x01, 0x07, 0x01, 0x60, 0x02, 0x7e, 0x7f, 0x01, 0x7f,
   // Function section
   0x03, 0x02, 0x01, 0x00,
   // Memory section: memory64, 1 page, no max
   0x05, 0x03, 0x01, 0x04, 0x01,
   // Export section: "store_load" -> func 0
   0x07, 0x0e, 0x01, 0x0a, 's', 't', 'o', 'r', 'e', '_', 'l', 'o', 'a', 'd', 0x00, 0x00,
   // Code section
   0x0a, 0x10, 0x01,
   0x0e, 0x00,             // body size, local count=0
   0x20, 0x00,             // local.get 0 (addr: i64)
   0x20, 0x01,             // local.get 1 (value: i32)
   0x36, 0x02, 0x00,       // i32.store align=2 offset=0
   0x20, 0x00,             // local.get 0 (addr: i64)
   0x28, 0x02, 0x00,       // i32.load align=2 offset=0
   0x0b,                   // end
};

// Module: export "grow_test" which grows memory by 1 page and returns old size (i64)
// (module
//   (memory (;0;) i64 1)
//   (func (export "grow_test") (result i64)
//     i64.const 1
//     memory.grow)
// )
wasm_code mem64_grow_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   // Type section: () -> (i64)
   0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7e,
   // Function section
   0x03, 0x02, 0x01, 0x00,
   // Memory section: memory64, 1 page, no max
   0x05, 0x03, 0x01, 0x04, 0x01,
   // Export section: "grow_test"
   0x07, 0x0d, 0x01, 0x09, 'g', 'r', 'o', 'w', '_', 't', 'e', 's', 't', 0x00, 0x00,
   // Code section
   0x0a, 0x08, 0x01,
   0x06, 0x00,       // body size=6, local count=0
   0x42, 0x01,       // i64.const 1
   0x40, 0x00,       // memory.grow 0
   0x0b,             // end
};

// Module with memory64 + maximum
// (memory (;0;) i64 1 10)
wasm_code mem64_with_max_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   // Type section: () -> (i64)
   0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7e,
   // Function section
   0x03, 0x02, 0x01, 0x00,
   // Memory section: flags=0x05 (memory64 + has_max), initial=1, max=10
   0x05, 0x04, 0x01, 0x05, 0x01, 0x0a,
   // Export section: "memory_size"
   0x07, 0x0f, 0x01, 0x0b, 'm', 'e', 'm', 'o', 'r', 'y', '_', 's', 'i', 'z', 'e', 0x00, 0x00,
   // Code section
   0x0a, 0x06, 0x01,
   0x04, 0x00,       // body size, local count=0
   0x3f, 0x00,       // memory.size
   0x0b,
};

} // anonymous namespace

// ── Tests ───────────────────────────────────────────────────────────────────

TEST_CASE("memory64: parse memory64 flag", "[memory64]") {
   for (auto eng : all_engines()) {
      SECTION(engine_name(eng)) {
         wasm_allocator wa;
         host_function_table table;
         // Should parse without error
         REQUIRE_NOTHROW(compiled_module(
            wasm_code(mem64_size_wasm.begin(), mem64_size_wasm.end()),
            std::move(table), &wa, {.eng = eng}));
      }
   }
}

TEST_CASE("memory64: memory.size returns i64", "[memory64]") {
   auto results = run_differential(mem64_size_wasm, "memory_size");
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      REQUIRE(!r.had_error);
      REQUIRE(r.value.has_value());
      // Memory was initialized with 1 page
      CHECK(r.value->to_ui64() == 1);
   }
}

TEST_CASE("memory64: memory.grow returns old size as i64", "[memory64]") {
   auto results = run_differential(mem64_grow_wasm, "grow_test");
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      REQUIRE(!r.had_error);
      REQUIRE(r.value.has_value());
      // grow(1) should return old size = 1
      CHECK(r.value->to_ui64() == 1);
   }
}

TEST_CASE("memory64: i32.store/load with i64 address", "[memory64]") {
   auto results = run_differential(mem64_store_load_wasm, "store_load",
                                   uint64_t(0), uint32_t(42));
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      REQUIRE(!r.had_error);
      REQUIRE(r.value.has_value());
      CHECK(r.value->to_ui32() == 42);
   }
}

// Same as mem64_store_load_wasm but with memory32 (flags=0x00)
wasm_code mem32_store_load_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   // Type section: (i32, i32) -> (i32)
   0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
   // Function section
   0x03, 0x02, 0x01, 0x00,
   // Memory section: memory32, 1 page, no max
   0x05, 0x03, 0x01, 0x00, 0x01,
   // Export section: "store_load" -> func 0
   0x07, 0x0e, 0x01, 0x0a, 's', 't', 'o', 'r', 'e', '_', 'l', 'o', 'a', 'd', 0x00, 0x00,
   // Code section
   0x0a, 0x10, 0x01,
   0x0e, 0x00,
   0x20, 0x00,
   0x20, 0x01,
   0x36, 0x02, 0x00,
   0x20, 0x00,
   0x28, 0x02, 0x00,
   0x0b,
};

TEST_CASE("memory64: mem32 baseline store/load at 100", "[memory64]") {
   auto results = run_differential(mem32_store_load_wasm, "store_load",
                                   uint32_t(100), uint32_t(42));
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      INFO("Error: " << r.error);
      REQUIRE(!r.had_error);
      REQUIRE(r.value.has_value());
      CHECK(r.value->to_ui32() == 42);
   }
}

TEST_CASE("memory64: store/load at addr 1", "[memory64]") {
   auto results = run_differential(mem64_store_load_wasm, "store_load",
                                   uint64_t(1), uint32_t(42));
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      INFO("Error: " << r.error);
      REQUIRE(!r.had_error);
      REQUIRE(r.value.has_value());
      CHECK(r.value->to_ui32() == 42);
   }
}

TEST_CASE("memory64: store/load at non-zero address", "[memory64]") {
   auto results = run_differential(mem64_store_load_wasm, "store_load",
                                   uint64_t(100), uint32_t(0xDEADBEEF));
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      INFO("Error: " << r.error);
      REQUIRE(!r.had_error);
      REQUIRE(r.value.has_value());
      CHECK(r.value->to_ui32() == 0xDEADBEEF);
   }
}

TEST_CASE("memory64: memory64 with maximum", "[memory64]") {
   auto results = run_differential(mem64_with_max_wasm, "memory_size");
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      REQUIRE(!r.had_error);
      REQUIRE(r.value.has_value());
      CHECK(r.value->to_ui64() == 1);
   }
}

TEST_CASE("memory64: OOB address traps", "[memory64]") {
   // Address 0x100000000 (4GB) should trap — exceeds 1 page (64KB)
   auto results = run_differential(mem64_store_load_wasm, "store_load",
                                   uint64_t(0x100000000ULL), uint32_t(1));
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      CHECK(r.had_error);
   }
}

TEST_CASE("memory64: address just past end traps", "[memory64]") {
   // 1 page = 65536 bytes. Address 65536 should trap for i32.load (needs 4 bytes)
   auto results = run_differential(mem64_store_load_wasm, "store_load",
                                   uint64_t(65536), uint32_t(1));
   for (auto& r : results) {
      INFO("Engine: " << r.engine_name);
      CHECK(r.had_error);
   }
}
