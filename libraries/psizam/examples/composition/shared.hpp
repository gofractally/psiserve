#pragma once

// shared.hpp — interface declarations shared between provider and consumer
// WASM modules, plus the host. This is the contract:
//
//   env       — host provides log_u64 (host → guest import)
//   greeter   — provider module exports (module → module)
//   processor — consumer module exports (host calls these)

#include <stdint.h>
#include <string_view>
#include <psio/guest_attrs.hpp>
#include <psio/structural.hpp>
#include <psio/wit_owned.hpp>

PSIO_PACKAGE(composition, "0.1.0");

// ── Host-provided interface ─────────────────────────────────────────
struct env
{
   PSIO_IMPORT(env, log_u64)
   static void log_u64(uint64_t n);

   static void log_string(std::string_view msg);
};

PSIO_INTERFACE(env, types(), funcs(func(log_u64, value),
                                   func(log_string, msg)))

// ── Provider-exported interface (greeter) ───────────────────────────
// The provider module implements these. The consumer module imports them.
struct greeter
{
   static uint32_t    add(uint32_t a, uint32_t b);
   static wit::string concat(std::string_view a, std::string_view b);
   static uint64_t    double_it(uint64_t v);
};

PSIO_INTERFACE(greeter, types(), funcs(func(add,       a, b),
                                       func(concat,    a, b),
                                       func(double_it, v)))

// ── Consumer-exported interface (processor) ─────────────────────────
// The consumer module implements these. The host calls them.
struct processor
{
   static uint32_t    test_add(uint32_t x, uint32_t y);
   static wit::string test_concat(std::string_view a, std::string_view b);
   static uint64_t    test_double(uint64_t v);
};

PSIO_INTERFACE(processor, types(), funcs(func(test_add,    x, y),
                                         func(test_concat, a, b),
                                         func(test_double, v)))
