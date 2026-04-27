#pragma once

// wasi:clocks@0.2.3 — wall-clock and monotonic-clock interfaces.
//
// Canonical WIT sources:
//   libraries/wasi/wit/0.2.3/clocks/wall-clock.wit
//   libraries/wasi/wit/0.2.3/clocks/monotonic-clock.wit
//
// These C++ declarations mirror the WIT through PSIO structural
// metadata. The inline stubs return defaults and are never called
// at runtime — psiserve's Linker wires the imports to host_function
// closures before instantiation.

#include <psio/reflect.hpp>
#include <psio/structural.hpp>
#include <psio/wit_resource.hpp>

#include <wasi/0.2.3/io.hpp>

#include <cstdint>

// =====================================================================
// wasi:clocks/wall-clock — datetime record, now, resolution
// =====================================================================

/// A time and date in seconds plus nanoseconds.
struct datetime
{
   uint64_t seconds      = 0;
   uint32_t nanoseconds  = 0;
};
PSIO_REFLECT(datetime, seconds, nanoseconds)

// =====================================================================
// Interface: wasi:clocks/wall-clock
// =====================================================================

struct wasi_clocks_wall_clock
{
   // now: func() -> datetime
   static inline datetime now()
   {
      return {};
   }

   // resolution: func() -> datetime
   static inline datetime resolution()
   {
      return {};
   }
};

// =====================================================================
// wasi:clocks/monotonic-clock — instant/duration type aliases,
//   now, resolution, subscribe-instant, subscribe-duration
// =====================================================================

// instant and duration are both u64 type aliases in the WIT.
using instant  = uint64_t;
using duration = uint64_t;

// =====================================================================
// Interface: wasi:clocks/monotonic-clock
// =====================================================================

struct wasi_clocks_monotonic_clock
{
   // now: func() -> instant
   static inline instant now()
   {
      return 0;
   }

   // resolution: func() -> duration
   static inline duration resolution()
   {
      return 0;
   }

   // subscribe-instant: func(when: instant) -> pollable
   static inline psio::own<pollable> subscribe_instant(instant /*when*/)
   {
      return psio::own<pollable>{0};
   }

   // subscribe-duration: func(when: duration) -> pollable
   static inline psio::own<pollable> subscribe_duration(duration /*when*/)
   {
      return psio::own<pollable>{0};
   }
};

// =====================================================================
// Package and interface registration
// =====================================================================

PSIO_PACKAGE(wasi_clocks, "0.2.3");
#undef  PSIO_CURRENT_PACKAGE_
#define PSIO_CURRENT_PACKAGE_ PSIO_PACKAGE_TYPE_(wasi_clocks)

PSIO_INTERFACE(wasi_clocks_wall_clock,
               types(datetime),
               funcs(func(now),
                     func(resolution)))

PSIO_INTERFACE(wasi_clocks_monotonic_clock,
               types(),
               funcs(func(now),
                     func(resolution),
                     func(subscribe_instant, when),
                     func(subscribe_duration, when)))
