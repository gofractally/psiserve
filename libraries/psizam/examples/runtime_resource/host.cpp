// host.cpp — native host for the runtime-resource example.
//
// Provides wasm_runtime + module_store + env to the blockchain WASM.
// module_store::get_module returns contract WASM bytes as a string
// (tests host→guest canonical string returns).

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
   std::unordered_map<std::string, std::vector<uint8_t>> store;

   // ── wasm_runtime ────────────────────────────────────────────────

   uint32_t load_module(std::string_view wasm_bytes)
   {
      std::vector<uint8_t> bytes(
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()),
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()) + wasm_bytes.size());

      auto policy = psizam::instance_policy{};
      auto mod = rt.prepare(psizam::wasm_bytes{bytes}, policy);
      uint32_t handle = static_cast<uint32_t>(modules.size());
      modules.push_back(std::move(mod));
      return handle;
   }

   uint32_t instantiate(uint32_t module_handle)
   {
      auto inst = rt.instantiate(modules[module_handle]);
      uint32_t handle = static_cast<uint32_t>(instances.size());
      instances.push_back(std::move(inst));
      return handle;
   }

   uint64_t call(uint32_t instance_handle,
                 std::string_view func_name,
                 uint64_t arg0, uint64_t arg1)
   {
      auto& inst = instances[instance_handle];
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         inst.backend_ptr());
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

   // ── module_store — returns contract bytes as string ─────────────

   std::string_view get_module(std::string_view name)
   {
      auto it = store.find(std::string{name});
      if (it == store.end()) return {};
      return {reinterpret_cast<const char*>(it->second.data()),
              it->second.size()};
   }

   // ── env ─────────────────────────────────────────────────────────

   void log(std::string_view msg) {
      std::cout << "  [" << msg << "]\n";
   }
};

PSIO_HOST_MODULE(Host,
   interface(wasm_runtime, load_module, instantiate, call,
             destroy_instance, destroy_module),
   interface(module_store, get_module),
   interface(env, log))

int main()
{
   Host host;
   host.store["calculator"] = std::vector<uint8_t>(
      std::begin(contract_wasm_bytes),
      std::end(contract_wasm_bytes));

   std::cout << "=== Runtime Resource Example ===\n\n";
   std::cout << "Blockchain WASM fetches contract bytes via string return,\n";
   std::cout << "loads into runtime, instantiates, calls add(7, 11).\n\n";

   psizam::hosted<Host, psizam::interpreter> vm{blockchain_wasm_bytes, host};

   auto result = vm.as<blockchain>().run_contract(
      std::string_view{"calculator"}, uint64_t{7}, uint64_t{11});

   std::cout << "\nResult: " << result << "\n";
   std::cout << "Expected: 18\n";
   std::cout << (result == 18 ? "PASSED" : "FAILED") << "\n";

   return result == 18 ? 0 : 1;
}
