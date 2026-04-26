#pragma once

// wasi:random@0.2.3 — random, insecure, and insecure-seed interfaces.
//
// Canonical WIT sources:
//   libraries/wasi/wit/0.2.3/random/random.wit
//   libraries/wasi/wit/0.2.3/random/insecure.wit
//   libraries/wasi/wit/0.2.3/random/insecure-seed.wit
//
// These C++ declarations mirror the WIT through PSIO structural
// metadata. The inline stubs return defaults and are never called
// at runtime — psiserve's Linker wires the imports to host_function
// closures before instantiation.

#include <psio1/reflect.hpp>
#include <psio1/structural.hpp>

#include <cstdint>
#include <tuple>
#include <vector>

// =====================================================================
// Interface: wasi:random/random
// =====================================================================

struct wasi_random_random
{
   // get-random-bytes: func(len: u64) -> list<u8>
   static inline std::vector<uint8_t> get_random_bytes(uint64_t /*len*/)
   {
      return {};
   }

   // get-random-u64: func() -> u64
   static inline uint64_t get_random_u64()
   {
      return 0;
   }
};

// =====================================================================
// Interface: wasi:random/insecure
// =====================================================================

struct wasi_random_insecure
{
   // get-insecure-random-bytes: func(len: u64) -> list<u8>
   static inline std::vector<uint8_t> get_insecure_random_bytes(uint64_t /*len*/)
   {
      return {};
   }

   // get-insecure-random-u64: func() -> u64
   static inline uint64_t get_insecure_random_u64()
   {
      return 0;
   }
};

// =====================================================================
// Interface: wasi:random/insecure-seed
// =====================================================================

struct wasi_random_insecure_seed
{
   // insecure-seed: func() -> tuple<u64, u64>
   static inline std::tuple<uint64_t, uint64_t> insecure_seed()
   {
      return {0, 0};
   }
};

// =====================================================================
// Package and interface registration
// =====================================================================

PSIO1_PACKAGE(wasi_random, "0.2.3");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(wasi_random)

PSIO1_INTERFACE(wasi_random_random,
               types(),
               funcs(func(get_random_bytes, len),
                     func(get_random_u64)))

PSIO1_INTERFACE(wasi_random_insecure,
               types(),
               funcs(func(get_insecure_random_bytes, len),
                     func(get_insecure_random_u64)))

PSIO1_INTERFACE(wasi_random_insecure_seed,
               types(),
               funcs(func(insecure_seed)))
