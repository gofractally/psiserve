#pragma once

// shared.hpp — the contract between host and guest for the hello
// example. Think of it as the WIT file: it declares interfaces, and
// nothing else. No bodies, no trampolines, no empty stubs — host and
// guest are written by different people and each owns its own side of
// the impl.
//
// Who provides what:
//   • env::log_u64, clock_api::now     — host implements them (see Host in
//                                        host.cpp). Guest calls them as
//                                        WASM imports resolved at link
//                                        time (see guest.cpp).
//   • greeter::* exports                — guest implements them (see guest.cpp);
//                                        host reaches them through
//                                        `vm.as<greeter>().…`.
//
// Coverage — the greeter interface deliberately spans the interesting
// lift/lower shapes on the host→guest call direction:
//
//    run(count)                    void return, one scalar arg
//    concat(a, b)                  string args, string return (return-area)
//    add(a, b, c)                  multiple scalar args, scalar return
//    sum_point(p)                  record arg (multi-slot lower)
//    make_point(x, y)              record return (return-area, multi-slot)
//    translate(p, dx, dy)          record in + record out (worst-case shape)
//    sum_list(xs)                  list<u32> arg (heap-alloc + flat ptr+len)
//    find_first(xs, needle)        list arg + optional return (return-area)

#include <stdint.h>
#include <optional>
#include <string_view>
#include <vector>
#include <psio1/guest_attrs.hpp>
#include <psio1/structural.hpp>
#include <psio1/wit_owned.hpp>

PSIO1_PACKAGE(hello, "0.1.0");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(hello)

// point — record type shared across the host/guest boundary. PSIO1_REFLECT
// gives psio's canonical_* machinery the member list it needs to compute
// size/align/flat_count and drive field-wise lower/lift.
struct point
{
   uint32_t x{};
   uint32_t y{};
};
PSIO1_REFLECT(point, x, y)

struct env
{
   PSIO1_IMPORT(env, log_u64)
   static void log_u64(uint64_t n);

   static void     log_string(std::string_view msg);
   static uint32_t sum_points_host(point a, point b);
};

struct clock_api
{
   PSIO1_IMPORT(clock_api, now)
   static uint64_t now();
};

// greeter — declarations only. On the guest, PSIO1_MODULE emits the
// canonical-ABI export thunks from an impl class (see guest.cpp); on
// the host, the reflected proxy invokes them via vm.as<greeter>().
struct greeter
{
   static void               run(uint64_t count);
   static wit::string        concat(std::string_view a, std::string_view b);
   static uint32_t           add(uint32_t a, uint32_t b, uint32_t c);
   static uint32_t           sum_point(point p);
   static point              make_point(uint32_t x, uint32_t y);
   static point              translate(point p, int32_t dx, int32_t dy);
   static uint32_t           sum_list(std::vector<uint32_t> xs);
   static std::optional<uint32_t>
                             find_first(std::vector<int32_t> xs, int32_t needle);
   static wit::vector<uint32_t> range(uint32_t n);
   static wit::vector<point>    make_grid(uint32_t w, uint32_t h);
};

PSIO1_INTERFACE(env,       types(), funcs(func(log_u64,           value),
                                        func(log_string,        msg),
                                        func(sum_points_host,   a, b)))
PSIO1_INTERFACE(clock_api, types(), funcs(func(now)))
PSIO1_INTERFACE(greeter,   types(point),
                          funcs(func(run,         count),
                                func(concat,      a, b),
                                func(add,         a, b, c),
                                func(sum_point,   p),
                                func(make_point,  x, y),
                                func(translate,   p, dx, dy),
                                func(sum_list,    xs),
                                func(find_first,  xs, needle),
                                func(range,       n),
                                func(make_grid,   w, h)))
