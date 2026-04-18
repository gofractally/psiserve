// provider_guest.cpp — the provider WASM module.
// Exports the greeter interface. Uses the host-provided env interface.

#include "shared.hpp"

#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

#include <string.h>

// WIT custom sections
PSIO_WIT_SECTION(greeter)
PSIO_WIT_SECTION(env)

// Guest-side import thunks for canonical types
PSIO_GUEST_IMPORTS(env, log_string)

void env::log_string(std::string_view msg) {
   _psio_import_call_env_log_string(msg);
}

struct greeter_impl
{
   uint32_t add(uint32_t a, uint32_t b)
   {
      env::log_u64(static_cast<uint64_t>(a) + b);
      return a + b;
   }

   wit::string concat(std::string_view a, std::string_view b)
   {
      env::log_string("provider: concat called");
      wit::string result(a.size() + b.size());
      memcpy(result.data(),            a.data(), a.size());
      memcpy(result.data() + a.size(), b.data(), b.size());
      return result;
   }

   uint64_t double_it(uint64_t v)
   {
      return v * 2;
   }
};

PSIO_MODULE(greeter_impl,
            add, concat, double_it)
