#pragma once

#include <wasi/0.2.3/random.hpp>

#include <psio/structural.hpp>

#include <cstdint>
#include <random>
#include <vector>

namespace wasi_host {

struct WasiRandomHost
{
   std::mt19937_64 rng{std::random_device{}()};

   std::vector<uint8_t> get_random_bytes(uint64_t len)
   {
      uint64_t cap = len > 65536 ? 65536 : len;
      std::vector<uint8_t> buf(cap);
      for (uint64_t i = 0; i < cap; i += 8)
      {
         uint64_t val = rng();
         size_t   n = (cap - i) < 8 ? (cap - i) : 8;
         std::memcpy(buf.data() + i, &val, n);
      }
      return buf;
   }

   uint64_t get_random_u64()
   {
      return rng();
   }

   std::vector<uint8_t> get_insecure_random_bytes(uint64_t len)
   {
      return get_random_bytes(len);
   }

   uint64_t get_insecure_random_u64()
   {
      return rng();
   }

   std::tuple<uint64_t, uint64_t> insecure_seed()
   {
      return {rng(), rng()};
   }
};

}  // namespace wasi_host

PSIO_HOST_MODULE(wasi_host::WasiRandomHost,
   interface(wasi_random_random, get_random_bytes, get_random_u64),
   interface(wasi_random_insecure, get_insecure_random_bytes, get_insecure_random_u64),
   interface(wasi_random_insecure_seed, insecure_seed))
