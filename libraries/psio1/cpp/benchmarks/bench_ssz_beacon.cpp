// SSZ head-to-head benchmark.
//
// Baseline: psio's native SSZ codec.
// Optional competitor: OffchainLabs/sszpp.
//
// ── Why head-to-head requires a GCC/libstdc++ build ──
// sszpp uses std::views::chunk / std::views::enumerate (C++23 range adaptors).
// libc++ (Apple Clang / LLVM's own libc++) does not implement these yet as of
// LLVM 22; libstdc++ (GCC 15+) does. Consequently PSIO1_BENCH_SSZPP=ON builds
// are currently Linux+GCC only.
//
// ── Setup (Linux / GCC) ──
//   git clone --depth 1 https://github.com/OffchainLabs/sszpp /tmp/sszpp
//   git clone --depth 1 https://github.com/chfast/intx        /tmp/intx
//   git clone --depth 1 https://github.com/prysmaticlabs/hashtree /tmp/hashtree
//   cd /tmp/hashtree && make
//   # Back in psio build:
//   CC=gcc-15 CXX=g++-15 cmake -B build/ssz -G Ninja \
//     -DCMAKE_BUILD_TYPE=Release \
//     -DPSIO_ENABLE_BENCHMARKS=ON \
//     -DPSIO_BENCH_SSZPP=ON \
//     -DPSIO_SSZPP_DIR=/tmp/sszpp \
//     -DPSIO_INTX_DIR=/tmp/intx \
//     -DPSIO_HASHTREE_DIR=/tmp/hashtree
//   cmake --build build/ssz --target psio_bench_ssz_beacon
//
// ── Two comparable workloads ──
//
// 1) Phase 0 mainnet genesis BeaconState (psio-only)
//    sszpp's beacon_state_t is Altair-shaped; Phase 0 can't round-trip
//    through it. Still, psio timings here establish our baseline.
//
// 2) Validator list (shared workload for both libraries)
//    21 063 validators (mainnet genesis count), 121 B each = ~2.5 MB.
//    This is the dominant cost inside BeaconState decode anyway.
//    Both libraries' Validator layouts match the SSZ spec exactly, so
//    the wire bytes are identical — psio encodes, sszpp decodes (or vice
//    versa) without translation.
//
// ── Input data ──
//   mkdir -p /tmp/beacon_data
//   curl -sL -o /tmp/beacon_data/mainnet-genesis.ssz \
//     https://github.com/eth-clients/mainnet/raw/main/metadata/genesis.ssz

#include "beacon_types.hpp"
#include "beacon_types_fulu.hpp"

#include <psio1/fracpack.hpp>
#include <psio1/from_ssz.hpp>
#include <psio1/ssz_view.hpp>
#include <psio1/to_ssz.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifdef PSIO1_HAVE_SSZPP
#  include "container.hpp"
#  include "lists.hpp"
#  include "validator.hpp"
#  include "ssz++.hpp"
#endif

// ── Harness ──────────────────────────────────────────────────────────────────

static std::vector<char> load_file(const char* path)
{
   std::ifstream f(path, std::ios::binary);
   if (!f)
      return {};
   f.seekg(0, std::ios::end);
   auto sz = f.tellg();
   f.seekg(0);
   std::vector<char> out(static_cast<std::size_t>(sz));
   f.read(out.data(), sz);
   return out;
}

struct timing
{
   double min_ns{0}, median_ns{0}, max_ns{0};
};

template <typename F>
static timing run_timed(const char* label, F&& body, int trials = 9)
{
   int inner = 1;
   {
      auto t0 = std::chrono::steady_clock::now();
      body();
      auto t1 = std::chrono::steady_clock::now();
      double single_ns =
          std::chrono::duration<double, std::nano>(t1 - t0).count();
      if (single_ns < 1'000'000.0)
      {
         inner = static_cast<int>(50'000'000.0 / std::max(1.0, single_ns));
         if (inner < 1) inner = 1;
      }
   }

   std::vector<double> samples;
   samples.reserve(trials);
   for (int i = 0; i < trials; ++i)
   {
      auto t0 = std::chrono::steady_clock::now();
      for (int j = 0; j < inner; ++j)
         body();
      auto t1 = std::chrono::steady_clock::now();
      double total_ns =
          std::chrono::duration<double, std::nano>(t1 - t0).count();
      samples.push_back(total_ns / inner);
   }
   std::sort(samples.begin(), samples.end());
   timing t;
   t.min_ns    = samples.front();
   t.median_ns = samples[samples.size() / 2];
   t.max_ns    = samples.back();

   auto fmt = [](double ns) {
      char buf[40];
      if (ns < 1000.0)
         std::snprintf(buf, sizeof(buf), "%5.0f ns", ns);
      else if (ns < 1'000'000.0)
         std::snprintf(buf, sizeof(buf), "%6.2f µs", ns / 1000.0);
      else
         std::snprintf(buf, sizeof(buf), "%6.2f ms", ns / 1'000'000.0);
      return std::string(buf);
   };
   std::printf("    %-50s  min=%-10s median=%-10s max=%-10s  (n=%d)\n",
               label, fmt(t.min_ns).c_str(), fmt(t.median_ns).c_str(),
               fmt(t.max_ns).c_str(), inner);
   return t;
}

[[maybe_unused]] static void sink(const void* p)
{
   asm volatile("" : : "r"(p) : "memory");
}

// ── Workload 0a: fracpack vs SSZ on the full BeaconState ─────────────────────
//
// BeaconState has ~2.5 MiB of inline fixed fields (randao_mixes alone is
// 2 MiB). fracpack's extensible-struct u16 size header caps the fixed region
// at 64 KiB — but DWNC structs don't emit that header. Marking BeaconState
// with definitionWillNotChange() lifts the cap.

static void bench_fracpack_vs_ssz_state(const std::vector<char>& ssz_raw)
{
   // sizeof(BeaconState) is ~2.56 MiB — randao_mixes alone is 2 MiB inline.
   // Default-construct on the heap, then decode in-place. Using the
   // by-value convert_from_ssz / from_frac overloads would materialize a
   // 2.56 MiB temporary on the stack, blowing the default 8 MiB thread
   // stack the moment we have more than a couple of them live.
   auto state = std::make_unique<eth::BeaconState>();
   psio1::convert_from_ssz(*state, ssz_raw);

   auto frac_buf = psio1::to_frac(*state);

   std::printf("\n[0a] fracpack vs SSZ, full BeaconState:\n");
   std::printf("    SSZ size:      %zu bytes (%.2f MiB)\n", ssz_raw.size(),
               ssz_raw.size() / (1024.0 * 1024.0));
   std::printf("    fracpack size: %zu bytes (%.2f MiB)  (%+zd B vs SSZ)\n",
               frac_buf.size(), frac_buf.size() / (1024.0 * 1024.0),
               static_cast<std::ptrdiff_t>(frac_buf.size()) -
                   static_cast<std::ptrdiff_t>(ssz_raw.size()));

   std::printf("  fracpack:\n");
   run_timed("psio1::to_frac (alloc + encode)", [&] {
      auto r = psio1::to_frac(*state);
      asm volatile("" : : "r,m"(r.data()) : "memory");
   });
   run_timed("psio1::from_frac (full decode)", [&] {
      auto r = std::make_unique<eth::BeaconState>();
      psio1::from_frac<eth::BeaconState>(
          *r, std::span<const char>(frac_buf.data(), frac_buf.size()));
      asm volatile("" : : "r,m"(r.get()) : "memory");
   });
   run_timed("psio1::fracpack_size (size probe only)", [&] {
      std::uint32_t sz = psio1::fracpack_size(*state);
      asm volatile("" : : "r"(sz));
   });

   std::printf("  SSZ (same BeaconState):\n");
   run_timed("psio1::convert_to_ssz (alloc + encode)", [&] {
      std::vector<char> r;
      psio1::convert_to_ssz(*state, r);
      asm volatile("" : : "r,m"(r.data()) : "memory");
   });
   run_timed("psio1::convert_from_ssz (full decode)", [&] {
      auto r = std::make_unique<eth::BeaconState>();
      psio1::convert_from_ssz(*r, ssz_raw);
      asm volatile("" : : "r,m"(r.get()) : "memory");
   });
   run_timed("psio1::ssz_size (size probe only)", [&] {
      std::uint32_t sz = psio1::ssz_size(*state);
      asm volatile("" : : "r"(sz));
   });

   // Isolate pack-walk cost from allocation/zero-init:
   // reuse pre-allocated buffers, overwrite in place each iteration.
   std::printf("  pre-allocated buffer (isolates pack walk cost):\n");
   {
      std::vector<char> reuse_frac(frac_buf.size() + 16);
      std::vector<char> reuse_ssz(ssz_raw.size() + 16);

      run_timed("fracpack encode-only (no alloc, no zero-init)", [&] {
         psio1::fixed_buf_stream fbs(reuse_frac.data(), reuse_frac.size());
         psio1::to_frac<eth::BeaconState>(*state, fbs);
         asm volatile("" : : "r,m"(reuse_frac.data()) : "memory");
      });
      run_timed("ssz encode-only (no alloc, no zero-init)", [&] {
         psio1::fixed_buf_stream fbs(reuse_ssz.data(), reuse_ssz.size());
         psio1::to_ssz(*state, fbs);
         asm volatile("" : : "r,m"(reuse_ssz.data()) : "memory");
      });
   }
}

// ── Workload 0b: fracpack vs SSZ on a shared validator-list payload ─────────

static void bench_fracpack_vs_ssz_validators(std::size_t n)
{
   std::vector<eth::Validator> vs;
   vs.reserve(n);
   for (std::size_t i = 0; i < n; ++i)
   {
      eth::Validator v{};
      for (std::size_t b = 0; b < 48; ++b)
         v.pubkey[b] = static_cast<std::uint8_t>((i * 13 + b * 7) & 0xFF);
      for (std::size_t b = 0; b < 32; ++b)
         v.withdrawal_credentials[b] = static_cast<std::uint8_t>((i ^ (b * 11)) & 0xFF);
      v.effective_balance            = 32ULL * 1'000'000'000ULL;
      v.slashed                      = (i % 17) == 0;
      v.activation_eligibility_epoch = i;
      v.activation_epoch             = i + 100;
      v.exit_epoch                   = std::uint64_t{1} << 62;
      v.withdrawable_epoch           = std::uint64_t{1} << 62;
      vs.push_back(v);
   }

   auto ssz_buf  = psio1::convert_to_ssz(vs);
   auto frac_buf = psio1::to_frac(vs);

   std::printf("\n[0] fracpack vs SSZ, std::vector<Validator> (%zu validators)\n", n);
   std::printf("    SSZ size:      %zu bytes (%.2f MiB)\n", ssz_buf.size(),
               ssz_buf.size() / (1024.0 * 1024.0));
   std::printf("    fracpack size: %zu bytes (%.2f MiB)  (%+zd B vs SSZ)\n",
               frac_buf.size(), frac_buf.size() / (1024.0 * 1024.0),
               static_cast<std::ptrdiff_t>(frac_buf.size()) -
                   static_cast<std::ptrdiff_t>(ssz_buf.size()));

   std::printf("  fracpack:\n");
   run_timed("psio1::to_frac (full encode)", [&] {
      auto r = psio1::to_frac(vs);
      asm volatile("" : : "r,m"(r.data()) : "memory");
   });
   run_timed("psio1::from_frac (full decode)", [&] {
      auto r = psio1::from_frac<std::vector<eth::Validator>>(
          std::span<const char>(frac_buf.data(), frac_buf.size()));
      asm volatile("" : : "r,m"(&r) : "memory");
   });
   run_timed("psio1::fracpack_size (size probe only)", [&] {
      std::uint32_t sz = psio1::fracpack_size(vs);
      asm volatile("" : : "r"(sz));
   });

   std::printf("  SSZ:\n");
   run_timed("psio1::convert_to_ssz (full encode)", [&] {
      std::vector<char> r;
      psio1::convert_to_ssz(vs, r);
      asm volatile("" : : "r,m"(r.data()) : "memory");
   });
   run_timed("psio1::convert_from_ssz (full decode)", [&] {
      auto r = psio1::convert_from_ssz<std::vector<eth::Validator>>(ssz_buf);
      asm volatile("" : : "r,m"(&r) : "memory");
   });
   run_timed("psio1::ssz_size (size probe only)", [&] {
      std::uint32_t sz = psio1::ssz_size(vs);
      asm volatile("" : : "r"(sz));
   });
}

// ── Workload 1: psio Phase 0 BeaconState ─────────────────────────────────────

static void bench_psio_beacon_state(const std::vector<char>& raw)
{
   std::printf("\n[1] psio Phase 0 BeaconState (%.2f MiB)\n",
               raw.size() / (1024.0 * 1024.0));
   (void)psio1::convert_from_ssz<eth::BeaconState>(raw);  // warm

   auto t_val = run_timed("psio1::ssz_validate", [&] {
      psio1::ssz_validate<eth::BeaconState>(raw);
   });

   eth::BeaconState decoded;
   auto             t_dec = run_timed("psio1::convert_from_ssz", [&] {
      decoded = psio1::convert_from_ssz<eth::BeaconState>(raw);
      sink(&decoded);
   });

   std::vector<char> re_encoded;
   auto              t_enc = run_timed("psio1::convert_to_ssz", [&] {
      re_encoded.clear();
      psio1::convert_to_ssz(decoded, re_encoded);
      sink(re_encoded.data());
   });

   run_timed("psio ssz_view 1 field (validator[137].eff_bal)", [&] {
      auto v  = psio1::ssz_view_of<eth::BeaconState>(raw);
      auto eb = v.field<11>()[137].field<2>();
      sink(&eb);
   });

   if (re_encoded.size() != raw.size() ||
       std::memcmp(re_encoded.data(), raw.data(), raw.size()) != 0)
      std::fprintf(stderr, "ERROR: psio round-trip mismatch\n");
   else
      std::printf("    psio round-trip: byte-identical ✓\n");

   (void)t_val; (void)t_dec; (void)t_enc;
}

// ── Workload 2: shared Validator list (psio + optional sszpp) ───────────────

static std::vector<char> build_validator_list(std::size_t n)
{
   std::vector<eth::Validator> vs;
   vs.reserve(n);
   for (std::size_t i = 0; i < n; ++i)
   {
      eth::Validator v{};
      for (std::size_t b = 0; b < 48; ++b)
         v.pubkey[b] = static_cast<std::uint8_t>((i * 13 + b * 7) & 0xFF);
      for (std::size_t b = 0; b < 32; ++b)
         v.withdrawal_credentials[b] = static_cast<std::uint8_t>((i ^ (b * 11)) & 0xFF);
      v.effective_balance            = 32ULL * 1'000'000'000ULL;
      v.slashed                      = (i % 17) == 0;
      v.activation_eligibility_epoch = i;
      v.activation_epoch             = i + 100;
      v.exit_epoch                   = std::uint64_t{1} << 62;
      v.withdrawable_epoch           = std::uint64_t{1} << 62;
      vs.push_back(v);
   }
   return psio1::convert_to_ssz(vs);
}

static void bench_validator_list(std::size_t n)
{
   auto raw = build_validator_list(n);
   std::printf("\n[2] Validator list: %zu validators, %.2f MiB\n",
               n, raw.size() / (1024.0 * 1024.0));

   // ── psio ──
   std::printf("  psio:\n");
   (void)psio1::convert_from_ssz<std::vector<eth::Validator>>(raw);

   run_timed("psio1::ssz_validate", [&] {
      psio1::ssz_validate<std::vector<eth::Validator>>(raw);
   });

   std::vector<eth::Validator> pd;
   run_timed("psio1::convert_from_ssz", [&] {
      pd = psio1::convert_from_ssz<std::vector<eth::Validator>>(raw);
      sink(pd.data());
   });

   run_timed("psio1::convert_to_ssz", [&] {
      auto out = psio1::convert_to_ssz(pd);
      sink(out.data());
   });

#ifdef PSIO1_HAVE_SSZPP
   std::printf("  sszpp (OffchainLabs):\n");

   // sszpp uses std::vector<std::byte>.
   std::vector<std::byte> raw_bytes(raw.size());
   std::memcpy(raw_bytes.data(), raw.data(), raw.size());

   using SszppValidatorList = ssz::list<ssz::validator_t, eth::VALIDATOR_REGISTRY_LIMIT>;

   // Warm.
   (void)ssz::deserialize<SszppValidatorList>(raw_bytes);

   run_timed("ssz::deserialize (full decode)", [&] {
      auto v = ssz::deserialize<SszppValidatorList>(raw_bytes);
      sink(&v);
   });

   auto sd = ssz::deserialize<SszppValidatorList>(raw_bytes);

   run_timed("ssz::serialize (full encode)", [&] {
      auto out = ssz::serialize(sd);
      sink(out.data());
   });

   // Correctness: sszpp should re-encode byte-identically.
   auto re = ssz::serialize(sd);
   if (re.size() != raw.size() ||
       std::memcmp(re.data(), raw.data(), raw.size()) != 0)
      std::fprintf(stderr, "ERROR: sszpp round-trip mismatch\n");
   else
      std::printf("    sszpp round-trip: byte-identical ✓\n");
#else
   std::printf("  sszpp: not enabled (see bench_ssz_beacon.cpp header for setup)\n");
#endif
}

// Compile-time layout sanity
static_assert(sizeof(eth::Validator) == 121,
              "Validator must be packed to 121 bytes; check __attribute__((packed))");
static_assert(psio1::ssz_fixed_size<eth::Validator>::value == 121);
static_assert(psio1::layout_detail::is_memcpy_layout_struct<eth::Validator>(),
              "Validator must qualify for the SSZ memcpy fast path");
static_assert(psio1::ssz_memcpy_ok_v<eth::Validator>,
              "ssz_memcpy_ok_v<Validator> should be true after packing");

// Diagnostic: are fracpack's fast paths firing for these types?
static constexpr bool k_has_batch_run_validator =
    psio1::run_detail::has_batchable_run<eth::Validator>();
static constexpr bool k_has_batch_run_state =
    psio1::run_detail::has_batchable_run<eth::BeaconState>();
static constexpr bool k_validator_bitwise =
    psio1::has_bitwise_serialization<eth::Validator>();

int main(int argc, char** argv)
{
   const char* path = (argc > 1) ? argv[1] : "/tmp/beacon_data/mainnet-genesis.ssz";
   auto        raw  = load_file(path);

   std::printf("=== SSZ head-to-head bench ===\n");
   std::printf("Host: %s\n", argv[0]);
   std::printf("sizeof(eth::Validator) = %zu (wire-size-matched: %s)\n",
               sizeof(eth::Validator),
               sizeof(eth::Validator) == 121 ? "✓" : "✗");
   std::printf("ssz_memcpy_ok_v<eth::Validator> = %s\n",
               psio1::ssz_memcpy_ok_v<eth::Validator> ? "true" : "false");
   std::printf("fracpack diagnostics:\n");
   std::printf("  has_bitwise_serialization<Validator>   = %s\n",
               k_validator_bitwise ? "true" : "false");
   std::printf("  has_batchable_run<Validator>           = %s\n",
               k_has_batch_run_validator ? "true" : "false");
   std::printf("  has_batchable_run<BeaconState>         = %s\n",
               k_has_batch_run_state ? "true" : "false");
#ifdef PSIO1_HAVE_SSZPP
   std::printf("sszpp: enabled\n");
#else
   std::printf("sszpp: disabled\n");
#endif

   if (!raw.empty())
   {
      bench_fracpack_vs_ssz_state(raw);
   }
   bench_fracpack_vs_ssz_validators(21063);

   if (!raw.empty())
   {
      bench_psio_beacon_state(raw);
   }

   // Head-to-head on REAL mainnet validator list extracted from the current
   // Fulu state. sszpp's beacon_state_t is Capella-shaped so can't decode
   // the full Fulu state, but its validator_t matches spec — both libraries
   // can decode the same validator-list bytes.
   {
      const char* fulu_path = "/tmp/beacon_data/mainnet-state-current.ssz";
      auto        fulu_raw  = load_file(fulu_path);
      if (!fulu_raw.empty())
      {
         std::printf("\n[3] HEAD-TO-HEAD on REAL mainnet validator list\n");
         auto fulu_state = std::make_unique<eth::fulu::BeaconState>();
         psio1::convert_from_ssz(*fulu_state, fulu_raw);

         std::size_t n = fulu_state->validators.size();
         std::printf("     (extracted from Fulu state: %zu validators)\n", n);

         std::vector<eth::Validator> vs;
         vs.reserve(n);
         for (std::size_t i = 0; i < n; ++i)
            vs.push_back(fulu_state->validators[i]);

         auto validator_bytes = psio1::convert_to_ssz(vs);
         std::printf("     validator list: %zu bytes (%.2f MiB)\n",
                     validator_bytes.size(),
                     validator_bytes.size() / (1024.0 * 1024.0));

         std::printf("     psio fast path: ssz_memcpy_ok_v<Validator> = %s, "
                     "sizeof(Validator) = %zu\n",
                     psio1::ssz_memcpy_ok_v<eth::Validator> ? "true" : "FALSE",
                     sizeof(eth::Validator));

         // Single-memcpy baseline — floor for any decoder on this hardware.
         std::size_t nv       = validator_bytes.size() / sizeof(eth::Validator);
         std::size_t byte_len = nv * sizeof(eth::Validator);
         run_timed("malloc + memcpy + free (floor)", [&] {
            void* p = std::malloc(byte_len);
            std::memcpy(p, validator_bytes.data(), byte_len);
            asm volatile("" : : "r,m"(p) : "memory");
            std::free(p);
         });

         std::printf("  psio:\n");
         run_timed("psio1::convert_from_ssz", [&] {
            std::vector<eth::Validator> decoded;
            psio1::convert_from_ssz(decoded, validator_bytes);
            asm volatile("" : : "r,m"(decoded.data()) : "memory");
         });
         run_timed("psio1::convert_to_ssz", [&] {
            auto r = psio1::convert_to_ssz(vs);
            asm volatile("" : : "r,m"(r.data()) : "memory");
         });

#ifdef PSIO1_HAVE_SSZPP
         std::printf("  sszpp (OffchainLabs):\n");
         std::printf("     sizeof(ssz::validator_t) = %zu "
                     "(psio1::eth::Validator is %zu packed)\n",
                     sizeof(ssz::validator_t), sizeof(eth::Validator));
         std::vector<std::byte> raw_bytes(validator_bytes.size());
         std::memcpy(raw_bytes.data(), validator_bytes.data(),
                     validator_bytes.size());
         using SszppValidatorList =
             ssz::list<ssz::validator_t, eth::VALIDATOR_REGISTRY_LIMIT>;
         (void)ssz::deserialize<SszppValidatorList>(raw_bytes);  // warm

         run_timed("ssz::deserialize", [&] {
            auto d = ssz::deserialize<SszppValidatorList>(raw_bytes);
            asm volatile("" : : "r,m"(&d) : "memory");
         });
         auto sd = ssz::deserialize<SszppValidatorList>(raw_bytes);
         run_timed("ssz::serialize", [&] {
            auto out = ssz::serialize(sd);
            asm volatile("" : : "r,m"(out.data()) : "memory");
         });
#else
         std::printf("  sszpp: not enabled (build with -DPSIO_BENCH_SSZPP=ON)\n");
#endif
      }
   }

   bench_validator_list(21063);  // mainnet genesis count

   return 0;
}
