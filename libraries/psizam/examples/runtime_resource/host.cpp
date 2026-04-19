// host.cpp — native host with resource handle tables.
//
// The HOST defines the resource limits — psizam doesn't know about
// them. The handle_table capacity is set by the node operator,
// not by the WASM engine or the guest.

#include <psizam/runtime.hpp>
#include <psizam/hosted.hpp>
#include <psizam/handle_table.hpp>

#include "blockchain_wasm.hpp"
#include "contract_wasm.hpp"
#include "shared.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// wasm_type_traits<name_id> is in psio/name.hpp

// ── Resource types (host-owned) ────────────────────────────────────

struct module_resource {
   psizam::module_handle mod;
};

struct instance_resource {
   psizam::instance inst;
};

// ── Host ───────────────────────────────────────────────────────────

struct Host
{
   psizam::runtime rt;

   // Handle tables — LIMITS ARE SET HERE, by the host operator.
   // psizam knows nothing about these limits.
   psizam::handle_table<module_resource, 16>    modules{16};
   psizam::handle_table<instance_resource, 8>   instances{4};  // cap at 4, table holds 8

   // Module store keyed by name_id
   std::unordered_map<uint64_t, std::vector<uint8_t>> store;

   // ── wasm_runtime resource methods ───────────────────────────────

   uint32_t module_create(std::string_view wasm_bytes)
   {
      std::vector<uint8_t> bytes(
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()),
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()) + wasm_bytes.size());
      auto mod = rt.prepare(psizam::wasm_bytes{bytes}, psizam::instance_policy{});
      auto handle = modules.create(module_resource{std::move(mod)});
      if (handle == decltype(modules)::invalid_handle)
         std::cerr << "  module_create: resource limit exceeded\n";
      return handle;
   }

   uint32_t module_instantiate(uint32_t module_handle)
   {
      auto* mod = modules.get(module_handle);
      if (!mod) return psizam::handle_table<instance_resource, 8>::invalid_handle;

      auto inst = rt.instantiate(mod->mod);
      auto handle = instances.create(instance_resource{std::move(inst)});
      if (handle == decltype(instances)::invalid_handle)
         std::cerr << "  module_instantiate: resource limit exceeded\n";
      return handle;
   }

   void module_drop(uint32_t handle)
   {
      modules.destroy(handle);
   }

   uint32_t instance_resolve(uint32_t instance_handle, psio::name_id func_name)
   {
      auto* res = instances.get(instance_handle);
      if (!res) return UINT32_MAX;
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         res->inst.backend_ptr());
      return be->resolve_export(func_name.str());
   }

   uint64_t instance_call(uint32_t instance_handle,
                          uint32_t func_index,
                          uint64_t arg0, uint64_t arg1)
   {
      auto* res = instances.get(instance_handle);
      if (!res) return 0;
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         res->inst.backend_ptr());
      auto r = be->call_by_index(
         res->inst.host_ptr(), func_index,
         static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1));
      return r ? static_cast<uint64_t>(r->to_ui32()) : 0;
   }

   void instance_drop(uint32_t handle)
   {
      instances.destroy(handle);
   }

   // ── module_store ────────────────────────────────────────────────

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
   interface(wasm_runtime, module_create, module_instantiate, module_drop,
             instance_resolve, instance_call, instance_drop),
   interface(module_store, get_module),
   interface(env, log))

int main()
{
   using namespace psio::literals;

   Host host;
   host.store["calculator"_n.value] = std::vector<uint8_t>(
      std::begin(contract_wasm_bytes),
      std::end(contract_wasm_bytes));

   std::cout << "=== Runtime Resource Example ===\n";
   std::cout << "  module limit:   " << host.modules.max_live() << "\n";
   std::cout << "  instance limit: " << host.instances.max_live() << "\n\n";

   psizam::hosted<Host, psizam::interpreter> vm{blockchain_wasm_bytes, host};

   auto result = vm.as<blockchain>().run_contract(
      "calculator"_n, uint64_t{7}, uint64_t{11});

   std::cout << "\nResult: " << result << "\n";
   std::cout << "Expected: 18\n";
   std::cout << "Modules alive: " << host.modules.live_count() << "\n";
   std::cout << "Instances alive: " << host.instances.live_count() << "\n";
   std::cout << (result == 18 ? "PASSED" : "FAILED") << "\n";

   return result == 18 ? 0 : 1;
}
