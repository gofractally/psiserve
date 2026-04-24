// Measure raw memory-bandwidth baseline on a current-mainnet (Fulu-fork)
// BeaconState. Our `eth::BeaconState` type models Phase 0 only, so we can't
// semantically decode this — but we can establish what a single-threaded
// pass over 310 MiB of bytes actually costs on this hardware, which bounds
// any SSZ decoder's theoretical minimum.
//
// Download with:
//   curl -L -H 'Accept: application/octet-stream' \
//     'https://beaconstate.ethstaker.cc/eth/v2/debug/beacon/states/finalized' \
//     -o /tmp/beacon_data/mainnet-state-current.ssz

#include "beacon_types_fulu.hpp"

#include <psio/from_ssz.hpp>
#include <psio/ssz_view.hpp>
#include <psio/to_ssz.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <span>
#include <vector>

static std::vector<char> load_file(const char* path)
{
   std::ifstream f(path, std::ios::binary);
   if (!f) return {};
   f.seekg(0, std::ios::end);
   auto sz = f.tellg();
   f.seekg(0);
   std::vector<char> out(static_cast<std::size_t>(sz));
   f.read(out.data(), sz);
   return out;
}

struct timing { double min_ns{}, median_ns{}, max_ns{}; };

template <typename F>
static timing run_timed(const char* label, std::size_t bytes, F&& body)
{
   int trials = 7;
   std::vector<double> samples;
   samples.reserve(trials);
   for (int i = 0; i < trials; ++i)
   {
      auto t0 = std::chrono::steady_clock::now();
      body();
      auto t1 = std::chrono::steady_clock::now();
      samples.push_back(
          std::chrono::duration<double, std::nano>(t1 - t0).count());
   }
   std::sort(samples.begin(), samples.end());
   timing t{samples.front(), samples[samples.size() / 2], samples.back()};

   double median_s = t.median_ns / 1e9;
   double gib_s    = (bytes / (1024.0 * 1024.0 * 1024.0)) / median_s;

   std::printf("  %-45s  min=%6.2f ms  median=%6.2f ms  (%.1f GiB/s)\n",
               label, t.min_ns / 1e6, t.median_ns / 1e6, gib_s);
   return t;
}

int main(int argc, char** argv)
{
   const char* path = (argc > 1)
                          ? argv[1]
                          : "/tmp/beacon_data/mainnet-state-current.ssz";
   auto raw = load_file(path);
   if (raw.empty())
   {
      std::fprintf(
          stderr,
          "No state at %s. Download with:\n"
          "  curl -L -H 'Accept: application/octet-stream' \\\n"
          "    'https://beaconstate.ethstaker.cc/eth/v2/debug/beacon/states/"
          "finalized' -o %s\n",
          path, path);
      return 1;
   }

   double mib = raw.size() / (1024.0 * 1024.0);
   std::printf("Input: %s (%.2f MiB, %zu bytes)\n", path, mib, raw.size());
   std::printf("Current mainnet fork: Fulu (check Eth-consensus-version "
               "header on download to confirm)\n\n");

   std::printf("Single-threaded memory-bandwidth baselines "
               "(any decoder's theoretical minimum):\n");

   std::vector<char> buf(raw.size());

   run_timed("memcpy (raw bytes → heap)", raw.size(), [&] {
      std::memcpy(buf.data(), raw.data(), raw.size());
      asm volatile("" : : "r,m"(buf.data()) : "memory");
   });

   run_timed("memset 0 (heap)", raw.size(), [&] {
      std::memset(buf.data(), 0, raw.size());
      asm volatile("" : : "r,m"(buf.data()) : "memory");
   });

   // Pretend-decode pass: just read every byte (compiler-opaque) to model
   // the minimum work a validator would do.
   run_timed("byte-sum scan (cold-cache-safe)", raw.size(), [&] {
      std::uint64_t sum = 0;
      for (auto c : raw)
         sum += static_cast<std::uint8_t>(c);
      asm volatile("" : : "r"(sum));
   });

   // What ssz_validate<T>() would ideally approach for T of this shape.
   // At ~60 GiB/s memcpy, a 310 MiB decode bottoms out around
   //   310 MiB / 60 GiB/s ≈ 5 ms on single core.

   double mib_val = raw.size() / (1024.0 * 1024.0);
   std::printf(
       "\nTheoretical floor for single-core decode of a %.1f MiB state is\n"
       "~%.1f ms on this hardware (bounded by memcpy speed).\n\n",
       mib_val, (raw.size() / 60.0 / (1024.0 * 1024.0 * 1024.0)) * 1000.0);

   // ── Actual psio decode of the Fulu state ─────────────────────────────────

   std::printf("psio on real Fulu-fork BeaconState:\n");

   run_timed("psio::ssz_validate<fulu::BeaconState>", raw.size(), [&] {
      psio::ssz_validate<eth::fulu::BeaconState>(raw);
   });

   // Heap-allocate — sizeof(BeaconState) is several MiB.
   auto state = std::make_unique<eth::fulu::BeaconState>();

   run_timed("psio::convert_from_ssz (full decode, in-place)", raw.size(), [&] {
      psio::convert_from_ssz(*state, raw);
   });

   std::printf("  validators: %zu\n", state->validators.size());
   std::printf("  slot: %llu\n", static_cast<unsigned long long>(state->slot));

   // Re-encode to confirm byte-identical round-trip.
   std::vector<char> re_encoded;
   run_timed("psio::convert_to_ssz (re-encode)", raw.size(), [&] {
      re_encoded.clear();
      psio::convert_to_ssz(*state, re_encoded);
   });

   bool identical = (re_encoded.size() == raw.size() &&
                     std::memcmp(re_encoded.data(), raw.data(), raw.size()) == 0);
   std::printf("  round-trip: %s\n", identical ? "byte-identical ✓" : "MISMATCH ✗");

   // Zero-copy view: a single-field access should be essentially free.
   run_timed("ssz_view<fulu::BeaconState> + read slot", raw.size(), [&] {
      auto v = psio::ssz_view_of<eth::fulu::BeaconState>(raw);
      std::uint64_t slot = v.field<2>();  // slot is field index 2
      asm volatile("" : : "r"(slot));
   });

   return 0;
}
