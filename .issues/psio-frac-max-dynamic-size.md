# psio: max_dynamic_size trait + auto-pick frac16 for bounded types

> **Superseded by [pssz-format-design.md](pssz-format-design.md) (2026-04-23).**
> The `max_encoded_size<T>` trait defined below is folded into pSSZ's size
> traits section, and the auto-pick logic is generalized there to choose
> between pssz8 / pssz16 / pssz32 rather than just frac16 / frac32.
> This file is kept for historical context; pick up work from the pSSZ
> design doc.


## Motivation

We've added:
- `PSIO_FRAC_MAX_FIXED_SIZE(T, N)` — per-type commitment to a wider header for
  extensible structs with fixed regions > 64 KiB (Phase D.3, 2026-04-23)
- `bounded_list<T, N>`, `bounded_string<N>`, `bounded_bytes<N>`,
  `bitlist<N>`, `bitvector<N>` — all carry compile-time capacity

The natural next step: for types whose **total encoded size** (fixed + variable
regions) is bounded at compile time, we can compute that bound and use the
narrower `frac_format_16` automatically, halving all offset widths.

## Scope

### 1. `max_encoded_size<T>` trait

Recursive upper bound on the encoded byte count for T:

| T | max_encoded_size<T> |
|---|---|
| primitives (u8…u64, float, double) | `sizeof(T)` |
| uint128 / uint256 | 16 / 32 |
| `std::array<T, N>` | `N * max_encoded_size<T>` |
| `bitvector<N>` / `std::bitset<N>` | `(N + 7) / 8` |
| `bitlist<N>` | `sizeof(length_prefix_t<N>) + (N + 7) / 8` |
| `bounded_list<T, N>` | `sizeof(length_prefix_t<sizeof(T)*N>) + N * max_encoded_size<T>` |
| `bounded_string<N>` | `sizeof(length_prefix_t<N>) + N` |
| `std::optional<T>` | `1 + max_encoded_size<T>` |
| `std::vector<T>` / `std::string` | **undefined** (unbounded) — fall back to frac32 |
| Reflected struct (DWNC) | `Σ max_encoded_size<field>` |
| Reflected struct (non-DWNC) | `sizeof(header) + Σ max_encoded_size<field>` |

Types with any unbounded member (`std::vector`, `std::string`, …) have no
compile-time max — the trait simply reports "unknown" for them.

### 2. `max_total_size<T>()` consteval helper

Combines the fixed region and max_encoded_size of variable fields. Returns
`std::optional<std::size_t>`: `nullopt` when some subfield is unbounded.

### 3. Auto-select format

```cpp
template <typename T>
using auto_frac_format =
    std::conditional_t<bounded_v<T> && *max_total_size_v<T> <= 0xffff,
                       frac_format_16,
                       frac_format_32>;
```

Then `to_frac<T>(value)` picks the right format without user intervention.

## Design questions to resolve

1. **Should the auto-select be opt-in?** Two camps:
   - Opt-in: safer for backwards compat; user must write `to_frac<T,
     auto_frac_format<T>>(v)` or wrap in a macro.
   - Opt-out: `to_frac` picks automatically; user overrides with explicit
     format. Changes wire format silently for existing code when a type
     transitions from unbounded to bounded members.

   Probably opt-in initially. The `PSIO_FRAC_AUTOPICK(T)` macro could flip
   it per-type.

2. **Cross-format embedding.** If struct A auto-picks frac16 and embeds B
   which auto-picks frac32 (because B has a `std::vector`), the mixed-format
   semantics need to be thought through. Probably: embedded types use their
   own format tag; offsets in A's fixed region are 2-byte pointing to B
   content, but B's own internal framing is frac32. Round-trip works because
   both sides agree on what each type's format is.

3. **Schema evolution.** A bounded type growing past 64 KiB total would
   silently flip from frac16 to frac32 — wire format breaks. Same hazard as
   the auto-deduce header width discussed in the D.3 design notes. Resolved
   the same way: `PSIO_FRAC_MAX_TOTAL_SIZE(T, N)` explicit commitment,
   independent of what the compiler can currently deduce.

## Benefits

Typical small-struct wins:

| Type | frac32 size | frac16 size | Saved |
|---|---:|---:|---:|
| Validator (all fixed) | 121 B | 121 B | 0 (no offsets) |
| `bounded_string<32>` alone | 5 B (u32 prefix + content) | already optimal | — |
| Small message with 3 variable fields | 12 B of offsets | 6 B of offsets | 6 B |
| Record with 10 variable fields | 40 B of offsets | 20 B of offsets | 20 B |

The payoff is consistent: half the offset overhead for any record whose
total size fits in u16. Matters most for small high-frequency records
(attestations, transactions, RPC messages).

## Deferred

Work queued; not blocking the Phase C hash_tree_root track. Natural
follow-up after modern-fork BeaconState perf numbers are captured
(because the decision "which format to pick" changes as types get larger,
and real-fork data tells us which types actually stay under the threshold).
