#pragma once

// shared.hpp — interfaces for the runtime-resource example.
//
// Uses name_id (u64) for all identifiers — no string dispatch on
// the hot path. Function names are resolved to integer indices at
// instantiate time via resolve_export.

#include <stdint.h>
#include <string_view>
#include <psio/guest_attrs.hpp>
#include <psio/name.hpp>
#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>

PSIO_PACKAGE(runtime_resource, "0.1.0");

// ── wasm_runtime — host-provided resource ──────────────────────────

struct wasm_runtime
{
   // Load a WASM module from bytes, returns a module handle
   PSIO_IMPORT(wasm_runtime, load_module)
   static uint32_t load_module(std::string_view wasm_bytes);

   // Instantiate a loaded module, returns an instance handle
   PSIO_IMPORT(wasm_runtime, instantiate)
   static uint32_t instantiate(uint32_t module_handle);

   // Resolve an export name to an integer index (once, at setup)
   PSIO_IMPORT(wasm_runtime, resolve_export)
   static uint32_t resolve_export(uint32_t instance_handle, psio::name_id func_name);

   // Call by pre-resolved index — no string lookup
   PSIO_IMPORT(wasm_runtime, call_by_index)
   static uint64_t call_by_index(uint32_t instance_handle,
                                 uint32_t func_index,
                                 uint64_t arg0, uint64_t arg1);

   PSIO_IMPORT(wasm_runtime, destroy_instance)
   static void destroy_instance(uint32_t instance_handle);

   PSIO_IMPORT(wasm_runtime, destroy_module)
   static void destroy_module(uint32_t module_handle);
};

PSIO_INTERFACE(wasm_runtime, types(),
   funcs(func(load_module,      wasm_bytes),
         func(instantiate,      module_handle),
         func(resolve_export,   instance_handle, func_name),
         func(call_by_index,    instance_handle, func_index, arg0, arg1),
         func(destroy_instance, instance_handle),
         func(destroy_module,   module_handle)))

// ── module_store — host returns contract bytes ─────────────────────

struct module_store
{
   static wit::string get_module(psio::name_id name);
};

PSIO_INTERFACE(module_store, types(),
   funcs(func(get_module, name)))

// ── env — basic logging ────────────────────────────────────────────

struct env
{
   PSIO_IMPORT(env, log)
   static void log(std::string_view msg);
};

PSIO_INTERFACE(env, types(),
   funcs(func(log, msg)))

// ── blockchain — the blockchain process exports ────────────────────

struct blockchain
{
   static uint64_t run_contract(psio::name_id contract_name,
                                uint64_t arg0, uint64_t arg1);
};

PSIO_INTERFACE(blockchain, types(),
   funcs(func(run_contract, contract_name, arg0, arg1)))

// ── calculator — the smart contract exports ────────────────────────

struct calculator
{
   static uint32_t add(uint32_t a, uint32_t b);
   static uint32_t multiply(uint32_t a, uint32_t b);
};

PSIO_INTERFACE(calculator, types(),
   funcs(func(add, a, b),
         func(multiply, a, b)))
