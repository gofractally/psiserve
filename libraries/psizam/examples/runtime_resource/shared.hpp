#pragma once

// shared.hpp — interfaces using psio::own<T> and psio::borrow<T>
// resource types from wit_resource.hpp.

#include <stdint.h>
#include <string_view>
#include <psio/guest_attrs.hpp>
#include <psio/name.hpp>
#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>
#include <psio/wit_resource.hpp>

PSIO_PACKAGE(runtime_resource, "0.1.0");

// ── Resource types (inherit wit_resource for own<T>/borrow<T>) ─────

struct wasm_module : psio::wit_resource {};
PSIO_REFLECT(wasm_module)

struct wasm_instance : psio::wit_resource {};
PSIO_REFLECT(wasm_instance)

// ── wasm_runtime — typed resource interface ────────────────────────

struct wasm_runtime
{
   // module resource
   PSIO_IMPORT(wasm_runtime, module_create)
   static psio::own<wasm_module> module_create(std::string_view wasm_bytes);

   PSIO_IMPORT(wasm_runtime, module_instantiate)
   static psio::own<wasm_instance> module_instantiate(
      psio::borrow<wasm_module> mod);

   PSIO_IMPORT(wasm_runtime, module_drop)
   static void module_drop(psio::own<wasm_module> mod);

   // instance resource
   PSIO_IMPORT(wasm_runtime, instance_resolve)
   static uint32_t instance_resolve(psio::borrow<wasm_instance> inst,
                                    psio::name_id func_name);

   PSIO_IMPORT(wasm_runtime, instance_call)
   static uint64_t instance_call(psio::borrow<wasm_instance> inst,
                                 uint32_t func_index,
                                 uint64_t arg0, uint64_t arg1);

   PSIO_IMPORT(wasm_runtime, instance_drop)
   static void instance_drop(psio::own<wasm_instance> inst);
};

PSIO_INTERFACE(wasm_runtime, types(wasm_module, wasm_instance),
   funcs(func(module_create,      wasm_bytes),
         func(module_instantiate, mod),
         func(module_drop,        mod),
         func(instance_resolve,   inst, func_name),
         func(instance_call,      inst, func_index, arg0, arg1),
         func(instance_drop,      inst)))

// ── Guest-side RAII wrappers ───────────────────────────────────────

#ifdef __wasm__
namespace rt {

class instance;

class module {
   uint32_t h_;
public:
   explicit module(psio::own<wasm_module> o) : h_(o.handle) {}
   module(module&& o) noexcept : h_(o.h_) { o.h_ = UINT32_MAX; }
   ~module() {
      if (h_ != UINT32_MAX)
         wasm_runtime::module_drop(psio::own<wasm_module>{h_});
   }
   module(const module&) = delete;

   instance instantiate();
   psio::borrow<wasm_module> borrow() const {
      return psio::borrow<wasm_module>{h_};
   }
};

class instance {
   uint32_t h_;
public:
   explicit instance(psio::own<wasm_instance> o) : h_(o.handle) {}
   instance(instance&& o) noexcept : h_(o.h_) { o.h_ = UINT32_MAX; }
   ~instance() {
      if (h_ != UINT32_MAX)
         wasm_runtime::instance_drop(psio::own<wasm_instance>{h_});
   }
   instance(const instance&) = delete;

   uint32_t resolve(psio::name_id name) {
      return wasm_runtime::instance_resolve(
         psio::borrow<wasm_instance>{h_}, name);
   }
   uint64_t call(uint32_t idx, uint64_t a0, uint64_t a1) {
      return wasm_runtime::instance_call(
         psio::borrow<wasm_instance>{h_}, idx, a0, a1);
   }
};

inline instance module::instantiate() {
   return instance{wasm_runtime::module_instantiate(borrow())};
}

} // namespace rt
#endif

// ── module_store ───────────────────────────────────────────────────

struct module_store
{
   static wit::string get_module(psio::name_id name);
};

PSIO_INTERFACE(module_store, types(),
   funcs(func(get_module, name)))

// ── env ────────────────────────────────────────────────────────────

struct env
{
   PSIO_IMPORT(env, log)
   static void log(std::string_view msg);
};

PSIO_INTERFACE(env, types(),
   funcs(func(log, msg)))

// ── blockchain ─────────────────────────────────────────────────────

struct blockchain
{
   static uint64_t run_contract(psio::name_id contract_name,
                                uint64_t arg0, uint64_t arg1);
};

PSIO_INTERFACE(blockchain, types(),
   funcs(func(run_contract, contract_name, arg0, arg1)))
