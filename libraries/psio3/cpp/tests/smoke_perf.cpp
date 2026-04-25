// Smoke performance comparison: v1 psio::ssz vs psio3::ssz on identical
// inputs. Not a rigorous benchmark — just a regression guard that would
// catch an order-of-magnitude slowdown on the basic codec path.
//
// Build manually:
//   clang++ -O3 -std=gnu++23 -I libraries/psio/cpp/include
//           -I libraries/psio3/cpp/include
//           libraries/psio3/cpp/tests/smoke_perf.cpp
//           -o smoke_perf
//
// (Linked by CMake target psio3_smoke_perf for convenience.)

#include <psio/fracpack.hpp>
#include <psio/from_ssz.hpp>
#include <psio/to_pssz.hpp>
#include <psio/to_ssz.hpp>
#include <psio/from_pssz.hpp>
#include <psio/from_bin.hpp>
#include <psio/to_bin.hpp>
#include <psio/from_borsh.hpp>
#include <psio/to_borsh.hpp>
#include <psio/from_bincode.hpp>
#include <psio/to_bincode.hpp>
#include <psio/from_avro.hpp>
#include <psio/to_avro.hpp>
#include <psio/flatbuf.hpp>
#include <psio/capnp_view.hpp>
#include <psio/wit_view.hpp>
#include <psio/reflect.hpp>

#include <psio3/bin.hpp>
#include <psio3/borsh.hpp>
#include <psio3/bincode.hpp>
#include <psio3/avro.hpp>
#include <psio3/flatbuf.hpp>
#include <psio3/capnp.hpp>
#include <psio3/wit.hpp>
#include <psio3/frac.hpp>
#include <psio3/pssz.hpp>
#include <psio3/reflect.hpp>
#include <psio3/ssz.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace v1 {
   struct Header
   {
      std::uint64_t                slot;
      std::uint64_t                proposer;
      std::vector<std::uint8_t>    state_root;
      std::vector<std::uint64_t>   transactions;
      std::string                  graffiti;
   };
   PSIO_REFLECT(Header, slot, proposer, state_root, transactions, graffiti)
}
namespace v1 {
   struct Validator
   {
      std::uint64_t pubkey_lo;
      std::uint64_t pubkey_hi;
      std::uint64_t effective_balance;
      std::uint64_t activation_epoch;
      std::uint64_t exit_epoch;
      bool          slashed;
   };
   PSIO_REFLECT(Validator, pubkey_lo, pubkey_hi, effective_balance,
                activation_epoch, exit_epoch, slashed)

   struct BeaconState
   {
      std::uint64_t          slot;
      std::vector<Validator> validators;
   };
   PSIO_REFLECT(BeaconState, slot, validators)
}
namespace v3 {
   struct Header
   {
      std::uint64_t                slot;
      std::uint64_t                proposer;
      std::vector<std::uint8_t>    state_root;
      std::vector<std::uint64_t>   transactions;
      std::string                  graffiti;
   };
   PSIO3_REFLECT(Header, slot, proposer, state_root, transactions, graffiti)

   // Fully-fixed leaf — every packsize of this is a compile-time
   // constant, so `vector<Validator>` packsize is `count * constant`
   // with zero per-element traversal.
   struct Validator
   {
      std::uint64_t pubkey_lo;
      std::uint64_t pubkey_hi;
      std::uint64_t effective_balance;
      std::uint64_t activation_epoch;
      std::uint64_t exit_epoch;
      bool          slashed;
   };
   PSIO3_REFLECT(Validator, pubkey_lo, pubkey_hi, effective_balance,
                 activation_epoch, exit_epoch, slashed)

   // BeaconState-shaped nested record — vector<Validator> exercises
   // the fixed-element vector fast path.
   struct BeaconState
   {
      std::uint64_t          slot;
      std::vector<Validator> validators;
   };
   PSIO3_REFLECT(BeaconState, slot, validators)
}
// PSIO3_FIELD_ATTRS must live outside the enclosing namespace — it
// specializes `psio3::annotate<X>` and needs to be visible as such.
PSIO3_FIELD_ATTRS(v3::Header, state_root,   psio3::length_bound{.max = 32})
PSIO3_FIELD_ATTRS(v3::Header, transactions, psio3::length_bound{.max = 64})
PSIO3_FIELD_ATTRS(v3::Header, graffiti,     psio3::length_bound{.max = 32})

template <typename F>
double ns_per_iter(std::size_t iters, F&& f)
{
   auto t0 = std::chrono::steady_clock::now();
   for (std::size_t i = 0; i < iters; ++i)
      f(i);
   auto t1 = std::chrono::steady_clock::now();
   auto dt =
      std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
   return static_cast<double>(dt) / static_cast<double>(iters);
}

int main()
{
   constexpr std::size_t ITERS = 200'000;

   // Report v3 pssz width auto-selection for this Header — proves the
   // length_bound annotations drove the narrower wire form.
   constexpr auto v3_max =
      ::psio3::max_encoded_size<v3::Header>();
   constexpr std::size_t v3_W = ::psio3::auto_pssz_width_v<v3::Header>;
   if constexpr (v3_max.has_value())
      std::printf("v3::Header max_encoded_size = %zu bytes → pssz W = %zu\n\n",
                  *v3_max, v3_W);
   else
      std::printf("v3::Header max_encoded_size = unbounded → pssz W = %zu\n\n",
                  v3_W);

   // Sample values.
   v1::Header h1{};
   v3::Header h3{};
   h1.slot = h3.slot = 12345;
   h1.proposer = h3.proposer = 7;
   h1.state_root.resize(32);
   h3.state_root.resize(32);
   for (std::size_t i = 0; i < 32; ++i)
      h1.state_root[i] = h3.state_root[i] = static_cast<std::uint8_t>(i);
   for (std::size_t i = 0; i < 8; ++i)
   {
      h1.transactions.push_back(i * 100);
      h3.transactions.push_back(i * 100);
   }
   h1.graffiti = h3.graffiti = "validator-comments";

   // ── Encode-only timing ────────────────────────────────────────────
   double enc_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_ssz(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::ssz{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });

   // ── Decode-only timing ────────────────────────────────────────────
   auto bytes_v1 = psio::convert_to_ssz(h1);
   auto bytes_v3 = psio3::encode(psio3::ssz{}, h3);

   double dec_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio::convert_from_ssz<v1::Header>(bytes_v1);
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::ssz{},
                                          std::span<const char>{bytes_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });

   // ── Primitive uint32 (tight loop) ─────────────────────────────────
   std::uint32_t x = 0xDEADBEEF;
   double enc_u32_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_ssz(x);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_u32_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::ssz{}, x);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });

   // ── Vector<u32> of 512 elements ────────────────────────────────────
   std::vector<std::uint32_t> vec(512);
   for (std::size_t i = 0; i < 512; ++i)
      vec[i] = static_cast<std::uint32_t>(i * 37);
   double enc_vec_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_ssz(vec);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_vec_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::ssz{}, vec);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });

   auto ratio = [](double v3, double v1) { return v3 / v1; };

   std::printf("workload               | v1 ns/op | v3 ns/op | v3/v1\n");
   std::printf("-----------------------+----------+----------+------\n");
   std::printf("[ssz] enc uint32       | %8.1f | %8.1f | %.2fx\n",
               enc_u32_v1, enc_u32_v3, ratio(enc_u32_v3, enc_u32_v1));
   std::printf("[ssz] enc vec<u32>[512]| %8.1f | %8.1f | %.2fx\n",
               enc_vec_v1, enc_vec_v3, ratio(enc_vec_v3, enc_vec_v1));
   std::printf("[ssz] enc Header       | %8.1f | %8.1f | %.2fx\n",
               enc_v1, enc_v3, ratio(enc_v3, enc_v1));
   std::printf("[ssz] dec Header       | %8.1f | %8.1f | %.2fx\n",
               dec_v1, dec_v3, ratio(dec_v3, dec_v1));
   double val_ssz_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      psio::ssz_validate<v1::Header>(std::span<const char>{bytes_v1});
      asm volatile("" : : : "memory");
   });
   double val_ssz_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::ssz{},
                                             std::span<const char>{bytes_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_ssz_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio::ssz_size(h1);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   double size_ssz_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::ssz{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[ssz] val Header       | %8.1f | %8.1f | %.2fx\n",
               val_ssz_v1, val_ssz_v3, ratio(val_ssz_v3, val_ssz_v1));
   std::printf("[ssz] size Header      | %8.1f | %8.1f | %.2fx\n",
               size_ssz_v1, size_ssz_v3, ratio(size_ssz_v3, size_ssz_v1));

   // ── pSSZ Header (v3 auto-picks W; v1 uses pssz32) ─────────────────
   double enc_pssz_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_pssz<psio::frac_format_pssz32>(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_pssz_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::pssz{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_pssz_v1 = psio::convert_to_pssz<psio::frac_format_pssz32>(h1);
   auto bytes_pssz_v3 = psio3::encode(psio3::pssz{}, h3);
   double dec_pssz_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      v1::Header v;
      psio::convert_from_pssz<psio::frac_format_pssz32, v1::Header>(
         v, bytes_pssz_v1);
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_pssz_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::pssz{},
                                          std::span<const char>{bytes_pssz_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[pssz]   enc Header    | %8.1f | %8.1f | %.2fx\n",
               enc_pssz_v1, enc_pssz_v3,
               ratio(enc_pssz_v3, enc_pssz_v1));
   std::printf("[pssz]   dec Header    | %8.1f | %8.1f | %.2fx\n",
               dec_pssz_v1, dec_pssz_v3,
               ratio(dec_pssz_v3, dec_pssz_v1));
   // v1 has no free pssz_validate function — v3 only.
   double val_pssz_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::pssz{},
                                             std::span<const char>{bytes_pssz_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_pssz_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio::pssz_size<psio::frac_format_pssz32>(h1);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   double size_pssz_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::pssz{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[pssz]   val Header    | %8s | %8.1f |  —  \n",
               "n/a", val_pssz_v3);
   std::printf("[pssz]   size Header   | %8.1f | %8.1f | %.2fx\n",
               size_pssz_v1, size_pssz_v3,
               ratio(size_pssz_v3, size_pssz_v1));

   // ── fracpack Header ────────────────────────────────────────────────
   double enc_frac_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_frac(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_frac_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::frac32{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_frac_v1 = psio::convert_to_frac(h1);
   auto bytes_frac_v3 = psio3::encode(psio3::frac32{}, h3);
   double dec_frac_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      v1::Header v;
      auto ok = psio::from_frac<v1::Header>(v, bytes_frac_v1);
      (void)ok;
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_frac_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::frac32{},
                                          std::span<const char>{bytes_frac_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[frac32] enc Header    | %8.1f | %8.1f | %.2fx\n",
               enc_frac_v1, enc_frac_v3,
               ratio(enc_frac_v3, enc_frac_v1));
   std::printf("[frac32] dec Header    | %8.1f | %8.1f | %.2fx\n",
               dec_frac_v1, dec_frac_v3,
               ratio(dec_frac_v3, dec_frac_v1));
   // v1 has no free fracpack_validate; v1 from_frac returns a bool but
   // also performs decode work — not a validate-only path.
   double val_frac_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::frac32{},
                                             std::span<const char>{bytes_frac_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_frac_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio::fracpack_size(h1);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   double size_frac_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::frac32{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[frac32] val Header    | %8s | %8.1f |  —  \n",
               "n/a", val_frac_v3);
   std::printf("[frac32] size Header   | %8.1f | %8.1f | %.2fx\n",
               size_frac_v1, size_frac_v3,
               ratio(size_frac_v3, size_frac_v1));

   // ── bin Header ─────────────────────────────────────────────────────
   double enc_bin_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_bin(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_bin_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::bin{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_bin_v1 = psio::convert_to_bin(h1);
   auto bytes_bin_v3 = psio3::encode(psio3::bin{}, h3);
   double dec_bin_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio::convert_from_bin<v1::Header>(bytes_bin_v1);
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_bin_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::bin{},
                                          std::span<const char>{bytes_bin_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[bin] enc Header       | %8.1f | %8.1f | %.2fx\n",
               enc_bin_v1, enc_bin_v3, ratio(enc_bin_v3, enc_bin_v1));
   std::printf("[bin] dec Header       | %8.1f | %8.1f | %.2fx\n",
               dec_bin_v1, dec_bin_v3, ratio(dec_bin_v3, dec_bin_v1));

   // v1 has no free bin_validate; v3 only.
   double val_bin_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::bin{},
                                             std::span<const char>{bytes_bin_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   // v1 size walk uses a thread-local cache; v3 has the same shape via size_of.
   double size_bin_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      static thread_local psio::bin_detail::bin_size_cache cache;
      cache.slots.clear();
      cache.consumed = 0;
      auto n = psio::compute_bin_size(h1, cache);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   double size_bin_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::bin{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[bin] val Header       | %8s | %8.1f |  —  \n",
               "n/a", val_bin_v3);
   std::printf("[bin] size Header      | %8.1f | %8.1f | %.2fx\n",
               size_bin_v1, size_bin_v3, ratio(size_bin_v3, size_bin_v1));

   // ── Nested workload: BeaconState with 256 Validators ────────────────
   //
   // Validator is fully-fixed — every field a primitive. The
   // fixed/variable split should compile packsize(state) down to
   // `const + state.validators.size() * const`. No per-Validator walk.
   v1::BeaconState state1{};
   v3::BeaconState state3{};
   state1.slot = state3.slot = 42;
   state1.validators.resize(256);
   state3.validators.resize(256);
   for (std::size_t i = 0; i < 256; ++i)
   {
      state1.validators[i].pubkey_lo = state3.validators[i].pubkey_lo = i * 7;
      state1.validators[i].pubkey_hi = state3.validators[i].pubkey_hi = i * 11;
      state1.validators[i].effective_balance =
         state3.validators[i].effective_balance = 32'000'000'000ull;
      state1.validators[i].activation_epoch =
         state3.validators[i].activation_epoch = 100;
      state1.validators[i].exit_epoch =
         state3.validators[i].exit_epoch = 0xFFFFFFFFull;
      state1.validators[i].slashed = state3.validators[i].slashed =
         (i % 50) == 0;
   }

   std::printf("\n-- BeaconState (256 validators, fully-fixed Validator) --\n");

   // Pre-compute encoded buffers for decode/validate.
   auto bs_ssz_v1     = psio::convert_to_ssz(state1);
   auto bs_ssz_v3     = psio3::encode(psio3::ssz{}, state3);
   auto bs_pssz_v1    = psio::convert_to_pssz<psio::frac_format_pssz32>(state1);
   auto bs_pssz_v3    = psio3::encode(psio3::pssz{}, state3);
   auto bs_frac_v1    = psio::convert_to_frac(state1);
   auto bs_frac_v3    = psio3::encode(psio3::frac32{}, state3);
   auto bs_bin_v1     = psio::convert_to_bin(state1);
   auto bs_bin_v3     = psio3::encode(psio3::bin{}, state3);
   auto bs_borsh_v1   = psio::convert_to_borsh(state1);
   auto bs_borsh_v3   = psio3::encode(psio3::borsh{}, state3);
   auto bs_bincode_v1 = psio::convert_to_bincode(state1);
   auto bs_bincode_v3 = psio3::encode(psio3::bincode{}, state3);

   // ssz: v1 has size + validate
   {
      double v1_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio::convert_from_ssz<v1::BeaconState>(bs_ssz_v1);
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio3::decode<v3::BeaconState>(psio3::ssz{},
                                                   std::span<const char>{bs_ssz_v3});
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v1_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio::convert_to_ssz(state1);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v3_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio3::encode(psio3::ssz{}, state3);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v1_val = ns_per_iter(ITERS, [&](std::size_t) {
         psio::ssz_validate<v1::BeaconState>(std::span<const char>{bs_ssz_v1});
      });
      double v3_val = ns_per_iter(ITERS, [&](std::size_t) {
         auto st = psio3::validate<v3::BeaconState>(psio3::ssz{},
                                                     std::span<const char>{bs_ssz_v3});
         asm volatile("" : : "r,m"(st) : "memory");
      });
      double v1_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio::ssz_size(state1);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      double v3_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio3::size_of(psio3::ssz{}, state3);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      std::printf("[ssz]     enc BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_enc, v3_enc, ratio(v3_enc, v1_enc));
      std::printf("[ssz]     dec BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_dec, v3_dec, ratio(v3_dec, v1_dec));
      std::printf("[ssz]     val BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_val, v3_val, ratio(v3_val, v1_val));
      std::printf("[ssz]     size BS    | %8.1f | %8.1f | %.2fx\n",
                  v1_sz, v3_sz, ratio(v3_sz, v1_sz));
   }

   // pssz: v1 has size only
   {
      double v1_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio::convert_to_pssz<psio::frac_format_pssz32>(state1);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v3_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio3::encode(psio3::pssz{}, state3);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v1_dec = ns_per_iter(ITERS, [&](std::size_t) {
         v1::BeaconState v;
         psio::convert_from_pssz<psio::frac_format_pssz32, v1::BeaconState>(
            v, bs_pssz_v1);
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio3::decode<v3::BeaconState>(psio3::pssz{},
                                                   std::span<const char>{bs_pssz_v3});
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_val = ns_per_iter(ITERS, [&](std::size_t) {
         auto st = psio3::validate<v3::BeaconState>(psio3::pssz{},
                                                     std::span<const char>{bs_pssz_v3});
         asm volatile("" : : "r,m"(st) : "memory");
      });
      double v1_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio::pssz_size<psio::frac_format_pssz32>(state1);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      double v3_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio3::size_of(psio3::pssz{}, state3);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      std::printf("[pssz]    enc BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_enc, v3_enc, ratio(v3_enc, v1_enc));
      std::printf("[pssz]    dec BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_dec, v3_dec, ratio(v3_dec, v1_dec));
      std::printf("[pssz]    val BS     | %8s | %8.1f |  —  \n",
                  "n/a", v3_val);
      std::printf("[pssz]    size BS    | %8.1f | %8.1f | %.2fx\n",
                  v1_sz, v3_sz, ratio(v3_sz, v1_sz));
   }

   // frac32: v1 has size only
   {
      double v1_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio::convert_to_frac(state1);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v3_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio3::encode(psio3::frac32{}, state3);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v1_dec = ns_per_iter(ITERS, [&](std::size_t) {
         v1::BeaconState v;
         auto ok = psio::from_frac<v1::BeaconState>(v, bs_frac_v1);
         (void)ok;
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio3::decode<v3::BeaconState>(psio3::frac32{},
                                                   std::span<const char>{bs_frac_v3});
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_val = ns_per_iter(ITERS, [&](std::size_t) {
         auto st = psio3::validate<v3::BeaconState>(psio3::frac32{},
                                                     std::span<const char>{bs_frac_v3});
         asm volatile("" : : "r,m"(st) : "memory");
      });
      double v1_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio::fracpack_size(state1);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      double v3_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio3::size_of(psio3::frac32{}, state3);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      std::printf("[frac32]  enc BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_enc, v3_enc, ratio(v3_enc, v1_enc));
      std::printf("[frac32]  dec BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_dec, v3_dec, ratio(v3_dec, v1_dec));
      std::printf("[frac32]  val BS     | %8s | %8.1f |  —  \n",
                  "n/a", v3_val);
      std::printf("[frac32]  size BS    | %8.1f | %8.1f | %.2fx\n",
                  v1_sz, v3_sz, ratio(v3_sz, v1_sz));
   }

   // bin: v1 has size only
   {
      double v1_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio::convert_to_bin(state1);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v3_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio3::encode(psio3::bin{}, state3);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v1_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio::convert_from_bin<v1::BeaconState>(bs_bin_v1);
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio3::decode<v3::BeaconState>(psio3::bin{},
                                                   std::span<const char>{bs_bin_v3});
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_val = ns_per_iter(ITERS, [&](std::size_t) {
         auto st = psio3::validate<v3::BeaconState>(psio3::bin{},
                                                     std::span<const char>{bs_bin_v3});
         asm volatile("" : : "r,m"(st) : "memory");
      });
      double v1_sz = ns_per_iter(ITERS, [&](std::size_t) {
         static thread_local psio::bin_detail::bin_size_cache cache;
         cache.slots.clear();
         cache.consumed = 0;
         auto n = psio::compute_bin_size(state1, cache);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      double v3_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio3::size_of(psio3::bin{}, state3);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      std::printf("[bin]     enc BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_enc, v3_enc, ratio(v3_enc, v1_enc));
      std::printf("[bin]     dec BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_dec, v3_dec, ratio(v3_dec, v1_dec));
      std::printf("[bin]     val BS     | %8s | %8.1f |  —  \n",
                  "n/a", v3_val);
      std::printf("[bin]     size BS    | %8.1f | %8.1f | %.2fx\n",
                  v1_sz, v3_sz, ratio(v3_sz, v1_sz));
   }

   // borsh: v3-only validate + size
   {
      double v1_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio::convert_to_borsh(state1);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v3_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio3::encode(psio3::borsh{}, state3);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v1_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio::convert_from_borsh<v1::BeaconState>(bs_borsh_v1);
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio3::decode<v3::BeaconState>(psio3::borsh{},
                                                   std::span<const char>{bs_borsh_v3});
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_val = ns_per_iter(ITERS, [&](std::size_t) {
         auto st = psio3::validate<v3::BeaconState>(psio3::borsh{},
                                                     std::span<const char>{bs_borsh_v3});
         asm volatile("" : : "r,m"(st) : "memory");
      });
      double v3_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio3::size_of(psio3::borsh{}, state3);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      std::printf("[borsh]   enc BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_enc, v3_enc, ratio(v3_enc, v1_enc));
      std::printf("[borsh]   dec BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_dec, v3_dec, ratio(v3_dec, v1_dec));
      std::printf("[borsh]   val BS     | %8s | %8.1f |  —  \n",
                  "n/a", v3_val);
      std::printf("[borsh]   size BS    | %8s | %8.1f |  —  \n",
                  "n/a", v3_sz);
   }

   // bincode: v3-only validate + size
   {
      double v1_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio::convert_to_bincode(state1);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v3_enc = ns_per_iter(ITERS, [&](std::size_t) {
         auto out = psio3::encode(psio3::bincode{}, state3);
         asm volatile("" : : "r,m"(out.data()) : "memory");
      });
      double v1_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio::convert_from_bincode<v1::BeaconState>(bs_bincode_v1);
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_dec = ns_per_iter(ITERS, [&](std::size_t) {
         auto v = psio3::decode<v3::BeaconState>(psio3::bincode{},
                                                   std::span<const char>{bs_bincode_v3});
         asm volatile("" : : "r,m"(v.slot) : "memory");
      });
      double v3_val = ns_per_iter(ITERS, [&](std::size_t) {
         auto st = psio3::validate<v3::BeaconState>(psio3::bincode{},
                                                     std::span<const char>{bs_bincode_v3});
         asm volatile("" : : "r,m"(st) : "memory");
      });
      double v3_sz = ns_per_iter(ITERS, [&](std::size_t) {
         auto n = psio3::size_of(psio3::bincode{}, state3);
         asm volatile("" : : "r,m"(n) : "memory");
      });
      std::printf("[bincode] enc BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_enc, v3_enc, ratio(v3_enc, v1_enc));
      std::printf("[bincode] dec BS     | %8.1f | %8.1f | %.2fx\n",
                  v1_dec, v3_dec, ratio(v3_dec, v1_dec));
      std::printf("[bincode] val BS     | %8s | %8.1f |  —  \n",
                  "n/a", v3_val);
      std::printf("[bincode] size BS    | %8s | %8.1f |  —  \n",
                  "n/a", v3_sz);
   }

   // ── borsh Header ───────────────────────────────────────────────────
   double enc_borsh_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_borsh(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_borsh_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::borsh{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_borsh_v1 = psio::convert_to_borsh(h1);
   auto bytes_borsh_v3 = psio3::encode(psio3::borsh{}, h3);
   double dec_borsh_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio::convert_from_borsh<v1::Header>(bytes_borsh_v1);
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_borsh_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::borsh{},
                                          std::span<const char>{bytes_borsh_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[borsh] enc Header     | %8.1f | %8.1f | %.2fx\n",
               enc_borsh_v1, enc_borsh_v3,
               ratio(enc_borsh_v3, enc_borsh_v1));
   std::printf("[borsh] dec Header     | %8.1f | %8.1f | %.2fx\n",
               dec_borsh_v1, dec_borsh_v3,
               ratio(dec_borsh_v3, dec_borsh_v1));
   double val_borsh_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::borsh{},
                                             std::span<const char>{bytes_borsh_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_borsh_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::borsh{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[borsh] val Header     | %8s | %8.1f |  —  \n",
               "n/a", val_borsh_v3);
   std::printf("[borsh] size Header    | %8s | %8.1f |  —  \n",
               "n/a", size_borsh_v3);

   // ── bincode Header ─────────────────────────────────────────────────
   double enc_bc_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_bincode(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_bc_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::bincode{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_bc_v1 = psio::convert_to_bincode(h1);
   auto bytes_bc_v3 = psio3::encode(psio3::bincode{}, h3);
   double dec_bc_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio::convert_from_bincode<v1::Header>(bytes_bc_v1);
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_bc_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::bincode{},
                                          std::span<const char>{bytes_bc_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[bincode] enc Header   | %8.1f | %8.1f | %.2fx\n",
               enc_bc_v1, enc_bc_v3, ratio(enc_bc_v3, enc_bc_v1));
   std::printf("[bincode] dec Header   | %8.1f | %8.1f | %.2fx\n",
               dec_bc_v1, dec_bc_v3, ratio(dec_bc_v3, dec_bc_v1));
   double val_bc_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::bincode{},
                                             std::span<const char>{bytes_bc_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_bc_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::bincode{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[bincode] val Header   | %8s | %8.1f |  —  \n",
               "n/a", val_bc_v3);
   std::printf("[bincode] size Header  | %8s | %8.1f |  —  \n",
               "n/a", size_bc_v3);

   // ── avro Header ────────────────────────────────────────────────────
   double enc_avro_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::convert_to_avro(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_avro_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::avro{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_avro_v1 = psio::convert_to_avro(h1);
   auto bytes_avro_v3 = psio3::encode(psio3::avro{}, h3);
   double dec_avro_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio::convert_from_avro<v1::Header>(bytes_avro_v1);
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_avro_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::avro{},
                                          std::span<const char>{bytes_avro_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[avro] enc Header      | %8.1f | %8.1f | %.2fx\n",
               enc_avro_v1, enc_avro_v3,
               ratio(enc_avro_v3, enc_avro_v1));
   std::printf("[avro] dec Header      | %8.1f | %8.1f | %.2fx\n",
               dec_avro_v1, dec_avro_v3,
               ratio(dec_avro_v3, dec_avro_v1));
   double val_avro_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::avro{},
                                             std::span<const char>{bytes_avro_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_avro_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::avro{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[avro] val Header      | %8s | %8.1f |  —  \n",
               "n/a", val_avro_v3);
   std::printf("[avro] size Header     | %8s | %8.1f |  —  \n",
               "n/a", size_avro_v3);

   // ── flatbuf native Header (v3 only — v1 uses different API shape) ─
   double enc_fb_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::flatbuf{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_fb_v3 = psio3::encode(psio3::flatbuf{}, h3);
   double dec_fb_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::flatbuf{},
                                          std::span<const char>{bytes_fb_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[flatbuf] enc Header   |    (n/a) | %8.1f |  —  \n",
               enc_fb_v3);
   std::printf("[flatbuf] dec Header   |    (n/a) | %8.1f |  —  \n",
               dec_fb_v3);
   double val_fb_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::flatbuf{},
                                             std::span<const char>{bytes_fb_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_fb_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::flatbuf{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[flatbuf] val Header   |    (n/a) | %8.1f |  —  \n",
               val_fb_v3);
   std::printf("[flatbuf] size Header  |    (n/a) | %8.1f |  —  \n",
               size_fb_v3);

   // ── capnp Header ───────────────────────────────────────────────────
   double enc_cp_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::capnp_pack(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_cp_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::capnp{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_cp_v1 = psio::capnp_pack(h1);
   auto bytes_cp_v3 = psio3::encode(psio3::capnp{}, h3);
   double dec_cp_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio::capnp_unpack<v1::Header>(bytes_cp_v1.data());
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_cp_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::capnp{},
                                          std::span<const char>{bytes_cp_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[capnp] enc Header     | %8.1f | %8.1f | %.2fx\n",
               enc_cp_v1, enc_cp_v3, ratio(enc_cp_v3, enc_cp_v1));
   std::printf("[capnp] dec Header     | %8.1f | %8.1f | %.2fx\n",
               dec_cp_v1, dec_cp_v3, ratio(dec_cp_v3, dec_cp_v1));
   double val_cp_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::capnp{},
                                             std::span<const char>{bytes_cp_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_cp_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::capnp{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[capnp] val Header     | %8s | %8.1f |  —  \n",
               "n/a", val_cp_v3);
   std::printf("[capnp] size Header    | %8s | %8.1f |  —  \n",
               "n/a", size_cp_v3);

   // ── wit Header ─────────────────────────────────────────────────────
   double enc_wit_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio::wit::pack(h1);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   double enc_wit_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto out = psio3::encode(psio3::wit{}, h3);
      asm volatile("" : : "r,m"(out.data()) : "memory");
   });
   auto bytes_wit_v1 = psio::wit::pack(h1);
   auto bytes_wit_v3 = psio3::encode(psio3::wit{}, h3);
   double dec_wit_v1 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio::wit::unpack<v1::Header>(bytes_wit_v1);
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   double dec_wit_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto v = psio3::decode<v3::Header>(psio3::wit{},
                                          std::span<const char>{bytes_wit_v3});
      asm volatile("" : : "r,m"(v.slot) : "memory");
   });
   std::printf("[wit] enc Header       | %8.1f | %8.1f | %.2fx\n",
               enc_wit_v1, enc_wit_v3, ratio(enc_wit_v3, enc_wit_v1));
   std::printf("[wit] dec Header       | %8.1f | %8.1f | %.2fx\n",
               dec_wit_v1, dec_wit_v3, ratio(dec_wit_v3, dec_wit_v1));
   double val_wit_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto st = psio3::validate<v3::Header>(psio3::wit{},
                                             std::span<const char>{bytes_wit_v3});
      asm volatile("" : : "r,m"(st) : "memory");
   });
   double size_wit_v3 = ns_per_iter(ITERS, [&](std::size_t) {
      auto n = psio3::size_of(psio3::wit{}, h3);
      asm volatile("" : : "r,m"(n) : "memory");
   });
   std::printf("[wit] val Header       | %8s | %8.1f |  —  \n",
               "n/a", val_wit_v3);
   std::printf("[wit] size Header      | %8s | %8.1f |  —  \n",
               "n/a", size_wit_v3);

   // ── Size comparison ────────────────────────────────────────────────
   std::printf("\nwire size (bytes)      | v1 | v3\n");
   std::printf("-----------------------+----+----\n");
   std::printf("[ssz]     Header       | %4zu | %4zu\n",
               bytes_v1.size(), bytes_v3.size());
   std::printf("[pssz]    Header       | %4zu | %4zu\n",
               bytes_pssz_v1.size(), bytes_pssz_v3.size());
   std::printf("[frac32]  Header       | %4zu | %4zu\n",
               bytes_frac_v1.size(), bytes_frac_v3.size());
   std::printf("[bin]     Header       | %4zu | %4zu\n",
               bytes_bin_v1.size(), bytes_bin_v3.size());
   std::printf("[borsh]   Header       | %4zu | %4zu\n",
               bytes_borsh_v1.size(), bytes_borsh_v3.size());
   std::printf("[bincode] Header       | %4zu | %4zu\n",
               bytes_bc_v1.size(), bytes_bc_v3.size());
   std::printf("[avro]    Header       | %4zu | %4zu\n",
               bytes_avro_v1.size(), bytes_avro_v3.size());
   std::printf("[flatbuf] Header       |  n/a | %4zu\n",
               bytes_fb_v3.size());
   std::printf("[capnp]   Header       | %4zu | %4zu\n",
               bytes_cp_v1.size(), bytes_cp_v3.size());
   std::printf("[wit]     Header       | %4zu | %4zu\n",
               bytes_wit_v1.size(), bytes_wit_v3.size());
}
