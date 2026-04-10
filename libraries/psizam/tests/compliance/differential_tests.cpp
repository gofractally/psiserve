// Differential compliance tests: run WASM modules through all backends
// and verify they produce identical results.

#include "differential_runner.hpp"
#include <catch2/catch.hpp>

using namespace psizam;
using namespace psizam::compliance;

namespace {

// ── Inline WASM modules ─────────────────────────────────────────────────────

// (i32, i32) -> i32: local.get 0 + local.get 1 + i32.add
wasm_code add_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
   0x03, 0x02, 0x01, 0x00,
   0x07, 0x07, 0x01, 0x03, 0x61, 0x64, 0x64, 0x00, 0x00,
   0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6a, 0x0b
};

// () -> i32: i32.const 42
wasm_code const42_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x01, 0x05, 0x01, 0x60, 0x00, 0x01, 0x7f,
   0x03, 0x02, 0x01, 0x00,
   0x07, 0x0b, 0x01, 0x07, 0x63, 0x6f, 0x6e, 0x73, 0x74, 0x34, 0x32, 0x00, 0x00,
   0x0a, 0x06, 0x01, 0x04, 0x00, 0x41, 0x2a, 0x0b
};

// (i32, i32) -> i32: i32.sub (subtraction — order-dependent)
wasm_code sub_wasm = {
   0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
   0x01, 0x07, 0x01, 0x60, 0x02, 0x7f, 0x7f, 0x01, 0x7f,
   0x03, 0x02, 0x01, 0x00,
   0x07, 0x07, 0x01, 0x03, 0x73, 0x75, 0x62, 0x00, 0x00, // export: "sub"
   0x0a, 0x09, 0x01, 0x07, 0x00, 0x20, 0x00, 0x20, 0x01, 0x6b, 0x0b
};

} // namespace

// ── Differential test cases ─────────────────────────────────────────────────

TEST_CASE("Differential: constant return", "[compliance]") {
   auto results = run_differential(const42_wasm, "const42");
   INFO(check_agreement(results));
   REQUIRE(check_agreement(results).empty());
   for (auto& r : results) {
      REQUIRE(r.value.has_value());
      CHECK(r.value->to_ui32() == 42u);
   }
}

TEST_CASE("Differential: i32 addition", "[compliance]") {
   auto results = run_differential(add_wasm, "add", (uint32_t)100, (uint32_t)200);
   INFO(check_agreement(results));
   REQUIRE(check_agreement(results).empty());
   for (auto& r : results) {
      REQUIRE(r.value.has_value());
      CHECK(r.value->to_ui32() == 300u);
   }
}

TEST_CASE("Differential: i32 addition edge cases", "[compliance]") {
   // Zero
   auto r1 = run_differential(add_wasm, "add", (uint32_t)0, (uint32_t)0);
   REQUIRE(check_agreement(r1).empty());
   CHECK(r1[0].value->to_ui32() == 0u);

   // Max
   auto r2 = run_differential(add_wasm, "add", (uint32_t)0xFFFFFFFF, (uint32_t)1);
   REQUIRE(check_agreement(r2).empty());
   CHECK(r2[0].value->to_ui32() == 0u); // overflow wraps

   // Large values
   auto r3 = run_differential(add_wasm, "add", (uint32_t)0x7FFFFFFF, (uint32_t)0x7FFFFFFF);
   REQUIRE(check_agreement(r3).empty());
}

TEST_CASE("Differential: i32 subtraction (order-dependent)", "[compliance]") {
   auto r1 = run_differential(sub_wasm, "sub", (uint32_t)10, (uint32_t)3);
   REQUIRE(check_agreement(r1).empty());
   CHECK(r1[0].value->to_ui32() == 7u);

   auto r2 = run_differential(sub_wasm, "sub", (uint32_t)3, (uint32_t)10);
   REQUIRE(check_agreement(r2).empty());
   // 3 - 10 wraps to 0xFFFFFFF9
   CHECK(r2[0].value->to_ui32() == 0xFFFFFFF9u);
}

TEST_CASE("Differential: all engines enumerate", "[compliance]") {
   auto engines = all_engines();
#if defined(__x86_64__) || defined(__aarch64__)
   CHECK(engines.size() == 3);
#else
   CHECK(engines.size() == 1);
#endif
}
