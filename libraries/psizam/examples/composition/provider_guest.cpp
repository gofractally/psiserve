// provider_guest.cpp — the provider WASM module.
// Exports the greeter interface with complex types.

#include "shared.hpp"

#include <psio1/guest_alloc.hpp>
#include <psizam/module.hpp>

#include <span>
#include <string.h>

PSIO1_WIT_SECTION(greeter)
PSIO1_WIT_SECTION(env)

PSIO1_GUEST_IMPORTS(env, log_string)

void env::log_string(std::string_view msg) {
   _psio_import_call_env_log_string(msg);
}

struct greeter_impl
{
   uint32_t add(uint32_t a, uint32_t b) {
      env::log_u64(static_cast<uint64_t>(a) + b);
      return a + b;
   }

   wit::string concat(std::string_view a, std::string_view b) {
      wit::string result(a.size() + b.size());
      memcpy(result.data(),            a.data(), a.size());
      memcpy(result.data() + a.size(), b.data(), b.size());
      return result;
   }

   uint64_t double_it(uint64_t v) { return v * 2; }

   point translate(point p, int32_t dx, int32_t dy) {
      return point{p.x + static_cast<uint32_t>(dx),
                   p.y + static_cast<uint32_t>(dy)};
   }

   uint32_t sum_list(std::span<const uint32_t> xs) {
      uint32_t s = 0;
      for (uint32_t v : xs) s += v;
      return s;
   }

   wit::vector<point> make_grid(uint32_t w, uint32_t h) {
      wit::vector<point> out(static_cast<size_t>(w) * h);
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            out[y * w + x] = point{x, y};
      return out;
   }
};

PSIO1_MODULE(greeter_impl,
            add, concat, double_it,
            translate, sum_list, make_grid)
