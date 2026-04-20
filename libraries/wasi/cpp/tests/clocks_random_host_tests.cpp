#include <wasi/0.2.3/clocks_host.hpp>
#include <wasi/0.2.3/random_host.hpp>

#include <catch2/catch.hpp>

using namespace wasi_host;
using psiber::Scheduler;

TEST_CASE("WasiClocksHost wall clock returns nonzero time", "[wasi][clocks]")
{
   WasiClocksHost clocks;

   auto dt = clocks.now();
   REQUIRE(dt.seconds > 0);
}

TEST_CASE("WasiClocksHost monotonic clock returns nonzero", "[wasi][clocks]")
{
   WasiClocksHost clocks;

   auto t1 = clocks.mono_now();
   auto t2 = clocks.mono_now();
   REQUIRE(t1 > 0);
   REQUIRE(t2 >= t1);
}

TEST_CASE("WasiRandomHost get_random_bytes", "[wasi][random]")
{
   WasiRandomHost rng;

   auto bytes = rng.get_random_bytes(32);
   REQUIRE(bytes.size() == 32);

   // Very unlikely to be all zeros
   bool all_zero = true;
   for (auto b : bytes)
      if (b != 0)
         all_zero = false;
   REQUIRE_FALSE(all_zero);
}

TEST_CASE("WasiRandomHost get_random_u64", "[wasi][random]")
{
   WasiRandomHost rng;

   auto v1 = rng.get_random_u64();
   auto v2 = rng.get_random_u64();
   // Astronomically unlikely to get the same value twice
   REQUIRE(v1 != v2);
}

TEST_CASE("WasiRandomHost insecure_seed", "[wasi][random]")
{
   WasiRandomHost rng;

   auto [a, b] = rng.insecure_seed();
   REQUIRE((a != 0 || b != 0));
}
