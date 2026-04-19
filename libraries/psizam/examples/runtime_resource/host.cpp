// host.cpp — native host for the runtime-resource example.
//
// Provides:
//   wasm_runtime  — backed by psizam::runtime
//   module_store  — returns embedded contract WASM bytes
//   env           — logging
//
// Loads the blockchain WASM, calls blockchain::run_contract,
// which internally uses wasm_runtime to load + call the contract.

#include <psizam/runtime.hpp>
#include <psizam/hosted.hpp>

#include "blockchain_wasm.hpp"   // embedded blockchain guest WASM
#include "contract_wasm.hpp"     // embedded contract guest WASM (for module_store)
#include "shared.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// ── Host implementation ────────────────────────────────────────────

struct Host
{
   psizam::runtime rt;

   // Handle tables
   std::vector<psizam::module_handle> modules;
   std::vector<psizam::instance>      instances;

   // Module store — maps contract names to WASM bytes
   std::unordered_map<std::string, std::vector<uint8_t>> store;

   // ── wasm_runtime implementation ─────────────────────────────────

   uint32_t load_module(std::string_view wasm_bytes)
   {
      std::vector<uint8_t> bytes(
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()),
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()) + wasm_bytes.size());

      // Use bounded allocator to avoid guard-page conflicts with
      // the outer blockchain WASM's allocator.
      auto policy = psizam::instance_policy{
         .memory = psizam::instance_policy::mem_safety::checked,
      };
      auto mod = rt.prepare(psizam::wasm_bytes{bytes}, policy);

      uint32_t handle = static_cast<uint32_t>(modules.size());
      modules.push_back(std::move(mod));
      return handle;
   }

   uint32_t instantiate(uint32_t module_handle)
   {
      try {
         auto inst = rt.instantiate(modules[module_handle]);
         uint32_t handle = static_cast<uint32_t>(instances.size());
         instances.push_back(std::move(inst));
         return handle;
      } catch (const std::exception& e) {
         std::cerr << "instantiate failed: " << e.what() << "\n";
         return UINT32_MAX;
      }
   }

   uint64_t call(uint32_t instance_handle,
                 std::string_view func_name,
                 uint64_t arg0, uint64_t arg1)
   {
      // For this demo: call the export by name with 16-wide flat_vals
      auto& inst = instances[instance_handle];
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         inst.backend_ptr());

      auto r = be->call_with_return(
         inst.host_ptr(), func_name,
         arg0, arg1,
         uint64_t{0}, uint64_t{0}, uint64_t{0}, uint64_t{0},
         uint64_t{0}, uint64_t{0}, uint64_t{0}, uint64_t{0},
         uint64_t{0}, uint64_t{0}, uint64_t{0}, uint64_t{0});

      return r ? r->to_ui64() : 0;
   }

   void destroy_instance(uint32_t handle)
   {
      if (handle < instances.size())
         instances[handle] = psizam::instance{};
   }

   void destroy_module(uint32_t handle)
   {
      if (handle < modules.size())
         modules[handle] = psizam::module_handle{};
   }

   // ── module_store implementation ─────────────────────────────────

   std::string_view get_module(std::string_view name)
   {
      auto it = store.find(std::string{name});
      if (it == store.end()) return {};
      return {reinterpret_cast<const char*>(it->second.data()),
              it->second.size()};
   }

   // ── env implementation ──────────────────────────────────────────

   void log(std::string_view msg)
   {
      std::cout << "  [" << msg << "]\n";
   }
};

PSIO_HOST_MODULE(Host,
   interface(wasm_runtime, load_module, instantiate, call,
             destroy_instance, destroy_module),
   interface(module_store, get_module),
   interface(env, log))

// ── Main ───────────────────────────────────────────────────────────

int main()
{
   Host host;

   // Register the contract WASM in the module store
   host.store["calculator"] = std::vector<uint8_t>(
      std::begin(contract_wasm_bytes),
      std::end(contract_wasm_bytes));

   std::cout << "=== Runtime Resource Example ===\n\n";
   std::cout << "Host loads blockchain WASM.\n";
   std::cout << "Blockchain WASM uses wasm_runtime resource to:\n";
   std::cout << "  1. Fetch contract bytes from module_store\n";
   std::cout << "  2. Load into runtime\n";
   std::cout << "  3. Instantiate\n";
   std::cout << "  4. Call add(7, 11)\n\n";

   // Load and run the blockchain WASM
   psizam::hosted<Host, psizam::interpreter> vm{blockchain_wasm_bytes, host};

   auto result = vm.as<blockchain>().run_contract(
      std::string_view{"calculator"}, uint64_t{7}, uint64_t{11});

   std::cout << "\nResult: " << result << "\n";
   std::cout << "Expected: 18\n";
   std::cout << (result == 18 ? "PASSED" : "FAILED") << "\n";

   return result == 18 ? 0 : 1;
}
