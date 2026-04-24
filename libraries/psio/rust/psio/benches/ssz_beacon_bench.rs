//! Full Ethereum Phase-0 BeaconState head-to-head — four-way comparison:
//!   1) psio SSZ    (ours)
//!   2) psio pSSZ   (ours — psiSSZ, the extensible format)
//!   3) ethereum_ssz (Lighthouse's production SSZ codec)
//!   4) ssz_rs      (ralexstokes's Python-aligned SSZ codec)
//!
//! Counterpart of C++'s `bench_ssz_beacon.cpp`. The workload is the real
//! mainnet genesis state (~5.15 MiB, 21 063 validators) decoded / encoded
//! end-to-end — not the synthetic Validator list that
//! `ssz_validator_bench.rs` uses.
//!
//! Both ethereum_ssz and ssz_rs define their own BeaconState types via
//! their derive macros — neither ships one out of the box (Lighthouse's
//! production `BeaconState` lives in their monolithic `types` crate).
//! We mirror beacon_types.rs field-by-field in each competitor's types.
//!
//! Usage:
//!   cargo bench -p psio --bench ssz_beacon_bench
//!       → psio SSZ + psio pSSZ only
//!   cargo bench -p psio --bench ssz_beacon_bench --features bench_ssz_rs
//!       → adds ssz_rs alongside
//!   cargo bench -p psio --bench ssz_beacon_bench --features bench_ethereum_ssz
//!       → adds ethereum_ssz alongside
//!   cargo bench -p psio --bench ssz_beacon_bench \
//!       --features "bench_ethereum_ssz bench_ssz_rs"
//!       → all four side by side
//!
//! Input: `/tmp/beacon_data/mainnet-genesis.ssz`. If missing, the bench
//! prints a skip note and exits 0.

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};

fn load_genesis() -> Option<Vec<u8>> {
    std::fs::read("/tmp/beacon_data/mainnet-genesis.ssz").ok()
}

// ── psio SSZ: uses our BeaconState in beacon_types.rs ──────────────────────

mod psio_ssz_impl {
    use psio::beacon_types::BeaconState;
    use psio::ssz::{from_ssz, ssz_validate, to_ssz};

    pub fn decode(raw: &[u8]) -> Box<BeaconState> {
        Box::new(from_ssz::<BeaconState>(raw).expect("psio ssz decode"))
    }
    pub fn encode(state: &BeaconState) -> Vec<u8> { to_ssz(state) }
    pub fn validate(raw: &[u8]) {
        ssz_validate::<BeaconState>(raw).expect("psio ssz validate");
    }
}

// ── psio pSSZ: same BeaconState, encoded via psiSSZ (Pssz32) ───────────────

mod psio_pssz_impl {
    use psio::beacon_types::BeaconState;
    use psio::pssz::{from_pssz, to_pssz, Pssz32};

    pub fn decode(raw: &[u8]) -> Box<BeaconState> {
        Box::new(from_pssz::<Pssz32, BeaconState>(raw).expect("psio pssz decode"))
    }
    pub fn encode(state: &BeaconState) -> Vec<u8> {
        to_pssz::<Pssz32, _>(state)
    }
}

// ── ethereum_ssz (Lighthouse) — mirrors BeaconState via Decode/Encode ──────
//
// Lighthouse's production `BeaconState` is in its `types` crate which
// brings a huge dep tree (consensus-types, BLS, etc.). Instead, we
// define a local BeaconState using ethereum_ssz + ssz_types — the same
// building blocks Lighthouse actually uses in production.

#[cfg(feature = "bench_ethereum_ssz")]
mod lighthouse_impl {
    use ssz::{Decode, Encode};
    use ssz_derive::{Decode as DeriveDecode, Encode as DeriveEncode};
    use ssz_types::{
        typenum::{U16777216, U1073741824, U2048, U32, U4, U4096, U48, U65536, U8192},
        BitList, BitVector, FixedVector, VariableList,
    };

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct Fork {
        pub previous_version: FixedVector<u8, typenum::U4>,
        pub current_version:  FixedVector<u8, typenum::U4>,
        pub epoch:            u64,
    }
    // typenum::U4 is imported via the ssz_types re-export below; alias
    // the path we use in field types.
    use ssz_types::typenum;

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct Checkpoint {
        pub epoch: u64,
        pub root:  FixedVector<u8, U32>,
    }

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct Validator {
        pub pubkey:                       FixedVector<u8, U48>,
        pub withdrawal_credentials:       FixedVector<u8, U32>,
        pub effective_balance:            u64,
        pub slashed:                      bool,
        pub activation_eligibility_epoch: u64,
        pub activation_epoch:             u64,
        pub exit_epoch:                   u64,
        pub withdrawable_epoch:           u64,
    }

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct AttestationData {
        pub slot:              u64,
        pub index:             u64,
        pub beacon_block_root: FixedVector<u8, U32>,
        pub source:            Checkpoint,
        pub target:            Checkpoint,
    }

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct PendingAttestation {
        pub aggregation_bits: BitList<U2048>,
        pub data:             AttestationData,
        pub inclusion_delay:  u64,
        pub proposer_index:   u64,
    }

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct Eth1Data {
        pub deposit_root:  FixedVector<u8, U32>,
        pub deposit_count: u64,
        pub block_hash:    FixedVector<u8, U32>,
    }

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct BeaconBlockHeader {
        pub slot:           u64,
        pub proposer_index: u64,
        pub parent_root:    FixedVector<u8, U32>,
        pub state_root:     FixedVector<u8, U32>,
        pub body_root:      FixedVector<u8, U32>,
    }

    #[derive(DeriveDecode, DeriveEncode, Debug, Clone, PartialEq)]
    pub struct BeaconState {
        pub genesis_time:            u64,
        pub genesis_validators_root: FixedVector<u8, U32>,
        pub slot:                    u64,
        pub fork:                    Fork,

        pub latest_block_header: BeaconBlockHeader,
        pub block_roots:         FixedVector<FixedVector<u8, U32>, U8192>,
        pub state_roots:         FixedVector<FixedVector<u8, U32>, U8192>,
        pub historical_roots:    VariableList<FixedVector<u8, U32>, U16777216>,

        pub eth1_data:          Eth1Data,
        pub eth1_data_votes:    VariableList<Eth1Data, U2048>,
        pub eth1_deposit_index: u64,

        pub validators: VariableList<Validator, U1073741824>,
        pub balances:   VariableList<u64, U1073741824>,

        pub randao_mixes: FixedVector<FixedVector<u8, U32>, U65536>,

        pub slashings: FixedVector<u64, U8192>,

        pub previous_epoch_attestations: VariableList<PendingAttestation, U4096>,
        pub current_epoch_attestations:  VariableList<PendingAttestation, U4096>,

        pub justification_bits:            BitVector<U4>,
        pub previous_justified_checkpoint: Checkpoint,
        pub current_justified_checkpoint:  Checkpoint,
        pub finalized_checkpoint:          Checkpoint,
    }

    pub fn decode(raw: &[u8]) -> Box<BeaconState> {
        Box::new(BeaconState::from_ssz_bytes(raw).expect("ethereum_ssz decode"))
    }
    pub fn encode(state: &BeaconState) -> Vec<u8> {
        state.as_ssz_bytes()
    }
}

// ── ssz_rs: mirror BeaconState using their derive ──────────────────────────
//
// ssz_rs types must be `Default` to allocate. The large fixed arrays
// (2 MiB randao_mixes) must go through `Vector<T, N>`; small structs use
// derive `SimpleSerialize`.

#[cfg(feature = "bench_ssz_rs")]
mod ssz_rs_impl {
    use ssz_rs::prelude::*;

    // Phase-0 constants (mirror of beacon_types.rs).
    const SLOTS_PER_HISTORICAL_ROOT:    usize = 8192;
    const HISTORICAL_ROOTS_LIMIT:       usize = 1 << 24;
    const SLOTS_PER_ETH1_VOTING_PERIOD: usize = 2048;
    const VALIDATOR_REGISTRY_LIMIT:     usize = 1 << 30;
    const EPOCHS_PER_HISTORICAL_VECTOR: usize = 65536;
    const EPOCHS_PER_SLASHINGS_VECTOR:  usize = 8192;
    const MAX_ATTESTATIONS:             usize = 128;
    const SLOTS_PER_EPOCH:              usize = 32;
    const MAX_VALIDATORS_PER_COMMITTEE: usize = 2048;
    const JUSTIFICATION_BITS_LENGTH:    usize = 4;
    const MAX_PENDING_ATTESTATIONS:     usize = SLOTS_PER_EPOCH * MAX_ATTESTATIONS;

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct Fork {
        pub previous_version: Vector<u8, 4>,
        pub current_version:  Vector<u8, 4>,
        pub epoch:            u64,
    }

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct Checkpoint {
        pub epoch: u64,
        pub root:  Vector<u8, 32>,
    }

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct Validator {
        pub pubkey:                       Vector<u8, 48>,
        pub withdrawal_credentials:       Vector<u8, 32>,
        pub effective_balance:            u64,
        pub slashed:                      bool,
        pub activation_eligibility_epoch: u64,
        pub activation_epoch:             u64,
        pub exit_epoch:                   u64,
        pub withdrawable_epoch:           u64,
    }

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct AttestationData {
        pub slot:              u64,
        pub index:             u64,
        pub beacon_block_root: Vector<u8, 32>,
        pub source:            Checkpoint,
        pub target:            Checkpoint,
    }

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct PendingAttestation {
        pub aggregation_bits: Bitlist<MAX_VALIDATORS_PER_COMMITTEE>,
        pub data:             AttestationData,
        pub inclusion_delay:  u64,
        pub proposer_index:   u64,
    }

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct Eth1Data {
        pub deposit_root:  Vector<u8, 32>,
        pub deposit_count: u64,
        pub block_hash:    Vector<u8, 32>,
    }

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct BeaconBlockHeader {
        pub slot:           u64,
        pub proposer_index: u64,
        pub parent_root:    Vector<u8, 32>,
        pub state_root:     Vector<u8, 32>,
        pub body_root:      Vector<u8, 32>,
    }

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct BeaconState {
        pub genesis_time:                 u64,
        pub genesis_validators_root:      Vector<u8, 32>,
        pub slot:                         u64,
        pub fork:                         Fork,

        pub latest_block_header:          BeaconBlockHeader,
        pub block_roots:                  Vector<Vector<u8, 32>, SLOTS_PER_HISTORICAL_ROOT>,
        pub state_roots:                  Vector<Vector<u8, 32>, SLOTS_PER_HISTORICAL_ROOT>,
        pub historical_roots:             List<Vector<u8, 32>, HISTORICAL_ROOTS_LIMIT>,

        pub eth1_data:                    Eth1Data,
        pub eth1_data_votes:              List<Eth1Data, SLOTS_PER_ETH1_VOTING_PERIOD>,
        pub eth1_deposit_index:           u64,

        pub validators:                   List<Validator, VALIDATOR_REGISTRY_LIMIT>,
        pub balances:                     List<u64, VALIDATOR_REGISTRY_LIMIT>,

        pub randao_mixes:                 Vector<Vector<u8, 32>, EPOCHS_PER_HISTORICAL_VECTOR>,

        pub slashings:                    Vector<u64, EPOCHS_PER_SLASHINGS_VECTOR>,

        pub previous_epoch_attestations:  List<PendingAttestation, MAX_PENDING_ATTESTATIONS>,
        pub current_epoch_attestations:   List<PendingAttestation, MAX_PENDING_ATTESTATIONS>,

        pub justification_bits:            Bitvector<JUSTIFICATION_BITS_LENGTH>,
        pub previous_justified_checkpoint: Checkpoint,
        pub current_justified_checkpoint:  Checkpoint,
        pub finalized_checkpoint:          Checkpoint,
    }

    pub fn decode(raw: &[u8]) -> Box<BeaconState> {
        Box::new(ssz_rs::deserialize::<BeaconState>(raw).expect("ssz_rs decode"))
    }
    pub fn encode(state: &BeaconState) -> Vec<u8> {
        ssz_rs::serialize(state).expect("ssz_rs encode")
    }
}

// ── Cross-validation: every SSZ impl must round-trip byte-identically,
// pSSZ to itself (format not wire-compatible with SSZ by design). ──────────

fn run_cross_validation(raw: &[u8]) {
    // psio SSZ: must be byte-identical on round-trip.
    let state = psio_ssz_impl::decode(raw);
    let re = psio_ssz_impl::encode(&state);
    assert_eq!(re.len(), raw.len(), "psio ssz: size mismatch on re-encode");
    assert_eq!(&re[..], raw, "psio ssz: byte-identical round-trip failed");
    eprintln!("✓ psio SSZ  round-trip byte-identical ({:.2} MiB)",
              raw.len() as f64 / (1024.0 * 1024.0));

    // psio pSSZ: encode, decode, re-encode — must match itself.
    let pssz1 = psio_pssz_impl::encode(&state);
    let decoded2 = psio_pssz_impl::decode(&pssz1);
    let pssz2 = psio_pssz_impl::encode(&decoded2);
    assert_eq!(pssz1, pssz2, "psio pSSZ: round-trip must be deterministic");
    eprintln!("✓ psio pSSZ round-trip deterministic ({:.2} MiB, {:+} B vs SSZ)",
              pssz1.len() as f64 / (1024.0 * 1024.0),
              pssz1.len() as isize - raw.len() as isize);

    #[cfg(feature = "bench_ethereum_ssz")]
    {
        let state = lighthouse_impl::decode(raw);
        let re = lighthouse_impl::encode(&state);
        assert_eq!(re.len(), raw.len(), "ethereum_ssz: size mismatch");
        assert_eq!(&re[..], raw, "ethereum_ssz: byte-identical round-trip failed");
        eprintln!("✓ ethereum_ssz round-trip byte-identical (Lighthouse)");
    }

    #[cfg(feature = "bench_ssz_rs")]
    {
        let state = ssz_rs_impl::decode(raw);
        let re = ssz_rs_impl::encode(&state);
        assert_eq!(re.len(), raw.len(), "ssz_rs: size mismatch");
        assert_eq!(&re[..], raw, "ssz_rs: byte-identical round-trip failed");
        eprintln!("✓ ssz_rs round-trip byte-identical");
    }
}

fn bench_decode(c: &mut Criterion, raw: &[u8]) {
    let mut group = c.benchmark_group("beacon_state_decode");
    group.throughput(Throughput::Bytes(raw.len() as u64));
    group.sample_size(10);

    group.bench_function("psio_ssz", |b| {
        b.iter(|| black_box(psio_ssz_impl::decode(raw)));
    });

    // pSSZ decode runs on the pSSZ-encoded bytes (not the SSZ input).
    let pssz_bytes = {
        let s = psio_ssz_impl::decode(raw);
        psio_pssz_impl::encode(&s)
    };
    // Set throughput on pSSZ to its own size so GiB/s is comparable.
    group.bench_function("psio_pssz", |b| {
        b.iter(|| black_box(psio_pssz_impl::decode(&pssz_bytes)));
    });

    #[cfg(feature = "bench_ethereum_ssz")]
    group.bench_function("ethereum_ssz", |b| {
        b.iter(|| black_box(lighthouse_impl::decode(raw)));
    });

    #[cfg(feature = "bench_ssz_rs")]
    group.bench_function("ssz_rs", |b| {
        b.iter(|| black_box(ssz_rs_impl::decode(raw)));
    });

    group.finish();
}

fn bench_encode(c: &mut Criterion, raw: &[u8]) {
    let mut group = c.benchmark_group("beacon_state_encode");
    group.throughput(Throughput::Bytes(raw.len() as u64));
    group.sample_size(10);

    let psio_state = psio_ssz_impl::decode(raw);
    group.bench_function("psio_ssz", |b| {
        b.iter(|| black_box(psio_ssz_impl::encode(&psio_state)));
    });
    group.bench_function("psio_pssz", |b| {
        b.iter(|| black_box(psio_pssz_impl::encode(&psio_state)));
    });

    #[cfg(feature = "bench_ethereum_ssz")]
    {
        let state = lighthouse_impl::decode(raw);
        group.bench_function("ethereum_ssz", |b| {
            b.iter(|| black_box(lighthouse_impl::encode(&state)));
        });
    }

    #[cfg(feature = "bench_ssz_rs")]
    {
        let state = ssz_rs_impl::decode(raw);
        group.bench_function("ssz_rs", |b| {
            b.iter(|| black_box(ssz_rs_impl::encode(&state)));
        });
    }

    group.finish();
}

fn bench_validate(c: &mut Criterion, raw: &[u8]) {
    let mut group = c.benchmark_group("beacon_state_validate");
    group.throughput(Throughput::Bytes(raw.len() as u64));
    group.sample_size(20);

    group.bench_function("psio_ssz", |b| {
        b.iter(|| black_box(psio_ssz_impl::validate(raw)));
    });
    // ethereum_ssz and ssz_rs don't expose a non-allocating validate.
    group.finish();
}

fn bench_view_access(c: &mut Criterion, raw: &[u8]) {
    use psio::beacon_types::{
        BeaconState, BeaconStateSszAccessors, ValidatorSszAccessors,
    };
    use psio::ssz_view::{ssz_view_of, SszView};

    let mut group = c.benchmark_group("beacon_state_view");
    group.throughput(Throughput::Elements(1));
    group.sample_size(50);

    group.bench_function("psio_ssz/field_in_nested_list", |b| {
        b.iter(|| {
            let v: SszView<BeaconState> = ssz_view_of(raw);
            let val = v.validators().get(10_000).effective_balance().get();
            black_box(val)
        });
    });

    #[cfg(feature = "bench_ethereum_ssz")]
    {
        group.bench_function("ethereum_ssz/full_decode_then_field", |b| {
            b.iter(|| {
                let state = lighthouse_impl::decode(raw);
                let val = state.validators[10_000].effective_balance;
                black_box(val)
            });
        });
    }

    #[cfg(feature = "bench_ssz_rs")]
    {
        group.bench_function("ssz_rs/full_decode_then_field", |b| {
            b.iter(|| {
                let state = ssz_rs_impl::decode(raw);
                let val = state.validators[10_000].effective_balance;
                black_box(val)
            });
        });
    }

    group.finish();
}

fn bench_root(c: &mut Criterion) {
    let Some(raw) = load_genesis() else {
        eprintln!("skip: /tmp/beacon_data/mainnet-genesis.ssz not found");
        eprintln!("      curl -sL -o /tmp/beacon_data/mainnet-genesis.ssz \\");
        eprintln!("        https://github.com/eth-clients/mainnet/raw/main/metadata/genesis.ssz");
        return;
    };
    eprintln!("=== BeaconState bench ===");
    eprintln!("genesis size: {} bytes ({:.2} MiB)",
              raw.len(), raw.len() as f64 / (1024.0 * 1024.0));

    run_cross_validation(&raw);
    bench_decode(c, &raw);
    bench_encode(c, &raw);
    bench_validate(c, &raw);
    bench_view_access(c, &raw);
}

criterion_group!(benches, bench_root);
criterion_main!(benches);
