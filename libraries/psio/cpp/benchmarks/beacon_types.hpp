#pragma once
// Ethereum Phase 0 BeaconState types.
//
// Reproduced from the Ethereum consensus-specs (phase0). Constants match
// Ethereum mainnet. Types use psio::bounded_list / psio::bitvector / etc.
// to carry SSZ-native bounds at the type level.
//
// Scope: enough to pack / unpack / validate / view a BeaconState with
// realistic shape. Cryptographic computations (hash_tree_root, signature
// verification) are out of scope — this is for wire-layer benchmarking.

#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace eth
{
   // ── Constants (mainnet preset) ────────────────────────────────────────────

   constexpr std::size_t SLOTS_PER_HISTORICAL_ROOT    = 8192;
   constexpr std::size_t HISTORICAL_ROOTS_LIMIT       = std::size_t{1} << 24;  // 16 777 216
   constexpr std::size_t SLOTS_PER_ETH1_VOTING_PERIOD = 2048;
   constexpr std::size_t VALIDATOR_REGISTRY_LIMIT     = std::size_t{1} << 40;  // 2^40 (bounded at spec level)
   constexpr std::size_t EPOCHS_PER_HISTORICAL_VECTOR = 65536;
   constexpr std::size_t EPOCHS_PER_SLASHINGS_VECTOR  = 8192;
   constexpr std::size_t MAX_ATTESTATIONS             = 128;
   constexpr std::size_t SLOTS_PER_EPOCH              = 32;
   constexpr std::size_t MAX_VALIDATORS_PER_COMMITTEE = 2048;
   constexpr std::size_t DEPOSIT_CONTRACT_TREE_DEPTH  = 32;
   constexpr std::size_t JUSTIFICATION_BITS_LENGTH    = 4;

   // ── Primitive type aliases ────────────────────────────────────────────────
   //
   // SSZ primitives reshape to C++ stdint equivalents. Bytes32 / BLSPubkey /
   // BLSSignature are fixed-size byte arrays (std::array<uint8_t, N>).

   using Slot       = std::uint64_t;
   using Epoch      = std::uint64_t;
   using CommitteeIndex = std::uint64_t;
   using ValidatorIndex = std::uint64_t;
   using Gwei       = std::uint64_t;
   using Root       = std::array<std::uint8_t, 32>;
   using Hash32     = std::array<std::uint8_t, 32>;
   using Bytes32    = std::array<std::uint8_t, 32>;
   using Version    = std::array<std::uint8_t, 4>;
   using DomainType = std::array<std::uint8_t, 4>;
   using BLSPubkey    = std::array<std::uint8_t, 48>;
   using BLSSignature = std::array<std::uint8_t, 96>;

   // ── Containers ────────────────────────────────────────────────────────────

   struct Fork
   {
      Version previous_version;
      Version current_version;
      Epoch   epoch;
   };

   struct ForkData
   {
      Version current_version;
      Root    genesis_validators_root;
   };

   struct Checkpoint
   {
      Epoch epoch;
      Root  root;
   };

   // __attribute__((packed)): sizeof(Validator) == 121 exactly, matching the
   // SSZ wire size. Without this, alignment padding between `slashed` (byte
   // 88) and `activation_eligibility_epoch` (aligned to 96) makes
   // sizeof(Validator) == 128, defeating the single-memcpy serialization path
   // for std::vector<Validator>. ARM64 and x86_64 handle the resulting
   // unaligned u64 accesses natively with no per-field cost.
   struct __attribute__((packed)) Validator
   {
      BLSPubkey pubkey;
      Bytes32   withdrawal_credentials;
      Gwei      effective_balance;
      bool      slashed;
      Epoch     activation_eligibility_epoch;
      Epoch     activation_epoch;
      Epoch     exit_epoch;
      Epoch     withdrawable_epoch;
   };

   struct AttestationData
   {
      Slot             slot;
      CommitteeIndex   index;
      Root             beacon_block_root;
      Checkpoint       source;
      Checkpoint       target;
   };

   // Phase 0 attestation body (pre-Altair). aggregation_bits is a Bitlist
   // bounded by MAX_VALIDATORS_PER_COMMITTEE.
   struct PendingAttestation
   {
      psio::bitlist<MAX_VALIDATORS_PER_COMMITTEE> aggregation_bits;
      AttestationData                             data;
      Slot                                        inclusion_delay;
      ValidatorIndex                              proposer_index;
   };

   struct Eth1Data
   {
      Root          deposit_root;
      std::uint64_t deposit_count;
      Hash32        block_hash;
   };

   struct BeaconBlockHeader
   {
      Slot           slot;
      ValidatorIndex proposer_index;
      Root           parent_root;
      Root           state_root;
      Root           body_root;
   };

   // ── BeaconState (Phase 0) ─────────────────────────────────────────────────

   struct BeaconState
   {
      // Versioning
      std::uint64_t genesis_time;
      Root          genesis_validators_root;
      Slot          slot;
      Fork          fork;

      // History
      BeaconBlockHeader                                   latest_block_header;
      std::array<Root, SLOTS_PER_HISTORICAL_ROOT>         block_roots;
      std::array<Root, SLOTS_PER_HISTORICAL_ROOT>         state_roots;
      psio::bounded_list<Root, HISTORICAL_ROOTS_LIMIT>    historical_roots;

      // Eth1
      Eth1Data                                                  eth1_data;
      psio::bounded_list<Eth1Data, SLOTS_PER_ETH1_VOTING_PERIOD> eth1_data_votes;
      std::uint64_t                                             eth1_deposit_index;

      // Registry
      psio::bounded_list<Validator, VALIDATOR_REGISTRY_LIMIT> validators;
      psio::bounded_list<Gwei, VALIDATOR_REGISTRY_LIMIT>      balances;

      // Randomness
      std::array<Bytes32, EPOCHS_PER_HISTORICAL_VECTOR> randao_mixes;

      // Slashings
      std::array<Gwei, EPOCHS_PER_SLASHINGS_VECTOR> slashings;

      // Attestations (Phase 0)
      psio::bounded_list<PendingAttestation,
                         SLOTS_PER_EPOCH * MAX_ATTESTATIONS> previous_epoch_attestations;
      psio::bounded_list<PendingAttestation,
                         SLOTS_PER_EPOCH * MAX_ATTESTATIONS> current_epoch_attestations;

      // Finality
      psio::bitvector<JUSTIFICATION_BITS_LENGTH> justification_bits;
      Checkpoint                                  previous_justified_checkpoint;
      Checkpoint                                  current_justified_checkpoint;
      Checkpoint                                  finalized_checkpoint;
   };

   // ── Reflection (invoked inside the namespace) ─────────────────────────────

   PSIO_REFLECT(Fork,
                definitionWillNotChange(),
                previous_version, current_version, epoch)

   PSIO_REFLECT(ForkData,
                definitionWillNotChange(),
                current_version, genesis_validators_root)

   PSIO_REFLECT(Checkpoint,
                definitionWillNotChange(),
                epoch, root)

   PSIO_REFLECT(Validator,
                definitionWillNotChange(),
                pubkey, withdrawal_credentials, effective_balance, slashed,
                activation_eligibility_epoch, activation_epoch, exit_epoch,
                withdrawable_epoch)

   PSIO_REFLECT(AttestationData,
                definitionWillNotChange(),
                slot, index, beacon_block_root, source, target)

   PSIO_REFLECT(PendingAttestation,
                aggregation_bits, data, inclusion_delay, proposer_index)

   PSIO_REFLECT(Eth1Data,
                definitionWillNotChange(),
                deposit_root, deposit_count, block_hash)

   PSIO_REFLECT(BeaconBlockHeader,
                definitionWillNotChange(),
                slot, proposer_index, parent_root, state_root, body_root)

   PSIO_REFLECT(BeaconState,
                definitionWillNotChange(),
                genesis_time, genesis_validators_root, slot, fork,
                latest_block_header, block_roots, state_roots, historical_roots,
                eth1_data, eth1_data_votes, eth1_deposit_index,
                validators, balances,
                randao_mixes, slashings,
                previous_epoch_attestations, current_epoch_attestations,
                justification_bits,
                previous_justified_checkpoint, current_justified_checkpoint,
                finalized_checkpoint)

}  // namespace eth
