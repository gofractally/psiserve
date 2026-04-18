// guest.cpp — the guest author's side of the hello contract.
//
// Cross-compiled to wasm32 (wasi-sdk reactor mode) by the example's
// CMakeLists. shared.hpp declares the interfaces; this file provides an
// impl class and binds it to canonical-ABI export thunks via
// PSIO_MODULE.

#include "shared.hpp"

#include <psio/guest_alloc.hpp>   // cabi_realloc (single-TU export)
#include <psizam/module.hpp>      // PSIO_MODULE

#include <span>       // zero-alloc borrowed list view
#include <string.h>   // memcpy — libc, not libc++: no fd_write pull-in

// ── Guest-side import thunks for canonical types ────────────────────
// PSIO_GUEST_IMPORTS declares the raw 16-wide WASM imports and helper
// call wrappers. The method bodies below use those to lower C++ args
// into flat_vals, call the host, and lift the return.
PSIO_GUEST_IMPORTS(env, log_string, sum_points_host)

void env::log_string(std::string_view msg) {
   _psio_import_call_env_log_string(msg);
}

uint32_t env::sum_points_host(point a, point b) {
   return static_cast<uint32_t>(
      _psio_import_call_env_sum_points_host(a, b));
}

struct greeter_impl
{
   // run — scalar demo + guest→host canonical calls.
   void run(uint64_t count)
   {
      uint64_t base = clock_api::now();
      for (uint64_t i = 0; i < count; ++i)
         env::log_u64(base + i);

      // Exercise guest→host canonical imports:
      env::log_string("hello from guest!");
      uint32_t s = env::sum_points_host(point{10, 20}, point{3, 7});
      env::log_u64(s);  // should print 40
   }

   // concat — borrowed string views in, owning wit::string out. The
   // size-only ctor reserves an n-byte buffer we fill via data(); the
   // return goes out through PSIO_MODULE's return-area thunk as a
   // canonical {ptr,len} pair.
   wit::string concat(std::string_view a, std::string_view b)
   {
      wit::string str(a.size() + b.size());
      memcpy(str.data(),             a.data(), a.size());
      memcpy(str.data() + a.size(), b.data(),  b.size());
      return str;
   }

   // Scalar multi-arg / scalar return — verifies plural flat params
   // survive the 16-wide padded call convention unshuffled.
   uint32_t add(uint32_t a, uint32_t b, uint32_t c) { return a + b + c; }

   // Record arg → scalar return: point lowers to 2 flat i32 slots, lifted
   // back into {x, y} before the impl sees it.
   uint32_t sum_point(point p) { return p.x + p.y; }

   // Scalar args → record return: record flat count is 2 (>1 result), so
   // canonical ABI spills it to a return area.
   point make_point(uint32_t x, uint32_t y) { return point{x, y}; }

   // Worst-case shape: record-in, record-out, with scalars mixed in.
   point translate(point p, int32_t dx, int32_t dy)
   {
      return point{p.x + static_cast<uint32_t>(dx),
                   p.y + static_cast<uint32_t>(dy)};
   }

   // list<u32> — on the wire this is (i32 ptr, i32 len). The host
   // lowers std::vector into that pair; the guest lifts via std::span
   // so no owning-container (i.e. libc++ std::vector) code is pulled
   // in. The buffer lives in linear memory for the call's duration.
   uint32_t sum_list(std::span<const uint32_t> xs)
   {
      uint32_t s = 0;
      for (uint32_t v : xs) s += v;
      return s;
   }

   // optional<u32> return — lowers to (disc:i32, payload:i32) = 2 flat
   // slots, so the result goes through the return-area spill path.
   std::optional<uint32_t> find_first(std::span<const int32_t> xs, int32_t needle)
   {
      for (uint32_t i = 0; i < xs.size(); ++i)
         if (xs[i] == needle) return i;
      return std::nullopt;
   }

   // Owning list<u32> return — allocate via wit::vector<T>(n), fill, hand
   // it out. The ComponentProxy thunk spills {ptr, len} to a return area
   // and the host lifts by copying the payload out of linear memory.
   wit::vector<uint32_t> range(uint32_t n)
   {
      wit::vector<uint32_t> out(n);
      for (uint32_t i = 0; i < n; ++i) out[i] = i;
      return out;
   }

   // list<point> return — lists of records work the same way provided
   // the record is trivially-copyable (two u32s). Each element lays out
   // canonically in the backing buffer, so the host memcpy's the whole
   // thing out in one shot on lift.
   wit::vector<point> make_grid(uint32_t w, uint32_t h)
   {
      wit::vector<point> out(static_cast<std::size_t>(w) * h);
      for (uint32_t y = 0; y < h; ++y)
         for (uint32_t x = 0; x < w; ++x)
            out[y * w + x] = point{x, y};
      return out;
   }
};

PSIO_MODULE(greeter_impl,
            run, concat,
            add, sum_point, make_point, translate,
            sum_list, find_first,
            range, make_grid)
