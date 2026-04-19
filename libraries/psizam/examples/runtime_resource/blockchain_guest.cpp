// blockchain_guest.cpp — the blockchain process WASM.
//
// Imports wasm_runtime, module_store, env from the host.
// Gets contract WASM bytes from module_store (string return),
// loads into runtime, instantiates, calls add.

#include "shared.hpp"
#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

#include <string.h>

PSIO_WIT_SECTION(blockchain)
PSIO_WIT_SECTION(wasm_runtime)
PSIO_WIT_SECTION(module_store)
PSIO_WIT_SECTION(env)

// ── Import thunks (canonical — string args or returns) ─────────────
PSIO_IMPORT_IMPL(wasm_runtime, load_module, call)
PSIO_IMPORT_IMPL(module_store, get_module)
PSIO_IMPORT_IMPL(env, log)

uint32_t wasm_runtime::load_module(std::string_view wasm_bytes)
   PSIO_IMPORT_IMPL_BODY(wasm_runtime, load_module, wasm_bytes)

uint64_t wasm_runtime::call(uint32_t instance_handle,
                            std::string_view func_name,
                            uint64_t arg0, uint64_t arg1)
   PSIO_IMPORT_IMPL_BODY(wasm_runtime, call,
                         instance_handle, func_name, arg0, arg1)

wit::string module_store::get_module(std::string_view name)
   PSIO_IMPORT_IMPL_BODY(module_store, get_module, name)

void env::log(std::string_view msg)
   PSIO_IMPORT_IMPL_BODY(env, log, msg)

// wasm_runtime scalar methods use PSIO_IMPORT (natural signature)

// ── Blockchain implementation ──────────────────────────────────────

struct blockchain_impl
{
   uint64_t run_contract(std::string_view contract_name,
                         uint64_t arg0, uint64_t arg1)
   {
      env::log("blockchain: fetching contract bytes");

      // Get contract WASM bytes from module_store (string return!)
      auto wasm_bytes = module_store::get_module(contract_name);

      env::log("blockchain: loading into runtime");
      auto mod = wasm_runtime::load_module(
         std::string_view{wasm_bytes.data(), wasm_bytes.size()});

      env::log("blockchain: instantiating");
      auto inst = wasm_runtime::instantiate(mod);

      env::log("blockchain: calling add");
      auto result = wasm_runtime::call(inst, "add", arg0, arg1);

      wasm_runtime::destroy_instance(inst);
      wasm_runtime::destroy_module(mod);

      env::log("blockchain: done");
      return result;
   }
};

PSIO_MODULE(blockchain_impl, run_contract)
