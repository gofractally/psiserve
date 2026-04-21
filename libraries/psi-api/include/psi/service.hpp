#pragma once

// psi/service.hpp — psibase service framework.
//
// PSIBASE_DISPATCH(Service) generates the `apply` export that:
//   1. Receives (method_id: name_id, data_ptr, data_len) — opaque fracpack bytes
//   2. Uses psio::get_member_function to match method_id to a reflected method
//   3. from_frac the args into the method's parameter tuple
//   4. Calls the method on a Service instance
//   5. to_frac the return value, writes it to the result buffer
//
// The service class is a plain struct with methods + PSIO_REFLECT.
// The developer never writes apply() or deals with serialization.
//
// Cross-contract calls use fracpack for the args blob:
//   auto args = psio::to_frac(std::tuple(from, to, amount, memo));
//   blockchain_api::call_contract("token"_n, "transfer"_n, args);

#include <psio/frac_ref.hpp>
#include <psio/fracpack.hpp>
#include <psio/name.hpp>
#include <psio/reflect.hpp>
#include <psio/view.hpp>

#include <cstdint>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <vector>

namespace psibase
{
   using name_id = uint64_t;

   [[noreturn]] inline void abort_message(const char* msg)
   {
      // In WASM, this traps. The host catches the trap, extracts
      // the message, and reports it to the caller.
      // For now, use __builtin_trap which the host's trap handler sees.
      (void)msg;
      __builtin_trap();
   }

   inline void check(bool condition, const char* msg)
   {
      if (!condition)
         abort_message(msg);
   }

   namespace detail
   {
      // Map view types back to owning types for fracpack deserialization.
      // string_view → string (fracpack stores strings, view reads them)
      template <typename T> struct to_value_type { using type = T; };
      template <> struct to_value_type<std::string_view> { using type = std::string; };
      template <typename T> using to_value_t = typename to_value_type<T>::type;

      template <typename R, typename C, typename... Args>
      std::tuple<to_value_t<std::remove_cvref_t<Args>>...> param_tuple_of(R (C::*)(Args...));

      template <typename R, typename C, typename... Args>
      R return_type_of(R (C::*)(Args...));
   }

   // Result buffer: the apply export writes fracpack return data here.
   // The host reads it after apply returns.
   inline thread_local std::vector<char> g_retval;

   inline void set_retval(const char* data, uint32_t len)
   {
      g_retval.assign(data, data + len);
   }

   inline std::span<const char> get_retval()
   {
      return {g_retval.data(), g_retval.size()};
   }

   /// Dispatch an action to a reflected service method.
   ///
   /// method_id: hash of the method name (psio::hash_name)
   /// data/len:  fracpack-encoded parameter tuple
   ///
   /// Returns 0 on success, non-zero on unknown method.
   template <typename ParamTuple, typename ViewT>
   struct frac_tuple_get;

   // Specialization: extract element I from a fracpack-encoded tuple
   template <typename... Ts>
   struct frac_tuple_get<std::tuple<Ts...>, void>
   {
      template <std::size_t I>
      static auto get(const char* data)
      {
         return psio::frac_detail::frac_view_field<std::tuple<Ts...>, I>(data);
      }
   };

   template <typename ParamTuple, typename F, std::size_t... I>
   decltype(auto) frac_tuple_call(F&& f, const char* data, std::index_sequence<I...>)
   {
      return f(frac_tuple_get<ParamTuple, void>::template get<I>(data)...);
   }

   template <typename Service>
   uint32_t dispatch(name_id method_id, const char* data, uint32_t len)
   {
      Service service{};

      bool called = psio::get_member_function<Service>(
         method_id,
         [&](auto member, auto /*names*/)
         {
            using ParamTuple = decltype(detail::param_tuple_of(member));
            using ReturnType = decltype(detail::return_type_of(member));

            auto param_data = std::span<const char>{data, len};

            check(psio::fracpack_validate<ParamTuple>(param_data),
                  "invalid action args");

            // frac_tuple_view: lightweight view over a fracpack-encoded
            // tuple. Uses frac_view_field for per-element access without
            // requiring PSIO_REFLECT on std::tuple.
            struct frac_tuple_view
            {
               const char* data;
               explicit frac_tuple_view(const char* d) : data(d) {}
            };
            frac_tuple_view param_view(param_data.data());

            constexpr auto N = std::tuple_size_v<ParamTuple>;

            if constexpr (std::is_void_v<ReturnType>)
            {
               frac_tuple_call<ParamTuple>(
                  [&](auto&&... args) {
                     (service.*member)(std::forward<decltype(args)>(args)...);
                  },
                  param_view.data,
                  std::make_index_sequence<N>{});
            }
            else
            {
               auto result = frac_tuple_call<ParamTuple>(
                  [&](auto&&... args) {
                     return (service.*member)(std::forward<decltype(args)>(args)...);
                  },
                  param_view.data,
                  std::make_index_sequence<N>{});

               auto rv = psio::to_frac(result);
               set_retval(rv.data(), static_cast<uint32_t>(rv.size()));
            }
         });

      return called ? 0 : 1;
   }

}  // namespace psibase

// ═══════════════════════════════════════════════════════════════════
// PSIBASE_DISPATCH — generates the `apply` export for a service.
//
// Usage:
//   struct TokenService {
//      void     transfer(name_id from, name_id to, uint64_t amount, std::string memo);
//      uint64_t balance(name_id account);
//   };
//   PSIO_REFLECT(TokenService,
//                method(transfer, from, to, amount, memo),
//                method(balance, account))
//   PSIBASE_DISPATCH(TokenService)
//
// Generates an `apply` export that receives (method_id, data_ptr, data_len)
// and dispatches to the matching reflected method via fracpack deserialization.
// ═══════════════════════════════════════════════════════════════════

#ifdef __wasm__

#define PSIBASE_DISPATCH(SERVICE)                                                  \
   extern "C" [[clang::export_name("apply")]]                                     \
   uint32_t apply(uint64_t method_id, uint32_t data_ptr, uint32_t data_len)        \
   {                                                                               \
      const char* data = reinterpret_cast<const char*>(data_ptr);                  \
      return ::psibase::dispatch<SERVICE>(method_id, data, data_len);              \
   }

#else

#define PSIBASE_DISPATCH(SERVICE)

#endif
