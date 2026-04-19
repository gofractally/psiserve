#pragma once

// shared.hpp — interfaces for the runtime-resource example.
//
// Three layers:
//   wasm_runtime  — host provides to blockchain WASM (load, instantiate, call)
//   blockchain    — blockchain WASM exports (the entry point host calls)
//   calculator    — smart contract exports (loaded by blockchain via runtime)

#include <stdint.h>
#include <string_view>
#include <psio/guest_attrs.hpp>
#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>

PSIO_PACKAGE(runtime_resource, "0.1.0");

// ── wasm_runtime — host-provided resource ──────────────────────────
// The blockchain WASM imports this. Opaque u32 handles for modules
// and instances. The host backs it with psizam::runtime.

struct wasm_runtime
{
   // Load a WASM module from bytes, returns a module handle
   PSIO_IMPORT(wasm_runtime, load_module)
   static uint32_t load_module(std::string_view wasm_bytes);

   // Instantiate a loaded module, returns an instance handle
   PSIO_IMPORT(wasm_runtime, instantiate)
   static uint32_t instantiate(uint32_t module_handle);

   // Call a function on an instance by name, passing scalar args
   // Returns the scalar result
   PSIO_IMPORT(wasm_runtime, call)
   static uint64_t call(uint32_t instance_handle,
                        std::string_view func_name,
                        uint64_t arg0, uint64_t arg1);

   // Destroy an instance
   PSIO_IMPORT(wasm_runtime, destroy_instance)
   static void destroy_instance(uint32_t instance_handle);

   // Destroy a module
   PSIO_IMPORT(wasm_runtime, destroy_module)
   static void destroy_module(uint32_t module_handle);
};

PSIO_INTERFACE(wasm_runtime, types(),
   funcs(func(load_module,      wasm_bytes),
         func(instantiate,      module_handle),
         func(call,             instance_handle, func_name, arg0, arg1),
         func(destroy_instance, instance_handle),
         func(destroy_module,   module_handle)))

// ── module_store — host pre-loads modules, returns handles ─────────
// Returns a module handle for a named contract. The host has already
// loaded the WASM bytes into the runtime — the blockchain WASM just
// gets the handle (a u32 scalar, no string return needed).

struct module_store
{
   PSIO_IMPORT(module_store, get_module_handle)
   static uint32_t get_module_handle(std::string_view name);
};

PSIO_INTERFACE(module_store, types(),
   funcs(func(get_module_handle, name)))

// ── env — basic logging ────────────────────────────────────────────

struct env
{
   PSIO_IMPORT(env, log)
   static void log(std::string_view msg);
};

PSIO_INTERFACE(env, types(),
   funcs(func(log, msg)))

// ── blockchain — the blockchain process exports ────────────────────
// The host calls this to start the blockchain process.

struct blockchain
{
   static uint64_t run_contract(std::string_view contract_name,
                                uint64_t arg0, uint64_t arg1);
};

PSIO_INTERFACE(blockchain, types(),
   funcs(func(run_contract, contract_name, arg0, arg1)))

// ── calculator — the smart contract exports ────────────────────────
// This is what the smart contract WASM exports. The blockchain
// process doesn't know about this interface at compile time —
// it calls by name through wasm_runtime::call.

struct calculator
{
   static uint32_t add(uint32_t a, uint32_t b);
   static uint32_t multiply(uint32_t a, uint32_t b);
};

PSIO_INTERFACE(calculator, types(),
   funcs(func(add, a, b),
         func(multiply, a, b)))
