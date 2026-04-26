# psio Format Feature Matrix (v1 audit + v2 parity tracker)

**Status:** updated 2026-04-24. This doc predates the v2 architecture
pivot. **For canonical v2 design, see `psio-v2-design.md`.** This doc
is retained as:

- **Section 1 & 3 & 4** — factual audit of what v1 currently has per
  format, in both C++ and Rust. Useful reference during v2 migration.
- **Section 6** — per-format TODO queue to bring v1 state up to v2
  feature parity (structure, size probes, validators, views, Rust
  ports, cross-val fixtures, benches).
- **Sections 2, 5, 6A** — **SUPERSEDED.** The canonical API surface,
  customization mechanism, and format-neutral dispatch are
  specified in `psio-v2-design.md` §§ 4–5. Content here is kept for
  historical reference; implementation follows the v2 design.
- **Section 7** — open questions, mostly resolved (see
  `psio-v2-design.md` § 10).

**Key v2 decisions that apply here:**

- Customization via `tag_invoke` CPOs + `format_tag_base<Derived>`
  CRTP (not class-template specialization of `codec_ops<Fmt>`).
- Format tags at top level (`psio1::ssz`, `psio1::frac`, …), not
  `psio1::fmt::*`.
- Two call forms: generic `psio1::encode(fmt, v)` + format-scoped
  `psio1::frac::encode(v)` — both compile to the same code via
  `format_tag_base` sugar.
- Heap factory is `make_boxed<T>(fmt, bytes)` (not
  `make_unique_from_<fmt>` / `make_shared_from_<fmt>`).
- Annotations live in `psio1::annotate<X>` (tuple of spec structs),
  not a series of tag composition `operator|` calls on individual
  attributes.
- Rename pass (v1 `convert_*` → v2 canonical) happens as part of the
  v2-to-primary-namespace promotion, not as a standalone sweep.

The per-format inventory tables below remain authoritative for
"what v1 looks like today" and feed into the v2 conformance harness
(one test fixture per row).

---

## 1. Formats in scope

psio currently ships thirteen wire formats, split into three paradigms:

| # | Format | Paradigm | Target namespace / verb |
|---|---|---|---|
| 1 | **ssz** | symmetric binary codec | `to_ssz` / `from_ssz` |
| 2 | **pssz** (psiSSZ) | symmetric binary codec | `to_pssz` / `from_pssz` |
| 3 | **frac** (fracpack) | symmetric binary codec | `to_frac` / `from_frac` |
| 4 | **bin** | symmetric binary codec | `to_bin` / `from_bin` |
| 5 | **key** (sort key) | symmetric binary codec | `to_key` / `from_key` |
| 6 | **avro** | symmetric binary codec | `to_avro` / `from_avro` |
| 7 | **borsh** | symmetric binary codec | `to_borsh` / `from_borsh` |
| 8 | **bincode** | symmetric binary codec | `to_bincode` / `from_bincode` |
| 9 | **json** | symmetric text codec | `to_json` / `from_json` |
| 10 | **flatbuf-native** | symmetric binary codec (zero-dep) | `to_flatbuf` / `from_flatbuf` (via unified `view<T, fb>`) |
| 11 | **flatbuf-lib** | adapter to Google flatbuffers runtime | `to_flatbuf(fbb, T)` / `from_flatbuf(...)` |
| 12 | **capnp** | symmetric binary codec (zero-dep) | `to_capnp` / `from_capnp` |
| 13 | **wit** | canonical-ABI codec + schema tooling | `to_wit` / `from_wit` (currently `psio1::wit::pack`/`unpack`) |

**Important**: flatbuf has two implementations. The native zero-dep one is
the default and lives in `flatbuf.hpp`; the runtime-adapter is in
`to_flatbuf.hpp` and depends on the Google flatbuffers library. Both must
continue to coexist; tests and benches compare them.

---

## 2. Canonical API surface (SUPERSEDED)

> **See `psio-v2-design.md` § 4 (Developer experience) and § 5
> (Architecture) for the canonical API.** The early sketch this
> section held has been replaced by:
>
> - Generic: `psio1::encode(fmt, v)` / `psio1::decode<T>(fmt, bytes)` /
>   `psio1::size(fmt, v)` / `psio1::validate<T>(fmt, bytes)` /
>   `psio1::make_boxed<T>(fmt, bytes)` — CPOs dispatching via
>   `tag_invoke`.
> - Format-scoped sugar: `psio1::ssz::encode(v)` etc. via
>   `format_tag_base<Derived>` CRTP.
> - No `make_unique_from_<fmt>` / `make_shared_from_<fmt>` per-format
>   functions — the generic `make_boxed` CPO covers it.
> - Storage ops (`bytes`, `size`, `format_of`, `to_buffer`,
>   `as_view`) as free functions; field access via natural `.`
>   operator on views.
> - Error model: `validate` returns `codec_status`; all other
>   ops `noexcept`. `[[nodiscard]]` + `-Werror=nodiscard` enforces
>   check.
>
> The tables in §§ 3, 4 below remain accurate as a v1 audit.

---

## 3. C++ feature matrix (current state)

Legend: ✅ present & canonical · ⚠️ present but non-canonical (naming mismatch, deprecate) · ❌ missing · — N/A for this paradigm

| Format | to_ stream | from_ stream | to_ (vec out) | to_ (vec ret) | from_ (T& out) | from_ (T ret) | make_unique_ | make_shared_ | size | validate | view | mut_view | schema export | schema import | Rust port | C++↔Rust xval |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| ssz | ✅ `to_ssz` | ✅ `from_ssz` | ⚠️ `convert_to_ssz` | ⚠️ `convert_to_ssz` | ⚠️ `convert_from_ssz` | ⚠️ `convert_from_ssz<T>` | ❌ | ❌ | ✅ `ssz_size` | ✅ `ssz_validate` | ✅ `ssz_view<T>` via `view<T, ssz_fmt>` | ❌ | ⚠️ inside schema.hpp | ⚠️ | ✅ | ✅ |
| pssz | ✅ `to_pssz` | ✅ `from_pssz` | ⚠️ `convert_to_pssz` | ⚠️ `convert_to_pssz` | ⚠️ `convert_from_pssz` | ⚠️ `convert_from_pssz<T>` | ❌ | ❌ | ✅ `pssz_size` | ❌ (validator lives per-trait but no top-level free fn) | ✅ `pssz_view<T, F>` | ❌ | ⚠️ | ⚠️ | ✅ | ✅ |
| frac | ✅ `to_frac` | ✅ `from_frac` | ❌ (no vec-out overload) | ✅ `to_frac(T)` | ✅ `from_frac(T&, span) -> bool` | ✅ `from_frac<T>(span) -> T` | ❌ | ❌ | ⚠️ `fracpack_size` (rename → `frac_size`) | ⚠️ `validate_frac` (rename → `frac_validate`) | ✅ `frac_view` / `view<T, frac>` | ✅ via `frac_ref` / `wview` | ✅ | ✅ | ✅ `.packed()` / `.unpacked()` (non-canonical style) | ✅ |
| bin | ✅ `to_bin` | ✅ `from_bin` | ⚠️ `convert_to_bin` | ⚠️ `convert_to_bin` | ⚠️ `convert_from_bin` | ❌ (no `convert_from_bin<T>` returning T) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| key | ✅ `to_key` | ✅ `from_key` | ❌ | ⚠️ `convert_to_key` | ⚠️ `convert_from_key` | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| avro | ✅ `to_avro` | ✅ `from_avro` | ⚠️ `convert_to_avro` | ⚠️ `convert_to_avro` | ⚠️ `convert_from_avro` | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| borsh | ✅ `to_borsh` | ✅ `from_borsh` | ⚠️ `convert_to_borsh` | ⚠️ `convert_to_borsh` | ⚠️ `convert_from_borsh` | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| bincode | ✅ `to_bincode` | ✅ `from_bincode` | ⚠️ `convert_to_bincode` | ⚠️ `convert_to_bincode` | ⚠️ `convert_from_bincode` | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| json | ✅ `to_json` | ✅ `from_json` | ❌ | ⚠️ `convert_to_json` → string + ⚠️ `format_json` pretty + ⚠️ `to_json_fast` | ❌ (no `convert_from_json(T&, sv)`) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | via schema.hpp | via schema.hpp | ✅ `to_json`/`from_json` | ❌ |
| flatbuf-native | ⚠️ class `fb_builder.pack(T)` (not a free `to_flatbuf`) | ⚠️ free `fb_unpack<T>(buf)` | ❌ | ❌ | ❌ | ⚠️ `fb_unpack<T>(buf)` | ❌ | ❌ | ❌ | ❌ | ✅ `fb_view<T>` / `view<T, fb>` | ⚠️ `fb_mut<T>` | ⚠️ (schema export not surfaced at top level — search for `fb_` in schema.hpp) | ⚠️ `fbs_parser.hpp` | ✅ `FbBuilder`, `FbUnpack`, `FbView` | ❌ |
| flatbuf-lib | ✅ `to_flatbuf(fbb, T)` (builder-shaped) + `to_flatbuf_finish(fbb, T)` | ✅ `from_flatbuf(...)` | — (flatbuffers runtime owns buffer) | — | ✅ | — | ❌ | ❌ | ❌ | ❌ | ⚠️ `flatbuf_view` | ❌ | — (fbs text via `emit_wit`-style sibling TBD) | — | ❌ | ❌ |
| capnp | ❌ (no `to_capnp(T, stream)`) | ❌ (no `from_capnp(T&, ...)`) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ✅ `capnp_view<T>` | ❌ | ⚠️ via `capnp_schema.hpp` | ✅ `capnp_parser.hpp` | ✅ `CapnpPack`, `CapnpUnpack`, `CapnpView` | ❌ |
| wit | ⚠️ `psio1::wit::pack(T)` (namespaced verb, not `to_wit`) | ⚠️ `psio1::wit::unpack<T>(buf)` | ❌ | ❌ | ❌ | ⚠️ `psio1::wit::unpack<T>` | ❌ | ❌ | ❌ | ⚠️ `psio1::wit::validate<T>(buf)` | ✅ `wit_view` / `view<T, wit>` | ✅ `wit_mut<T>`, `wit_owned<T>` | ✅ `generate_wit<T>()` + `emit_wit` | ✅ `wit_parser.hpp` | ✅ `psio1::wit::pack`/`unpack` (also non-canonical) | ✅ (conformance_tests.rs) |

### Additional format-specific quirks

- **json**: returns `std::string`, not `std::vector<char>`. Has two extra siblings: `format_json` (pretty-print) and `to_json_fast` (alternate impl). Under canonicalization these become `json_pretty` and `json_fast`.
- **wit**: uses namespaced verb `psio1::wit::pack` instead of free `to_wit`. Also has `canonicalize<T>` which is wit-specific (canonical-ABI doesn't need it in the pack path but exposes it for re-canonicalizing existing buffers).
- **flatbuf-native vs flatbuf-lib**: entirely different entry points. Native uses class builder `fb_builder.pack(token)`; library uses `to_flatbuf(fbb, T)` with `FlatBufferBuilder` argument. Keep both but rename native's entry points to fit the `to_/from_` pattern (see TODOs below).
- **capnp**: view-only today. No writer. Gap: full symmetric codec.
- **bin**: no size/validate/view — this is the oldest/simplest format; gap is largest here.

---

## 4. Rust feature matrix (current state)

Legend same as above. "Canonical" = free function in `psio1::<fmt>` module.

| Format | to_ fn | from_ fn | size fn | validate fn | from_…_boxed | View | Mut | Schema export | Schema import | C++↔Rust xval |
|---|---|---|---|---|---|---|---|---|---|---|
| ssz | ✅ `to_ssz` | ✅ `from_ssz` | ❌ (only `T::ssz_size(&self)` method) | ✅ `ssz_validate` | ❌ | ✅ `SszView<'a, T>` | ❌ | ❌ | ❌ | ✅ |
| pssz | ✅ `to_pssz` | ✅ `from_pssz` | ❌ (method only) | ✅ `pssz_validate` | ❌ | ✅ `PsszView<'a, T, F>` | ❌ | ❌ | ❌ | ✅ |
| frac | ⚠️ `.packed()` method (not free `to_frac`) | ⚠️ `.unpacked(&[u8])` method | ⚠️ `.packed_size()` method | ❌ | ❌ | ✅ | ✅ (via fracpack builder) | ❌ | ❌ | ✅ |
| bin | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| key | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| avro | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| borsh | ❌ (use `borsh` crate externally for bench cross-val only) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | — |
| bincode | ❌ (same — crate only) | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | — |
| json | ✅ `to_json` (returns String) | ✅ `from_json` | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ | ❌ |
| flatbuf-native | ⚠️ `FbBuilder` type | ⚠️ `FbUnpack` trait | ❌ | ❌ | ❌ | ✅ `FbView` | ⚠️ via mutation module | ✅ `to_fbs_text` / `to_fbs_schema<T>` | ✅ `parse_fbs` | ❌ |
| flatbuf-lib | — (Rust doesn't wrap Google flatbuffers runtime) | — | — | — | — | — | — | — | — | — |
| capnp | ⚠️ `CapnpPack` trait | ⚠️ `CapnpUnpack` trait | ❌ | ✅ `validate` fn | ❌ | ✅ `CapnpView` | ✅ via mutation module | ✅ `to_capnp_schema` | ✅ `parse_capnp` | ❌ |
| wit | ⚠️ trait-method style? | ⚠️ trait-method style? | ❌ | ❌ | ❌ | ✅ (wit::view) | ✅ (wit::mutation) | ✅ `to_wit_schema` | ✅ `parse_wit` | ✅ conformance_tests.rs |

---

## 5. Canonicalization plan (SUPERSEDED)

> **See `psio-v2-design.md` § 7 (Coexistence / migration) for the
> actual plan.** Summary of deltas from this early sketch:
>
> - v2 lives in a fresh `psio1::v2::` namespace during development,
>   not a rename-in-place of v1. No `[[deprecated]]` aliases during
>   the development phase.
> - When v2 promotes to primary, the legacy `convert_*`, `format_json`,
>   `to_json_fast`, `fracpack_size`, `validate_frac` names become
>   thin deprecated forwarders to the canonical v2 path.
> - Rename semantics (e.g. `format_json` → `json_pretty`) stay
>   applicable — but they happen as part of the namespace promotion,
>   not as a standalone pass.
> - `make_unique_from_<fmt>` / `make_shared_from_<fmt>` per-format
>   names are gone — replaced by the single `psio1::make_boxed<T>(fmt,
>   bytes)` CPO and its format-scoped sugar `fmt::make_boxed<T>(bytes)`.
> - Flatbuf dual-impl disambiguation: flatbuf-native keeps `psio1::flatbuf`;
>   flatbuf-lib lives under `psio1::flatbuf_lib::` (decided).
>
> Cross-validation fixtures per format remain a concrete TODO — see
> § 6.8 below.

---

## 6. Feature-completeness TODO queue

Each row below is a standalone work item sized for a single PR.

### 6.1 Naming canonicalization (breaking-but-aliased)

- [ ] T-01: Add canonical `to_ssz` / `from_ssz` overloads, deprecate `convert_to_ssz` / `convert_from_ssz`. Update all callers in cpp/ + benchmarks.
- [ ] T-02: Same for pssz. Also add top-level `pssz_validate<F, T>(span)` free fn.
- [ ] T-03: Rename `fracpack_size` → `frac_size`, `validate_frac` → `frac_validate`. Keep old names as `[[deprecated]]` aliases.
- [ ] T-04: Rename `format_json` → `json_pretty`, `to_json_fast` → `json_fast`. Keep old names as `[[deprecated]]` aliases.
- [ ] T-05: Same canonical overloads for bin/key/avro/borsh/bincode; deprecate `convert_*`.
- [ ] T-06: For flatbuf-native: add free `to_flatbuf<T>(T) → vec`, `from_flatbuf<T>(span) → T`. Keep `fb_builder` class as the power-user interface.
- [ ] T-07: For flatbuf-lib: decide disambiguation (see Section 5B). Recommend moving to `psio1::flatbuf_lib::` namespace so both `to_flatbuf` free functions coexist.
- [ ] T-08: For wit: add free `to_wit` / `from_wit` / `wit_size` / `wit_validate`. Keep `psio1::wit::pack` etc. as deprecated aliases.

### 6.2 Feature additions — heap factories

- [ ] T-10: Implement `psio1::make_boxed<T>(fmt, bytes)` CPO that
      default-inits `T` on the heap (no POD zero-init) then dispatches
      to the format's `tag_invoke(decode, fmt, bytes, *ptr)`. One
      header helper `psio/detail/make_default_init.hpp` provides the
      primitive. (Replaces the earlier per-format `make_unique_from_<fmt>`
      /`make_shared_from_<fmt>` scheme.)
- [ ] T-11: Bench sweep: measure decode time for BeaconState via
      `make_boxed<T>(ssz, bytes)` vs v1's `convert_from_ssz<BeaconState>`
      to confirm the zero-init saving in C++.
- [ ] T-12: Rust counterpart: `from_<fmt>_boxed<T>` free fns (since
      Rust's `Box::<MaybeUninit<T>>::new_uninit()` is the natural
      idiom; no Rc/Arc variants needed for decoded values).

### 6.3 Feature additions — missing size probes

- [ ] T-20: `bin_size<T>(const T&)`.
- [ ] T-21: `key_size<T>(const T&)`.
- [ ] T-22: `avro_size<T>(const T&)`.
- [ ] T-23: `borsh_size<T>(const T&)`.
- [ ] T-24: `bincode_size<T>(const T&)`.
- [ ] T-25: `json_size<T>(const T&)` (count bytes without allocating).
- [ ] T-26: `flatbuf_size<T>(const T&)` (both native and library variants).
- [ ] T-27: `capnp_size<T>(const T&)` (pairs with the writer in T-45).
- [ ] T-28: `wit_size<T>(const T&)`.

### 6.4 Feature additions — missing validators

- [ ] T-30: `bin_validate<T>(span)`.
- [ ] T-31: `key_validate<T>(span)`.
- [ ] T-32: `avro_validate<T>(span)` (possible — avro is schema-driven).
- [ ] T-33: `borsh_validate<T>(span)` — likely implementable as trial-decode only; document as such or mark N/A.
- [ ] T-34: `bincode_validate<T>(span)` — same.
- [ ] T-35: `json_validate<T>(string_view)` — syntactic + type-directed.
- [ ] T-36: `flatbuf_validate<T>(span)` for both impls.
- [ ] T-37: `capnp_validate<T>(span)` free fn (Rust has one; C++ should mirror).
- [ ] T-38: `wit_validate<T>(span)` — promote from namespaced `psio1::wit::validate` to free.

### 6.5 Feature additions — views / mutable views

- [ ] T-40: Implement `bin_view<T>` (or declare N/A — bin has no offsets, zero-copy view is limited to POD segments).
- [ ] T-41: `json_view<T>` — decide whether it's tree (like simdjson dom) or value-directed walk.
- [ ] T-42: `flatbuf_mut<T>` (both impls; native has `fb_mut` partial, library missing).
- [ ] T-43: `capnp_mut<T>` for in-place writes.
- [ ] T-44: `ssz_mut<T>` and `pssz_mut<T>` — currently views are read-only. Cp-paste `wit_mut` shape.

### 6.6 Feature additions — full symmetric codecs

- [ ] T-45: **capnp writer** — implement `to_capnp(T, stream)` + all heap/size/validate friends. Today Rust has `CapnpPack` but C++ is view-only. Largest single work item in the matrix.
- [ ] T-46: **Rust: port bin, key, avro** from C++. Needed for cross-validation and for Rust-side bench coverage.
- [ ] T-47: **Rust: borsh/bincode** — use external crates (already done for bench); decide whether to have native psio impls at all, or permanently leave as "cross-validated via external crate".

### 6.7 Schema tooling

- [ ] T-50: C++ `to_ssz_schema<T>()` and `parse_ssz(text)` — today schema lives inside `schema.hpp` mixed with fracpack; split out.
- [ ] T-51: C++ `to_pssz_schema<T>()` and `parse_pssz(text)`.
- [ ] T-52: C++ `to_bin_schema<T>()` — may be trivially derived from reflect.
- [ ] T-53: Rust: expose `to_<fmt>_schema<T>()` for every format that has a C++ counterpart.

### 6.8 Cross-validation tests

- [ ] T-60: Emit C++ fixtures for bin, commit to `libraries/psio1/cpp/tests/fixtures/`, write Rust assertion tests.
- [ ] T-61: Same for key.
- [ ] T-62: Same for avro.
- [ ] T-63: Same for json (text equality modulo whitespace for non-fast paths).
- [ ] T-64: Same for flatbuf-native.
- [ ] T-65: Same for capnp (once C++ writer exists — T-45).

### 6.9 Rename / feature: pssz → psiSSZ

- [ ] T-70: Finalize marketing rename. Identifier prefix stays `pssz` for now (short, already-widespread in code); documentation / public-facing name says "psiSSZ". Cross-reference in docs.

### 6.10 Bench coverage

- [ ] T-80: Bench matrix: for each of { ssz, pssz, frac, flatbuf-native, flatbuf-lib, capnp, wit, bin, key, json, borsh, bincode } × { encode, decode, validate, view-access } on the real BeaconState workload (where the format can express it) and on a synthetic mid-size struct (bench_modern_state).
- [ ] T-81: Publish the matrix as `libraries/psio1/cpp/benchmarks/README.md` with numbers for each machine we've run on.

---

## 6A. Format-neutral API and conformance harness (SUPERSEDED)

> **See `psio-v2-design.md` §§ 4.4 + 5.2 + 5.2.5 + 5.2.6 for the
> canonical design.** The key deltas from this early sketch:
>
> - Customization point is `tag_invoke` CPO + `format_tag_base<Derived>`
>   CRTP, NOT `codec_ops<Fmt>` class-template specialization.
> - Format tags sit at top level (`psio1::ssz`, `psio1::frac`, etc.),
>   NOT in a `psio1::ssz_fmt` / `psio1::frac_fmt<W>` naming scheme.
>   They inherit from `format_tag_base<Self>` to get the scoped form
>   for free.
> - `make_boxed<T>(fmt, bytes)` is a single CPO — no separate
>   `make_unique`/`make_shared` helpers in the generic API.
> - Error model: `validate` returns `codec_status`; all other ops
>   `noexcept` (assume valid input per § 5.4). No common-error
>   vs per-format-error split.
> - Buffer type associated via the tag struct's `buffer_type` alias.
> - Conformance harness is `FOR_EACH_V2_FMT(M, fixture, value)` macro
>   enumerating format tags; test body applies the six conformance
>   checks generically.
>
> TODO items T-90 to T-99 below are superseded by the v2 design's
> step-by-step format rollout (one PR per format in the v2
> namespace, per `psio-v2-design.md` § 7).

---

## 7. Open naming questions (RESOLVED)

All seven original questions have been answered through the v2 design:

| # | Question | Resolution |
|---|---|---|
| 1 | `fracpack_size` → `frac_size`? | **Yes** — part of v2 canonical naming; v1 name becomes `[[deprecated]]` alias on v2 promotion. |
| 2 | pssz → psiSSZ rename fold-in? | **No** — identifier stays `pssz`; public name "psiSSZ" lives in docs only. |
| 3 | `convert_*` — deprecate or delete? | **Deprecate for one release, then delete.** Applied on v2 promotion (`psio-v2-design.md` § 7). |
| 4 | Rust `from_<fmt>_boxed` naming divergence OK? | **Yes** — Rust-idiomatic, no Rc/Arc distinction. |
| 5 | `format_json` → `json_pretty`? | **Yes** — rename happens on v2 promotion. |
| 6 | flatbuf dual-impl disambiguation? | **`psio1::flatbuf_lib::` namespace** for the runtime-adapter; `psio1::flatbuf` tag for native. |
| 7 | Generic `psio1::encode<Fmt>(v)` vs per-format `to_<fmt>(v)` as default? | **Generic is default** in docs and examples; per-format sugar via `format_tag_base` covers the short form (`psio1::frac::encode(v)`). Low-level `to_frac(v, stream&)` stays for advanced use. |

Open questions going forward live in `psio-v2-design.md` § 11.

---

## 8. Pointer back to this doc

When adding a new format, update:
- Section 1 table (paradigm classification)
- Section 3 (C++ feature matrix)
- Section 4 (Rust feature matrix)
- Section 6 (new TODOs for whatever cells are ❌)

When closing a TODO, check it off here in addition to the PR that
implements it. This file is the tracker.
