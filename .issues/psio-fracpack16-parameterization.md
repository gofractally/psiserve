# psio: add fracpack16 via Format parametrization

## Goal

Add `fracpack16` — a second fracpack wire format with u16 offsets/sizes
(vs the existing u32). Same layout, same semantics, smaller records.
Use case: database row storage where most records fit in 64KB and every
byte saved multiplies by billions of rows.

## Design (agreed)

- **frac16 = frac32 with `uint32_t` → `uint16_t`.** Nothing else changes:
  same offsets, same extensible-struct `u16 fixed_size` header, same
  reserved offset values (0=empty, 1=None).
- **No wire-level format bit.** The caller knows which format they have
  from context (DB schema, RPC descriptor). If truly ambiguous, use
  `std::variant<Frac32Buf, Frac16Buf>` at the schema level.
- **Cross-format embedding is just an opaque variant arm** — already
  supported by frac32's existing mechanism for arbitrary byte blobs.
- **It's a sibling codec, not a mode.** Picks: `to_frac`/`to_frac16`.

## Implementation plan (Path C — clean-room parametrization)

1. **New file `fracpack_v2.hpp`** with a fully parametrized rewrite:
   `template <typename T, typename Format = frac_format_32>
   struct is_packable;`
2. **Verify byte-identical output** to the legacy `fracpack.hpp` at
   `Format = frac_format_32` by running the existing 326-test suite
   against v2 under a `PSIO_FRACPACK_V2` compile flag.
3. **Once all tests pass**, delete `fracpack.hpp`, rename v2 in its
   place. Add `to_frac16`/`from_frac16`/`validate_frac16` aliases for
   the `frac_format_16` instantiation.
4. **Phase 3** (separate): same treatment for `frac_ref.hpp` views.

## Format tags (drop-in definition)

```cpp
struct frac_format_32 {
   using size_type                        = std::uint32_t;
   static constexpr std::size_t size_bytes      = 4;
   static constexpr std::uint64_t max_buf_bytes = 0xFFFFFFFFull;
};
struct frac_format_16 {
   using size_type                        = std::uint16_t;
   static constexpr std::size_t size_bytes      = 2;
   static constexpr std::uint64_t max_buf_bytes = 0xFFFFull;
};
```

## Edit checklist for `fracpack_v2.hpp`

### Template-parameter threading (~15 specializations)

Add trailing `typename Format = frac_format_32` to each of:

- `is_packable_reflected<T, Reflected, Format>`
- `is_packable<T, Format>`
- `base_packable_impl<T, Derived, Format>`
- `packable_container_memcpy_impl<T, Derived, Format>`
- `packable_container_impl<T, Derived, Format>`
- `packable_associative_container_impl<T, Derived, Format>`
- `packable_sequence_container_impl<T, Derived, Format>`
- `is_packable<std::string, Format>`
- `is_packable<std::string_view, Format>`
- `is_packable<std::span<T>, Format>` (both const and non-const)
- `is_packable<std::vector<T>, Format>` (both memcpy and sequence variants)
- `is_packable<std::array<T, N>, Format>` (both memcpy and non-memcpy variants)
- `is_packable<std::optional<T>, Format>`
- `is_packable<std::tuple<Ts...>, Format>`
- `is_packable<std::variant<Ts...>, Format>`

Inside each specialization body, replace every recursive
`is_packable<X>::…` with `is_packable<X, Format>::…` (~60 call sites
across the file). Replace helper instantiations
(`base_packable_impl<T, is_packable<T>>` etc) similarly.

### Wire-format width substitutions

- `fixed_size = 4` constants on variable-size types →
  `fixed_size = Format::size_bytes` (~6 specializations)
- `write_raw(uint32_t(0))` → `write_raw(typename Format::size_type(0))` (offset placeholder)
- `write_raw(uint32_t(1))` → `write_raw(typename Format::size_type(1))` (Option None sentinel)
- `rewrite_raw(fixed_pos, heap_pos - fixed_pos)` →
  `rewrite_raw(fixed_pos, static_cast<typename Format::size_type>(heap_pos - fixed_pos))`
- `is_packable<uint32_t>::pack(byte_count_or_size, stream)` →
  `is_packable<typename Format::size_type, Format>::pack(..., stream)`
  (String, Vec, Variant data_size)
- `offset < 4` → `offset < Format::size_bytes` (verify_extensions)
- `(end_fixed_pos - fixed_pos) % 4` → `… % Format::size_bytes`

### Unchanged

- `u16 fixed_size` header on extensible structs (already 2 bytes)
- Variant tag (u8, high bit reserved)
- Reserved offset values 0, 1 (fit in both widths)
- Sequential heap layout rules

### Public API (post-swap)

```cpp
// Existing spelled as frac_format_32 instantiation
template<typename T, typename Format = frac_format_32>
std::vector<char> to_frac(const T& value);

template<typename T, typename Format = frac_format_32>
bool from_frac(T& out, std::span<const char> data);

template<typename T, typename Format = frac_format_32>
validation_t validate_frac(std::span<const char> data);

// frac16 aliases
template<typename T> auto to_frac16(const T& v)
   { return to_frac<T, frac_format_16>(v); }
template<typename T> bool from_frac16(T& out, std::span<const char> d)
   { return from_frac<T, frac_format_16>(out, d); }
template<typename T> validation_t validate_frac16(std::span<const char> d)
   { return validate_frac<T, frac_format_16>(d); }
```

## Verification strategy

1. **Byte-identity**: for a suite of test values, assert
   `to_frac<T>(v) == to_frac<T, frac_format_32>(v)` (under the flag).
   Any divergence means the parametrization forgot a Format-dependent
   site.
2. **Round-trip**: `from_frac<T, F>(to_frac<T, F>(v)) == v` for
   `F ∈ {frac_format_32, frac_format_16}`.
3. **Existing 326-test suite** must pass unchanged under v2 — this is
   the main regression guard.
4. **New frac16-specific tests**:
   - Round-trip a struct with u32, string, optional<u64>, vector<u32>
   - Verify packed size is exactly `legacy_size - 2*num_offsets - 2*num_size_fields`
   - Reject values that exceed 64KB total at pack time

## Related rename (Phase 0 — DONE in c7abbdb)

Renamed for API symmetry with `to_X`/`from_X`:

- `fracpack_validate` → `validate_frac`
- `fracpack_validate_compatible` → `validate_frac_compatible`
- `fracpack_validate_strict` → `validate_frac_strict`
- `capnp_validate` → `validate_capnp`
- Test file `fracpack_validate_tests.cpp` → `validate_frac_tests.cpp`

## Scope this session

- ✅ Phase 0 rename (committed)
- ❌ Phase 1 parametrization — not started; scope larger than estimated

## Estimate for next session

Full Phase 1 (pack/unpack only, not views): **~3 hours of careful
editing** with frequent rebuild-verify cycles against the existing test
suite. Phase 3 (views) is another ~1–2 hours. Cross-language rewrites
(Rust/Zig/Go/JS/TS/Moonbit) each follow the same pattern independently.
