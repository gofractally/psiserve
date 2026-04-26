// host.cpp — uses psio1::own<T>/borrow<T> with handle_table.

#include <psizam/runtime.hpp>
#include <psizam/hosted.hpp>
#include <psizam/handle_table.hpp>

#include "blockchain_wasm.hpp"
#include "contract_wasm.hpp"
#include "shared.hpp"

#include <cstdint>
#include <iostream>
#include <unordered_map>
#include <vector>

struct module_resource {
   psizam::module_handle mod;
};

struct instance_resource {
   psizam::instance inst;
};

struct Host
{
   psizam::runtime rt;
   psizam::handle_table<module_resource, 16>   modules{16};
   psizam::handle_table<instance_resource, 8>  instances{4};
   std::unordered_map<uint64_t, std::vector<uint8_t>> store;

   // ── wasm_runtime ────────────────────────────────────────────────

   psio1::own<wasm_module> module_create(std::string_view wasm_bytes)
   {
      std::vector<uint8_t> bytes(
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()),
         reinterpret_cast<const uint8_t*>(wasm_bytes.data()) + wasm_bytes.size());
      // Explicitly request the interpreter tier: `instance_resolve` and
      // `instance_call` below `static_cast` `backend_ptr()` to the
      // interpreter backend type, which is only sound when this matches
      // the dispatched kind. The cross-backend proxy lands in Step 10
      // of psizam-runtime-api-maturation.
      auto mod = rt.prepare(psizam::wasm_bytes{bytes}, psizam::instance_policy{
         .initial = psizam::instance_policy::compile_tier::interpret,
      });
      return psio1::own<wasm_module>{modules.create(module_resource{std::move(mod)})};
   }

   psio1::own<wasm_instance> module_instantiate(psio1::borrow<wasm_module> mod)
   {
      auto* m = modules.get(mod.handle);
      if (!m) return psio1::own<wasm_instance>{UINT32_MAX};
      auto inst = rt.instantiate(m->mod);
      return psio1::own<wasm_instance>{instances.create(instance_resource{std::move(inst)})};
   }

   void module_drop(psio1::own<wasm_module> mod)
   {
      modules.destroy(mod.handle);
   }

   uint32_t instance_resolve(psio1::borrow<wasm_instance> inst, psio1::name_id func_name)
   {
      auto* res = instances.get(inst.handle);
      if (!res) return UINT32_MAX;
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         res->inst.backend_ptr());
      return be->resolve_export(func_name.str());
   }

   uint64_t instance_call(psio1::borrow<wasm_instance> inst,
                          uint32_t func_index,
                          uint64_t arg0, uint64_t arg1)
   {
      auto* res = instances.get(inst.handle);
      if (!res) return 0;
      auto* be = static_cast<psizam::backend<std::nullptr_t, psizam::interpreter>*>(
         res->inst.backend_ptr());
      auto r = be->call_by_index(
         res->inst.host_ptr(), func_index,
         static_cast<uint32_t>(arg0), static_cast<uint32_t>(arg1));
      return r ? static_cast<uint64_t>(r->to_ui32()) : 0;
   }

   void instance_drop(psio1::own<wasm_instance> inst)
   {
      instances.destroy(inst.handle);
   }

   // ── module_store ────────────────────────────────────────────────

   std::string_view get_module(psio1::name_id name)
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

PSIO1_HOST_MODULE(Host,
   interface(wasm_runtime, module_create, module_instantiate, module_drop,
             instance_resolve, instance_call, instance_drop),
   interface(module_store, get_module),
   interface(env, log))

int main()
{
   using namespace psio1::literals;

   Host host;
   host.store["calculator"_n.value] = std::vector<uint8_t>(
      std::begin(contract_wasm_bytes),
      std::end(contract_wasm_bytes));

   std::cout << "=== Runtime Resource Example (own<T>/borrow<T>) ===\n";
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
