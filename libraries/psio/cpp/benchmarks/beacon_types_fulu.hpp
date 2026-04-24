#pragma once
// Ethereum Fulu-fork BeaconState types — accumulates Altair/Bellatrix/Capella/
// Deneb/Electra/Fulu additions on top of the Phase 0 shape.
//
// Reference: https://ethereum.github.io/consensus-specs/specs/fulu/beacon-chain/
// Cross-checked against OffchainLabs/sszpp lib/beacon_state.hpp.
//
// Lives in namespace eth::fulu to coexist with Phase 0 eth::BeaconState.

#include "beacon_types.hpp"

#include <psio/bitset.hpp>
#include <psio/bounded.hpp>
#include <psio/ext_int.hpp>
#include <psio/reflect.hpp>

#include <array>
#include <cstdint>

namespace eth::fulu
{
   // ── Constants (mainnet preset) ────────────────────────────────────────────

   using eth::EPOCHS_PER_HISTORICAL_VECTOR;
   using eth::EPOCHS_PER_SLASHINGS_VECTOR;
   using eth::HISTORICAL_ROOTS_LIMIT;
   using eth::JUSTIFICATION_BITS_LENGTH;
   using eth::SLOTS_PER_EPOCH;
   using eth::SLOTS_PER_HISTORICAL_ROOT;
   using eth::VALIDATOR_REGISTRY_LIMIT;

   constexpr std::size_t EPOCHS_PER_ETH1_VOTING_PERIOD = 64;
   constexpr std::size_t SYNC_COMMITTEE_SIZE           = 512;
   constexpr std::size_t BYTES_PER_LOGS_BLOOM          = 256;
   constexpr std::size_t MAX_EXTRA_DATA_BYTES          = 32;
   constexpr std::size_t PENDING_DEPOSITS_LIMIT        = std::size_t{1} << 27;
   constexpr std::size_t PENDING_PARTIAL_WITHDRAWALS_LIMIT = std::size_t{1} << 27;
   constexpr std::size_t PENDING_CONSOLIDATIONS_LIMIT  = 262144;
   constexpr std::size_t MIN_SEED_LOOKAHEAD            = 1;

   // ── Type aliases ──────────────────────────────────────────────────────────

   using eth::BLSPubkey;
   using eth::BLSSignature;
   using eth::Bytes32;
   using eth::Epoch;
   using eth::Gwei;
   using eth::Root;
   using eth::Slot;
   using eth::ValidatorIndex;
   using eth::Version;

   using ExecutionAddress  = std::array<std::uint8_t, 20>;
   using LogsBloom         = std::array<std::uint8_t, BYTES_PER_LOGS_BLOOM>;
   using ParticipationFlag = std::uint8_t;

   // ── Reuse from Phase 0 ────────────────────────────────────────────────────
   using eth::BeaconBlockHeader;
   using eth::Checkpoint;
   using eth::Eth1Data;
   using eth::Fork;
   using eth::Validator;

   // ── Altair ────────────────────────────────────────────────────────────────

   struct SyncCommittee
   {
      std::array<BLSPubkey, SYNC_COMMITTEE_SIZE> pubkeys;
      BLSPubkey                                  aggregate_pubkey;
   };

   // ── Bellatrix → Deneb (all fields accumulated) ────────────────────────────
   //
   // ExecutionPayloadHeader grew across forks. The Fulu shape is exactly the
   // Deneb shape — no new execution-payload-header fields in Electra/Fulu.
   // Mixed fixed+variable container (extra_data is variable).

   struct ExecutionPayloadHeader
   {
      Bytes32           parent_hash;
      ExecutionAddress  fee_recipient;
      Bytes32           state_root;
      Bytes32           receipts_root;
      LogsBloom         logs_bloom;
      Bytes32           prev_randao;
      std::uint64_t     block_number;
      std::uint64_t     gas_limit;
      std::uint64_t     gas_used;
      std::uint64_t     timestamp;
      psio::bounded_list<std::uint8_t, MAX_EXTRA_DATA_BYTES> extra_data;
      psio::uint256     base_fee_per_gas;
      Bytes32           block_hash;
      Root              transactions_root;
      Root              withdrawals_root;  // Capella
      std::uint64_t     blob_gas_used;     // Deneb
      std::uint64_t     excess_blob_gas;   // Deneb
   };

   // ── Capella ───────────────────────────────────────────────────────────────

   struct HistoricalSummary
   {
      Root block_summary_root;
      Root state_summary_root;
   };

   // ── Electra ───────────────────────────────────────────────────────────────

   struct PendingDeposit
   {
      BLSPubkey    pubkey;
      Bytes32      withdrawal_credentials;
      Gwei         amount;
      BLSSignature signature;
      Slot         slot;
   };

   struct PendingPartialWithdrawal
   {
      ValidatorIndex validator_index;
      Gwei           amount;
      Epoch          withdrawable_epoch;
   };

   struct PendingConsolidation
   {
      ValidatorIndex source_index;
      ValidatorIndex target_index;
   };

   // ── Fulu BeaconState — full fork-accumulated shape ────────────────────────

   struct BeaconState
   {
      // Phase 0 base
      std::uint64_t                                     genesis_time;
      Root                                              genesis_validators_root;
      Slot                                              slot;
      Fork                                              fork;
      BeaconBlockHeader                                 latest_block_header;
      std::array<Root, SLOTS_PER_HISTORICAL_ROOT>       block_roots;
      std::array<Root, SLOTS_PER_HISTORICAL_ROOT>       state_roots;
      psio::bounded_list<Root, HISTORICAL_ROOTS_LIMIT>  historical_roots;
      Eth1Data                                          eth1_data;
      psio::bounded_list<Eth1Data,
                          EPOCHS_PER_ETH1_VOTING_PERIOD * SLOTS_PER_EPOCH>
                                                        eth1_data_votes;
      std::uint64_t                                     eth1_deposit_index;
      psio::bounded_list<Validator, VALIDATOR_REGISTRY_LIMIT> validators;
      psio::bounded_list<Gwei, VALIDATOR_REGISTRY_LIMIT>      balances;
      std::array<Bytes32, EPOCHS_PER_HISTORICAL_VECTOR> randao_mixes;
      std::array<Gwei, EPOCHS_PER_SLASHINGS_VECTOR>     slashings;

      // Altair
      psio::bounded_list<ParticipationFlag, VALIDATOR_REGISTRY_LIMIT>
                                                        previous_epoch_participation;
      psio::bounded_list<ParticipationFlag, VALIDATOR_REGISTRY_LIMIT>
                                                        current_epoch_participation;

      // Finality
      psio::bitvector<JUSTIFICATION_BITS_LENGTH>        justification_bits;
      Checkpoint                                        previous_justified_checkpoint;
      Checkpoint                                        current_justified_checkpoint;
      Checkpoint                                        finalized_checkpoint;

      // Altair (continued)
      psio::bounded_list<std::uint64_t, VALIDATOR_REGISTRY_LIMIT> inactivity_scores;
      SyncCommittee                                     current_sync_committee;
      SyncCommittee                                     next_sync_committee;

      // Bellatrix / Capella / Deneb
      ExecutionPayloadHeader                            latest_execution_payload_header;

      // Capella
      std::uint64_t                                     next_withdrawal_index;
      ValidatorIndex                                    next_withdrawal_validator_index;
      psio::bounded_list<HistoricalSummary, HISTORICAL_ROOTS_LIMIT> historical_summaries;

      // Electra
      std::uint64_t                                     deposit_requests_start_index;
      Gwei                                              deposit_balance_to_consume;
      Gwei                                              exit_balance_to_consume;
      Epoch                                             earliest_exit_epoch;
      Gwei                                              consolidation_balance_to_consume;
      Epoch                                             earliest_consolidation_epoch;
      psio::bounded_list<PendingDeposit, PENDING_DEPOSITS_LIMIT> pending_deposits;
      psio::bounded_list<PendingPartialWithdrawal,
                          PENDING_PARTIAL_WITHDRAWALS_LIMIT>      pending_partial_withdrawals;
      psio::bounded_list<PendingConsolidation, PENDING_CONSOLIDATIONS_LIMIT>
                                                        pending_consolidations;

      // Fulu (single new field)
      std::array<ValidatorIndex, (MIN_SEED_LOOKAHEAD + 1) * SLOTS_PER_EPOCH>
                                                        proposer_lookahead;
   };

   // ── Reflection ────────────────────────────────────────────────────────────

   PSIO_REFLECT(SyncCommittee,
                definitionWillNotChange(),
                pubkeys, aggregate_pubkey)

   PSIO_REFLECT(ExecutionPayloadHeader,
                parent_hash, fee_recipient, state_root, receipts_root,
                logs_bloom, prev_randao, block_number, gas_limit, gas_used,
                timestamp, extra_data, base_fee_per_gas, block_hash,
                transactions_root, withdrawals_root, blob_gas_used,
                excess_blob_gas)

   PSIO_REFLECT(HistoricalSummary,
                definitionWillNotChange(),
                block_summary_root, state_summary_root)

   PSIO_REFLECT(PendingDeposit,
                definitionWillNotChange(),
                pubkey, withdrawal_credentials, amount, signature, slot)

   PSIO_REFLECT(PendingPartialWithdrawal,
                definitionWillNotChange(),
                validator_index, amount, withdrawable_epoch)

   PSIO_REFLECT(PendingConsolidation,
                definitionWillNotChange(),
                source_index, target_index)

   // BeaconState: DWNC — consensus types are spec-stable; lifts the u16
   // header cap so our 2.5+ MiB inline fixed region compiles.
   PSIO_REFLECT(BeaconState,
                definitionWillNotChange(),
                genesis_time, genesis_validators_root, slot, fork,
                latest_block_header, block_roots, state_roots, historical_roots,
                eth1_data, eth1_data_votes, eth1_deposit_index,
                validators, balances,
                randao_mixes, slashings,
                previous_epoch_participation, current_epoch_participation,
                justification_bits,
                previous_justified_checkpoint, current_justified_checkpoint,
                finalized_checkpoint,
                inactivity_scores, current_sync_committee, next_sync_committee,
                latest_execution_payload_header,
                next_withdrawal_index, next_withdrawal_validator_index,
                historical_summaries,
                deposit_requests_start_index,
                deposit_balance_to_consume, exit_balance_to_consume,
                earliest_exit_epoch,
                consolidation_balance_to_consume, earliest_consolidation_epoch,
                pending_deposits, pending_partial_withdrawals,
                pending_consolidations,
                proposer_lookahead)

}  // namespace eth::fulu
