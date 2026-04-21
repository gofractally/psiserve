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

#include <psio/fracpack.hpp>
#include <psio/name.hpp>
#include <psio/reflect.hpp>

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
      template <typename R, typename C, typename... Args>
      std::tuple<std::remove_cvref_t<Args>...> param_tuple_of(R (C::*)(Args...));

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

            // Deserialize fracpack args into the parameter tuple.
            // TODO: replace with psio::view<const ParamTuple> for
            // zero-copy dispatch once psio view infrastructure is
            // ported (psio::get<I>, tuple_size for views, etc.)
            auto params = psio::from_frac<ParamTuple>(
               std::span<const char>(data, len));

            if constexpr (std::is_void_v<ReturnType>)
            {
               std::apply([&](auto&&... args) {
                  (service.*member)(std::forward<decltype(args)>(args)...);
               }, std::move(params));
            }
            else
            {
               auto result = std::apply([&](auto&&... args) {
                  return (service.*member)(std::forward<decltype(args)>(args)...);
               }, std::move(params));

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
