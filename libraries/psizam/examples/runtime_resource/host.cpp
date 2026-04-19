// host.cpp — native host for the runtime-resource example.
//
// Uses name_id for all lookups. Provides resolve_export + call_by_index
// so the blockchain WASM resolves names once and dispatches by integer.

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

   // Module store keyed by name_id (u64) — no string hashing
   std::unordered_map<uint64_t, std::vector<uint8_t>> store;

   // ── wasm_runtime ────────────────────────────────────────────────

   uint32_t load_module(std::string_view wasm_bytes)
   {
      std::vector<uint8_t> bytes(
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()),
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()) + wasm_bytes.size());
      auto mod = rt.prepare(psizam::wasm_bytes{bytes}, psizam::instance_policy{});
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

   // Resolve export name → integer index (once per function)
   uint32_t resolve_export(uint32_t instance_handle, psio::name_id func_name)
   {
      auto& inst = instances[instance_handle];
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         inst.backend_ptr());
      // name_id → string → export index. This decode+lookup happens
      // ONCE per resolve. All subsequent calls use the integer index.
      return be->resolve_export(func_name.str());
   }

   // Call by pre-resolved index — no string lookup
   uint64_t call_by_index(uint32_t instance_handle,
                          uint32_t func_index,
                          uint64_t arg0, uint64_t arg1)
   {
      auto& inst = instances[instance_handle];
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         inst.backend_ptr());
      auto r = be->call_by_index(
         inst.host_ptr(), func_index,
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

   // ── module_store — keyed by name_id ─────────────────────────────

   std::string_view get_module(psio::name_id name)
   {
      auto it = store.find(name.value);
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
   interface(wasm_runtime, load_module, instantiate, resolve_export,
             call_by_index, destroy_instance, destroy_module),
   interface(module_store, get_module),
   interface(env, log))

int main()
{
   using namespace psio::literals;

   Host host;
   host.store["calculator"_n.value] = std::vector<uint8_t>(
      std::begin(contract_wasm_bytes),
      std::end(contract_wasm_bytes));

   std::cout << "=== Runtime Resource Example (name_id + call_by_index) ===\n\n";

   psizam::hosted<Host, psizam::interpreter> vm{blockchain_wasm_bytes, host};

   auto result = vm.as<blockchain>().run_contract(
      "calculator"_n, uint64_t{7}, uint64_t{11});

   std::cout << "\nResult: " << result << "\n";
   std::cout << "Expected: 18\n";
   std::cout << (result == 18 ? "PASSED" : "FAILED") << "\n";

   return result == 18 ? 0 : 1;
}
