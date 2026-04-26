#pragma once

// shared.hpp — interfaces using psio1::own<T> and psio1::borrow<T>
// resource types from wit_resource.hpp.

#include <stdint.h>
#include <string_view>
#include <psio1/guest_attrs.hpp>
#include <psio1/name.hpp>
#include <psio1/structural.hpp>
#include <psio1/wit_owned.hpp>
#include <psio1/wit_resource.hpp>

PSIO1_PACKAGE(runtime_resource, "0.1.0");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(runtime_resource)

// ── Resource types (inherit wit_resource for own<T>/borrow<T>) ─────

struct wasm_module : psio1::wit_resource {};
PSIO1_REFLECT(wasm_module)

struct wasm_instance : psio1::wit_resource {};
PSIO1_REFLECT(wasm_instance)

// ── wasm_runtime — typed resource interface ────────────────────────

struct wasm_runtime
{
   // module resource
   PSIO1_IMPORT(wasm_runtime, module_create)
   static psio1::own<wasm_module> module_create(std::string_view wasm_bytes);

   PSIO1_IMPORT(wasm_runtime, module_instantiate)
   static psio1::own<wasm_instance> module_instantiate(
      psio1::borrow<wasm_module> mod);

   PSIO1_IMPORT(wasm_runtime, module_drop)
   static void module_drop(psio1::own<wasm_module> mod);

   // instance resource
   PSIO1_IMPORT(wasm_runtime, instance_resolve)
   static uint32_t instance_resolve(psio1::borrow<wasm_instance> inst,
                                    psio1::name_id func_name);

   PSIO1_IMPORT(wasm_runtime, instance_call)
   static uint64_t instance_call(psio1::borrow<wasm_instance> inst,
                                 uint32_t func_index,
                                 uint64_t arg0, uint64_t arg1);

   PSIO1_IMPORT(wasm_runtime, instance_drop)
   static void instance_drop(psio1::own<wasm_instance> inst);
};

PSIO1_INTERFACE(wasm_runtime, types(wasm_module, wasm_instance),
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
   explicit module(psio1::own<wasm_module> o) : h_(o.handle) {}
   module(module&& o) noexcept : h_(o.h_) { o.h_ = UINT32_MAX; }
   ~module() {
      if (h_ != UINT32_MAX)
         wasm_runtime::module_drop(psio1::own<wasm_module>{h_});
   }
   module(const module&) = delete;

   instance instantiate();
   psio1::borrow<wasm_module> borrow() const {
      return psio1::borrow<wasm_module>{h_};
   }
};

class instance {
   uint32_t h_;
public:
   explicit instance(psio1::own<wasm_instance> o) : h_(o.handle) {}
   instance(instance&& o) noexcept : h_(o.h_) { o.h_ = UINT32_MAX; }
   ~instance() {
      if (h_ != UINT32_MAX)
         wasm_runtime::instance_drop(psio1::own<wasm_instance>{h_});
   }
   instance(const instance&) = delete;

   uint32_t resolve(psio1::name_id name) {
      return wasm_runtime::instance_resolve(
         psio1::borrow<wasm_instance>{h_}, name);
   }
   uint64_t call(uint32_t idx, uint64_t a0, uint64_t a1) {
      return wasm_runtime::instance_call(
         psio1::borrow<wasm_instance>{h_}, idx, a0, a1);
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
   static wit::string get_module(psio1::name_id name);
};

PSIO1_INTERFACE(module_store, types(),
   funcs(func(get_module, name)))

// ── env ────────────────────────────────────────────────────────────

struct env
{
   PSIO1_IMPORT(env, log)
   static void log(std::string_view msg);
};

PSIO1_INTERFACE(env, types(),
   funcs(func(log, msg)))

// ── blockchain ─────────────────────────────────────────────────────

struct blockchain
{
   static uint64_t run_contract(psio1::name_id contract_name,
                                uint64_t arg0, uint64_t arg1);
};

PSIO1_INTERFACE(blockchain, types(),
   funcs(func(run_contract, contract_name, arg0, arg1)))
