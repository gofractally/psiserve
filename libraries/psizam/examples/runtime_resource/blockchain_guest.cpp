// blockchain_guest.cpp — the blockchain process WASM.
//
// Imports wasm_runtime and module_store from the host. Uses them
// to get a pre-loaded contract module handle, instantiate it, and
// call its add function.

#include "shared.hpp"
#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

#include <string.h>

PSIO_WIT_SECTION(blockchain)
PSIO_WIT_SECTION(wasm_runtime)
PSIO_WIT_SECTION(module_store)
PSIO_WIT_SECTION(env)

// ── Import thunks ──────────────────────────────────────────────────
// Canonical (string args) → 16-wide thunks
PSIO_IMPORT_IMPL(wasm_runtime, call)
PSIO_IMPORT_IMPL(module_store, get_module_handle)
PSIO_IMPORT_IMPL(env, log)

uint64_t wasm_runtime::call(uint32_t instance_handle,
                            std::string_view func_name,
                            uint64_t arg0, uint64_t arg1)
   PSIO_IMPORT_IMPL_BODY(wasm_runtime, call,
                         instance_handle, func_name, arg0, arg1)

// module_store — takes string, returns scalar handle
uint32_t module_store::get_module_handle(std::string_view name)
   PSIO_IMPORT_IMPL_BODY(module_store, get_module_handle, name)

// env
void env::log(std::string_view msg)
   PSIO_IMPORT_IMPL_BODY(env, log, msg)

// wasm_runtime scalar methods — use PSIO_IMPORT natural signature
// (declared with PSIO_IMPORT in shared.hpp)

// ── Blockchain implementation ──────────────────────────────────────

struct blockchain_impl
{
   uint64_t run_contract(std::string_view contract_name,
                         uint64_t arg0, uint64_t arg1)
   {
      env::log("blockchain: getting module handle");

      // Host already loaded the contract — just get the handle
      auto mod = module_store::get_module_handle(contract_name);

      env::log("blockchain: instantiating");
      auto inst = wasm_runtime::instantiate(mod);

      env::log("blockchain: calling add");
      auto result = wasm_runtime::call(inst, "add", arg0, arg1);

      wasm_runtime::destroy_instance(inst);

      env::log("blockchain: done");
      return result;
   }
};

PSIO_MODULE(blockchain_impl, run_contract)
