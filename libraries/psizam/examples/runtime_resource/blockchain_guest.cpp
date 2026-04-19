// blockchain_guest.cpp — the blockchain process WASM.
//
// Uses name_id for all identifiers. Resolves export indices once
// at setup, calls by integer index — no string lookup on hot path.

#include "shared.hpp"
#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

PSIO_WIT_SECTION(blockchain)
PSIO_WIT_SECTION(wasm_runtime)
PSIO_WIT_SECTION(module_store)
PSIO_WIT_SECTION(env)

// ── Import thunks ──────────────────────────────────────────────────
PSIO_IMPORT_IMPL(wasm_runtime, load_module)
PSIO_IMPORT_IMPL(module_store, get_module)
PSIO_IMPORT_IMPL(env, log)

uint32_t wasm_runtime::load_module(std::string_view wasm_bytes)
   PSIO_IMPORT_IMPL_BODY(wasm_runtime, load_module, wasm_bytes)

wit::string module_store::get_module(psio::name_id name)
   PSIO_IMPORT_IMPL_BODY(module_store, get_module, name)

void env::log(std::string_view msg)
   PSIO_IMPORT_IMPL_BODY(env, log, msg)

// Scalar imports — natural WASM signature via PSIO_IMPORT

// ── Blockchain implementation ──────────────────────────────────────

struct blockchain_impl
{
   uint64_t run_contract(psio::name_id contract_name,
                         uint64_t arg0, uint64_t arg1)
   {
      env::log("blockchain: fetching contract bytes");

      // Get contract WASM bytes by name (u64, one flat_val — no string)
      auto wasm_bytes = module_store::get_module(contract_name);

      env::log("blockchain: loading into runtime");
      auto mod = wasm_runtime::load_module(wasm_bytes.view());

      env::log("blockchain: instantiating");
      auto inst = wasm_runtime::instantiate(mod);

      // Resolve "add" to an integer index — ONCE
      using namespace psio::literals;
      auto add_idx = wasm_runtime::resolve_export(inst, "add"_n);

      env::log("blockchain: calling by index");
      // Call by integer index — no string lookup
      auto result = wasm_runtime::call_by_index(inst, add_idx, arg0, arg1);

      wasm_runtime::destroy_instance(inst);
      wasm_runtime::destroy_module(mod);

      env::log("blockchain: done");
      return result;
   }
};

PSIO_MODULE(blockchain_impl, run_contract)
