// blockchain_guest.cpp — the blockchain process WASM.
//
// Imports wasm_runtime (load/instantiate/call) and module_store
// (get contract bytes). Exports blockchain::run_contract which
// the host calls to execute a smart contract action.
//
// This demonstrates: WASM orchestrating other WASMs through a
// host-provided runtime resource.

#include "shared.hpp"
#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

#include <string.h>

PSIO_WIT_SECTION(blockchain)
PSIO_WIT_SECTION(wasm_runtime)
PSIO_WIT_SECTION(module_store)
PSIO_WIT_SECTION(env)

// ── Import thunks ──────────────────────────────────────────────────
// Canonical (string args) → 16-wide flat_val thunks
PSIO_IMPORT_IMPL(wasm_runtime, load_module, call)
PSIO_IMPORT_IMPL(module_store, get_module)
PSIO_IMPORT_IMPL(env, log)

// wasm_runtime canonical methods
uint32_t wasm_runtime::load_module(std::string_view wasm_bytes)
   PSIO_IMPORT_IMPL_BODY(wasm_runtime, load_module, wasm_bytes)

uint64_t wasm_runtime::call(uint32_t instance_handle,
                            std::string_view func_name,
                            uint64_t arg0, uint64_t arg1)
   PSIO_IMPORT_IMPL_BODY(wasm_runtime, call,
                         instance_handle, func_name, arg0, arg1)

// wasm_runtime scalar methods — use PSIO_IMPORT natural signature
// (declared with PSIO_IMPORT in shared.hpp, linked directly)

// module_store
wit::string module_store::get_module(std::string_view name)
   PSIO_IMPORT_IMPL_BODY(module_store, get_module, name)

// env
void env::log(std::string_view msg)
   PSIO_IMPORT_IMPL_BODY(env, log, msg)

// ── Blockchain implementation ──────────────────────────────────────

struct blockchain_impl
{
   uint64_t run_contract(std::string_view contract_name,
                         uint64_t arg0, uint64_t arg1)
   {
      env::log("blockchain: loading contract");

      // Get contract bytes from the module store
      auto wasm_bytes = module_store::get_module(contract_name);

      env::log("blockchain: loading into runtime");

      // Load into the runtime
      auto mod = wasm_runtime::load_module(wasm_bytes.view());

      env::log("blockchain: instantiating");

      // Instantiate
      auto inst = wasm_runtime::instantiate(mod);

      env::log("blockchain: calling add");

      // Call the contract's add function
      auto result = wasm_runtime::call(inst, "add", arg0, arg1);

      // Clean up
      wasm_runtime::destroy_instance(inst);
      wasm_runtime::destroy_module(mod);

      env::log("blockchain: done");

      return result;
   }
};

PSIO_MODULE(blockchain_impl, run_contract)
