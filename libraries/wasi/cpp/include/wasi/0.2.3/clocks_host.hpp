#pragma once

#include <wasi/0.2.3/clocks.hpp>
#include <wasi/0.2.3/io_host.hpp>

#include <psio/structural.hpp>

#include <chrono>

namespace wasi_host {

struct WasiClocksHost
{

   // ── wasi:clocks/wall-clock ────────────────────────────────────────

   datetime now()
   {
      auto tp = std::chrono::system_clock::now();
      auto dur = tp.time_since_epoch();
      auto secs = std::chrono::duration_cast<std::chrono::seconds>(dur);
      auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(dur - secs);
      return {static_cast<uint64_t>(secs.count()),
              static_cast<uint32_t>(ns.count())};
   }

   datetime resolution()
   {
      return {0, 1};
   }

   // ── wasi:clocks/monotonic-clock ───────────────────────────────────

   instant mono_now()
   {
      auto tp = std::chrono::steady_clock::now();
      return static_cast<uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              tp.time_since_epoch())
              .count());
   }

   duration mono_resolution()
   {
      return 1;
   }

   psio::own<pollable> subscribe_instant(instant when)
   {
      // Timer pollables are not yet backed by real timer fds.
      // For now, create a pollable that's always ready if the
      // deadline has passed, or never ready otherwise.
      (void)when;
      return psio::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
   }

   psio::own<pollable> subscribe_duration(duration when)
   {
      (void)when;
      return psio::own<pollable>{psizam::handle_table<pollable_data, 256>::invalid_handle};
   }
};

}  // namespace wasi_host

PSIO_HOST_MODULE(wasi_host::WasiClocksHost,
   interface(wasi_clocks_wall_clock, now, resolution),
   interface(wasi_clocks_monotonic_clock, mono_now, mono_resolution,
             subscribe_instant, subscribe_duration))
