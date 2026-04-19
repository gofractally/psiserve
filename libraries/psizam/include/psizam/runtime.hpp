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

#include <psizam/host_function_table.hpp>

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

   // Internal accessors for template code in this header
   host_function_table& table();
   void set_host_ptr(void* p);

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
   template <typename Tag>
   auto as();

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

   // Internal accessors for template code
   void* backend_ptr();
   void* host_ptr();

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
   // Registers host C++ methods as imports for a prepared module.
   // Uses PSIO_HOST_MODULE reflection to walk interfaces and register
   // each method with the module's host function table.
   template <typename HostImpl>
   void provide(module_handle& mod, HostImpl& host);

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
   // Wire a prepared module's imports to a live instance's exports.
   // Must be called BEFORE instantiate(). The bridge entries are
   // registered in the module's host function table; the backend
   // resolves them at construction time.
   //
   // Typed (shared header available):
   template <typename InterfaceTag>
   void bind(module_handle& consumer_mod, instance& provider);

   // Dynamic (WIT-driven, no shared header):
   void bind(module_handle& consumer_mod, instance& provider,
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

// ═════════════════════════════════════════════════════════════════════
// Template implementation (needs full backend + proxy types)
// ═════════════════════════════════════════════════════════════════════

} // namespace psizam

// Include after the class definitions so the template body can see
// the proxy/adapter types from hosted.hpp.
#include <psizam/hosted.hpp>

namespace psizam {

template <typename Tag>
auto instance::as() {
   using backend_t = backend<std::nullptr_t, interpreter>;
   using info      = ::psio::detail::interface_info<Tag>;
   using adapter   = detail::void_proxy_adapter<backend_t, info>;
   using proxy_t   = typename info::template proxy<adapter>;
   return proxy_t{*static_cast<backend_t*>(backend_ptr()), host_ptr()};
}

// ── provide<HostImpl> ───────────────────────────────────────────────
// Walks PSIO_HOST_MODULE interfaces and registers each method with the
// module's host function table. Scalar methods get fast trampolines
// (natural arg count, direct call). Canonical methods (strings, records,
// lists) get the 16-wide handler with type-specialized lift/lower.

namespace detail_runtime {

   template <typename IfaceImpl, std::size_t I, typename HostImpl>
   void register_one_host_method(host_function_table& table,
                                  std::string_view iface_name)
   {
      constexpr auto method_ptr = std::get<I>(IfaceImpl::methods);
      using MTypes = detail::member_fn_types<
         std::remove_const_t<decltype(method_ptr)>>;
      std::string mod_name{iface_name};
      std::string fn_name{IfaceImpl::names[I]};

      if constexpr (MTypes::all_scalar) {
         table.template add<method_ptr>(mod_name, fn_name);
      } else {
         // Canonical path: 16-wide (i64×16) → i64 with type-specialized
         // lift/lower compiled at this template instantiation.
         host_function_table::entry e;
         e.module_name = mod_name;
         e.func_name   = fn_name;
         e.signature.params.assign(16, types::i64);
         e.signature.ret = {types::i64};

         e.slow_dispatch = [](void* host_void, native_value* args,
                              char* memory) -> native_value {
            auto* host = static_cast<HostImpl*>(host_void);
            using MType      = detail::member_fn_types<
               std::remove_const_t<decltype(method_ptr)>>;
            using ReturnType = typename MType::ReturnType;

            ::psio::native_value slots[16];
            for (int i = 0; i < 16; ++i)
               slots[i].i64 = args[i].i64;

            const uint8_t* mem = reinterpret_cast<const uint8_t*>(memory);
            detail::host_lift_policy lift{slots, mem};
            // Decompose the member pointer to extract arg types.
            // Use a helper to strip the const from constexpr auto.
            using MemPtrType = std::remove_const_t<decltype(method_ptr)>;
            auto fn_args = [&]<typename H, typename R, typename... As>(R (H::*)(As...)) {
               return detail::lift_canonical_args<method_ptr>(
                  lift, ::psio::TypeList<std::remove_cvref_t<As>...>{});
            }(MemPtrType{});

            if constexpr (std::is_void_v<ReturnType>) {
               std::apply([host](auto&&... a) {
                  (host->*method_ptr)(std::forward<decltype(a)>(a)...);
               }, fn_args);
               return native_value{uint64_t{0}};
            } else {
               auto result = std::apply([host](auto&&... a) {
                  return (host->*method_ptr)(std::forward<decltype(a)>(a)...);
               }, fn_args);
               uint64_t rv = 0;
               if constexpr (std::is_integral_v<ReturnType>)
                  rv = static_cast<uint64_t>(result);
               else if constexpr (std::is_floating_point_v<ReturnType>) {
                  if constexpr (sizeof(ReturnType) == 4) {
                     union { float f; uint32_t u; } cvt{result};
                     rv = cvt.u;
                  } else {
                     union { double f; uint64_t u; } cvt{result};
                     rv = cvt.u;
                  }
               }
               return native_value{rv};
            }
         };

         table.add_entry(std::move(e));
      }
   }

   template <typename IfaceImpl, std::size_t... Is>
   void register_iface_methods(host_function_table& table,
                                std::string_view iface_name,
                                std::index_sequence<Is...>)
   {
      using HostImpl = typename IfaceImpl::host;
      (register_one_host_method<IfaceImpl, Is, HostImpl>(table, iface_name), ...);
   }

   template <typename IfaceImpl>
   void register_one_interface(host_function_table& table)
   {
      using iface_tag  = typename IfaceImpl::tag;
      using iface_info = ::psio::detail::interface_info<iface_tag>;
      constexpr auto n =
         std::tuple_size_v<std::remove_cvref_t<decltype(IfaceImpl::methods)>>;
      register_iface_methods<IfaceImpl>(
         table, std::string_view{iface_info::name},
         std::make_index_sequence<n>{});
   }
}

// ── bind<InterfaceTag> ───────────────────────────────────────────
// Creates bridge entries in the consumer module's host function table
// that route calls to the provider's live instance. Uses the bridge
// executor for complex types, direct forwarding for scalars.

namespace detail_runtime {

   template <typename FnPtr>
   struct bind_fn_traits;

   template <typename R, typename... Args>
   struct bind_fn_traits<R(*)(Args...)> {
      using ReturnType = R;
      using ArgTypes   = ::psio::TypeList<std::remove_cvref_t<Args>...>;
      static constexpr bool all_scalar =
         detail::is_scalar_wasm_type_v<R> &&
         (detail::is_scalar_wasm_type_v<std::remove_cvref_t<Args>> && ...);
   };

   // Bridge entry: forward call from consumer to provider's live backend
   template <typename FnPtr>
   void register_bind_entry(
      host_function_table& table,
      std::string_view iface_name,
      std::string_view func_name,
      void* provider_backend,
      void* provider_host)
   {
      using Traits = bind_fn_traits<FnPtr>;

      host_function_table::entry e;
      e.module_name = std::string{iface_name};
      e.func_name   = std::string{func_name};
      e.signature.params.assign(16, types::i64);
      e.signature.ret = {types::i64};

      using backend_t = backend<std::nullptr_t, interpreter>;
      auto* be_ptr = static_cast<backend_t*>(provider_backend);
      void* host_ptr = provider_host;
      std::string export_name{func_name};

      // Forward call to provider's live backend.
      // For all signatures: use the 16-wide flat_val convention
      // (same as PSIO_MODULE thunks). The provider's export expects
      // 16 i64 args regardless of the logical param count.
      e.slow_dispatch = [be_ptr, host_ptr, export_name](
         void*, native_value* args, char*) -> native_value
      {
         auto r = be_ptr->call_with_return(
            host_ptr, std::string_view{export_name},
            static_cast<uint64_t>(args[0].i64),
            static_cast<uint64_t>(args[1].i64),
            static_cast<uint64_t>(args[2].i64),
            static_cast<uint64_t>(args[3].i64),
            static_cast<uint64_t>(args[4].i64),
            static_cast<uint64_t>(args[5].i64),
            static_cast<uint64_t>(args[6].i64),
            static_cast<uint64_t>(args[7].i64),
            static_cast<uint64_t>(args[8].i64),
            static_cast<uint64_t>(args[9].i64),
            static_cast<uint64_t>(args[10].i64),
            static_cast<uint64_t>(args[11].i64),
            static_cast<uint64_t>(args[12].i64),
            static_cast<uint64_t>(args[13].i64),
            static_cast<uint64_t>(args[14].i64),
            static_cast<uint64_t>(args[15].i64));
         native_value rv;
         rv.i64 = r ? r->to_ui64() : 0;
         return rv;
      };

      table.add_entry(std::move(e));
   }

   template <typename InterfaceTag, std::size_t... Is>
   void bind_interface_methods(
      host_function_table& table,
      void* provider_backend,
      void* provider_host,
      std::index_sequence<Is...>)
   {
      using info = ::psio::detail::interface_info<InterfaceTag>;
      using func_types = typename info::func_types;

      (register_bind_entry<std::tuple_element_t<Is, func_types>>(
         table, info::name, info::func_names[Is],
         provider_backend, provider_host), ...);
   }
}

template <typename InterfaceTag>
void runtime::bind(module_handle& consumer_mod, instance& provider) {
   if (!consumer_mod || !provider) return;

   using info = ::psio::detail::interface_info<InterfaceTag>;
   constexpr auto N = info::func_names.size();

   detail_runtime::bind_interface_methods<InterfaceTag>(
      consumer_mod.table(),
      provider.backend_ptr(),
      provider.host_ptr(),
      std::make_index_sequence<N>{});
}

template <typename HostImpl>
void runtime::provide(module_handle& mod, HostImpl& host) {
   if (!mod) return;
   mod.set_host_ptr(&host);

   if constexpr (requires { typename ::psio::detail::impl_info<HostImpl>::interfaces; })
   {
      using interfaces = typename ::psio::detail::impl_info<HostImpl>::interfaces;
      [&]<typename... IfaceImpls>(std::tuple<IfaceImpls...>*) {
         (detail_runtime::register_one_interface<IfaceImpls>(mod.table()), ...);
      }(static_cast<interfaces*>(nullptr));
   }
}

} // namespace psizam
