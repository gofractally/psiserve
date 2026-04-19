// consumer_guest.cpp — imports greeter, exports processor.
// Uses ImportProxy for all import method bodies.

#include "shared.hpp"

#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

#include <span>
#include <string.h>

PSIO_WIT_SECTION(processor)
PSIO_WIT_SECTION(greeter)
PSIO_WIT_SECTION(env)

// ── Env imports ────────────────────────────────────────────────────
PSIO_GUEST_IMPORTS(env, log_string)
void env::log_string(std::string_view msg) {
   _psio_import_call_env_log_string(msg);
}

// ── Greeter imports — all methods via ImportProxy ──────────────────
PSIO_IMPORT_IMPL(greeter, add, concat, double_it,
                 translate, sum_list, make_grid)

uint32_t greeter::add(uint32_t a, uint32_t b)
   PSIO_IMPORT_IMPL_BODY(greeter, add, a, b)

wit::string greeter::concat(std::string_view a, std::string_view b)
   PSIO_IMPORT_IMPL_BODY(greeter, concat, a, b)

uint64_t greeter::double_it(uint64_t v)
   PSIO_IMPORT_IMPL_BODY(greeter, double_it, v)

point greeter::translate(point p, int32_t dx, int32_t dy)
   PSIO_IMPORT_IMPL_BODY(greeter, translate, p, dx, dy)

uint32_t greeter::sum_list(std::vector<uint32_t> xs)
   PSIO_IMPORT_IMPL_BODY(greeter, sum_list, xs)

wit::vector<point> greeter::make_grid(uint32_t w, uint32_t h)
   PSIO_IMPORT_IMPL_BODY(greeter, make_grid, w, h)

// ── Processor implementation ────────────────────────────────────────

struct processor_impl
{
   uint32_t test_add(uint32_t x, uint32_t y) {
      return greeter::add(x, y);
   }

   wit::string test_concat(std::string_view a, std::string_view b) {
      return greeter::concat(a, b);
   }

   uint64_t test_double(uint64_t v) {
      return greeter::double_it(v);
   }

   point test_translate(point p, int32_t dx, int32_t dy) {
      return greeter::translate(p, dx, dy);
   }

   uint32_t test_sum_list(std::span<const uint32_t> xs) {
      // Pass the span's data+size directly as flat_vals — no vector alloc
      psizam::flat_val slots[16] = {};
      std::size_t idx = 0;
      psizam::guest_import_lower(slots, idx, xs);
      auto r = _psio_raw_greeter_sum_list(
         slots[0],  slots[1],  slots[2],  slots[3],
         slots[4],  slots[5],  slots[6],  slots[7],
         slots[8],  slots[9],  slots[10], slots[11],
         slots[12], slots[13], slots[14], slots[15]);
      return static_cast<uint32_t>(r);
   }

   wit::vector<point> test_make_grid(uint32_t w, uint32_t h) {
      return greeter::make_grid(w, h);
   }
};

PSIO_MODULE(processor_impl,
            test_add, test_concat, test_double,
            test_translate, test_sum_list, test_make_grid)
