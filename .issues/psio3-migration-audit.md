# psio v1 → psio3 migration audit

Generated 2026-04-26.  Goal: psio3 is the *complete* replacement for v1 —
enhanced API, modularity, and performance — with **nothing left behind
unintentionally**.  This document is the running checklist; every v1
header has a row, classified as ported / supersede-with-rationale /
needs-port / explicit-cut.

Items flagged **REVIEW** below the relevant table need your sign-off
before they're closed.

## Header inventory: status by file

### Ported and feature-equivalent (or richer)

| v1 header | v3 location | notes |
|---|---|---|
| `bitset.hpp` | `wrappers.hpp` (`psio3::bitvector<N>` / `bitlist<N>`) | folded with other size-bounded wrappers |
| `bounded.hpp` | `wrappers.hpp` (`psio3::bounded<T,N>`, `utf8_string<N>`, `byte_array<N>`) | richer types; codecs need to recognise them (followup task) |
| `ext_int.hpp` | `ext_int.hpp` | direct port |
| `from_avro.hpp` + `to_avro.hpp` | `avro.hpp` | one tag header per format; encode + decode + validate via CPOs |
| `from_bin.hpp` + `to_bin.hpp` | `bin.hpp` | as above |
| `from_bincode.hpp` + `to_bincode.hpp` | `bincode.hpp` | |
| `from_borsh.hpp` + `to_borsh.hpp` | `borsh.hpp` | |
| `from_key.hpp` + `to_key.hpp` | `key.hpp` | wire byte-identical (`psio3_v1_parity_key_tests` 13/13) |
| `from_pssz.hpp` + `to_pssz.hpp` | `pssz.hpp` (+ `pssz_view.hpp`) | unified into one tag with `pssz-auto` width selection |
| `from_ssz.hpp` + `to_ssz.hpp` | `ssz.hpp` (+ `ssz_view.hpp`) | |
| `from_json.hpp` + `to_json.hpp` + `to_json_fast.hpp` | `json.hpp` | one fast-path encoder + decoder |
| `varint.hpp` | `varint/{leb128,prefix2,prefix3}.hpp` | **richer** — three algorithms, NEON fast path, microbenched |
| `view.hpp` | `view.hpp` | capability-tier API (read-only / mut-borrow / owning) |
| `reflect.hpp` | `reflect.hpp` | `attr(F, specs)` / `definitionWillNotChange()` keyword dispatch (last commit) |
| `stream.hpp` | `stream.hpp` | direct port |
| `pssz_view.hpp` | `pssz_view.hpp` | direct port |
| `ssz_view.hpp` | `ssz_view.hpp` | direct port |
| `flatbuf.hpp` + `to_flatbuf.hpp` | `flatbuf.hpp` + `flatbuf_lib.hpp` | reflect-based serializer + native libflatbuffers adapter |
| `capnp_view.hpp` (subset) | `capnp.hpp` | psio reflect + libcapnp adapter; **see open items** |
| `tuple.hpp` | folded into format codecs | `std::tuple` support is in `wit.hpp` and elsewhere directly |

### Subdirectory contents

| v1 path | v3 disposition |
|---|---|
| `from_json/map.hpp` | folded into `json.hpp` |
| `to_json/map.hpp` | folded into `json.hpp` |
| `json/any.hpp` | dynamic JSON value lives in `dynamic_json.hpp` / `dynamic_value.hpp` |
| `detail/layout.hpp` | folded into per-codec impl (`is_memcpy_layout_struct` predicates) |
| `detail/run_detector.hpp` | DWNC fast paths inlined in each codec (`bin`, `frac`, `borsh`, `bincode`) |

---

### Needs port (high priority)

These are real features actively used downstream that have no v3
equivalent.  No reason to drop them.

- [ ] **`name.hpp`** — compressed name identifier.  Encodes ≤18-char
      lowercase-alphanum-hyphen strings into a u64 via context-dependent
      arithmetic coding (445 lines, MIT-licensed from Mark Thomas Nelson,
      adapted by you).  **Used in `pfs/keys.hpp`, `pfs/store.hpp` as
      `psio::name_id` for the universal tenant identifier.**  Likely
      also load-bearing in psizam / psiserve / psitri / psi-api.
      Direct port — no dependencies on the rest of v1 beyond stdlib.

- [x] **`compress_name.hpp`** — method-name compressor.  503 lines,
      MIT-licensed.  **DISTINCT** from `name.hpp`: different alphabet
      (27 symbols, lowercase a-z only) and different frequency tables
      → produces different u64 outputs for the same input.  Pairs with
      `name.hpp`: `name` encodes general identifiers (a-z, 0-9, hyphen
      — namespaces, accounts, paths), `compress_name` encodes method
      names (pure-letter — RPC method dispatch).  Public API:
      `method_to_number(string_view) → u64`, `number_to_method(u64) →
      string`, `is_hash_name(u64)` (when name didn't compress, value
      is a hash).  Will be needed when v3 acquires RPC method dispatch
      and/or `get_type_name` ports (which uses
      `detail::method_to_number` for non-reflected types).  Direct
      port, mechanical.  Earlier classification as "SUPERSEDED" was
      wrong.

- [ ] **`structural.hpp`** — `PSIO_PACKAGE(name, version)`,
      `PSIO_INTERFACE`, etc. — L2 schema declaration macros (437 lines).
      Required for the schema-as-contract workflow (the "single source
      of truth" path that emits WIT and validates against it).

- [ ] **`emit_wit.hpp`** — schema IR → WIT text emitter.  Required if
      we want C++ types → WIT files.

- [ ] **`wit_parser.hpp`** — recursive-descent WIT text parser.
      `wit_parse(text) → wit_world`.  Required for schema-import
      workflow ("read this `.wit`, generate matching C++ stubs").

- [ ] **`wit_encode.hpp`** — `wit_world` → Component Model binary.
      Required for emitting deployable Component artefacts (vs only
      consuming the canonical ABI which `wit.hpp` already does).

- [ ] **`wit_gen.hpp`** — generate WIT text from PSIO-reflected C++
      types.  Used by `psi-api/db.hpp` (`psio::generate_wit_binary<store>`).
      Foundational for the "API definition is C++ types" pattern.

- [x] **`wit_types.hpp`** — WIT IR data structures.  `wit_prim` enum
      (13 primitives), `wit_type_kind` enum (11 kinds including
      resource/own/borrow), `wit_attribute` / `wit_named_type` /
      `wit_type_def` / `wit_func` / `wit_interface` / `wit_world`
      aggregate structs all reflected via PSIO3_REFLECT.  Foundation
      for the rest of the WIT toolchain.  Test: psio3_wit_types_tests
      (74 assertions / 10 cases — primitive idx round-trip, kind enum
      stability, reflection wiring on every struct, in-memory
      construction sample).  Includes `pzam_wit_world` back-compat
      alias for psizam consumers.

- [x] **`wit_resource.hpp`** — `psio3::wit_resource`, `psio3::own<T>`,
      `psio3::borrow<T>`.  Ported as `psio3/wit_resource.hpp`.  The
      "real surface area" is ~50 lines of code (vs 299 of doc):
      empty marker base, is_wit_resource_v trait, RAII own<T>,
      bare borrow<T>, and a wit_resource_drop<T> customisation point
      that defaults to no-op.  wasm_type_traits<own<T>> /
      wasm_type_traits<borrow<T>> stay in `namespace psizam` (already
      did, runtime concern).  Resolves the "psio3 or separate
      library?" question — psio3 owns the vocabulary, psizam owns
      the runtime traits + handle table.  Test:
      psio3_wit_resource_tests (5 cases / 15 assertions including
      RAII drop count, move semantics, release()).

- [ ] **`wit_owned.hpp`** + **`wview.hpp`** — owning value + WASM
      Canonical ABI projection types.  Counterpart to `view<T, wit>`
      with ownership semantics.  Used by guest-side WASM glue.

- [ ] **`wit_view.hpp`** — runtime view for WIT-encoded data.  May
      already be covered by `view<T, wit>` once `wit.hpp` is wired
      into the `view<T, Fmt>` framework.  **REVIEW after porting.**

- [ ] **`wit_constexpr.hpp`** — constexpr WIT text generator.  Used to
      embed WIT text in compiled binaries.

- [ ] **`wit_types.hpp`** — shared WIT type tags.  Likely a small port.

- [ ] **`canonical_abi.hpp`** (1169 lines) — full canonical ABI:
      compile-time layout / serialize / validate / **rebase**.
      `wit.hpp` covers encode/decode (1010 lines) but rebase (move
      buffer-relative pointers when the host moves the buffer) and
      some edge-case validation may be missing.  **REVIEW**: compare
      v1 canonical_abi.hpp surface against v3 wit.hpp; port any
      missing operations.

- [ ] **`attributes.hpp`** — WIT attribute registries (231 lines).
      Two registries opt-in via ADL overloads.  Needed before the
      WIT generator side can attach C++-side attribute metadata.

- [→psizam] **`guest_alloc.hpp`** — `cabi_realloc` for WASM guest.
      Pure WASM concern (`__heap_base` + `__builtin_wasm_memory_grow`),
      no psio dependency.  **Belongs in psizam**, not psio3 — psizam
      already owns the host-side WASM glue (host_function,
      canonical_dispatch, component, runtime).  Tracked under psizam.

- [→psizam] **`guest_attrs.hpp`** — clang attribute macros
      (`import_module` / `import_name` / `export_name`) for guest
      WASM ABI wiring.  Pure compiler-attribute spelling, no psio
      surface.  **Belongs in psizam**.  Tracked under psizam.

- [ ] **`schema.hpp` (full)** — v1 has a 2521-line `schema.hpp` with
      `SchemaBuilder`, schema diff, WIT roundtrip.  v3's is 310 lines
      (Phase 14a — runtime schema value + static bridge).  Missing:
      `SchemaBuilder` programmatic API, schema-diff, schema-to-WIT
      roundtrip.  Already tracked separately as Phase 14b/14c.

- [ ] **`fracpack.hpp`** (2602 lines) — v1's main format.  v3 has
      `frac.hpp`.  Wire bytes parity confirmed via
      `psio3_v1_parity_*_tests`, but **`frac.hpp` does NOT yet support
      `vector<variable>`** (see `frac_supports<V3Order>` in
      bench_perf_report.cpp).  Need feature-completion in v3, not a
      separate port — but tracking the gap.

- [ ] **`frac_ref.hpp`** (1389 lines) — zero-copy mutable views over
      fracpack-encoded data.  Capability tiers from buffer type:
      `span<const char>` → read-only, `span<char>` → fixed-size
      overwrites, `vector<char>` → read+write+resize.  **Partially
      replaced by `view.hpp` + `mutable_view.hpp`** — v3 already has
      the capability-tier scaffold, but feature parity (in-place
      vector resize, sub-record mutation) needs to be confirmed.
      **REVIEW**: compare frac_ref.hpp to view.hpp+mutable_view.hpp
      operation-by-operation.

- [ ] **`fbs_parser.hpp`** — recursive-descent FlatBuffers `.fbs` IDL
      text parser.  Needed for "read existing `.fbs` schema and
      reflect the types defined there".  Companion to v1's
      `to_flatbuf.hpp` (already in v3 as `flatbuf.hpp`).

- [ ] **`capnp_parser.hpp`** — recursive-descent Cap'n Proto `.capnp`
      IDL parser.  Same role as fbs_parser for the capnp side.

- [ ] **`capnp_schema.hpp`** — generate `.capnp` schema text from
      PSIO-reflected types.  v3's `capnp.hpp` has the wire codec but
      not the schema-text emitter.

### Needs port (low priority — small utilities)

- [x] **`bytes_view.hpp`** — `using bytes_view = std::span<const uint8_t>`.
      One alias; trivial.  Ported as `psio3/bytes_view.hpp` (with
      `mutable_bytes_view` sibling).  Test in `util_tests.cpp`.

- [~] **`check.hpp`** — `psio::check(cond, msg)` and `abort_error`
      helpers.  Used internally by v1 codecs (bitset, from_avro,
      from_json, etc.).  **SUPERSEDED**: zero downstream callers
      (`grep -rn psio::check libraries/` outside `/psio/cpp/` returns
      empty); v3 has `codec_status` / `codec_exception` /
      `validate_or_throw` for the same role with a richer error model.
      No port — confirmed.

- [~] **`ctype.hpp`** — backward-compat alias for `CView` / `COwned`
      (renamed `WView` / `WOwned` in `wview.hpp`).  6-line shim.
      **SUPERSEDED**: psio3 has no `CView` / `COwned` legacy to
      preserve; the rename happened pre-v3.  No port — flagged in
      REVIEW.

- [ ] **`chrono.hpp`** (413 lines) — `std::chrono::duration` /
      `time_point` packing for fracpack.  **DEFERRED**: no current
      downstream users serialise chrono types through reflection
      (`grep -rn "std::chrono::duration\|std::chrono::time_point"
      libraries/psi-api libraries/pfs libraries/psitri` — only one
      hit, in psizam watchdog as a runtime-internal type, not a
      reflected field).  v1's chrono.hpp wires the type into a
      single `is_packable` specialisation; v3 needs a tag_invoke
      overload per codec (bin, ssz, pssz, frac, borsh, bincode,
      avro, key, json — 9 codecs).  When the first downstream user
      needs it, port at that point — the right pattern is unwrap
      `duration` to its `count()` and recurse, but we lack a precedent
      for "treat as transparent wrapper of inner type" across all
      codecs today.  Tracked, not blocking.

- [ ] **`untagged.hpp`** — 10 lines; defines `psio_is_untagged(T*)`
      ADL hook for marking variants as untagged.  Trivial port; the
      v3 variant codec needs to consult this hook.

- [x] **`to_hex.hpp`** (82 lines) — hex string conversion utility.
      Standalone; trivial port.  Ported as `psio3/to_hex.hpp`
      (`hex`, `to_hex`, `from_hex` free functions).  Test in
      `util_tests.cpp`.

- [x] **`untagged.hpp`** — ADL hook for marking variants as untagged.
      Ported as `psio3/untagged.hpp` (`psio3_is_untagged` primary
      template).  Codec wiring (variant codec consults the hook) is a
      separate follow-up — for now the marker is registerable.

- [x] **`get_type_name.hpp`** — compile-time type-name string for
      arbitrary T.  Earlier classification as "SUPERSEDED" was wrong
      — it's actually **load-bearing** for schema/WIT generation.
      `psio3::reflect<T>::name` only handles reflected user types;
      `get_type_name` handles the full universe: primitives
      (`int32`, `uint64`, `f64`), `std::vector<T>`, `std::optional<T>`,
      `std::variant<...>`, `std::tuple<...>`, `std::chrono::duration`,
      `std::array<T,N>`, AND reflected types via the
      `is_reflected<T>` requires-clause overload.  v1 schema.hpp:1822
      consumes it directly to render type strings in schema output.
      Depends on `compress_name.hpp` for non-reflected type-name
      compression.  Will be needed when the schema/WIT toolchain
      ports.  Direct port, mechanical (~250 lines).

- [~] **`tuple.hpp`** — `std::tuple` reflection helpers
      (`tuple_get`, `tuple_for_each`).  **SUPERSEDED**: zero
      downstream callers; v3 codecs handle tuples directly via
      `tag_invoke` dispatch.  No port — flagged in REVIEW.

### Internal helpers (likely supersede)

These are v1 implementation details that v3 may have replaced with
better-shaped equivalents.  Flagging for review.

- [ ] **`fpconv.h`** + **`powers.h`** — third-party (Boost license,
      from night-shift/fpconv) double-to-string converter, used by
      `to_json_fast.hpp`.  v3's `json.hpp` may use a different
      backend (`std::to_chars`?).  **REVIEW**: confirm v3 JSON
      emitter behaviour matches v1's perf and precision; if so, drop
      the bundled fpconv.

- [ ] **`detail/layout.hpp`** — folded into per-codec
      `is_memcpy_layout_struct<T>` predicates in v3.  Functionally
      ported; the file as a separate module is not.  **No port needed.**

- [ ] **`detail/run_detector.hpp`** — DWNC contiguous-bitwise-field
      run detector.  Folded into each v3 codec's encode hot path.
      **No port needed.**

- [ ] **`json/any.hpp`** — dynamic JSON `any` value.  Replaced in v3
      by `dynamic_json.hpp` + `dynamic_value.hpp`.  **No port needed**
      assuming the v3 dynamic-value type covers the same use cases.
      **REVIEW**: confirm.

---

### New in v3 (no v1 counterpart)

For completeness — items added in v3 that didn't exist in v1.  Listed
so a v1 → v3 migration story is comprehensive.

- `annotate.hpp` — annotation system (specs, `applies_to`, merging).
- `format_tag_base.hpp` — `format_tag_base<Derived>` CRTP.
- `cpo.hpp` — tag_invoke CPOs (`encode`, `decode`, `validate`,
  `validate_or_throw`, `size_of`, etc.).
- `adapter.hpp` — codec adapter machinery for legacy types.
- `error.hpp` — `codec_status` (no-throw) + `codec_exception` +
  `validate_or_throw` dual API.
- `validate_strict_walker.hpp` — strict-validation walker.
- `transcode.hpp` — format-to-format conversion via dynamic schema.
- `dynamic_*.hpp` (`bin`, `frac`, `json`, `pssz`, `ssz`, `value`) —
  schema-driven dynamic codec operations.
- `max_size.hpp` — compile-time encoded-size upper bound, drives
  `pssz-auto` width selection.
- `shapes.hpp` — shape concept system (Primitive, Optional, Variant,
  Record, ...).
- `wrappers.hpp` — rich wrapper types (`bounded<T,N>`,
  `utf8_string<N>`, `byte_array<N>`, `bitvector<N>`, `bitlist<N>`)
  with `inherent_annotations`.
- `mutable_view.hpp` — capability-tier mutable views.
- `buffer.hpp` — buffer abstraction shared across codecs.
- `storage.hpp` — storage selectors (`owning`, `const_borrow`,
  `mut_borrow`).
- `conformance.hpp` — conformance-test utilities.
- `varint/` — header-only LEB128 / prefix2 / prefix3 with NEON
  fast path.
- `compat/psio_aliases.hpp` — v1 → v3 backward-compat aliases for
  consumer migration.

---

## Coverage table — at a glance

```
Total v1 headers:           67
  Ported (named):           21
  Ported (folded):           4   (detail/, json/, from_json/, to_json/)
  Needs-port (high):        16
  Needs-port (low):          8
  Likely-supersede:          4   (REVIEW required)
  -----------------------------
  Outstanding:              28
```

The **28 outstanding items** are what stands between psio3 being a
strict superset of psio v1.  Of those:
- **9 are downstream-blocking** (`name`, `wit_resource`, `wit_gen`,
  `wit_parser`, `wit_encode`, `attributes`, `structural`, full
  `schema`, `frac vector<variable>`).
- **8 are "complete the WIT/Component-Model story"** (the rest of
  the wit_*.hpp family, `canonical_abi.hpp`, guest_*.hpp,
  emit_wit.hpp, capnp_parser, capnp_schema, fbs_parser).
- **8 are small utilities** (`bytes_view`, `check`, `chrono`,
  `untagged`, `to_hex`, `get_type_name`, `tuple`, `compress_name`).
- **3 are internal helpers** (`fpconv`/`powers`, possibly
  superseded; `ctype` shim).

## Recommended ordering

1. **`name.hpp`** — single-file port, no deps, downstream-blocking.
   ~30-60 minutes.
2. **Small utilities** (`bytes_view`, `check`, `to_hex`,
   `get_type_name`, `untagged`, `compress_name`) as a single batch —
   each is <100 lines, all standalone.
3. **`chrono.hpp`** — needs per-codec `tag_invoke` registration; one
   commit per format or one commit covering all.
4. **`structural.hpp` + `attributes.hpp` + `emit_wit.hpp` +
   schema.hpp full** — schema declaration + emission as a single
   working-block.
5. **`wit_parser.hpp` + `wit_encode.hpp` + `wit_gen.hpp` +
   `wit_resource.hpp`** — full WIT toolchain.  Likely the biggest
   chunk.
6. **`canonical_abi.hpp` rebase + edge cases** — feature-complete
   WIT vs `wit.hpp`.
7. **`frac_ref.hpp` parity** — confirm or close gaps in
   `view.hpp` + `mutable_view.hpp`.
8. **`fracpack` `vector<variable>` support** — close the
   `frac_supports<V3Order>` gap.
9. **`capnp_parser.hpp` + `capnp_schema.hpp` + `fbs_parser.hpp`** —
   schema-text input/output side.
10. **`guest_alloc.hpp` + `guest_attrs.hpp` + `wview.hpp` +
    `wit_owned.hpp`** — WASM guest glue.

## Items flagged for review (do not port without sign-off)

- `compress_name.hpp` — supersede by `name.hpp`?
- `wit_resource.hpp` / `own<T>` / `borrow<T>` — psio3 or separate library?
- `wit_view.hpp` — already covered by `view<T, wit>` after `wit.hpp` lands?
- `canonical_abi.hpp` — what's missing vs `wit.hpp`?  rebase op specifically?
- `check.hpp` — supersede by codec_status / codec_exception?
- `ctype.hpp` — drop the back-compat alias entirely?
- `chrono.hpp` — which formats need chrono support — all of them?
- `tuple.hpp` — orphaned callers?
- `fpconv.h` / `powers.h` — drop in favour of `std::to_chars`?
- `json/any.hpp` — `dynamic_value.hpp` covers all use cases?
- `frac_ref.hpp` vs `view.hpp` + `mutable_view.hpp` — full parity?

---

*This document is the source of truth for migration progress.  When a
checkbox flips to ✅, the row in the corresponding section above
should also be moved to "Ported and feature-equivalent".*
