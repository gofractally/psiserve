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

// Count the number of flat slots consumed by an arg type list
template <typename... Ts>
constexpr size_t count_flat_slots(::psio::TypeList<Ts...>)
{
   return (flat_count_v<Ts> + ... + std::size_t{0});
}

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

      std::string iface_name{info::name};
      std::string fn_name{info::func_names[I]};

      host_function_table::entry e;
      e.module_name = iface_name;
      e.func_name   = fn_name;
      e.signature.params.assign(16, types::i64);
      e.signature.ret = {types::i64};

      // Capture provider pointer for the bridge closure
      instance_t* provider_ptr = &provider;
      instance_t* consumer_ptr = &consumer;

      e.slow_dispatch = [provider_ptr, consumer_ptr, fn_name](
         void* host_void, native_value* args, char* consumer_memory) -> native_value
      {
         return bridge_call<FnPtr>(
            provider_ptr, consumer_ptr,
            host_void, args, consumer_memory, fn_name);
      };

      consumer.table.add_entry(std::move(e));
   }

   // ── Bridge call implementation ───────────────────────────────────
   // This is the core of module-to-module calling. It:
   // 1. Lifts args from consumer's flat slots + linear memory
   // 2. Lowers them into provider's linear memory
   // 3. Calls the provider's export
   // 4. Lifts the result from provider
   // 5. Lowers it back into consumer's linear memory (if needed)
   template <typename FnPtr>
   static native_value bridge_call(
      instance_t* provider,
      instance_t* consumer,
      void* host_void,
      native_value* args,
      char* consumer_memory,
      const std::string& export_name)
   {
      using Traits = detail::bridge_fn_traits<FnPtr>;
      using Ret = typename Traits::ReturnType;
      using ArgTypes = typename Traits::ArgTypes;

      ::psio::native_value slots[16];
      for (int i = 0; i < 16; ++i)
         slots[i].i64 = args[i].i64;

      const uint8_t* consumer_mem =
         reinterpret_cast<const uint8_t*>(consumer_memory);

      // 1. Lift args from consumer
      detail::host_lift_policy lift{slots, consumer_mem};
      auto lifted_args = [&]<typename... As>(::psio::TypeList<As...>) {
         return std::tuple{canonical_lift_flat<As>(lift)...};
      }(ArgTypes{});

      // 2. Lower into provider
      detail::void_host_lower_policy<backend_t> lp{*provider->be, host_void};
      std::apply([&](const auto&... a) {
         (canonical_lower_flat(a, lp), ...);
      }, lifted_args);

      // 3. Call provider's export with 16-wide flat vals
      auto& fv = lp.flat_values;
      auto slot_u64 = [&](size_t i) -> uint64_t {
         return i < fv.size() ? fv[i].i64 : uint64_t{0};
      };

      auto r = provider->be->call_with_return(
         host_void, std::string_view{export_name},
         slot_u64(0),  slot_u64(1),  slot_u64(2),  slot_u64(3),
         slot_u64(4),  slot_u64(5),  slot_u64(6),  slot_u64(7),
         slot_u64(8),  slot_u64(9),  slot_u64(10), slot_u64(11),
         slot_u64(12), slot_u64(13), slot_u64(14), slot_u64(15));

      // 4+5. Handle the return value
      if constexpr (std::is_void_v<Ret>) {
         return native_value{uint64_t{0}};
      } else {
         uint64_t raw_ret = r ? r->to_ui64() : uint64_t{0};
         using U = std::remove_cvref_t<Ret>;
         constexpr size_t rflat = flat_count_v<U>;

         if constexpr (rflat <= psio::MAX_FLAT_RESULTS) {
            // Single-slot flat return (scalar) — pass through
            native_value rv;
            rv.i64 = raw_ret;
            return rv;
         } else if constexpr (std::is_same_v<U, psio::owned<std::string, psio::wit>>) {
            // String return: provider wrote {ptr, len} into a return area.
            // Read the string from provider's memory, allocate in consumer's
            // memory, copy the bytes, write {ptr, len} into consumer's
            // return area.
            uint32_t provider_retptr = static_cast<uint32_t>(raw_ret);
            const uint8_t* provider_mem =
               reinterpret_cast<const uint8_t*>(provider->be->get_context().linear_memory());

            uint32_t s_ptr, s_len;
            std::memcpy(&s_ptr, provider_mem + provider_retptr, 4);
            std::memcpy(&s_len, provider_mem + provider_retptr + 4, 4);

            // Consumer's retptr is the next arg slot after the real args
            constexpr size_t num_arg_flats = detail::count_flat_slots(ArgTypes{});
            uint32_t consumer_retptr = static_cast<uint32_t>(slots[num_arg_flats].i64);

            // Allocate in consumer's memory for the string data
            uint32_t consumer_str_ptr = 0;
            if (s_len > 0) {
               auto alloc_ret = consumer->be->call_with_return(
                  host_void, std::string_view{"cabi_realloc"},
                  uint32_t{0}, uint32_t{0}, uint32_t{1}, s_len);
               consumer_str_ptr = alloc_ret ? alloc_ret->to_ui32() : 0u;
               // Copy string bytes from provider to consumer
               char* consumer_mem_w = consumer->be->get_context().linear_memory();
               std::memcpy(consumer_mem_w + consumer_str_ptr,
                           provider_mem + s_ptr, s_len);
            }

            // Write {ptr, len} into consumer's return area
            char* consumer_mem_w = consumer->be->get_context().linear_memory();
            std::memcpy(consumer_mem_w + consumer_retptr, &consumer_str_ptr, 4);
            std::memcpy(consumer_mem_w + consumer_retptr + 4, &s_len, 4);

            native_value rv;
            rv.i64 = static_cast<uint64_t>(consumer_retptr);
            return rv;
         } else if constexpr (detail_dispatch::is_wit_vector<U>::value) {
            // Vector return: similar to string but for arrays
            using E = typename detail_dispatch::is_wit_vector<U>::element_type;
            uint32_t provider_retptr = static_cast<uint32_t>(raw_ret);
            const uint8_t* provider_mem =
               reinterpret_cast<const uint8_t*>(provider->be->get_context().linear_memory());

            uint32_t e_ptr, e_len;
            std::memcpy(&e_ptr, provider_mem + provider_retptr, 4);
            std::memcpy(&e_len, provider_mem + provider_retptr + 4, 4);

            constexpr size_t num_arg_flats = detail::count_flat_slots(ArgTypes{});
            uint32_t consumer_retptr = static_cast<uint32_t>(slots[num_arg_flats].i64);

            uint32_t byte_len = e_len * static_cast<uint32_t>(sizeof(E));
            uint32_t consumer_e_ptr = 0;
            if (byte_len > 0) {
               auto alloc_ret = consumer->be->call_with_return(
                  host_void, std::string_view{"cabi_realloc"},
                  uint32_t{0}, uint32_t{0},
                  uint32_t{static_cast<uint32_t>(alignof(E))}, byte_len);
               consumer_e_ptr = alloc_ret ? alloc_ret->to_ui32() : 0u;
               char* consumer_mem_w = consumer->be->get_context().linear_memory();
               std::memcpy(consumer_mem_w + consumer_e_ptr,
                           provider_mem + e_ptr, byte_len);
            }

            char* consumer_mem_w = consumer->be->get_context().linear_memory();
            std::memcpy(consumer_mem_w + consumer_retptr, &consumer_e_ptr, 4);
            std::memcpy(consumer_mem_w + consumer_retptr + 4, &e_len, 4);

            native_value rv;
            rv.i64 = static_cast<uint64_t>(consumer_retptr);
            return rv;
         } else {
            // Record/optional: use psio's canonical_lift_fields + lower_fields
            uint32_t provider_retptr = static_cast<uint32_t>(raw_ret);
            const uint8_t* provider_mem =
               reinterpret_cast<const uint8_t*>(provider->be->get_context().linear_memory());

            detail::host_lift_policy ret_lift{nullptr, provider_mem};
            U result = psio::canonical_lift_fields<U>(ret_lift, provider_retptr);

            constexpr size_t num_arg_flats = detail::count_flat_slots(ArgTypes{});
            uint32_t consumer_retptr = static_cast<uint32_t>(slots[num_arg_flats].i64);

            lower_into_consumer_retarea<U>(
               result, consumer, host_void,
               consumer->be->get_context().linear_memory(),
               consumer_retptr);

            native_value rv;
            rv.i64 = static_cast<uint64_t>(consumer_retptr);
            return rv;
         }
      }
   }

   // Lower a result value into the consumer's return area.
   // Handles strings, records, optionals by allocating in consumer's
   // memory via cabi_realloc and writing the canonical layout.
   template <typename U>
   static void lower_into_consumer_retarea(
      const U& value,
      instance_t* consumer,
      void* host_void,
      char* consumer_memory,
      uint32_t retptr)
   {
      // Use a store policy that writes to consumer's linear memory
      // and allocates via consumer's cabi_realloc
      struct consumer_store_policy {
         char*        mem;
         instance_t*  inst;
         void*        host;

         uint32_t alloc(uint32_t align, uint32_t size) {
            if (size == 0) return 0;
            auto ret = inst->be->call_with_return(
               host, std::string_view{"cabi_realloc"},
               uint32_t{0}, uint32_t{0}, align, size);
            return ret ? ret->to_ui32() : 0u;
         }

         void store_u8(uint32_t off, uint8_t v)   { std::memcpy(mem + off, &v, 1); }
         void store_u16(uint32_t off, uint16_t v) { std::memcpy(mem + off, &v, 2); }
         void store_u32(uint32_t off, uint32_t v) { std::memcpy(mem + off, &v, 4); }
         void store_u64(uint32_t off, uint64_t v) { std::memcpy(mem + off, &v, 8); }
         void store_f32(uint32_t off, float v)    { std::memcpy(mem + off, &v, 4); }
         void store_f64(uint32_t off, double v)   { std::memcpy(mem + off, &v, 8); }
         void store_bytes(uint32_t off, const char* data, uint32_t len) {
            if (len > 0) std::memcpy(mem + off, data, len);
         }
      };

      consumer_store_policy sp{consumer_memory, consumer, host_void};
      psio::canonical_lower_fields(value, sp, retptr);
   }
};

}  // namespace psizam
