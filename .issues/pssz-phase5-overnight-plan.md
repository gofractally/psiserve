# pSSZ Phase 5 — overnight work plan

User went to bed 2026-04-23. Wants full report in the morning covering:

1. **pssz_view** zero-copy accessor (task #55)
2. **bounded<T, N>** unification (task #56)
3. **Rust feature parity** audit + pSSZ port (task #57)
4. Final report + doc update (task #58)

## Execution order (to maximize useful completion)

1. **pssz_view first** — smallest scope, additive, tests cleanly → less regression risk
2. **bounded<T, N> unification second** — larger refactor but well-contained
3. **Rust audit + porting third** — scope unknown until audited
4. **Report + docs last**

## Done criteria per phase

### 5a: pssz_view
- [ ] `psio/pssz_view.hpp` with `pssz_view<T, F>::field<I>()` for fixed and variable fields
- [ ] `psio/from_pssz.hpp` gains `pssz_validate<T, F>(buffer)` that checks structural integrity without materializing
- [ ] Unit tests in `pssz_tests.cpp`: view a nested struct, read every field, compare to decoded copy
- [ ] Bench rows for `pssz-view-all` / `pssz-view-one` in `bench_fracpack.cpp`
- [ ] All 699+ existing tests still pass

### 5b: bounded<T, N>
- [ ] `bounded<T, N>` in `psio/bounded.hpp` — generic wrapper holding a T (string/vector/array/etc) with compile-time cap N
- [ ] Type aliases preserved: `bounded_string<N> = bounded<std::string, N>`, `bounded_list<T,N> = bounded<std::vector<T>, N>`, `bounded_bytes<N> = bounded<std::vector<uint8_t>, N>`
- [ ] All format codecs (pack_bin, bincode, borsh, avro, ssz, pssz, fracpack, schema.hpp) updated to use generic unwrap
- [ ] All existing tests still pass — wire format must be byte-identical

### 5c: Rust parity
- [ ] Audit checklist of C++ features present in Rust:
  - ext_int (u128/i128/u256)
  - bounded_*
  - SSZ encode/decode
  - pSSZ encode/decode
  - Varint/bincode/borsh/avro if applicable
- [ ] Port the highest-leverage missing piece (likely pSSZ since it's new and benefits from matched design)
- [ ] Cargo test passes on `libraries/psio/rust/`
- [ ] Round-trip compatibility test: encode in C++, decode in Rust (and vice-versa) for at least one pSSZ type

### 5d: Report
- [ ] Update `.issues/pssz-format-design.md` with final numbers and any design-time learnings
- [ ] If significant findings, add to MEMORY.md
- [ ] Final consolidated report as the user's "morning read"

## Risk and fallback policy

- If 5b or 5c blows up scope, stop cleanly, note the stopping point in `.issues/pssz-format-design.md`, and write an honest partial-completion report. **Don't ship half-working code.**
- Do not break existing tests. If a refactor introduces a regression, revert and document.
- Do not skip-hooks on any commit (CLAUDE.md rule).
- Verify round-trip byte identity on every format touched.

## Quick wins to bank first

- Keep pssz_view additive — don't touch existing headers.
- For bounded unification: do it with the aliases strategy (new type under the hood, legacy names still resolve) so blast radius is minimized.
- For Rust: start by reading the README and src/lib.rs to understand the existing structure. Only port what has unambiguous C++ equivalents.
