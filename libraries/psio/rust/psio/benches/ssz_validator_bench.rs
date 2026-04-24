//! SSZ Validator head-to-head: psio (ours) vs ethereum_ssz (Lighthouse)
//! vs ssz_rs (ralexstokes).
//!
//! Competitor crates are opt-in via Cargo features so the default build
//! has zero external dependency:
//!
//!   cargo bench -p psio --bench ssz_validator_bench
//!       → psio-only numbers
//!
//!   cargo bench -p psio --bench ssz_validator_bench --features bench_ethereum_ssz
//!       → add Lighthouse's ethereum_ssz
//!
//!   cargo bench -p psio --bench ssz_validator_bench --features bench_ssz_rs
//!       → add ralexstokes's ssz_rs
//!
//!   cargo bench -p psio --bench ssz_validator_bench \
//!       --features "bench_ethereum_ssz bench_ssz_rs"
//!       → all three side by side
//!
//! Each implementation defines its own `Validator` type with the same
//! field layout. Cross-validation asserts they all produce byte-identical
//! 121-byte output for the same input before timing begins.

use criterion::{black_box, criterion_group, criterion_main, Criterion, Throughput};

// ── psio's Validator (DWNC, all-fixed) ──────────────────────────────────────

mod psio_impl {
    use psio::ssz::{SszPack, SszUnpack};
    use psio::ssz_struct_dwnc;

    #[derive(Debug, PartialEq, Eq, Clone)]
    pub struct Validator {
        pub pubkey:                       [u8; 48],
        pub withdrawal_credentials:       [u8; 32],
        pub effective_balance:            u64,
        pub slashed:                      bool,
        pub activation_eligibility_epoch: u64,
        pub activation_epoch:             u64,
        pub exit_epoch:                   u64,
        pub withdrawable_epoch:           u64,
    }
    ssz_struct_dwnc!(Validator {
        pubkey:                       [u8; 48],
        withdrawal_credentials:       [u8; 32],
        effective_balance:            u64,
        slashed:                      bool,
        activation_eligibility_epoch: u64,
        activation_epoch:             u64,
        exit_epoch:                   u64,
        withdrawable_epoch:           u64,
    });

    pub fn sample() -> Validator {
        let mut pk = [0u8; 48];
        for i in 0..48 { pk[i] = (i + 1) as u8; }
        let mut wc = [0u8; 32];
        for i in 0..32 { wc[i] = (100 + i) as u8; }
        Validator {
            pubkey: pk,
            withdrawal_credentials: wc,
            effective_balance: 32_000_000_000,
            slashed: true,
            activation_eligibility_epoch: 10,
            activation_epoch: 20,
            exit_epoch: u64::MAX,
            withdrawable_epoch: u64::MAX,
        }
    }

    pub fn sample_list(n: usize) -> Vec<Validator> {
        (0..n).map(|i| {
            let mut v = sample();
            v.activation_epoch = i as u64;
            v
        }).collect()
    }

    pub fn pack_list(list: &[Validator]) -> Vec<u8> {
        let mut out = Vec::with_capacity(list.len() * 121);
        for v in list { v.ssz_pack(&mut out); }
        out
    }

    pub fn unpack_list(bytes: &[u8]) -> Vec<Validator> {
        let mut out = Vec::with_capacity(bytes.len() / 121);
        let mut pos = 0;
        while pos < bytes.len() {
            out.push(Validator::ssz_unpack(&bytes[pos .. pos + 121]).unwrap());
            pos += 121;
        }
        out
    }
}

// ── Lighthouse's ethereum_ssz ───────────────────────────────────────────────

#[cfg(feature = "bench_ethereum_ssz")]
mod lighthouse_impl {
    // Published crate names (`ethereum_ssz` + `ethereum_ssz_derive`) are
    // re-exported as `ssz` + `ssz_derive` — same pattern Lighthouse
    // consensus layer uses in production.
    use ssz::{Decode, Encode};
    use ssz_derive::{Decode as DeriveDecode, Encode as DeriveEncode};
    use ssz_types::{typenum::{U32, U48}, FixedVector};

    // Matches the Lighthouse production Validator layout: FixedVector for
    // the byte arrays (SSZ `Vector[uint8, N]`) rather than `[u8; N]`.
    #[derive(DeriveEncode, DeriveDecode, Debug, PartialEq, Clone)]
    pub struct Validator {
        pub pubkey: FixedVector<u8, U48>,
        pub withdrawal_credentials: FixedVector<u8, U32>,
        pub effective_balance: u64,
        pub slashed: bool,
        pub activation_eligibility_epoch: u64,
        pub activation_epoch: u64,
        pub exit_epoch: u64,
        pub withdrawable_epoch: u64,
    }

    pub fn sample() -> Validator {
        let pk: Vec<u8> = (1..=48).collect();
        let wc: Vec<u8> = (100..132).collect();
        Validator {
            pubkey: FixedVector::new(pk).unwrap(),
            withdrawal_credentials: FixedVector::new(wc).unwrap(),
            effective_balance: 32_000_000_000,
            slashed: true,
            activation_eligibility_epoch: 10,
            activation_epoch: 20,
            exit_epoch: u64::MAX,
            withdrawable_epoch: u64::MAX,
        }
    }

    pub fn sample_list(n: usize) -> Vec<Validator> {
        (0..n).map(|i| {
            let mut v = sample();
            v.activation_epoch = i as u64;
            v
        }).collect()
    }

    pub fn pack_list(list: &[Validator]) -> Vec<u8> {
        let mut out = Vec::with_capacity(list.len() * 121);
        for v in list { v.ssz_append(&mut out); }
        out
    }

    pub fn unpack_list(bytes: &[u8]) -> Vec<Validator> {
        let mut out = Vec::with_capacity(bytes.len() / 121);
        let mut pos = 0;
        while pos < bytes.len() {
            out.push(Validator::from_ssz_bytes(&bytes[pos .. pos + 121]).unwrap());
            pos += 121;
        }
        out
    }
}

// ── ralexstokes's ssz_rs ────────────────────────────────────────────────────

#[cfg(feature = "bench_ssz_rs")]
mod ssz_rs_impl {
    use ssz_rs::prelude::*;

    #[derive(SimpleSerialize, Default, Debug, PartialEq, Clone)]
    pub struct Validator {
        pub pubkey: Vector<u8, 48>,
        pub withdrawal_credentials: Vector<u8, 32>,
        pub effective_balance: u64,
        pub slashed: bool,
        pub activation_eligibility_epoch: u64,
        pub activation_epoch: u64,
        pub exit_epoch: u64,
        pub withdrawable_epoch: u64,
    }

    pub fn sample() -> Validator {
        let mut pk: Vec<u8> = (1..=48).collect();
        let mut wc: Vec<u8> = (100u8..132u8).collect();
        Validator {
            pubkey: pk.try_into().unwrap(),
            withdrawal_credentials: wc.try_into().unwrap(),
            effective_balance: 32_000_000_000,
            slashed: true,
            activation_eligibility_epoch: 10,
            activation_epoch: 20,
            exit_epoch: u64::MAX,
            withdrawable_epoch: u64::MAX,
        }
    }

    pub fn sample_list(n: usize) -> Vec<Validator> {
        (0..n).map(|i| {
            let mut v = sample();
            v.activation_epoch = i as u64;
            v
        }).collect()
    }

    pub fn pack_list(list: &[Validator]) -> Vec<u8> {
        let mut out = Vec::with_capacity(list.len() * 121);
        for v in list { ssz_rs::serialize(v).unwrap().iter().for_each(|b| out.push(*b)); }
        out
    }

    pub fn unpack_list(bytes: &[u8]) -> Vec<Validator> {
        let mut out = Vec::with_capacity(bytes.len() / 121);
        let mut pos = 0;
        while pos < bytes.len() {
            let v: Validator = ssz_rs::deserialize(&bytes[pos .. pos + 121]).unwrap();
            out.push(v);
            pos += 121;
        }
        out
    }
}

// ── Cross-validation: before benching, every impl must produce identical
// bytes for the same sample Validator.

fn run_cross_validation() {
    use psio::ssz::SszPack;

    let psio_bytes = {
        let v = psio_impl::sample();
        let mut out = Vec::new();
        v.ssz_pack(&mut out);
        out
    };
    assert_eq!(psio_bytes.len(), 121, "psio Validator must be 121 bytes");

    #[cfg(feature = "bench_ethereum_ssz")]
    {
        use ssz::Encode;
        let v = lighthouse_impl::sample();
        let bytes = v.as_ssz_bytes();
        assert_eq!(bytes, psio_bytes,
            "lighthouse ethereum_ssz must match psio byte-for-byte");
        eprintln!("✓ ethereum_ssz produces identical Validator bytes");
    }

    #[cfg(feature = "bench_ssz_rs")]
    {
        let v = ssz_rs_impl::sample();
        let bytes = ssz_rs::serialize(&v).unwrap();
        assert_eq!(bytes, psio_bytes,
            "ssz_rs must match psio byte-for-byte");
        eprintln!("✓ ssz_rs produces identical Validator bytes");
    }
}

// ── Criterion benchmarks ───────────────────────────────────────────────────

fn bench_encode(c: &mut Criterion) {
    const LIST_SIZES: &[usize] = &[100, 1_000, 10_000];
    let mut group = c.benchmark_group("validator_encode");

    for &n in LIST_SIZES {
        let list = psio_impl::sample_list(n);
        group.throughput(Throughput::Bytes((n * 121) as u64));

        group.bench_function(format!("psio/{}", n), |b| {
            b.iter(|| black_box(psio_impl::pack_list(&list)));
        });

        #[cfg(feature = "bench_ethereum_ssz")]
        {
            let list = lighthouse_impl::sample_list(n);
            group.bench_function(format!("ethereum_ssz/{}", n), |b| {
                b.iter(|| black_box(lighthouse_impl::pack_list(&list)));
            });
        }

        #[cfg(feature = "bench_ssz_rs")]
        {
            let list = ssz_rs_impl::sample_list(n);
            group.bench_function(format!("ssz_rs/{}", n), |b| {
                b.iter(|| black_box(ssz_rs_impl::pack_list(&list)));
            });
        }
    }
    group.finish();
}

fn bench_decode(c: &mut Criterion) {
    const LIST_SIZES: &[usize] = &[100, 1_000, 10_000];
    let mut group = c.benchmark_group("validator_decode");

    for &n in LIST_SIZES {
        let list = psio_impl::sample_list(n);
        let bytes = psio_impl::pack_list(&list);
        group.throughput(Throughput::Bytes(bytes.len() as u64));

        group.bench_function(format!("psio/{}", n), |b| {
            b.iter(|| black_box(psio_impl::unpack_list(&bytes)));
        });

        #[cfg(feature = "bench_ethereum_ssz")]
        group.bench_function(format!("ethereum_ssz/{}", n), |b| {
            b.iter(|| black_box(lighthouse_impl::unpack_list(&bytes)));
        });

        #[cfg(feature = "bench_ssz_rs")]
        group.bench_function(format!("ssz_rs/{}", n), |b| {
            b.iter(|| black_box(ssz_rs_impl::unpack_list(&bytes)));
        });
    }
    group.finish();
}

fn bench_view_access(c: &mut Criterion) {
    // Zero-copy view access — psio's strength. Compare against whatever
    // zero-copy mechanism the competitor offers (both have partial/lazy
    // decoders to various degrees).
    use psio::ssz_view::{ssz_view_of, SszView};

    let list = psio_impl::sample_list(1_000);
    let bytes = psio_impl::pack_list(&list);
    let mut group = c.benchmark_group("validator_view");
    group.throughput(Throughput::Elements(1_000));

    group.bench_function("psio/view_field_access_1000", |b| {
        use psio_impl::ValidatorSszAccessors;
        b.iter(|| {
            let mut total: u64 = 0;
            for i in 0..1_000 {
                let slice = &bytes[i * 121 .. (i + 1) * 121];
                let v: SszView<psio_impl::Validator> = ssz_view_of(slice);
                total = total.wrapping_add(v.effective_balance().get());
                total = total.wrapping_add(v.activation_epoch().get());
            }
            black_box(total)
        });
    });

    // ethereum_ssz: no zero-copy view; must decode. Fair to compare
    // against the full decode path.
    #[cfg(feature = "bench_ethereum_ssz")]
    group.bench_function("ethereum_ssz/full_decode_1000", |b| {
        let lh_list = lighthouse_impl::sample_list(1_000);
        let lh_bytes = lighthouse_impl::pack_list(&lh_list);
        b.iter(|| {
            let decoded = lighthouse_impl::unpack_list(&lh_bytes);
            let mut total: u64 = 0;
            for v in &decoded {
                total = total.wrapping_add(v.effective_balance);
                total = total.wrapping_add(v.activation_epoch);
            }
            black_box(total)
        });
    });

    #[cfg(feature = "bench_ssz_rs")]
    group.bench_function("ssz_rs/full_decode_1000", |b| {
        b.iter(|| {
            let decoded = ssz_rs_impl::unpack_list(&bytes);
            let mut total: u64 = 0;
            for v in &decoded {
                total = total.wrapping_add(v.effective_balance);
                total = total.wrapping_add(v.activation_epoch);
            }
            black_box(total)
        });
    });

    group.finish();
}

fn bench_root(c: &mut Criterion) {
    run_cross_validation();
    bench_encode(c);
    bench_decode(c);
    bench_view_access(c);
}

criterion_group!(benches, bench_root);
criterion_main!(benches);
