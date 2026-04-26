// blockchain_guest.cpp — uses psio1::own<T>/borrow<T> resource types.

#include "shared.hpp"
#include <psio1/guest_alloc.hpp>
#include <psizam/module.hpp>

PSIO1_WIT_SECTION(blockchain)
PSIO1_WIT_SECTION(wasm_runtime)
PSIO1_WIT_SECTION(module_store)
PSIO1_WIT_SECTION(env)

PSIO1_IMPORT_IMPL(wasm_runtime, module_create)
PSIO1_IMPORT_IMPL(module_store, get_module)
PSIO1_IMPORT_IMPL(env, log)

psio1::own<wasm_module> wasm_runtime::module_create(std::string_view wasm_bytes)
   PSIO1_IMPORT_IMPL_BODY(wasm_runtime, module_create, wasm_bytes)

wit::string module_store::get_module(psio1::name_id name)
   PSIO1_IMPORT_IMPL_BODY(module_store, get_module, name)

void env::log(std::string_view msg)
   PSIO1_IMPORT_IMPL_BODY(env, log, msg)

struct blockchain_impl
{
   uint64_t run_contract(psio1::name_id contract_name,
                         uint64_t arg0, uint64_t arg1)
   {
      env::log("blockchain: fetching contract bytes");
      auto wasm_bytes = module_store::get_module(contract_name);

      env::log("blockchain: creating module resource");
      rt::module mod{wasm_runtime::module_create(wasm_bytes.view())};

      env::log("blockchain: instantiating");
      rt::instance inst{mod.instantiate()};

      using namespace psio1::literals;
      auto add_idx = inst.resolve("add"_n);

      env::log("blockchain: calling by index");
      auto result = inst.call(add_idx, arg0, arg1);

      env::log("blockchain: done (resources auto-dropped)");
      return result;
   }
};

PSIO1_MODULE(blockchain_impl, run_contract)
