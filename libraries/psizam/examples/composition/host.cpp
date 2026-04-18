// host.cpp — the composition demo host.
//
// Loads two WASM modules:
//   - provider: exports the greeter interface (add, concat, double_it)
//   - consumer: imports greeter, exports processor interface
//
// The host wires consumer's greeter imports to provider's greeter exports,
// then calls consumer's processor exports to exercise module-to-module calling.

#include <psizam/composition.hpp>

#include "provider_wasm.hpp"
#include "consumer_wasm.hpp"
#include "shared.hpp"

#include <cstdint>
#include <iostream>

// ── Host-side impl of env ───────────────────────────────────────────

struct Host
{
   void log_u64(std::uint64_t n)
   {
      std::cout << "  [host] log_u64(" << n << ")\n";
   }

   void log_string(std::string_view msg)
   {
      std::cout << "  [host] log_string(\"" << msg << "\")\n";
   }
};

PSIO_HOST_MODULE(Host,
         interface(env, log_u64, log_string))

// ── Driver ──────────────────────────────────────────────────────────

int main()
{
   Host host;
   psizam::composition<Host, psizam::interpreter> comp{host};

   // Add modules
   auto& provider = comp.add(provider_wasm_bytes);
   auto& consumer = comp.add(consumer_wasm_bytes);

   // Register host functions for both modules
   comp.register_host<Host>(provider);
   comp.register_host<Host>(consumer);

   // Wire consumer's greeter imports to provider's greeter exports
   comp.link<greeter>(consumer, provider);

   // Instantiate all modules
   comp.instantiate();

   // Test scalar add: consumer calls provider's greeter::add
   std::cout << "test_add(7, 11):\n";
   uint32_t sum = consumer.as<processor>().test_add(7u, 11u);
   std::cout << "  result = " << sum << "\n";
   std::cout << "  expected = 18\n\n";

   // Test scalar double: consumer calls provider's greeter::double_it
   std::cout << "test_double(21):\n";
   uint64_t dbl = consumer.as<processor>().test_double(uint64_t{21});
   std::cout << "  result = " << dbl << "\n";
   std::cout << "  expected = 42\n\n";

   // Test string concat: consumer calls provider's greeter::concat
   // This exercises the full bridge with memory copying between modules
   std::cout << "test_concat(\"hello, \", \"world\"):\n";
   auto joined = consumer.as<processor>().test_concat(
      std::string_view{"hello, "}, std::string_view{"world"});
   std::cout << "  result = \"" << joined.view() << "\"\n";
   std::cout << "  expected = \"hello, world\"\n\n";

   // Verify results
   bool pass = true;
   if (sum != 18) {
      std::cout << "FAIL: test_add returned " << sum << ", expected 18\n";
      pass = false;
   }
   if (dbl != 42) {
      std::cout << "FAIL: test_double returned " << dbl << ", expected 42\n";
      pass = false;
   }
   if (joined.view() != "hello, world") {
      std::cout << "FAIL: test_concat returned \"" << joined.view()
                << "\", expected \"hello, world\"\n";
      pass = false;
   }

   if (pass) {
      std::cout << "All composition tests PASSED.\n";
   }

   return pass ? 0 : 1;
}
