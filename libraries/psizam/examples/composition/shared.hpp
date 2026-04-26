#pragma once

// shared.hpp — interface declarations shared between provider and consumer
// WASM modules, plus the host. This is the contract:
//
//   env       — host provides log_u64 (host → guest import)
//   greeter   — provider module exports (module → module)
//   processor — consumer module exports (host calls these)

#include <stdint.h>
#include <string_view>
#include <psio1/guest_attrs.hpp>
#include <psio1/structural.hpp>
#include <psio1/wit_owned.hpp>

PSIO1_PACKAGE(composition, "0.1.0");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(composition)

// ── Host-provided interface ─────────────────────────────────────────
struct env
{
   PSIO1_IMPORT(env, log_u64)
   static void log_u64(uint64_t n);

   static void log_string(std::string_view msg);
};

PSIO1_INTERFACE(env, types(), funcs(func(log_u64, value),
                                   func(log_string, msg)))

// ── Shared record types ─────────────────────────────────────────────

struct point {
   uint32_t x{};
   uint32_t y{};
};
PSIO1_REFLECT(point, x, y)

// ── Provider-exported interface (greeter) ───────────────────────────
// Coverage: scalars, strings, records, lists of scalars, lists of records.
struct greeter
{
   static uint32_t    add(uint32_t a, uint32_t b);
   static wit::string concat(std::string_view a, std::string_view b);
   static uint64_t    double_it(uint64_t v);
   static point       translate(point p, int32_t dx, int32_t dy);
   static uint32_t    sum_list(std::vector<uint32_t> xs);
   static wit::vector<point> make_grid(uint32_t w, uint32_t h);
};

PSIO1_INTERFACE(greeter, types(point),
   funcs(func(add,       a, b),
         func(concat,    a, b),
         func(double_it, v),
         func(translate, p, dx, dy),
         func(sum_list,  xs),
         func(make_grid, w, h)))

// ── Consumer-exported interface (processor) ─────────────────────────
struct processor
{
   static uint32_t    test_add(uint32_t x, uint32_t y);
   static wit::string test_concat(std::string_view a, std::string_view b);
   static uint64_t    test_double(uint64_t v);
   static point       test_translate(point p, int32_t dx, int32_t dy);
   static uint32_t    test_sum_list(std::vector<uint32_t> xs);
   static wit::vector<point> test_make_grid(uint32_t w, uint32_t h);
};

PSIO1_INTERFACE(processor, types(),
   funcs(func(test_add,       x, y),
         func(test_concat,    a, b),
         func(test_double,    v),
         func(test_translate, p, dx, dy),
         func(test_sum_list,  xs),
         func(test_make_grid, w, h)))
