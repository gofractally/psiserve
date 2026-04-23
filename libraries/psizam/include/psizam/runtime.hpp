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

#include <psizam/canonical_dispatch.hpp>
#include <psizam/gas.hpp>
#include <psizam/gas_pool.hpp>
#include <psizam/host_function_table.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace psizam {

namespace detail { class instance_be; }   // detail/instance_be.hpp

struct module_handle_impl;

namespace detail_runtime {
   // Reach into an incomplete-in-this-TU module_handle_impl for its
   // late-bound live_be back-pointer. Defined in runtime.cpp where the
   // full type is visible. Used by bind<>'s bridge lambdas to get the
   // consumer's instance_be at call time (consumer is typically not
   // instantiated yet at bind time, so the pointer is resolved late).
   detail::instance_be* live_be_of(const module_handle_impl* mod);
}

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

// gas_state + gas_handler_t live in <psizam/gas.hpp> — the unified
// public surface for the metering subsystem. See the design in
// libraries/psizam/docs/gas-metering-design.md.

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

   // ── External memory (caller-provided) ─────────────────────────
   // If non-null, the instance uses this pre-allocated buffer
   // instead of mmap'ing its own guarded region. The buffer must
   // be at least wasm_allocator::prefix_size() + (1 << mem_budget)
   // bytes, already read/write. Caller owns the memory.
   // mem_budget is a power-of-2 exponent (e.g. 16=64KB, 22=4MB).
   // When set, memory mode is forced to checked (no guard pages).
   char*    external_memory   = nullptr;
   uint8_t  mem_budget        = 0;  // 0 = use max_pages, >0 = 1<<mem_budget bytes

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
   // that hold the same shared_ptr. DEPRECATED (pre-pool stop-gap).
   // Orphaned from codegen today — the live path is `pool` below.
   std::shared_ptr<gas_state> shared_gas;

   // Multi-module gas tracking. When set, this instance leases gas from
   // the shared pool at lease boundaries (handler entry and instance
   // teardown). Multiple instances pointing at the same pool collectively
   // bill against a single budget — the canonical way to account gas
   // across a consumer → provider call chain. See psizam/gas_pool.hpp
   // for the lease mechanics.
   std::shared_ptr<gas_pool> pool;
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

   // Shared reference to the impl — used by runtime::instantiate to
   // keep the module alive for the instance's lifetime so bridge
   // lambdas on the module's host_function_table can safely deref
   // the late-bound `live_be` back-pointer at call time.
   std::shared_ptr<module_handle_impl> share_impl() const { return impl_; }
};

// ═════════════════════════════════════════════════════════════════════
// Instance — live execution context, tx/request-scoped
// ═════════════════════════════════════════════════════════════════════

struct instance_impl;

// Backend variant the instance was constructed with. Public re-export
// of detail::backend_kind so callers can guard `as<Tag>()` (which is
// presently interpreter-only — see Step 10).
enum class backend_kind : uint8_t {
   interpreter,
   jit,
   jit2,
   jit_llvm,
};

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

   // Which backend this instance is running on. Set by `instantiate()`
   // from `instance_policy::initial`.
   backend_kind kind() const;

   // ── Typed call (shared header available) ────────────────────────
   // Returns a proxy: inst.as<greeter>().concat("a", "b")
   //
   // Works for any backend kind — the proxy routes through
   // `instance_be::call_export_canonical`, an erased call surface
   // (Step 10).
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

   // ── Run start ───────────────────────────────────────────────────
   // Convenience: run module.start (if present) then resolve and call
   // `_start`. Returns the WASI exit code captured from
   // wasi_exit_exception, or 0 if `_start` returns normally without
   // calling proc_exit. Other psizam exceptions propagate.
   int run_start();

   // Internal accessors for template code
   void* backend_ptr();
   void* host_ptr();

   // Erased instance-backend pointer for `as<Tag>()`'s template body.
   // Forward-declared in detail/instance_be.hpp; the cast to the
   // forward-declared type is finalized when hosted.hpp pulls the full
   // declaration in below.
   detail::instance_be* get_instance_be();

   instance_impl* get() const { return impl_.get(); }
};

// ═════════════════════════════════════════════════════════════════════
// Runtime configuration
// ═════════════════════════════════════════════════════════════════════

struct runtime_config {
   // Number of pre-mapped 4GB guarded regions. Each reserves ~8GB of
   // virtual address space (4GB usable + guard pages). Used by
   // mem_safety::guarded instances (trusted, config-launched services).
   //
   // Address space cost: guarded_pool_size * 8GB.
   // Default 64 = 512GB virtual. Adjust down on systems with limited
   // address space (32-bit hosts, constrained containers).
   std::size_t guarded_pool_size = 64;

   // Shared arena for mem_safety::checked / unchecked instances.
   // Sub-instances that don't get their own guarded region allocate
   // from this arena. Pure address space reservation (PROT_NONE);
   // no physical memory committed until pages are touched.
   //
   // Address space cost: arena_size_gb * 1GB.
   std::size_t arena_size_gb = 16;
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
#include <psizam/bridge_executor.hpp>
#include <psizam/hosted.hpp>

namespace psizam {

template <typename Tag>
auto instance::as() {
   // Step 10: dispatch through `instance_be::call_export_canonical` so
   // the proxy works for any backend kind (interpreter / jit / jit2 /
   // jit_llvm). The adapter no longer carries a typed Backend
   // parameter; it routes everything through the abstract base.
   using info    = ::psio::detail::interface_info<Tag>;
   using adapter = detail::void_proxy_adapter_erased<info>;
   using proxy_t = typename info::template proxy<adapter>;
   return proxy_t{*get_instance_be(), host_ptr()};
}

// ── provide<HostImpl> ───────────────────────────────────────────────
// Walks PSIO_HOST_MODULE interfaces and registers each method with the
// module's host function table. Scalar methods get fast trampolines
// (natural arg count, direct call). Canonical methods (strings, records,
// lists, resources, expected) get the 16-wide handler with type-specialized
// lift/lower via the canonical dispatch machinery.

namespace detail_runtime {

   // Lower policy that writes flat values into a native_value array.
   // Used by slow_dispatch to lower the host method's return value
   // back to the guest's flat-value convention.
   struct return_lower_policy
   {
      native_value* out;
      std::size_t   idx = 0;
      char*         memory = nullptr;

      return_lower_policy(native_value* o, char* m) : out(o), memory(m) {}

      void emit_i32(uint32_t v)  { out[idx].i64 = 0; out[idx].i32 = v; ++idx; }
      void emit_i64(uint64_t v)  { out[idx].i64 = v; ++idx; }
      void emit_f32(float v)     { out[idx].i64 = 0; out[idx].f32 = v; ++idx; }
      void emit_f64(double v)    { out[idx].i64 = 0; out[idx].f64 = v; ++idx; }

      uint32_t alloc(uint32_t align, uint32_t size) {
         return detail::host_cabi_realloc(align, size);
      }
      void store_u8(uint32_t off, uint8_t v) {
         if (memory) memory[off] = static_cast<char>(v);
      }
      void store_u16(uint32_t off, uint16_t v) {
         if (memory) std::memcpy(memory + off, &v, 2);
      }
      void store_u32(uint32_t off, uint32_t v) {
         if (memory) std::memcpy(memory + off, &v, 4);
      }
      void store_u64(uint32_t off, uint64_t v) {
         if (memory) std::memcpy(memory + off, &v, 8);
      }
      void store_f32(uint32_t off, float v) {
         if (memory) std::memcpy(memory + off, &v, 4);
      }
      void store_f64(uint32_t off, double v) {
         if (memory) std::memcpy(memory + off, &v, 8);
      }
      void store_bytes(uint32_t off, const char* data, uint32_t len) {
         if (memory && len > 0)
            std::memcpy(memory + off, data, len);
      }
   };

   template <typename IfaceImpl, std::size_t I, typename HostImpl>
   void register_one_host_method(host_function_table& table,
                                  std::string_view iface_name,
                                  HostImpl* host_ptr)
   {
      constexpr auto method_ptr = std::get<I>(IfaceImpl::methods);
      using MTypes = detail::member_fn_types<
         std::remove_const_t<decltype(method_ptr)>>;
      std::string mod_name{iface_name};
      std::string fn_name{IfaceImpl::names[I]};

      if constexpr (MTypes::all_scalar) {
         table.template add<method_ptr>(mod_name, fn_name);
         table.entries_mutable().back().host_override = host_ptr;
      } else {
         // Canonical path: 16-wide (i64×16) → i64 with type-specialized
         // lift/lower compiled at this template instantiation.
         host_function_table::entry e;
         e.module_name    = mod_name;
         e.func_name      = fn_name;
         e.host_override  = host_ptr;
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
               }, std::move(fn_args));
               return native_value{uint64_t{0}};
            } else {
               auto result = std::apply([host](auto&&... a) {
                  return (host->*method_ptr)(std::forward<decltype(a)>(a)...);
               }, std::move(fn_args));

               // Canonical ABI has two return-value protocols:
               //   * rflat <= MAX_FLAT_RESULTS:  lower into flat slots,
               //                                 return slot 0.
               //   * rflat >  MAX_FLAT_RESULTS:  caller allocates a
               //                                 return area and passes
               //                                 its pointer as the next
               //                                 arg slot after the real
               //                                 args; the callee writes
               //                                 canonical fields there
               //                                 and returns the pointer.
               //
               // `canonical_lower_flat` only handles the first case —
               // without this branch the extra slots that it emits would
               // be dropped because `return ret_slots[0]` can only carry
               // one i64 out.
               constexpr std::size_t rflat =
                  ::psio::canonical_flat_count_v<ReturnType>;

               if constexpr (rflat <= ::psio::MAX_FLAT_RESULTS) {
                  native_value        ret_slots[16] = {};
                  return_lower_policy rlp{ret_slots, memory};
                  canonical_lower_flat(result, rlp);
                  return ret_slots[0];
               } else {
                  // Number of flat arg slots the caller consumed before
                  // it placed the return-area pointer.
                  constexpr std::size_t arg_flats = []<typename H, typename R,
                                                       typename... As>(
                     R (H::*)(As...))
                  {
                     return (std::size_t{0} + ... +
                        ::psio::canonical_flat_count_v<
                           std::remove_cvref_t<As>>);
                  }(method_ptr);

                  const uint32_t retptr =
                     static_cast<uint32_t>(args[arg_flats].i32);

                  return_lower_policy rlp{nullptr, memory};
                  ::psio::canonical_lower_fields(result, rlp, retptr);

                  native_value rv;
                  rv.i64 = 0;
                  rv.i32 = retptr;
                  return rv;
               }
            }
         };

         table.add_entry(std::move(e));
      }
   }

   template <typename IfaceImpl, typename HostImpl, std::size_t... Is>
   void register_iface_methods(host_function_table& table,
                                std::string_view iface_name,
                                HostImpl* host_ptr,
                                std::index_sequence<Is...>)
   {
      (register_one_host_method<IfaceImpl, Is, HostImpl>(table, iface_name, host_ptr), ...);
   }

   template <typename IfaceImpl, typename HostImpl>
   void register_one_interface(host_function_table& table, HostImpl* host_ptr)
   {
      using iface_tag  = typename IfaceImpl::tag;
      using iface_info = ::psio::detail::interface_info<iface_tag>;
      constexpr auto n =
         std::tuple_size_v<std::remove_cvref_t<decltype(IfaceImpl::methods)>>;
      register_iface_methods<IfaceImpl>(
         table, std::string_view{iface_info::name},
         host_ptr,
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

   // Bridge entry: forward a consumer import to a provider export.
   //
   // For all-scalar signatures (bool / int / float all the way through):
   // the fast path uses `instance_be::call_export_canonical` directly,
   // passing the 16-slot flat call without marshalling — scalars don't
   // need memory translation.
   //
   // For signatures with strings / lists / records: compile a
   // bridge_program at bind time (template introspection of FnPtr's
   // C++ types) and execute it at call time via
   // `bridge::execute_bridge_erased`. The mini-interpreter handles
   // cross-module canonical-ABI marshalling: copy strings/lists/
   // records between consumer and provider linear memories via
   // cabi_realloc on each side. Same path composition.hpp uses,
   // generalized from the typed `module_instance<Host, BackendKind>`
   // to `detail::instance_be` (backend-erased).
   template <typename FnPtr>
   void register_bind_entry(
      host_function_table& table,
      std::string_view iface_name,
      std::string_view func_name,
      detail::instance_be* provider_be,
      void* provider_host,
      module_handle_impl* consumer_mod)   // for late-bound consumer live_be
   {
      using Traits = bind_fn_traits<FnPtr>;

      host_function_table::entry e;
      e.module_name = std::string{iface_name};
      e.func_name   = std::string{func_name};
      e.signature.params.assign(16, types::i64);
      e.signature.ret = {types::i64};

      std::string export_name{func_name};

      if constexpr (Traits::all_scalar) {
         // Fast scalar path — no marshalling needed. Cache the export
         // index on first call and route through `call_export_by_index`
         // (matching composition's pattern; avoids a string resolve on
         // every bridge invocation and is what the JIT fast path uses
         // internally).
         auto cached_idx = std::make_shared<uint32_t>(
            std::numeric_limits<uint32_t>::max());
         e.slow_dispatch = [provider_be, provider_host, export_name, cached_idx](
            void*, native_value* args, char*) -> native_value
         {
            if (*cached_idx == std::numeric_limits<uint32_t>::max())
               *cached_idx = provider_be->resolve_export(export_name);
            uint64_t slots[16];
            for (int i = 0; i < 16; ++i)
               slots[i] = static_cast<uint64_t>(args[i].i64);
            auto r = provider_be->call_export_by_index(
               provider_host, *cached_idx, slots, 16);
            native_value rv;
            rv.i64 = r ? r->to_ui64() : 0;
            return rv;
         };
      } else {
         // Threaded-dispatch path — compile a bridge_program once from
         // the FnPtr's C++ type signature, then at each call run the
         // mini-interpreter against consumer + provider linear memories.
         //
         // The bridge_program outlives the lambda via shared_ptr; when
         // the last bridge_executor call captures it, it stays alive
         // until the host_function_table entry (and thus the module)
         // is destroyed.
         auto prog_ptr = std::make_shared<bridge::bridge_program>(
            bridge::compile_bridge<FnPtr>(func_name));

         e.slow_dispatch = [prog_ptr, provider_be, provider_host, consumer_mod](
            void*, native_value* args, char*) -> native_value
         {
            // Consumer's live_be is filled in at instantiate time
            // (after bind). At call time it must be non-null.
            detail::instance_be* consumer_be = live_be_of(consumer_mod);
            return bridge::execute_bridge_erased(
               *prog_ptr, consumer_be, provider_be, provider_host, args);
         };
      }

      table.add_entry(std::move(e));
   }

   template <typename InterfaceTag, std::size_t... Is>
   void bind_interface_methods(
      host_function_table& table,
      detail::instance_be* provider_be,
      void* provider_host,
      module_handle_impl* consumer_mod,
      std::index_sequence<Is...>)
   {
      using info = ::psio::detail::interface_info<InterfaceTag>;
      using func_types = typename info::func_types;

      (register_bind_entry<std::tuple_element_t<Is, func_types>>(
         table, info::name, info::func_names[Is],
         provider_be, provider_host, consumer_mod), ...);
   }
}

template <typename InterfaceTag>
void runtime::bind(module_handle& consumer_mod, instance& provider) {
   if (!consumer_mod || !provider) return;

   using info = ::psio::detail::interface_info<InterfaceTag>;
   constexpr auto N = info::func_names.size();

   detail_runtime::bind_interface_methods<InterfaceTag>(
      consumer_mod.table(),
      provider.get_instance_be(),
      provider.host_ptr(),
      consumer_mod.get(),
      std::make_index_sequence<N>{});
}

template <typename HostImpl>
void runtime::provide(module_handle& mod, HostImpl& host) {
   if (!mod) return;

   // Publish the host pointer on the module so `instantiate()` can thread
   // it through to the backend's execution context. Without this the JIT
   // sees host=null in its entry trampoline and crashes with
   // "jit control-flow corruption" on the first export call (the
   // interpreter's slow_dispatch path tolerates null; JIT does not).
   mod.set_host_ptr(&host);

   if constexpr (requires { typename ::psio::detail::impl_info<HostImpl>::interfaces; })
   {
      using interfaces = typename ::psio::detail::impl_info<HostImpl>::interfaces;
      [&]<typename... IfaceImpls>(std::tuple<IfaceImpls...>*) {
         (detail_runtime::register_one_interface<IfaceImpls>(mod.table(), &host), ...);
      }(static_cast<interfaces*>(nullptr));
   }
}

} // namespace psizam
