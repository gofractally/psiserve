// Tests for the typed C++ API (pzam_typed.hpp).

#include <psizam/pzam_typed.hpp>
#include <catch2/catch.hpp>

using namespace psizam;
using namespace psizam::detail;

// Test host with reflected methods
struct test_host {
   uint32_t add(uint32_t a, uint32_t b) { return a + b; }
   void log_value(uint32_t v) { last_logged = v; }
   uint64_t get_counter() { return counter; }

   uint32_t last_logged = 0;
   uint64_t counter = 42;
};
PSIO1_REFLECT(test_host,
   method(add, a, b),
   method(log_value, v),
   method(get_counter))

// Test exports struct (just declares the shape)
struct test_exports {
   uint32_t init();
   uint32_t compute(uint32_t x, uint32_t y);
   void cleanup();
};
PSIO1_REFLECT(test_exports,
   method(init),
   method(compute, x, y),
   method(cleanup))

TEST_CASE("typed API: register_reflected adds all host methods", "[pzam_typed]") {
   host_function_table table;
   register_reflected<test_host>(table, "env");

   CHECK(table.size() == 3);

   // Verify the entries have correct names
   const auto& entries = table.entries();
   REQUIRE(entries.size() == 3);
   CHECK(entries[0].module_name == "env");
   CHECK(entries[0].func_name == "add");
   CHECK(entries[1].func_name == "log_value");
   CHECK(entries[2].func_name == "get_counter");
}

TEST_CASE("typed API: register_reflected with custom module name", "[pzam_typed]") {
   host_function_table table;
   register_reflected<test_host>(table, "mymod");

   const auto& entries = table.entries();
   REQUIRE(entries.size() == 3);
   CHECK(entries[0].module_name == "mymod");
   CHECK(entries[1].module_name == "mymod");
   CHECK(entries[2].module_name == "mymod");
}

TEST_CASE("typed API: register_reflected functions are callable", "[pzam_typed]") {
   host_function_table table;
   register_reflected<test_host>(table, "env");

   test_host host;

   // Call the 'add' function through the trampoline
   native_value args[2];
   args[0].i32 = 10;
   args[1].i32 = 20;
   auto result = table.call(&host, 0, args, nullptr);
   CHECK(result.i32 == 30);

   // Call 'log_value'
   args[0].i32 = 99;
   table.call(&host, 1, args, nullptr);
   CHECK(host.last_logged == 99);

   // Call 'get_counter'
   result = table.call(&host, 2, nullptr, nullptr);
   CHECK(result.i64 == 42);
}

TEST_CASE("typed API: export proxy type is constructible", "[pzam_typed]") {
   // Verify the export proxy template compiles
   // (can't test execution without a real module)
   using proxy_t = pzam_export_proxy<test_exports>;
   static_assert(!std::is_same_v<proxy_t, void>);
}
