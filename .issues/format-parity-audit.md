# Format parity audit — C++ vs Rust vs competitor libraries

Generated 2026-04-23. Updates whenever a format's Rust impl changes.

## Summary matrix

| Format | C++ impl | Rust impl | Competitor compared | Cross-val tests |
|---|---|---|---|---|
| **fracpack** | ✅ full | ✅ full | — (psio-native) | ✅ 6 shapes byte-identical |
| **pssz** | ✅ full | ✅ **full parity** (primitives, ext-int, arrays, bounded, bit types, structs, widths, min/max size, auto-format) | — (new) | ✅ **7+ shapes** incl. nested structs |
| **ssz** | ✅ full + view + validate | ✅ **full parity** (primitives, ext-int, arrays, bounded, bit types, structs, views, validate) | OffchainLabs/sszpp (C++) benched: psio 1.47× enc, 2.04× dec | ✅ **14+ shapes byte-identical** incl. Phase 0 Validator |
| **capnp** | ✅ psio views + native libcapnp | ✅ `psio::capnp` native | libcapnp (C++) benched: psio 3-45× faster | ⚠ Rust self-verify only |
| **flatbuf** | ✅ psio reflect + native flatbuf | ✅ `psio::flatbuf` native | libflatbuffers benched: psio ≥ parity | ⚠ Rust self-verify only |
| **wit** | ✅ canonical_abi | ✅ `psio::wit` full | wit-bindgen / wasm-tools — not benched | ⚠ Rust self-verify only |
| **bincode** | ✅ encoder + decoder | ❌ **missing** | `bincode` crate (rust) — not cross-checked | ⚠ C++ fixtures recorded only |
| **borsh** | ✅ encoder + decoder | ❌ **missing** | `borsh` crate (rust) — not cross-checked | ⚠ C++ fixtures recorded only |
| **avro** | ✅ encoder + decoder | ❌ **missing** | `apache-avro` crate (rust) — not cross-checked | ⚠ C++ fixtures recorded only |
| **pack_bin** (psibase legacy) | ✅ | ❌ **missing** | — | — |
| **json** | ✅ fast path | ✅ (`json.rs`) | serde_json (rust) — not cross-checked | ⚠ psio-only tests |

"byte-identical" means the Rust encoder produces the exact same bytes as
the C++ encoder for the same logical input, verified by a cross-validation
test in `libraries/psio/rust/psio/src/cross_validation_tests.rs` (or the
pssz test module).

## Test coverage details

### C++ tests — `libraries/psio/cpp/tests/`

| File | Subject | Cases |
|---|---|---:|
| `frac16_tests.cpp` | fracpack round-trip per type | ~40 |
| `frac16_fuzz_generated.cpp` | fracpack fuzzer | many |
| `frac16_mutation_tests.cpp` | in-place mutation | ~20 |
| `validate_frac_tests.cpp` | validator edge cases | ~30 |
| `view_tests.cpp` | frac_view zero-copy | ~25 |
| `frac_ref_tests.cpp` | frac_ref mutable view | ~10 |
| `psio_structural_tests.cpp` | PSIO_REFLECT coverage | ~20 |
| `psio_attribute_tests.cpp` | field attributes | ~10 |
| `ssz_tests.cpp` | SSZ round-trip | ~28 |
| `pssz_tests.cpp` | pSSZ round-trip + view | 10 |
| `beacon_state_tests.cpp` | Eth BeaconState decode | ~5 |
| `capnp_view_tests.cpp` | capnp views | ~15 |
| `capnp_schema_tests.cpp` | capnp schema emit | ~10 |
| `capnp_parser_tests.cpp` | capnp schema parse | ~10 |
| `fbs_parser_tests.cpp` | flatbuf schema parse | ~10 |
| `wit_attribute_tests.cpp` | wit attributes | ~10 |
| `wit_resource_tests.cpp` | wit resources | ~10 |
| `wit_variant_tests.cpp` | wit variants | ~10 |
| `bounded_tests.cpp` | bounded_* classes | ~10 |
| `ext_int_tests.cpp` | uint128/int128/uint256 | ~10 |
| `bitset_tests.cpp` | bitvector/bitlist | ~10 |
| `schema_bounded_tests.cpp` | schema w/ bounds | ~10 |
| `json_tests.cpp` | JSON I/O | ~15 |
| `bin_extensibility_tests.cpp` | pack_bin forward-compat | ~10 |
| `multiformat_tests.cpp` | multi-format parity | ~5 |

**Total: ~700 cases, 83K assertions.**

### Rust tests — `libraries/psio/rust/psio/src/`

| File | Subject | Cases |
|---|---|---:|
| `wit/conformance_tests.rs` | WIT Canonical ABI conformance | 148 |
| `flatbuf/mod.rs` | flatbuf internals | 61 |
| `capnp/mod.rs` | capnp internals | 52 |
| `dynamic_view.rs` | runtime dispatch | 48 |
| `view.rs` | frac_view | 25 |
| `json.rs` | JSON I/O | 24 |
| `schema_import/wit_parser.rs` | wit schema parse | 21 |
| `dynamic_schema.rs` | dynamic schema | 18 |
| `wit/view.rs` | wit views | 15 |
| `schema_import/fbs_parser.rs` | flatbuf schema parse | 15 |
| **`pssz.rs`** | **pSSZ (new this session)** | **13 (incl 6 cross-val)** |
| `schema_import/capnp_parser.rs` | capnp schema parse | 12 |
| **`cross_validation_tests.rs`** | **fracpack C++↔Rust (new this session)** | **6** |
| (other modules: 10–40 each) | … | … |

**Total: 528 tests.**

## Areas where Rust tests lag C++

### High-impact gaps

1. **SSZ — bootstrap port now in place (2026-04-23).** Rust module
   `libraries/psio/rust/psio/src/ssz.rs` covers primitives (int/uint
   8/16/32/64, f32, f64, bool), String, Vec, Option. 11 tests total,
   6 cross-validate against C++ hex fixtures and produce byte-identical
   output.

   **Still missing** compared to C++: bitvector/bitlist, reflected
   structs via derive macro, ssz_view zero-copy, ssz_validate, ext_int
   (uint256). Those are the next chunks.

2. **ext_int (uint128/int128/uint256) — no Rust impl.** C++ has custom
   structs + round-trip tests. Rust would need:
   - uint128 = `u128` native
   - int128 = `i128` native
   - uint256 needs a dedicated struct (4 × u64)

3. **bincode / borsh / avro / pack_bin — no Rust psio impl.** These
   formats have canonical Rust crates (bincode, borsh, apache-avro).
   Cross-validation would mean: psio C++ encode → Rust decode via the
   upstream crate. If they disagree, either psio's encoder deviates
   from the spec or the fixture is wrong. Worth doing for the spec-
   defined ones (bincode, borsh).

### Lower-impact gaps

4. **capnp / flatbuf / wit** — Rust has native impls with good test
   coverage, but no tests confirm the Rust encoder's output is byte-
   identical to C++'s on the same input. The per-language tests are
   self-consistent (Rust round-trips Rust) but could drift. A light
   cross-validation suite would be valuable.

5. **JSON** — Rust has it but not cross-validated against C++'s
   `to_json_fast`. Might differ on float formatting, field ordering
   in maps, etc.

## Wire format fingerprint reference

Emitted by `/tmp/xval/emit_fixtures.cpp` on 2026-04-23. Each row is
hex bytes. Use these to cross-check any new Rust implementation.

### `u32{0xDEADBEEF}` — identical across every LE format

All formats: `efbeadde`.

### `string("hello")`

| Format | Hex | Size | Note |
|---|---|---:|---|
| fracpack | `0500000068656c6c6f` | 9 | 4-byte LE length + bytes |
| fracpack16 | `050068656c6c6f` | 7 | 2-byte LE length + bytes |
| bincode | `050000000000000068656c6c6f` | 13 | 8-byte u64 LE length |
| borsh | `0500000068656c6c6f` | 9 | 4-byte u32 LE length |
| ssz | `68656c6c6f` | 5 | implicit sizing — no prefix |
| pssz32 | `68656c6c6f` | 5 | same as ssz |

### `Vec<u32>{1, 2, 3}`

| Format | Hex | Size |
|---|---|---:|
| fracpack | `0c000000` + `010000000200000003000000` | 16 |
| fracpack16 | `0c00` + `010000000200000003000000` | 14 |
| bincode | `0300000000000000` + data | 20 |
| borsh | `03000000` + data | 16 |
| ssz | `010000000200000003000000` | 12 |
| pssz32 | `010000000200000003000000` | 12 |

### `Option<u32>{Some(42)}`

| Format | Hex | Size | Encoding |
|---|---|---:|---|
| fracpack | `040000002a000000` | 8 | pointer(4)→payload |
| bincode | `012a000000` | 5 | selector + payload |
| borsh | `012a000000` | 5 | selector + payload |
| ssz | `012a000000` | 5 | Union[null, T] |
| pssz32 | `2a000000` | 4 | no selector, fixed inner |

### `Option<u32>{None}`

| Format | Hex | Size |
|---|---|---:|
| fracpack | `01000000` | 4 — sentinel |
| bincode | `00` | 1 |
| borsh | `00` | 1 |
| ssz | `00` | 1 — Union tag |
| pssz32 | *(empty)* | 0 — min-size rule |

### Extensible struct `{name: "alice", value: 77}`

| Format | Hex | Size |
|---|---|---:|
| fracpack | `08000800` + `00004d000000` + `05000000616c696365` | 21 |
| bincode | `05000000000000006` + `16c6963654d000000` | 17 |
| borsh | `0500000061` + `6c6963654d000000` | 13 |
| ssz | `08000000` + `4d000000616c696365` | 13 |
| pssz32 | `08000000` + `080000004d000000616c696365` | 17 |

## Recommended next steps (prioritized)

1. **Port SSZ to Rust** — biggest gap; major use case. Follow the
   pSSZ bootstrap pattern: primitives → String → Vec → Option → later
   a derive macro. Cross-validation fixtures already above.
2. **Add cross-validation for wit/capnp/flatbuf** — low effort, high
   confidence payoff. Use the C++ emitters already in the repo.
3. **Add ext_int to Rust** — trivial for u128/i128 (native), needs a
   small struct for uint256.
4. **Decide on bincode / borsh / avro / pack_bin in Rust** — are these
   worth native psio impls, or can we accept that Rust callers use the
   upstream crates and rely on C++ for these formats in mixed pipelines?

## Commits and files touched this audit

- `/tmp/xval/emit_fixtures.cpp` — fixture generator (temp, not committed)
- `libraries/psio/rust/psio/src/cross_validation_tests.rs` — new,
  6 fracpack cross-val tests
- `libraries/psio/rust/psio/src/lib.rs` — registered the module
- `.issues/format-parity-audit.md` — this file

All 534 Rust tests pass (528 prior + 6 new cross-val). All 703 C++ tests pass.
