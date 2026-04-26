#include <catch2/catch.hpp>
#include "../benchmarks/beacon_types.hpp"

#include <psio1/from_ssz.hpp>
#include <psio1/ssz_view.hpp>
#include <psio1/to_ssz.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

// ── Data generation ──────────────────────────────────────────────────────────
//
// Build a realistic small-scale BeaconState for round-trip testing. Full
// mainnet has ~1M validators — for the test, use 100 validators so the buffer
// stays small enough to check assertions quickly. All fields that could be
// populated are populated to exercise every encoding path.

namespace
{
   eth::Root make_root(std::uint64_t seed)
   {
      eth::Root r{};
      for (std::size_t i = 0; i < r.size(); ++i)
         r[i] = static_cast<std::uint8_t>((seed >> (i % 8 * 8)) + i);
      return r;
   }

   eth::BLSPubkey make_pubkey(std::uint64_t seed)
   {
      eth::BLSPubkey p{};
      for (std::size_t i = 0; i < p.size(); ++i)
         p[i] = static_cast<std::uint8_t>((seed * 13 + i * 7) & 0xFF);
      return p;
   }

   eth::Validator make_validator(std::uint64_t idx)
   {
      eth::Validator v{};
      v.pubkey                 = make_pubkey(idx);
      v.withdrawal_credentials = make_root(idx ^ 0x1234567890ABCDEFULL);
      v.effective_balance      = 32ULL * 1'000'000'000ULL;
      v.slashed                = (idx % 17) == 0;
      v.activation_eligibility_epoch = idx;
      v.activation_epoch       = idx + 100;
      v.exit_epoch             = std::uint64_t{1} << 62;  // FAR_FUTURE_EPOCH
      v.withdrawable_epoch     = std::uint64_t{1} << 62;
      return v;
   }

   eth::BeaconState make_sample_state(std::size_t n_validators)
   {
      eth::BeaconState s{};
      s.genesis_time            = 1606824023;
      s.genesis_validators_root = make_root(0xAAAA);
      s.slot                    = 12345;
      s.fork.previous_version   = {0x00, 0x00, 0x00, 0x00};
      s.fork.current_version    = {0x01, 0x00, 0x00, 0x00};
      s.fork.epoch              = 0;

      s.latest_block_header.slot           = 12344;
      s.latest_block_header.proposer_index = 42;
      s.latest_block_header.parent_root    = make_root(0x100);
      s.latest_block_header.state_root     = make_root(0x101);
      s.latest_block_header.body_root      = make_root(0x102);

      // Ring buffers: populate with deterministic roots.
      for (std::size_t i = 0; i < eth::SLOTS_PER_HISTORICAL_ROOT; ++i)
         s.block_roots[i] = make_root(i * 2);
      for (std::size_t i = 0; i < eth::SLOTS_PER_HISTORICAL_ROOT; ++i)
         s.state_roots[i] = make_root(i * 2 + 1);

      // Small historical roots list
      for (std::size_t i = 0; i < 8; ++i)
         s.historical_roots.push_back(make_root(0xDEAD + i));

      s.eth1_data.deposit_root  = make_root(0xE1);
      s.eth1_data.deposit_count = n_validators;
      s.eth1_data.block_hash    = make_root(0xE2);

      // Tiny eth1_data_votes to exercise the List[Container] variable path.
      for (std::size_t i = 0; i < 4; ++i)
      {
         eth::Eth1Data d{};
         d.deposit_root  = make_root(0xE3 + i);
         d.deposit_count = n_validators;
         d.block_hash    = make_root(0xE4 + i);
         s.eth1_data_votes.push_back(d);
      }
      s.eth1_deposit_index = n_validators;

      // Validators + balances (length = n_validators)
      for (std::size_t i = 0; i < n_validators; ++i)
      {
         s.validators.push_back(make_validator(i));
         s.balances.push_back(32ULL * 1'000'000'000ULL + i);
      }

      // Randao mixes
      for (std::size_t i = 0; i < eth::EPOCHS_PER_HISTORICAL_VECTOR; ++i)
         s.randao_mixes[i] = make_root(i ^ 0xFEED);

      // Slashings (all zero by default — leave as is)

      // Attestations — keep small to bound test size.
      for (std::size_t a = 0; a < 8; ++a)
      {
         eth::PendingAttestation pa{};
         for (std::size_t b = 0; b < 64; ++b)
            pa.aggregation_bits.push_back((b + a) % 3 == 0);
         pa.data.slot              = s.slot - 1;
         pa.data.index             = a;
         pa.data.beacon_block_root = make_root(0x300 + a);
         pa.data.source.epoch      = 10;
         pa.data.source.root       = make_root(0x400);
         pa.data.target.epoch      = 11;
         pa.data.target.root       = make_root(0x401);
         pa.inclusion_delay        = 1;
         pa.proposer_index         = a;
         s.previous_epoch_attestations.push_back(pa);
         s.current_epoch_attestations.push_back(pa);
      }

      // Justification
      s.justification_bits.set(0, true);
      s.justification_bits.set(2, true);
      s.previous_justified_checkpoint.epoch = 10;
      s.previous_justified_checkpoint.root  = make_root(0x500);
      s.current_justified_checkpoint.epoch  = 11;
      s.current_justified_checkpoint.root   = make_root(0x501);
      s.finalized_checkpoint.epoch          = 9;
      s.finalized_checkpoint.root           = make_root(0x502);

      return s;
   }

   // Structural equality for BeaconState (for round-trip verification).
   // Members are all POD-ish or have == operators; we walk explicitly to
   // get useful failure context.
   bool states_equal(const eth::BeaconState& a, const eth::BeaconState& b)
   {
      if (a.genesis_time != b.genesis_time) return false;
      if (a.genesis_validators_root != b.genesis_validators_root) return false;
      if (a.slot != b.slot) return false;
      if (std::memcmp(&a.fork, &b.fork, sizeof(eth::Fork)) != 0) return false;
      if (std::memcmp(&a.latest_block_header, &b.latest_block_header,
                      sizeof(eth::BeaconBlockHeader)) != 0)
         return false;
      if (a.block_roots != b.block_roots) return false;
      if (a.state_roots != b.state_roots) return false;
      if (a.historical_roots.size() != b.historical_roots.size()) return false;
      for (std::size_t i = 0; i < a.historical_roots.size(); ++i)
         if (a.historical_roots[i] != b.historical_roots[i]) return false;
      if (std::memcmp(&a.eth1_data, &b.eth1_data, sizeof(eth::Eth1Data)) != 0) return false;
      if (a.eth1_data_votes.size() != b.eth1_data_votes.size()) return false;
      for (std::size_t i = 0; i < a.eth1_data_votes.size(); ++i)
         if (std::memcmp(&a.eth1_data_votes[i], &b.eth1_data_votes[i],
                         sizeof(eth::Eth1Data)) != 0)
            return false;
      if (a.eth1_deposit_index != b.eth1_deposit_index) return false;
      if (a.validators.size() != b.validators.size()) return false;
      for (std::size_t i = 0; i < a.validators.size(); ++i)
         if (std::memcmp(&a.validators[i], &b.validators[i], sizeof(eth::Validator)) != 0)
            return false;
      if (a.balances.size() != b.balances.size()) return false;
      for (std::size_t i = 0; i < a.balances.size(); ++i)
         if (a.balances[i] != b.balances[i]) return false;
      if (a.randao_mixes != b.randao_mixes) return false;
      if (a.slashings != b.slashings) return false;

      auto pending_eq = [](const eth::PendingAttestation& x,
                           const eth::PendingAttestation& y) {
         if (x.aggregation_bits.size() != y.aggregation_bits.size()) return false;
         for (std::size_t i = 0; i < x.aggregation_bits.size(); ++i)
            if (x.aggregation_bits.test(i) != y.aggregation_bits.test(i)) return false;
         if (std::memcmp(&x.data, &y.data, sizeof(eth::AttestationData)) != 0)
            return false;
         return x.inclusion_delay == y.inclusion_delay &&
                x.proposer_index == y.proposer_index;
      };

      if (a.previous_epoch_attestations.size() != b.previous_epoch_attestations.size())
         return false;
      for (std::size_t i = 0; i < a.previous_epoch_attestations.size(); ++i)
         if (!pending_eq(a.previous_epoch_attestations[i],
                         b.previous_epoch_attestations[i]))
            return false;

      if (a.current_epoch_attestations.size() != b.current_epoch_attestations.size())
         return false;
      for (std::size_t i = 0; i < a.current_epoch_attestations.size(); ++i)
         if (!pending_eq(a.current_epoch_attestations[i],
                         b.current_epoch_attestations[i]))
            return false;

      if (a.justification_bits != b.justification_bits) return false;
      if (std::memcmp(&a.previous_justified_checkpoint, &b.previous_justified_checkpoint,
                      sizeof(eth::Checkpoint)) != 0) return false;
      if (std::memcmp(&a.current_justified_checkpoint, &b.current_justified_checkpoint,
                      sizeof(eth::Checkpoint)) != 0) return false;
      if (std::memcmp(&a.finalized_checkpoint, &b.finalized_checkpoint,
                      sizeof(eth::Checkpoint)) != 0) return false;

      return true;
   }
}

// ── Compile-time sanity ──────────────────────────────────────────────────────

// In-memory sizes may include alignment padding; what matters is the SSZ wire
// size, which ssz_fixed_size reports independently of C++ alignment.
static_assert(psio1::ssz_is_fixed_size_v<eth::Fork>);
static_assert(psio1::ssz_fixed_size<eth::Fork>::value == 4 + 4 + 8);

static_assert(psio1::ssz_is_fixed_size_v<eth::Checkpoint>);
static_assert(psio1::ssz_fixed_size<eth::Checkpoint>::value == 8 + 32);

static_assert(psio1::ssz_is_fixed_size_v<eth::Validator>);
static_assert(psio1::ssz_fixed_size<eth::Validator>::value == 121);

static_assert(!psio1::ssz_is_fixed_size_v<eth::PendingAttestation>);  // has bitlist
static_assert(!psio1::ssz_is_fixed_size_v<eth::BeaconState>);

// ── Round-trip tests ─────────────────────────────────────────────────────────

TEST_CASE("beacon: BeaconState round-trip (100 validators)", "[beacon][ssz]")
{
   auto state = make_sample_state(100);

   auto bytes = psio1::convert_to_ssz(state);
   INFO("state size = " << bytes.size() << " bytes");
   REQUIRE(bytes.size() > 0);

   auto back = psio1::convert_from_ssz<eth::BeaconState>(bytes);
   REQUIRE(states_equal(state, back));
}

TEST_CASE("beacon: BeaconState validate", "[beacon][ssz][validate]")
{
   auto state = make_sample_state(50);
   auto bytes = psio1::convert_to_ssz(state);
   REQUIRE_NOTHROW(psio1::ssz_validate<eth::BeaconState>(bytes));

   // Corrupt a variable offset within the container — should fail.
   // First variable field is `historical_roots`. Find its offset slot: after
   // the fixed header prefix (genesis_time + genesis_validators_root + slot +
   // fork + latest_block_header + block_roots + state_roots) = 8 + 32 + 8 +
   // sizeof(Fork) + sizeof(BeaconBlockHeader) + 2 * 8192 * 32 bytes.
   auto bad = bytes;
   constexpr std::size_t header_before_hist_roots =
       8 + 32 + 8 + 16 + (8 + 8 + 32 + 32 + 32) + 2 * eth::SLOTS_PER_HISTORICAL_ROOT * 32;
   std::uint32_t bogus = static_cast<std::uint32_t>(bad.size()) + 1;
   std::memcpy(bad.data() + header_before_hist_roots, &bogus, 4);
   REQUIRE_THROWS(psio1::ssz_validate<eth::BeaconState>(bad));
}

// ── View tests: navigate without full decode ─────────────────────────────────

TEST_CASE("beacon: ssz_view field-by-field", "[beacon][ssz][view]")
{
   auto state = make_sample_state(200);
   auto bytes = psio1::convert_to_ssz(state);
   auto view  = psio1::ssz_view_of<eth::BeaconState>(bytes);

   // PSIO1_REFLECT-generated named accessors (same pattern as frac_view).
   std::uint64_t gt = view.genesis_time();
   REQUIRE(gt == state.genesis_time);

   std::uint64_t sl = view.slot();
   REQUIRE(sl == state.slot);

   auto vs = view.validators();
   REQUIRE(vs.size() == 200);

   auto v137 = vs[137];
   std::uint64_t eb = v137.effective_balance();
   REQUIRE(eb == 32ULL * 1'000'000'000ULL);

   auto bal = view.balances();
   REQUIRE(bal.size() == 200);
   std::uint64_t b137 = bal[137];
   REQUIRE(b137 == 32ULL * 1'000'000'000ULL + 137);
}

TEST_CASE("beacon: ssz_view access doesn't materialize the whole state",
          "[beacon][ssz][view]")
{
   // The whole point of ssz_view: access a single deeply-nested field
   // without allocating anything. This test just confirms compilation and
   // behavior; true no-alloc verification would need an allocator hook.

   auto state = make_sample_state(500);
   auto bytes = psio1::convert_to_ssz(state);
   auto view  = psio1::ssz_view_of<eth::BeaconState>(bytes);

   // validators[42].activation_epoch — should be value 142 per make_validator
   std::uint64_t a42 = view.validators()[42].activation_epoch();
   REQUIRE(a42 == 142);
}

// ── Real data: mainnet Phase 0 genesis BeaconState ───────────────────────────
//
// Download with:
//   curl -sL -o /tmp/beacon_data/mainnet-genesis.ssz \
//     https://github.com/eth-clients/mainnet/raw/main/metadata/genesis.ssz
//
// File is ~5.2 MB of raw SSZ (no snappy compression). If absent, the test
// skips rather than failing.

static const char* k_mainnet_genesis_path = "/tmp/beacon_data/mainnet-genesis.ssz";

namespace
{
   bool read_file_if_present(const char* path, std::vector<char>& out)
   {
      std::ifstream f(path, std::ios::binary);
      if (!f) return false;
      f.seekg(0, std::ios::end);
      auto sz = f.tellg();
      f.seekg(0);
      out.resize(static_cast<std::size_t>(sz));
      f.read(out.data(), sz);
      return f.good() || f.eof();
   }
}

TEST_CASE("beacon: load real mainnet Phase 0 genesis state", "[beacon][real]")
{
   std::vector<char> raw;
   if (!read_file_if_present(k_mainnet_genesis_path, raw))
   {
      WARN("mainnet genesis SSZ not found at " << k_mainnet_genesis_path
           << " — skipping (run curl to fetch)");
      return;
   }
   INFO("loaded " << raw.size() << " bytes from " << k_mainnet_genesis_path);

   // 1) Validate structural integrity without materializing.
   auto t0 = std::chrono::steady_clock::now();
   REQUIRE_NOTHROW(psio1::ssz_validate<eth::BeaconState>(raw));
   auto t1 = std::chrono::steady_clock::now();
   auto validate_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
   INFO("validate_us=" << validate_us);

   // 2) Zero-copy navigate to key fields.
   auto view = psio1::ssz_view_of<eth::BeaconState>(raw);

   // Mainnet Phase 0 launch: 1606824023 (Dec 1 2020, 12:00:23 UTC)
   std::uint64_t gt = view.genesis_time();
   REQUIRE(gt == 1606824023);

   std::uint64_t sl = view.slot();
   REQUIRE(sl == 0);

   // Validator count (genesis): 21063 validators at launch.
   auto validators = view.validators();
   INFO("validator_count=" << validators.size());
   REQUIRE(validators.size() == 21063);

   // Every genesis validator has 32 ETH effective balance.
   for (std::size_t i = 0; i < std::min<std::size_t>(validators.size(), 100); ++i)
   {
      std::uint64_t eb = validators[i].effective_balance();
      REQUIRE(eb == 32ULL * 1'000'000'000ULL);
   }

   // 3) Full decode — this is the heavy path.
   auto t2 = std::chrono::steady_clock::now();
   auto state_ptr = std::make_unique<eth::BeaconState>();
   psio1::convert_from_ssz(*state_ptr, raw);
   auto& state = *state_ptr;
   auto t3 = std::chrono::steady_clock::now();
   auto decode_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
   INFO("full_decode_us=" << decode_us);

   REQUIRE(state.genesis_time == 1606824023);
   REQUIRE(state.slot == 0);
   REQUIRE(state.validators.size() == 21063);
   REQUIRE(state.balances.size() == 21063);

   // 4) Re-encode should produce byte-identical output (round-trip determinism).
   auto re_encoded = psio1::convert_to_ssz(state);
   REQUIRE(re_encoded.size() == raw.size());
   REQUIRE(std::memcmp(re_encoded.data(), raw.data(), raw.size()) == 0);

   // Stdout for a record of what happened.
   std::fprintf(stderr,
                "[mainnet-genesis] size=%.2f MB  validate=%lld us  full_decode=%lld us  "
                "validators=%zu\n",
                raw.size() / (1024.0 * 1024.0),
                static_cast<long long>(validate_us),
                static_cast<long long>(decode_us),
                state.validators.size());
}
