// host.cpp — composition demo exercising complex types across modules.

#include <psizam/composition.hpp>

#include "provider_wasm.hpp"
#include "consumer_wasm.hpp"
#include "shared.hpp"

#include <cstdint>
#include <iostream>

struct Host
{
   void log_u64(std::uint64_t n) {
      std::cout << "  [host] log_u64(" << n << ")\n";
   }
   void log_string(std::string_view msg) {
      std::cout << "  [host] log_string(\"" << msg << "\")\n";
   }
};

PSIO1_HOST_MODULE(Host,
         interface(env, log_u64, log_string))

int main()
{
   Host host;
   psizam::composition<Host, psizam::interpreter> comp{host};

   auto& provider = comp.add(provider_wasm_bytes);
   auto& consumer = comp.add(consumer_wasm_bytes);
   comp.register_host<Host>(provider);
   comp.register_host<Host>(consumer);
   comp.link<greeter>(consumer, provider);
   comp.instantiate();

   bool pass = true;
   auto check = [&](const char* name, bool ok) {
      std::cout << (ok ? "  PASS" : "  FAIL") << ": " << name << "\n";
      if (!ok) pass = false;
   };

   // ── Scalar i32 ───────────────────────────────────────────────────
   std::cout << "\n=== scalar i32 add ===\n";
   uint32_t sum = consumer.as<processor>().test_add(7u, 11u);
   check("add(7, 11) = 18", sum == 18);

   // ── Scalar i64 ───────────────────────────────────────────────────
   std::cout << "\n=== scalar i64 double ===\n";
   uint64_t dbl = consumer.as<processor>().test_double(uint64_t{21});
   check("double(21) = 42", dbl == 42);

   // ── String concat ────────────────────────────────────────────────
   std::cout << "\n=== string concat ===\n";
   auto joined = consumer.as<processor>().test_concat(
      std::string_view{"hello, "}, std::string_view{"world"});
   check("concat = 'hello, world'", joined.view() == "hello, world");

   // ── Record round-trip (point) ────────────────────────────────────
   std::cout << "\n=== record translate ===\n";
   point tp = consumer.as<processor>().test_translate(
      point{10, 20}, -3, 7);
   check("translate({10,20}, -3, 7) = {7, 27}",
         tp.x == 7 && tp.y == 27);

   // ── List of scalars (dozen items) ────────────────────────────────
   std::cout << "\n=== list<u32> sum (12 items) ===\n";
   std::vector<uint32_t> nums = {1,2,3,4,5,6,7,8,9,10,11,12};
   uint32_t ls = consumer.as<processor>().test_sum_list(nums);
   check("sum([1..12]) = 78", ls == 78);

   // ── List of records (make_grid 4×3 = 12 points) ──────────────────
   std::cout << "\n=== list<point> make_grid(4, 3) ===\n";
   auto grid = consumer.as<processor>().test_make_grid(4u, 3u);
   check("grid has 12 points", grid.size() == 12);
   bool grid_ok = true;
   for (std::size_t i = 0; i < grid.size(); ++i) {
      uint32_t ex = static_cast<uint32_t>(i % 4);
      uint32_t ey = static_cast<uint32_t>(i / 4);
      if (grid[i].x != ex || grid[i].y != ey) {
         std::cout << "    grid[" << i << "] = (" << grid[i].x << "," << grid[i].y
                   << "), expected (" << ex << "," << ey << ")\n";
         grid_ok = false;
      }
   }
   check("grid contents correct", grid_ok);

   // ── Summary ──────────────────────────────────────────────────────
   std::cout << "\n" << (pass ? "All composition tests PASSED." : "SOME TESTS FAILED.") << "\n";
   return pass ? 0 : 1;
}
