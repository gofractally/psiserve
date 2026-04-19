#pragma once

// psizam/runtime.hpp — Top-level WASM runtime: load, compile, link,
// cache, and instantiate WASM modules with policy-driven execution.
//
// Phase 1: native API surface + gas state + instance policy.
//
// Usage:
//   psizam::runtime rt({.guarded_pool_size = 64});
//   rt.provide<chain_api, crypto_api>(host);
//   rt.register_library("libc++", archive_bytes{libc_data});
//
//   auto tmpl = rt.prepare(wasm_bytes{contract_data}, {
//      .trust_level = runtime_trust::untrusted,
//      .floats      = float_mode::soft,
//      .memory      = mem_safety::checked,
//      .initial     = compile_tier::jit1,
//      .optimized   = compile_tier::jit_llvm,
//      .metering    = meter_mode::gas_trap,
//      .gas_budget  = 1'000'000,
//   });
//
//   auto inst = rt.instantiate(tmpl);
//   inst.as<greeter>().run(5);

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psizam {

// ═════════════════════════════════════════════════════════════════════
// Type-safe byte containers
// ═════════════════════════════════════════════════════════════════════

struct wasm_bytes {
   std::span<const uint8_t> data;
   explicit wasm_bytes(std::span<const uint8_t> d) : data(d) {}

   template <typename Container>
   explicit wasm_bytes(const Container& c)
      : data(reinterpret_cast<const uint8_t*>(c.data()), c.size()) {}
};

struct pzam_bytes {
   std::span<const uint8_t> data;
   explicit pzam_bytes(std::span<const uint8_t> d) : data(d) {}
};

struct archive_bytes {
   std::span<const uint8_t> data;
   explicit archive_bytes(std::span<const uint8_t> d) : data(d) {}
};

// ═════════════════════════════════════════════════════════════════════
// Gas state — monotonic consumed counter + moving deadline
// ═════════════════════════════════════════════════════════════════════
//
// Lives in host memory (not WASM linear memory). During JIT execution,
// consumed/deadline are held in dedicated registers and spilled at
// call boundaries. Shared across instances via shared_ptr when the
// policy groups them under one gas counter.

struct gas_state {
   uint64_t consumed = 0;     // monotonic, only increments
   uint64_t deadline = 0;     // handler advances this on yield/restock
};

// ═════════════════════════════════════════════════════════════════════
// Gas handler — called when consumed >= deadline
// ═════════════════════════════════════════════════════════════════════

struct execution_context_base;  // forward

using gas_handler_t = void (*)(gas_state* gas, void* user_data);

// ═════════════════════════════════════════════════════════════════════
// Instance policy
// ═════════════════════════════════════════════════════════════════════

struct instance_policy {

   // ── Runtime trust ───────────────────────────────────────────────
   enum class runtime_trust { untrusted, trusted };

   // ── Safety ──────────────────────────────────────────────────────
   enum class float_mode  { soft, native };
   enum class mem_safety  { guarded, checked, unchecked };

   runtime_trust trust_level = runtime_trust::untrusted;
   float_mode    floats      = float_mode::soft;
   mem_safety    memory      = mem_safety::checked;

   // ── Compilation ─────────────────────────────────────────────────
   enum class compile_tier  { interpret, jit1, jit2, jit_llvm };
   enum class compile_trust { native, sandboxed };

   compile_tier   initial    = compile_tier::jit1;
   compile_tier   optimized  = compile_tier::jit_llvm;
   compile_trust  compile    = compile_trust::native;
   uint32_t       compile_timeout_ms = 5000;

   // ── Resources ───────────────────────────────────────────────────
   uint32_t  max_pages = 256;
   uint32_t  max_stack = 64 * 1024;

   // ── Metering ────────────────────────────────────────────────────
   enum class meter_mode {
      none,        // no counter, no checks
      gas_trap,    // consumed >= deadline → trap
      gas_yield,   // consumed >= deadline → yield fiber, advance deadline
      timeout,     // consumed >= deadline → check wall clock, advance or trap
   };

   meter_mode  metering   = meter_mode::gas_trap;
   uint64_t    gas_budget = 1'000'000;
   uint64_t    gas_slice  = 10'000;
   uint32_t    timeout_ms = 5000;

   // ── Gas sharing ─────────────────────────────────────────────────
   // If set, this instance shares the gas counter with other instances
   // that hold the same shared_ptr. Used for billing a whole call chain
   // as one unit regardless of how many isolated modules are crossed.
   std::shared_ptr<gas_state> shared_gas;
};

// ═════════════════════════════════════════════════════════════════════
// Unresolved import descriptor
// ═════════════════════════════════════════════════════════════════════

struct unresolved_import {
   std::string module_name;
   std::string func_name;
   std::string wit_signature;  // from component-wit section, if available
};

// ═════════════════════════════════════════════════════════════════════
// Module handle — prepared module, ready to instantiate
// ═════════════════════════════════════════════════════════════════════

struct module_handle_impl;

class module_handle {
   std::shared_ptr<module_handle_impl> impl_;

public:
   module_handle() = default;
   explicit module_handle(std::shared_ptr<module_handle_impl> p) : impl_(std::move(p)) {}

   explicit operator bool() const { return impl_ != nullptr; }

   // Introspection
   uint32_t compile_time_ms() const;
   uint32_t native_code_size() const;
   uint32_t wasm_size() const;

   std::vector<std::string> wit_sections() const;

   module_handle_impl* get() const { return impl_.get(); }
};

// ═════════════════════════════════════════════════════════════════════
// Instance — live execution context, tx/request-scoped
// ═════════════════════════════════════════════════════════════════════

struct instance_impl;

class instance {
   std::unique_ptr<instance_impl> impl_;

public:
   instance();
   explicit instance(std::unique_ptr<instance_impl> p);
   ~instance();

   instance(instance&&) noexcept;
   instance& operator=(instance&&) noexcept;

   instance(const instance&) = delete;
   instance& operator=(const instance&) = delete;

   explicit operator bool() const { return impl_ != nullptr; }

   // ── Typed call (shared header available) ────────────────────────
   // Returns a proxy: inst.as<greeter>().concat("a", "b")
   // template <typename Tag>
   // auto as();

   // ── Dynamic call (WIT-driven, no shared header) ─────────────────
   // native_value call(std::string_view interface_name,
   //                   std::string_view func_name,
   //                   native_value* args);

   // ── Gas ─────────────────────────────────────────────────────────
   uint64_t gas_consumed() const;
   uint64_t gas_deadline() const;
   void     set_deadline(uint64_t deadline);
   void     interrupt();  // set deadline to 0 (consumed >= 0 always)

   gas_state*       gas();
   const gas_state* gas() const;

   // ── Memory ──────────────────────────────────────────────────────
   char*       linear_memory();
   std::size_t memory_size() const;

   instance_impl* get() const { return impl_.get(); }
};

// ═════════════════════════════════════════════════════════════════════
// Runtime configuration
// ═════════════════════════════════════════════════════════════════════

struct runtime_config {
   std::size_t guarded_pool_size = 64;    // for mem_safety::guarded
   std::size_t arena_size_gb     = 256;   // for mem_safety::checked/unchecked
};

// ═════════════════════════════════════════════════════════════════════
// Runtime
// ═════════════════════════════════════════════════════════════════════

struct runtime_impl;

class runtime {
   std::unique_ptr<runtime_impl> impl_;

public:
   explicit runtime(runtime_config config = {});
   ~runtime();

   runtime(runtime&&) noexcept;
   runtime& operator=(runtime&&) noexcept;

   // ── Library registration ────────────────────────────────────────
   void register_library(std::string_view name, archive_bytes archive);
   void register_library(std::string_view name, wasm_bytes wasm);

   // ── Host interface registration ─────────────────────────────────
   // template <typename... Interfaces, typename Host>
   // void provide(Host& host);

   // ── Module preparation ──────────────────────────────────────────
   module_handle prepare(wasm_bytes wasm, const instance_policy& policy);
   module_handle load_cached(pzam_bytes pzam);

   // ── Import checking ─────────────────────────────────────────────
   std::vector<unresolved_import> check(wasm_bytes wasm) const;

   // ── Instantiation ───────────────────────────────────────────────
   instance instantiate(const module_handle& tmpl);
   instance instantiate(const module_handle& tmpl,
                        const instance_policy& override_policy);

   // ── Instance binding (live instance as provider) ────────────────
   void bind(instance& consumer, instance& provider,
             std::string_view interface_name);

   // ── Cache management ────────────────────────────────────────────
   struct cache_stats {
      std::size_t modules_cached;
      std::size_t pzam_bytes_total;
      std::size_t compile_time_total_ms;
   };

   cache_stats stats() const;
   void evict(wasm_bytes wasm);
   void clear_cache();
};

} // namespace psizam
