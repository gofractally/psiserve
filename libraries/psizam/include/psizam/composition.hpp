#pragma once

// composition.hpp — multi-module WASM composition via typed interface wiring.
//
// Loads multiple WASM modules, each with its own backend + linear memory
// (isolated mode), and wires imports of one module to exports of another
// using the PSIO interface reflection system.
//
// Example:
//
//   Host host;
//   psizam::composition<Host, psizam::interpreter> comp{host};
//
//   auto& provider = comp.add(provider_wasm);
//   auto& consumer = comp.add(consumer_wasm);
//
//   // Wire consumer's greeter imports to provider's greeter exports:
//   comp.link<greeter>(consumer, provider);
//
//   // Register host functions for all modules:
//   comp.register_host<Host>(consumer);
//   comp.register_host<Host>(provider);
//
//   comp.instantiate();
//
//   // Call a consumer export:
//   consumer.as<processor>().process(42);

#include <psizam/backend.hpp>
#include <psizam/bridge_executor.hpp>
#include <psizam/canonical_dispatch.hpp>
#include <psizam/host_function.hpp>
#include <psizam/host_function_table.hpp>
#include <psizam/hosted.hpp>

#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace psizam
{

// Forward declaration
template <typename Host, typename BackendKind>
class composition;

template <typename Host, typename BackendKind>
struct module_instance;

namespace detail
{

// Bridge function trait decomposition
template <typename FnPtr>
struct bridge_fn_traits;

template <typename R, typename... Args>
struct bridge_fn_traits<R (*)(Args...)>
{
   using ReturnType = R;
   using ArgTypes   = ::psio::TypeList<std::remove_cvref_t<Args>...>;
   static constexpr std::size_t arg_count = sizeof...(Args);
};

}  // namespace detail

// ── module_instance ─────────────────────────────────────────────────
// Holds one WASM module's backend, allocator, bytes, and host function
// table. Each module is isolated — its own linear memory, its own
// function dispatch table.
template <typename Host, typename BackendKind>
struct module_instance
{
   using backend_t = backend<std::nullptr_t, BackendKind>;

   wasm_allocator              alloc;
   std::vector<std::uint8_t>   wasm_copy;
   host_function_table         table;
   std::unique_ptr<backend_t>  be;
   Host*                       host_ptr    = nullptr;
   bool                        instantiated = false;

   module_instance() = default;
   module_instance(module_instance&&) = default;
   module_instance& operator=(module_instance&&) = default;

   // Return a typed proxy for calling this module's exports.
   template <typename Tag>
   auto as()
   {
      using info    = ::psio::detail::interface_info<Tag>;
      using adapter = detail::void_proxy_adapter<backend_t, info>;
      using proxy_t = typename info::template proxy<adapter>;
      return proxy_t{*be, static_cast<void*>(host_ptr)};
   }
};

// ── composition ─────────────────────────────────────────────────────
// Owns multiple module_instance objects and provides wiring between
// them.
template <typename Host, typename BackendKind>
class composition
{
public:
   using instance_t = module_instance<Host, BackendKind>;
   using backend_t  = typename instance_t::backend_t;

   explicit composition(Host& host) : host_ptr_(&host) {}

   // Add a module. Returns a reference to the module_instance.
   // The module is NOT instantiated yet — call instantiate() after
   // all link() and register_host() calls.
   template <typename Bytes>
   instance_t& add(const Bytes& wasm_bytes)
   {
      auto inst = std::make_unique<instance_t>();
      inst->wasm_copy.assign(std::begin(wasm_bytes), std::end(wasm_bytes));
      inst->host_ptr = host_ptr_;
      auto& ref = *inst;
      modules_.push_back(std::move(inst));
      return ref;
   }

   // Register host C++ methods as imports for a specific module.
   // Walks Impl's PSIO_HOST_MODULE interfaces and registers each
   // method with the module's host_function_table.
   template <typename Impl>
   void register_host(instance_t& mod)
   {
      if constexpr (requires { typename ::psio::detail::impl_info<Impl>::interfaces; })
      {
         using interfaces = typename ::psio::detail::impl_info<Impl>::interfaces;
         [&]<typename... IfaceImpls>(std::tuple<IfaceImpls...>*) {
            (register_host_iface<IfaceImpls>(mod), ...);
         }(static_cast<interfaces*>(nullptr));
      }
   }

   // Wire consumer's imports of InterfaceTag to provider's exports.
   template <typename InterfaceTag>
   void link(instance_t& consumer, instance_t& provider)
   {
      using info = ::psio::detail::interface_info<InterfaceTag>;
      constexpr auto N = info::func_names.size();
      link_interface_methods<InterfaceTag>(consumer, provider,
                                          std::make_index_sequence<N>{});
   }

   // Instantiate all modules. Must be called after all link() calls.
   void instantiate()
   {
      for (auto& mod : modules_)
      {
         if (!mod->instantiated)
         {
            mod->be = std::make_unique<backend_t>(
               mod->wasm_copy, std::move(mod->table),
               static_cast<void*>(mod->host_ptr), &mod->alloc);
            mod->instantiated = true;
         }
      }
   }

   Host& host() { return *host_ptr_; }

   // ── Unresolved import introspection ──────────────────────────────
   struct unresolved_import {
      std::string module_name;
      std::string func_name;
      std::size_t module_index;
   };

   std::vector<unresolved_import> unresolved() const
   {
      std::vector<unresolved_import> result;
      for (std::size_t mi = 0; mi < modules_.size(); ++mi) {
         auto& mod = *modules_[mi];
         // Parse the WASM to find imports, check which are registered
         // in the module's host_function_table
         // For now, return empty — full implementation needs import
         // section parsing from the WASM binary
      }
      return result;
   }

private:
   Host*                                        host_ptr_;
   std::vector<std::unique_ptr<instance_t>>     modules_;

   // ── Host function registration ───────────────────────────────────

   template <typename IfaceImpl>
   void register_host_iface(instance_t& mod)
   {
      using iface_tag  = typename IfaceImpl::tag;
      using iface_info = ::psio::detail::interface_info<iface_tag>;
      constexpr auto n =
         std::tuple_size_v<std::remove_cvref_t<decltype(IfaceImpl::methods)>>;
      register_host_methods<IfaceImpl>(
         mod, iface_info::name,
         std::make_index_sequence<n>{});
   }

   template <typename IfaceImpl, std::size_t... Is>
   void register_host_methods(instance_t& mod, std::string_view iface_name,
                              std::index_sequence<Is...>)
   {
      (register_one_host_method<IfaceImpl, Is>(mod, iface_name), ...);
   }

   template <typename IfaceImpl, std::size_t I>
   void register_one_host_method(instance_t& mod, std::string_view iface_name)
   {
      constexpr auto method_ptr = std::get<I>(IfaceImpl::methods);
      using MTypes = detail::member_fn_types<std::remove_const_t<decltype(method_ptr)>>;
      std::string mod_name{iface_name};
      std::string fn_name{IfaceImpl::names[I]};

      if constexpr (MTypes::all_scalar) {
         mod.table.template add<method_ptr>(mod_name, fn_name);
      } else {
         register_canonical_host_entry<method_ptr>(mod, mod_name, fn_name);
      }
   }

   template <auto MemPtr>
   void register_canonical_host_entry(instance_t& mod,
                                      const std::string& iface_name,
                                      const std::string& fn_name)
   {
      host_function_table::entry e;
      e.module_name = iface_name;
      e.func_name   = fn_name;
      e.signature.params.assign(16, types::i64);
      e.signature.ret = {types::i64};

      e.slow_dispatch = [](void* host_void, native_value* args, char* memory) -> native_value {
         auto* host = static_cast<Host*>(host_void);
         using MType      = detail::member_fn_types<decltype(MemPtr)>;
         using ReturnType = typename MType::ReturnType;

         ::psio::native_value slots[16];
         for (int i = 0; i < 16; ++i)
            slots[i].i64 = args[i].i64;

         const uint8_t* mem = reinterpret_cast<const uint8_t*>(memory);
         detail::host_lift_policy lift{slots, mem};
         auto fn_args = [&]<typename H, typename R, typename... As>(R (H::*)(As...)) {
            return detail::lift_canonical_args<MemPtr>(
               lift, ::psio::TypeList<std::remove_cvref_t<As>...>{});
         }(MemPtr);

         if constexpr (std::is_void_v<ReturnType>) {
            std::apply([host](auto&&... a) {
               (host->*MemPtr)(std::forward<decltype(a)>(a)...);
            }, fn_args);
            return native_value{uint64_t{0}};
         } else {
            auto result = std::apply([host](auto&&... a) {
               return (host->*MemPtr)(std::forward<decltype(a)>(a)...);
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

      mod.table.add_entry(std::move(e));
   }

   // ── Module-to-module wiring ──────────────────────────────────────

   template <typename InterfaceTag, std::size_t... Is>
   void link_interface_methods(instance_t& consumer, instance_t& provider,
                               std::index_sequence<Is...>)
   {
      (link_one_method<InterfaceTag, Is>(consumer, provider), ...);
   }

   template <typename InterfaceTag, std::size_t I>
   void link_one_method(instance_t& consumer, instance_t& provider)
   {
      using info = ::psio::detail::interface_info<InterfaceTag>;
      using func_types = typename info::func_types;
      using FnPtr = std::tuple_element_t<I, func_types>;
      using Traits = detail::bridge_fn_traits<FnPtr>;
      using Ret = typename Traits::ReturnType;
      using ArgTypes = typename Traits::ArgTypes;

      std::string iface_name{info::name};
      std::string fn_name{info::func_names[I]};

      host_function_table::entry e;
      e.module_name = iface_name;
      e.func_name   = fn_name;
      e.signature.params.assign(16, types::i64);
      e.signature.ret = {types::i64};

      instance_t* provider_ptr = &provider;
      instance_t* consumer_ptr = &consumer;

      // Detect scalar-only signatures at compile time.
      // Scalar bridge: forward args directly — no lift, no lower, no copy.
      constexpr bool all_args_scalar = []<typename... As>(::psio::TypeList<As...>) {
         return (detail::is_scalar_wasm_type_v<As> && ...);
      }(ArgTypes{});
      constexpr bool ret_scalar = detail::is_scalar_wasm_type_v<Ret> ||
                                  std::is_void_v<Ret>;

      // Shared state for lazy export index resolution.
      // Resolved on first call (after instantiation), cached for all subsequent.
      auto cached_idx = std::make_shared<uint32_t>(std::numeric_limits<uint32_t>::max());

      if constexpr (all_args_scalar && ret_scalar) {
         // FAST PATH: scalar-only — forward args, call by index
         e.slow_dispatch = [provider_ptr, fn_name, cached_idx](
            void* host_void, native_value* args, char*) -> native_value
         {
            if (*cached_idx == std::numeric_limits<uint32_t>::max())
               *cached_idx = provider_ptr->be->resolve_export(fn_name);

            auto r = provider_ptr->be->call_by_index(
               host_void, *cached_idx,
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
      } else {
         // THREADED-DISPATCH PATH: compile a bridge program from the
         // function's type signature and execute it via computed-goto.
         auto* prog_ptr = new bridge::bridge_program(
            bridge::compile_bridge<FnPtr>(fn_name));

         // Resolve export index lazily on first call
         e.slow_dispatch = [prog_ptr, provider_ptr, consumer_ptr](
            void* host_void, native_value* args, char* consumer_memory) -> native_value
         {
            if (!prog_ptr->has_resolved_index())
               prog_ptr->export_index = provider_ptr->be->resolve_export(
                  prog_ptr->export_name);

            return bridge::execute_bridge<Host, BackendKind>(
               *prog_ptr, consumer_ptr, provider_ptr,
               host_void, args, consumer_memory);
         };
      }

      consumer.table.add_entry(std::move(e));
   }

};

// ── Linking mode tags ───────────────────────────────────────────────
// Used with the composition constructor to annotate per-module linking:
//   composition<Host, llvm> vm{static_link(a), b, c, host};
// Bare bytes = isolated (default). static_link(bytes) = shared memory.

template <typename Bytes>
struct static_link_t { const Bytes& wasm; };

template <typename Bytes>
constexpr static_link_t<Bytes> static_link(const Bytes& bytes) { return {bytes}; }

}  // namespace psizam
