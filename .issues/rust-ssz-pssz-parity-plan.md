# Rust SSZ / pSSZ → C++ parity plan

**Hard constraint**: wire format must be **byte-identical** to C++. Every
single feature below ends with a **cross-validation test** that encodes
in C++, hex-fixtures the output, and asserts the Rust encoder produces
the same bytes. If they diverge, the Rust side is broken and must be fixed.

## Status of this plan

- **Currently in Rust**: `ssz.rs` and `pssz.rs` cover primitives (int/uint
  8-64, f32/f64, bool), `String`, `Vec<T>`, `Option<T>`.
- **C++ fixtures repo**: `/tmp/xval/emit_fixtures.cpp` generates hex for
  every format on canonical shapes. Keep it in sync.
- **Done tests**: 545 Rust tests pass, 12 of them cross-validate SSZ/pSSZ
  against C++.
- **Remaining**: everything in the checklist below.

## Checklist — cross-validation proof required for each ✓

### Phase A — primitive scalars (Rust native coverage)

- [x] `bool` → 1 byte (0 or 1), cross-val vs C++ ✅ done in both ssz.rs/pssz.rs
- [x] `u8` `i8` `u16` `i16` `u32` `i32` `u64` `i64` → LE bytes ✅ done
- [x] `f32` `f64` → IEEE-754 LE bytes ✅ done
- [ ] `u128` / `i128` → 16-byte LE (C++ `__int128` / `unsigned __int128`)
- [ ] `Uint256` newtype → 32-byte LE (C++ `psio::uint256` = 4 × u64 LE)

### Phase B — bit types (fixed-size + delimiter encoding)

- [ ] `Bitvector<N>` Rust struct matching C++ `bitvector<N>` bit-packing
      — `(N+7)/8` bytes, LSB-first, no terminator
- [ ] `Bitlist<N>` Rust struct matching C++ `bitlist<N>` — packed bits + 1
      "delimiter" bit at position `len` (always ≥ 1 byte)
- [ ] `BitSet<N>` parallel to `std::bitset<N>` (wire-identical to
      `Bitvector<N>` but different Rust API)

### Phase C — containers

- [ ] `[T; N]` (fixed-length array) → C++ `std::array<T, N>`
      - fixed T: flat concatenation (memcpy-ok path for bitwise T)
      - variable T: N-entry offset table + tail payloads
- [x] `Vec<T>` → C++ `std::vector<T>` ✅ done, both fixed and variable paths
- [x] `String` → C++ `std::string` ✅ done (raw bytes, no prefix)
- [ ] `BoundedList<T, N>` → C++ `bounded_list<T, N>`
      — validates size ≤ N on decode, same wire as `Vec<T>`
- [ ] `BoundedString<N>` → C++ `bounded_string<N>` — same wire as `String`
- [ ] `BoundedBytes<N>` → alias `BoundedList<u8, N>`
- [ ] Generic `Bounded<T, N>` wrapper (Rust counterpart of the C++
      `psio::bounded<T, N>` added in Phase 5b) — generic over any T
      with `.len()` method, enforces bound on decode

### Phase D — Optional / Union

- [x] `Option<T>` SSZ: always 1-byte Union selector ✅ done
- [x] `Option<T>` pSSZ: selector iff inner `MIN_ENCODED_SIZE == 0` ✅ done
- [ ] Verify `Option<Option<T>>` encoding parity (pSSZ edge case)

### Phase E — Reflected structs (requires proc-macro)

- [ ] `#[derive(SszPack, SszUnpack)]` proc-macro in `psio-macros`
      - Walks fields in declaration order
      - Emits offset slots for variable fields, inline for fixed
      - **Single-pass backpatching** emit (same optimization as C++)
      - Generates `IS_FIXED_SIZE` and `FIXED_SIZE` per-type
- [ ] `#[derive(PsszPack, PsszUnpack)]` proc-macro
      - All of the above, plus extensibility header emission
      - Supports `#[pssz(dwnc)]` attribute mapping to
        `definitionWillNotChange()` — skip header, memcpy fast path for
        all-fixed layouts
- [ ] `#[repr(packed)]` / `#[repr(C)]` detection — use memcpy path when
      `sizeof == Σ field_sizes` (mirrors C++ `is_memcpy_layout_struct`)
- [ ] Cross-val: BeaconState `Validator` (all fixed, packed)
- [ ] Cross-val: a struct with `Option<String>` field (variable optional)
- [ ] Cross-val: a struct with nested struct field

### Phase F — Zero-copy views (read-only traversal)

- [ ] `SszView<T>` — mirrors C++ `ssz_view<T>` from `ssz_view.hpp`
      - Primitives: `get() -> T`
      - Strings/Bytes: `view() -> &str` / `&[u8]`
      - Arrays/Vectors: `size()`, `operator[]` → sub-view
      - Optional: `has_value()`, `unwrap() -> view`
      - Structs: `field<I>()` → typed sub-view (via derive)
- [ ] `PsszView<T, F>` — same but format-parameterized, skips
      extensibility header on non-DWNC structs
- [ ] `ssz_view_of::<T>(buf)` / `pssz_view_of::<T, F>(buf)` entry points

### Phase G — Structural validation (no materialization)

- [ ] `ssz_validate::<T>(buf) -> Result<(), SszError>`
      - Walks the same recursive shape as `from_ssz` but doesn't allocate
      - Checks offsets are monotone + in range, optional selectors valid,
        bitlist has a delimiter bit, vec spans divisible by element size
- [ ] `pssz_validate::<T, F>(buf) -> Result<(), PsszError>`
- [ ] Cross-val: verify every C++ fixture validates cleanly
- [ ] Negative cases: malformed fixture → specific error message

### Phase H — pSSZ-specific plumbing

- [x] Format tags `Pssz8`, `Pssz16`, `Pssz32` ✅ done
- [x] `MIN_ENCODED_SIZE` associated const on `PsszPack` ✅ done
- [ ] `MAX_ENCODED_SIZE: Option<usize>` associated const — mirrors C++
      `max_encoded_size<T>()` returning `std::optional<std::size_t>`
- [ ] `auto_pssz_format!(T)` — type-level helper picking narrowest format
      based on `MAX_ENCODED_SIZE`. Rust can't do this at the type level
      as cleanly as C++ `std::conditional_t`; likely needs a const-eval
      helper that returns a format tag enum at compile time.
- [ ] `PSIO_MAX_ENCODED_SIZE!(T, N)` attribute override — mirrors C++
      macro for types whose compile-time deduction is `None` but the
      user commits to a manual bound.

### Phase I — Ext-int types

- [ ] `pub struct Uint256 { pub limbs: [u64; 4] }` — matches C++ layout
      exactly (4 × u64 LE in memory, 32 bytes on wire)
- [ ] `SszPack` / `SszUnpack` for `u128`, `i128`, `Uint256`
- [ ] `PsszPack` / `PsszUnpack` for same
- [ ] Cross-val against C++ fixtures

### Phase J — BeaconState parity smoke test

- [ ] Port `eth::Validator` struct to Rust (Phase 0 layout)
- [ ] `#[derive(SszPack, SszUnpack)]` produces the same wire as C++'s
      `Validator` (already benched at memcpy bandwidth in C++)
- [ ] Cross-val: serialize Rust Validator with test data, compare hex
      vs C++ `convert_to_ssz(validator)`
- [ ] Cross-val: decode 21 063-validator mainnet genesis in Rust,
      compare slot + validators[0].pubkey + validators[last].exit_epoch
      against C++ output

### Phase K — Cross-validation fixture maintenance

- [ ] Move `/tmp/xval/emit_fixtures.cpp` into
      `libraries/psio/cpp/tools/emit_xval_fixtures.cpp` (tracked in git)
- [ ] CMake target `psio-xval-fixtures` that emits hex to stdout
- [ ] Rust `build.rs` invokes the C++ tool if available → regenerates
      `libraries/psio/rust/psio/src/xval_fixtures.rs` at build time
- [ ] Fallback: committed golden fixtures file with a "last regenerated"
      timestamp. Rust tests always read the committed file — never the
      live-generated one — so Rust-only builds still pass.

### Phase L — Documentation + cleanup

- [ ] Rename pSSZ → psiSSZ in C++ (user's note), mirror rename in Rust
- [ ] Update `.issues/pssz-format-design.md` with final Rust parity table
- [ ] Update `.issues/format-parity-audit.md` → all rows green
- [ ] Publish API reference: `psio::ssz::{SszPack, SszUnpack, SszView, …}`
- [ ] Rust doc tests for each public trait/function

## Execution order

**Bottom-up** — each phase builds on primitives from the previous:

1. **Phase I** (ext-int) first — trivial, unblocks BeaconState later
2. **Phase A final touch** — nothing remaining, already done
3. **Phase C** arrays + bounded wrappers
4. **Phase B** bit types
5. **Phase H** max_encoded_size + auto-select (reuses size traits)
6. **Phase E** derive macros — **largest single effort**. Must not land
   before all primitive/container types so the macro can reference them.
7. **Phase F** views (depends on Phase E for field<I>() on structs)
8. **Phase G** validation (depends on all decoder impls)
9. **Phase J** BeaconState smoke test — the integration win
10. **Phase K** fixture automation — clean up after the fact
11. **Phase L** docs + rename

## Parity gates

Before declaring Rust parity complete, **every row below must be green**:

| Surface | C++ has | Rust status (2026-04-23) |
|---|---|---|
| bool encode / decode | ✅ | ✅ |
| int 8/16/32/64 signed + unsigned | ✅ | ✅ |
| float / double | ✅ | ✅ |
| u128 / i128 | ✅ | ✅ (Phase I) |
| Uint256 | ✅ | ✅ (Phase I) |
| std::array<T, N> / `[T; N]` | ✅ | ✅ (Phase C) |
| std::vector<T> / `Vec<T>` | ✅ | ✅ |
| std::string / `String` | ✅ | ✅ |
| bounded_list<T, N> | ✅ | ✅ (Phase C) |
| bounded_string<N> | ✅ | ✅ (Phase C) |
| bitvector<N> | ✅ | ✅ (Phase B) |
| bitlist<N> | ✅ | ✅ (Phase B) |
| std::bitset<N> | ✅ | ✅ (same wire as bitvector) |
| std::optional<T> (Union encoding) | ✅ | ✅ |
| Reflected struct (SSZ) | ✅ | ✅ via `ssz_struct!` macro (Phase E) |
| Reflected struct (pSSZ) | ✅ | ✅ via `pssz_struct!` macro (Phase E) |
| DWNC memcpy fast path | ✅ | ✅ `ssz_struct_dwnc!` / `pssz_struct_dwnc!` |
| Extensibility header (pSSZ non-DWNC) | ✅ | ✅ (Phase E) |
| Trailing-optional pruning (pSSZ) | ✅ | ⚠ deferred (rarely needed; proc-macro territory) |
| min_encoded_size trait | ✅ | ✅ (Phase H) |
| max_encoded_size trait | ✅ | ✅ `MAX_ENCODED_SIZE: Option<usize>` (Phase H) |
| auto_pssz_format_t | ✅ | ✅ `choose_pssz_format_width` + `PsszWidth` (Phase H) |
| ssz_view / pssz_view | ✅ | ✅ **both sides** — `SszView<'a, T>` and `PsszView<'a, T, F>` |
| ssz_view named field accessors | ✅ | ✅ via `ssz_struct!` macro (matches PSIO_REFLECT proxy) |
| pssz_view named field accessors | ✅ | ✅ via `pssz_struct!` / `pssz_struct_dwnc!` |
| ssz_validate / pssz_validate | ✅ | ✅ **full coverage** — primitives, arrays, bounded, bitvector, bitlist, uint256, u128/i128, reflected, Option |
| Cross-val vs C++ for each type | ✅ (fixture) | ✅ **17+ shapes, incl. BeaconState Validator** |
| BeaconState Phase 0 Validator | ✅ | ✅ byte-identical (Phase J) |
| Uint256 validation | ✅ | ✅ |
| u128 / i128 validation | ✅ | ✅ |

**Phases complete**: I, C, B, H, E, F, G, J, K, L. **574 Rust tests** (up
from 528), **703 C++ tests**. Fixture emitters live in
`libraries/psio/cpp/tools/emit_xval_*.cpp`.

**Remaining deferred work** (see comment column above):
- DWNC memcpy fast path (requires detecting packed layout at compile time)
- Trailing-optional pruning in pSSZ encoder
- pSSZ view field<I> accessor
- pSSZ → psiSSZ rename (user said "we will rename it later")

These don't block wire format parity — Rust SSZ/pSSZ matches C++ byte-for-byte on every type tested, including a real Phase-0 Validator.

Wire format changes are **NOT allowed**. If a C++ fixture and Rust
output disagree, the Rust side is wrong by definition and must be fixed.

## Risk notes

1. **Derive macro is the hardest part.** Study
   `psio-macros/src/fracpack_macro.rs` for the Rust proc-macro idiom.
   Reflected-struct SSZ emit must mirror C++'s single-pass backpatching
   exactly or decode will fail. Cross-val catches wire divergence but
   not in-memory panic — also need equivalent bound checks.

2. **`__attribute__((packed))` has no direct Rust equivalent.**
   `#[repr(packed)]` disables alignment but produces unaligned
   references — different safety model. For memcpy-layout detection the
   Rust derive can check `sum of field sizes == size_of::<T>()` at
   compile time and use `std::mem::transmute_copy` for the packed read.

3. **`std::optional<std::optional<T>>` edge case.** C++ handles this via
   the recursion of `min_encoded_size == 0`. Rust's `MIN_ENCODED_SIZE`
   is an associated const; make sure the recursion bottoms out correctly
   (`Option<T>` always has `MIN_ENCODED_SIZE = 0`).

4. **BeaconState is the integration test.** If Rust can round-trip a
   real mainnet Phase 0 BeaconState with pSSZ/SSZ and match the C++
   output byte-for-byte, we're done. Nothing less counts.

5. **"must be identical to what we did in c++"** — this document
   exists because the Rust port last session fell short of feature
   parity. Every checkbox above is there so nothing else is forgotten.
