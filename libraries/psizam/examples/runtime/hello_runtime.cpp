// hello_runtime.cpp — Phase 1: native C++ hello world using the runtime API.
//
// Loads the hello_world guest WASM via the runtime, exercises:
//   - instance_policy configuration
//   - gas_state (monotonic consumed + moving deadline)
//   - module_handle preparation
//   - instance lifecycle
//   - typed proxy calls
//
// This is a NATIVE example — the runtime loads WASM but the driver
// is plain C++. Phase 2 will move the driver into WASM (blockchain
// process launching smart contracts).

#include <psizam/runtime.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

// Read a file into a byte vector
static std::vector<uint8_t> read_file(const char* path) {
   std::ifstream f(path, std::ios::binary);
   if (!f) return {};
   return {std::istreambuf_iterator<char>(f), {}};
}

// Gas handler: print consumed and advance deadline
static void gas_handler_print(psizam::gas_state* gas, void*) {
   std::cout << "  [gas] consumed=" << gas->consumed
             << " deadline=" << gas->deadline << "\n";
   gas->deadline += 10000;  // advance by another slice
}

int main(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: hello_runtime <guest.wasm>\n";
      return 1;
   }

   auto wasm_data = read_file(argv[1]);
   if (wasm_data.empty()) {
      std::cerr << "Cannot read " << argv[1] << "\n";
      return 1;
   }

   using namespace psizam;

   // ── Create runtime ──────────────────────────────────────────────
   runtime rt({
      .guarded_pool_size = 4,   // small pool for demo
      .arena_size_gb     = 1,
   });

   // ── Prepare module with policy ──────────────────────────────────
   auto policy = instance_policy{
      .trust_level = instance_policy::runtime_trust::untrusted,
      .floats      = instance_policy::float_mode::soft,
      .memory      = instance_policy::mem_safety::checked,
      .initial     = instance_policy::compile_tier::jit1,
      .optimized   = instance_policy::compile_tier::jit_llvm,
      .metering    = instance_policy::meter_mode::gas_trap,
      .gas_budget  = 1'000'000,
   };

   std::cout << "Policy:\n"
             << "  trust:    untrusted\n"
             << "  floats:   soft\n"
             << "  memory:   checked\n"
             << "  initial:  jit1\n"
             << "  optimized: jit_llvm\n"
             << "  metering: gas_trap\n"
             << "  budget:   " << policy.gas_budget << "\n\n";

   // ── Shared gas state ────────────────────────────────────────────
   auto gas = std::make_shared<gas_state>();
   gas->consumed = 0;
   gas->deadline = policy.gas_budget;
   policy.shared_gas = gas;

   std::cout << "Gas state:\n"
             << "  consumed: " << gas->consumed << "\n"
             << "  deadline: " << gas->deadline << "\n\n";

   // ── Check unresolved imports ────────────────────────────────────
   auto unresolved = rt.check(wasm_bytes{wasm_data});
   if (!unresolved.empty()) {
      std::cout << "Unresolved imports:\n";
      for (auto& imp : unresolved)
         std::cout << "  " << imp.module_name << "." << imp.func_name << "\n";
      std::cout << "\n";
   }

   // TODO Phase 2: prepare, instantiate, call via typed proxy
   // auto tmpl = rt.prepare(wasm_bytes{wasm_data}, policy);
   // auto inst = rt.instantiate(tmpl);
   // inst.as<greeter>().run(5);
   // std::cout << "Gas consumed: " << inst.gas_consumed() << "\n";

   std::cout << "Runtime API surface compiles and links.\n";
   std::cout << "Phase 2: wire prepare/instantiate/call.\n";

   return 0;
}
