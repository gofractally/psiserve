// host.cpp — native host for the runtime-resource example.
//
// Provides wasm_runtime + module_store + env to the blockchain WASM.
// Pre-loads the contract WASM into the runtime and returns handles.

#include <psizam/runtime.hpp>
#include <psizam/hosted.hpp>

#include "blockchain_wasm.hpp"
#include "contract_wasm.hpp"
#include "shared.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

struct Host
{
   psizam::runtime rt;
   std::vector<psizam::module_handle> modules;
   std::vector<psizam::instance>      instances;

   // Pre-loaded module handles by name
   std::unordered_map<std::string, uint32_t> module_handles;

   // ── wasm_runtime ────────────────────────────────────────────────

   uint32_t instantiate(uint32_t module_handle)
   {
      try {
         auto inst = rt.instantiate(modules[module_handle]);
         uint32_t handle = static_cast<uint32_t>(instances.size());
         instances.push_back(std::move(inst));
         return handle;
      } catch (const std::exception& e) {
         std::cerr << "  instantiate failed: " << e.what() << "\n";
         return UINT32_MAX;
      }
   }

   uint64_t call(uint32_t instance_handle,
                 std::string_view func_name,
                 uint64_t arg0, uint64_t arg1)
   {
      auto& inst = instances[instance_handle];
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         inst.backend_ptr());

      // Pass args matching the export's actual signature.
      // The contract uses natural WASM types (not 16-wide PSIO_MODULE).
      auto r = be->call_with_return(
         inst.host_ptr(), func_name,
         static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1));

      return r ? static_cast<uint64_t>(r->to_ui32()) : 0;
   }

   void destroy_instance(uint32_t handle) {
      if (handle < instances.size())
         instances[handle] = psizam::instance{};
   }

   void destroy_module(uint32_t handle) {
      if (handle < modules.size())
         modules[handle] = psizam::module_handle{};
   }

   // ── module_store ────────────────────────────────────────────────
   // Returns a pre-loaded module handle (scalar u32, no string return)

   uint32_t get_module_handle(std::string_view name)
   {
      auto it = module_handles.find(std::string{name});
      if (it == module_handles.end()) return UINT32_MAX;
      return it->second;
   }

   // ── env ─────────────────────────────────────────────────────────

   void log(std::string_view msg) {
      std::cout << "  [" << msg << "]\n";
   }

   // ── Pre-load a module ───────────────────────────────────────────

   void preload(const std::string& name,
                const uint8_t* data, size_t size)
   {
      std::vector<uint8_t> bytes(data, data + size);
      auto policy = psizam::instance_policy{};
      auto mod = rt.prepare(psizam::wasm_bytes{bytes}, policy);
      uint32_t handle = static_cast<uint32_t>(modules.size());
      modules.push_back(std::move(mod));
      module_handles[name] = handle;
   }
};

PSIO_HOST_MODULE(Host,
   interface(wasm_runtime, instantiate, call,
             destroy_instance, destroy_module),
   interface(module_store, get_module_handle),
   interface(env, log))

int main()
{
   Host host;

   // Pre-load the calculator contract into the runtime
   host.preload("calculator",
      contract_wasm_bytes.data(), contract_wasm_bytes.size());

   std::cout << "=== Runtime Resource Example ===\n\n";
   std::cout << "Host loads blockchain WASM.\n";
   std::cout << "Blockchain WASM uses wasm_runtime resource to:\n";
   std::cout << "  1. Get pre-loaded module handle from module_store\n";
   std::cout << "  2. Instantiate via wasm_runtime\n";
   std::cout << "  3. Call add(7, 11)\n\n";

   psizam::hosted<Host, psizam::interpreter> vm{blockchain_wasm_bytes, host};

   auto result = vm.as<blockchain>().run_contract(
      std::string_view{"calculator"}, uint64_t{7}, uint64_t{11});

   std::cout << "\nResult: " << result << "\n";
   std::cout << "Expected: 18\n";
   std::cout << (result == 18 ? "PASSED" : "FAILED") << "\n";

   return result == 18 ? 0 : 1;
}
