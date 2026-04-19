#pragma once

// shared.hpp — interfaces for the runtime-resource example.
//
// Resources: module and instance are opaque u32 handles with
// constructor/method/drop semantics. The host manages handle
// tables with generation counters and capacity limits.

#include <stdint.h>
#include <string_view>
#include <psio/guest_attrs.hpp>
#include <psio/name.hpp>
#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>

PSIO_PACKAGE(runtime_resource, "0.1.0");

// ── wasm_runtime — host-provided resource interface ────────────────
// Resources are u32 handles. Methods take the handle as first arg.
// Drop functions release the handle back to the pool.

struct wasm_runtime
{
   // module resource
   PSIO_IMPORT(wasm_runtime, module_create)
   static uint32_t module_create(std::string_view wasm_bytes);

   PSIO_IMPORT(wasm_runtime, module_instantiate)
   static uint32_t module_instantiate(uint32_t module_handle);

   PSIO_IMPORT(wasm_runtime, module_drop)
   static void module_drop(uint32_t module_handle);

   // instance resource
   PSIO_IMPORT(wasm_runtime, instance_resolve)
   static uint32_t instance_resolve(uint32_t instance_handle,
                                    psio::name_id func_name);

   PSIO_IMPORT(wasm_runtime, instance_call)
   static uint64_t instance_call(uint32_t instance_handle,
                                 uint32_t func_index,
                                 uint64_t arg0, uint64_t arg1);

   PSIO_IMPORT(wasm_runtime, instance_drop)
   static void instance_drop(uint32_t instance_handle);
};

PSIO_INTERFACE(wasm_runtime, types(),
   funcs(func(module_create,      wasm_bytes),
         func(module_instantiate, module_handle),
         func(module_drop,        module_handle),
         func(instance_resolve,   instance_handle, func_name),
         func(instance_call,      instance_handle, func_index, arg0, arg1),
         func(instance_drop,      instance_handle)))

// ── Guest-side RAII wrappers ───────────────────────────────────────
// These live in the shared header so the guest gets type-safe
// resource handles with automatic drop.

#ifdef __wasm__
namespace rt {

class instance;

class module {
   uint32_t h_;
public:
   explicit module(uint32_t h) : h_(h) {}
   module(module&& o) noexcept : h_(o.h_) { o.h_ = UINT32_MAX; }
   ~module() { if (h_ != UINT32_MAX) wasm_runtime::module_drop(h_); }
   module(const module&) = delete;
   module& operator=(const module&) = delete;

   instance instantiate();
   uint32_t handle() const { return h_; }
};

class instance {
   uint32_t h_;
public:
   explicit instance(uint32_t h) : h_(h) {}
   instance(instance&& o) noexcept : h_(o.h_) { o.h_ = UINT32_MAX; }
   ~instance() { if (h_ != UINT32_MAX) wasm_runtime::instance_drop(h_); }
   instance(const instance&) = delete;
   instance& operator=(const instance&) = delete;

   uint32_t resolve(psio::name_id name) {
      return wasm_runtime::instance_resolve(h_, name);
   }
   uint64_t call(uint32_t func_idx, uint64_t a0, uint64_t a1) {
      return wasm_runtime::instance_call(h_, func_idx, a0, a1);
   }
   uint32_t handle() const { return h_; }
};

inline instance module::instantiate() {
   return instance{wasm_runtime::module_instantiate(h_)};
}

} // namespace rt
#endif

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
