#pragma once

#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string_view>

#ifdef COMPILING_WASM
#include <psibase/check.hpp>
#endif

namespace psio
{
   inline std::string_view error_to_str(std::string_view msg)
   {
      return msg;
   }

   template <typename T>
   [[noreturn]] void abort_error(const T& msg)
   {
#ifdef COMPILING_WASM
      psibase::abortMessage(error_to_str(msg));
#elif defined(__EXCEPTIONS)
      throw std::runtime_error((std::string)error_to_str(msg));
#else
      std::fprintf(stderr, "Fatal: %.*s\n", (int)error_to_str(msg).size(), error_to_str(msg).data());
      std::abort();
#endif
   }

   template <typename T>
   void check(bool cond, const T& msg)
   {
      if (!cond)
         abort_error(msg);
   }
};  // namespace psio
