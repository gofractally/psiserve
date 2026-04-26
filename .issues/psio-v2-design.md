# psio v2 — Unified Codec Architecture

**Status:** design plus psio2 implementation in progress, 2026-04-24. This
doc is the contract the implementation is measured against.

**Scope:** a full-parity replacement of psio's serialization entry points.
During development it lives under `psio2::` / `PSIO2` in `libraries/psio2`;
once complete, it should be mechanically renamed to `psio1::` / `PSIO` and v1
can remain only as a reference oracle during migration.

---

## 1. Goals

1. **One API surface for every format.** Users pick a format by name
   (`psio1::ssz`, `psio1::frac`, ...) and call `psio1::encode(fmt, value)`.
   Adding a new format means defining `tag_invoke` overloads for a
   small set of CPOs; no new free functions, no user-visible verb per
   format.
2. **Shape-based dispatch, not type-based.** Formats dispatch on
   *what the data looks like* (primitive, fixed-size sequence,
   variable sequence, record, ...), not on *which C++ type carries
   it*. Users' types participate via reflection; the *semantic* mapping
   from a C++ type to a wire shape is driven by annotations on the
   reflection — **not by the type itself.** `std::string` can be
   text, opaque bytes, or hex depending on how the field was
   annotated; the type is not intrusively changed.
3. **Non-intrusive on user types.** Serialization concerns do not
   leak into user struct definitions. Bounds, encoding hints, DWNC,
   and format-specific overrides live on the reflection, not as
   wrapper types around user fields. The library serves the user's
   types, not vice versa.
4. **Format as a first-class value in the type system.** Results keep
   format identity: `encode(ssz{}, v)` returns `buffer<T, ssz, …>`;
   the format tag and the logical type flow through the API so
   conversions and views compose without losing information.
5. **Auto-convert.** `encode(ToFmt{}, view<T, FromFmt>{})` works
   without a per-pair conversion trait, because views satisfy the
   same shape concepts as raw values.
6. **Exception-optional.** The library works with exceptions enabled
   OR disabled. Exception-disabled builds are a first-class use case
   (WASM guests, embedded).
7. **Performance at least as good as v1.** v2 is a template reshuffle
   over existing serialization algorithms. Measured by bench parity
   on the mainnet-genesis BeaconState workload; v2 must meet or
   exceed v1 throughput within noise. No asm-level lockstep is
   required — the gate is bench-based.

## 2. Non-goals

1. **No algorithmic changes.** Same reflection walks, same memcpy fast
   paths, same offset-table backpatching. If v2 changes the bit-level
   logic, it's a bug.
2. **No runtime dispatch.** All format/storage selection is compile
   time. No virtuals, no type erasure in hot paths.
3. **No breaking change to v1 users during development.** v1 entry
   points (`psio1::to_ssz`, `psio1::convert_to_ssz`, `psio1::from_ssz`,
   ...) keep working while v2 matures. Migration or deprecation is a
   separate phase after v2 ships all official formats.
4. **No new format-pair conversion matrix.** Cross-format conversion
   falls out of shape-polymorphic encode; we don't write 13×13 specialized
   converters.

## 3. Requirements

### 3.1 Functional

- [R1] Every official v1 format is reachable through v2: ssz, pssz, frac,
  bin, key, avro, borsh, bincode, json, native flatbuf, capnp, and wit. The
  upstream FlatBuffers library path is a demo/benchmark comparison only, not
  part of the official codec surface.
- [R2] Every format provides: `encode`, `decode`, `size` (if cheap),
  `validate` (where structurally possible), `make_boxed` (default-init
  + decode, no zero-init), view access (zero-copy for offset-based
  formats, lazy-indexing for others).
- [R3] Three storage backings for `buffer<T, F, Store>` and
  `view<T, F, Store>`: owning vector, mutable span, const span.
  Read ops work on all three; write ops require non-const backings.
- [R4] `mutable_view<T, F>` supports edits backed by an internal arena
  (single alloc/free for the session; editable overlay over an
  optional source view). Exports to an owning `buffer<T, F>` via
  `canonicalize`.
- [R5] Customization point mechanism is **`tag_invoke`** (P1895-style
  CPOs). Format authors never specialize templates in `psio1::`; they
  define hidden-friend `tag_invoke` overloads on their format tag
  struct. See § 5.2.
- [R6] Shape concepts are minimal: `Primitive`, `Enum`,
  `FixedSequence<N>`, `VariableSequence`, `Optional`, `Variant`,
  `Bitfield`, `Record`. Semantic variation within a concept (e.g. "is
  this `std::string` text, opaque bytes, or hex?") is expressed as an
  **annotation on the reflection of the containing field**, not as a
  separate concept or a wrapper type.
- [R7] Reflection annotations carry all per-field serialization
  hints: size bounds, byte-vs-text, DWNC, custom-wire-type mapping,
  default values. User types stay clean of serialization concerns.
  See § 5.3.
- [R8] **Field access via natural `.` operator** on the view; view's
  public surface is exactly T's reflected field accessors. Storage /
  byte operations are free functions in `psio1::` (`psio1::bytes(v)`,
  `psio1::size(v)`, `psio1::format_of(v)`, `psio1::to_buffer(v)`, …).
  This makes "using a view feels like using the rich object" a hard
  invariant: any `v.name()` call on a view either resolves to T's
  field or is a compile error — it will never accidentally call a
  library method. Operators (`[]`, `*`, `bool`, `begin`/`end`) stay
  on the view for sequence, optional, and iterable shapes since they
  don't collide with identifiers.
- [R9] Cross-format conversion works automatically:
  `encode(ToFmt{}, view<T, FromFmt>{})` walks the source view using
  the shape-accessor protocol and emits target-format bytes, no
  intermediate T materialization. No per-pair specialization
  required; optional per-pair optimizations possible later.
- [R10] Format-specific options (pssz width, frac width, json pretty)
  flow through the format tag at compile time. Runtime options (if
  any) go through a per-format `config` type; users with defaults
  see no config in their call sites.
- [R11] **Runtime reflection is first-class (see § 5.2.5).** Every
  query in `psio1::reflect<T>` is constexpr-callable, so the same API
  serves compile-time schema generation and runtime RPC/JSON
  dispatch. In particular:
  - `index_of(std::string_view)` uses a compile-time-built perfect
    hash for O(1) name lookup at runtime.
  - `index_of_field_number(uint32_t)` for protobuf/capnp-style
    dispatch.
  - `visit_field_by_name` / `visit_field_by_number` / `for_each_field`
    perform typed visitor dispatch.
  - `make_proxy<Derived>` generates custom proxy classes (RPC stubs,
    view accessors, printers) with one method per field routed through
    `Derived`'s handler.
  Reflection covers **public, non-static data members only** —
  private/protected/static are excluded by design.
- [R12] **Dynamic / type-erased access is first-class (see § 5.2.6).**
  The library supports operating on encoded bytes without a
  compile-time C++ type, given a runtime `psio1::schema` value:
  - `psio1::schema` captures everything `reflect<T>` has, as a
    runtime-manipulable value.
  - `reflect<T>::schema()` is a consteval bridge from static →
    dynamic.
  - Per-format dynamic CPOs: `validate_dynamic`, `encode_dynamic`,
    `decode_dynamic`.
  - `psio1::transcode(schema, fromFmt, in, toFmt, out)` transforms
    bytes between formats without materializing a C++ object.
  - `psio1::dynamic_view` navigates bytes by runtime field-name
    path; `psio1::dynamic_value` owns bytes + schema.
  - Format authors declare which dynamic ops they implement via
    `supports_dynamic_{validate,encode,decode,transcode}` constexpr
    bools.
  Use cases: gateway RPC services (schemas loaded at runtime),
  schema-aware transcoders, untrusted-boundary validators.
- [R13] **Error model (see § 5.4):**
  - `validate` is the only operation that can report an error.
  - Returns `[[nodiscard]] psio1::codec_status` — a result type that
    the compiler refuses to let the user silently drop.
  - With exceptions enabled, users opt in to throwing via
    `.or_throw()` or by letting the status object propagate through
    a helper.
  - With exceptions disabled, users MUST check via `if (!status)` or
    explicitly drop via `(void) …`; the `[[nodiscard]]` warning is
    elevated to `-Werror` in project CI so unchecked statuses fail
    to compile.
  - `decode`, `encode`, `view::field()`, view-chain access, and
    `make_boxed` are **`noexcept`** and assume the input has been
    validated. Passing unvalidated bytes to them is undefined
    behavior; callers that accept untrusted input must validate
    first.

### 3.2 Non-functional

- [N1] **Perf parity.** On the real mainnet-genesis BeaconState and
  the 21063-Validator list workloads, v2 encode/decode/validate
  throughput must meet or exceed v1 within ±2%. Measured by the
  existing bench harness. **This is the CI gate for performance.**
  No asm-level lockstep requirement — the abstraction is allowed to
  produce different instruction schedules as long as throughput
  holds.
- [N2] Zero heap allocs introduced by the v2 scaffold. If v1
  round-tripped with N allocations, v2 rounds-trips with N.
- [N3] Compile times: v2 instantiation cost is within 20% of v1.
  Monitored; not a CI gate.
- [N4] Header weight: the primary codec header (`psio/codec.hpp`, staged as
  `psio2/codec.hpp`) pulls in no more than the union of v1 format-specific
  headers.
- [N5] **Works without exceptions.** The library compiles and
  functions under `-fno-exceptions`. Exception-using convenience
  (`.or_throw()`, etc.) is conditionally compiled.
- [N6] **`-Werror=nodiscard` friendly.** The project enables
  `-Werror=nodiscard` in CI; unchecked `codec_status` values fail
  to compile. This makes "did you validate?" a compile-time
  invariant for exception-disabled builds.

### 3.3 Quality

- [Q1] Every v1 fixture (Validator, BeaconState, Named, Point,
  PendingAttestation, Eth1Data, and anything else in
  `libraries/psio1/cpp/tests/`) has a v2 byte-parity test.
- [Q2] Format parity harness is parametric — add a format, one line in
  `FOR_EACH_CODEC_FMT`, all fixtures automatically covered.
- [Q3] Shape-concept violations produce readable compile errors that
  name the missing concept, not a 50-line template instantiation dump.

---

## 4. Developer experience

Three audiences, three views of the library.

### 4.1 End user (data at a pickup / drop-off boundary)

The common case: serialize a value, hand the bytes to something else,
later reconstitute.

```cpp
#include <psio1/codec.hpp>

// Encode — format-scoped form is the common-case sugar:
auto buf = psio1::ssz::encode(validator);
write_file(path, psio1::bytes(buf));

// Untrusted input: validate first. Status MUST be checked —
// [[nodiscard]] + project -Werror=nodiscard makes this fail to
// compile if dropped:
auto raw = read_file(path);
if (auto status = psio1::ssz::validate<Validator>(raw); !status)
   return handle_error(status.error());

// After validate passes, field access looks like native C++:
auto v = psio1::view<Validator, psio1::ssz>(raw);
std::cout << v.effective_balance().get() << "\n";
std::cout << v.validators()[10000].pubkey().get(5).get() << "\n";

// With exceptions enabled, one-liner:
psio1::ssz::validate<Validator>(raw).or_throw();
auto v2 = psio1::view<Validator, psio1::ssz>(raw);

// Decode-to-heap (no POD zero-init):
auto heap = psio1::ssz::make_boxed<Validator>(raw);

// Storage-layer operations are free functions, never method calls
// on the view — the view's `.` surface belongs entirely to T's
// fields, so there are no collisions regardless of what T's
// fields are named:
auto   bytes  = psio1::bytes(v);        // std::span<const char>
size_t n      = psio1::size(v);         // byte count
auto   fmt    = psio1::format_of(v);    // psio1::ssz{}
auto   owner  = psio1::to_buffer(v);    // materialize owning copy

// Generic CPO form also works — useful when the format is a
// template parameter rather than a hard-coded choice:
template <typename F>
auto save(const MyCfg& cfg, F fmt) { return psio1::encode(fmt, cfg); }
```

The user names the format exactly once per call (`psio1::ssz`). No
`to_ssz`, no `convert_from_ssz`, no tag types with `_fmt` suffixes,
no nested namespaces. **Access to fields uses the natural `.`
operator** — the view's public surface is exactly the user's
reflected fields, nothing more. Storage/byte operations live in
`psio1::` free functions so they can never collide with field names.

### 4.2 User writing their own types

Reflection is the primary extension point. Simple types get nothing
format-specific:

```cpp
struct Transfer {
   std::string from;
   std::string to;
   uint64_t    amount;
   std::vector<std::string> memo;
};
PSIO1_REFLECT(Transfer, from, to, amount, memo)

auto ssz_bytes  = psio1::encode(psio1::ssz{},    xfer);
auto json_text  = psio1::encode(psio1::json{},   xfer);
auto frac_bytes = psio1::encode(psio1::frac32{}, xfer);
```

**Field-level serialization hints** are expressed as annotations on
the reflection — not as type wrappers around user fields. Size
bounds, byte-vs-text semantics, DWNC, and custom wire mappings all
live here:

```cpp
struct Validator {
   std::string              pubkey;           // opaque bytes, fixed length
   std::string              withdrawal_creds;  // opaque bytes, fixed length
   uint64_t                 effective_balance;
   bool                     slashed;
   std::array<uint64_t, 4>  epochs;
};
PSIO1_REFLECT(Validator,
   pubkey           |= psio1::as_bytes | psio1::fixed_size<48>,
   withdrawal_creds |= psio1::as_bytes | psio1::fixed_size<32>,
   effective_balance,
   slashed,
   epochs,
   psio1::definition_will_not_change)
```

Or:

```cpp
struct BeaconState {
   std::vector<Validator> validators;   // capped at 2^30 by consensus spec
   std::vector<uint64_t>  balances;     // same
};
PSIO1_REFLECT(BeaconState,
   validators |= psio1::max_size<(1ull << 30)>,
   balances   |= psio1::max_size<(1ull << 30)>)
```

The user's struct is the user's struct. `std::string pubkey` stays
`std::string pubkey` — no `bounded_bytes<48>` wrapper forcing the
serialization concern into the type system. The annotation on the
reflection tells psio how to treat it.

**Custom container types** that already model `VariableSequence`
structurally (have `size()`, `begin()`/`end()`, and either
`push_back` or `resize` + indexing) just work. Types that don't fit
the structural match opt in via a single trait specialization — this
is the escape hatch, not the primary path.

### 4.3 Contributor adding a new format

Inherit from `psio1::format_tag_base<MyFmt>` (CRTP) and define hidden-
friend `tag_invoke` overloads. The CRTP base provides format-scoped
static-member aliases (`MyFmt::encode(v)`, `MyFmt::decode<T>(bytes)`,
`MyFmt::validate<T>(bytes)`, …) for free — no extra code required
per format. The format is a self-contained module that extends psio
via ADL.

```cpp
namespace my_ns {
   struct my_fmt : psio1::format_tag_base<my_fmt> {
      // Per-format compile-time traits the generic layer queries:
      using buffer_type                = std::vector<char>;
      static constexpr bool size_is_cheap  = true;
      static constexpr bool view_is_buffer = true;

      // Hidden-friend CPO implementations. Each runs on any source
      // that satisfies the shape concept the inner body expects.
      template <typename T, typename Sink>
      friend void tag_invoke(
         decltype(psio1::encode), my_fmt, const T& v, Sink& s) noexcept
      {
         /* walk v via reflection / shape concepts, write to s */
      }

      template <typename T>
      friend T tag_invoke(
         decltype(psio1::decode), my_fmt, std::span<const char> b) noexcept
      {
         /* assumes validated bytes; build T */
      }

      template <typename T>
      friend uint32_t tag_invoke(
         decltype(psio1::size), my_fmt, const T& v) noexcept
      {
         /* compute encoded size */
      }

      template <typename T>
      friend psio1::codec_status tag_invoke(
         decltype(psio1::validate), my_fmt, std::span<const char> b) noexcept
      {
         /* structural walk; return status */
      }
   };
}
```

Defining those four hidden friends is the entire format API. All
user-facing entry points (`psio1::encode` / `decode` / `size` /
`validate`), all format-scoped aliases (`my_fmt::encode(v)` etc.),
all storage backings on `buffer` / `view`, all v1-compat sugar, and
all cross-format conversions come along for free.

Third parties can add formats in their own namespace without
touching psio headers. `psio1::encode(their_fmt{}, value)` and
`their_ns::their_fmt::encode(value)` both work.

**Two call forms, same code.** Users pick whichever reads better at
each call site:

```cpp
// Generic form — tag passed as value. Useful when format is a
// template parameter or when writing format-neutral code:
psio1::encode(psio1::frac{}, v, sink);

// Format-scoped form — shorter when format is known:
psio1::frac::encode(v, sink);
```

Both resolve to the same `tag_invoke` overload; the scoped form is
sugar supplied by `format_tag_base`.

### 4.4 Format-neutral test harness

```cpp
template <typename F>
struct codec_conformance {
   template <typename T> static void round_trip(const T&);
   template <typename T> static void size_matches_length(const T&);
   template <typename T> static void validate_accepts_own_output(const T&);
   template <typename T> static void heap_decode_equals_direct(const T&);
   template <typename T> static void re_encode_is_deterministic(const T&);
};

#define FOR_EACH_V2_FMT(M, ...) \
   M(psio1::ssz,    __VA_ARGS__) \
   M(psio1::pssz32, __VA_ARGS__) \
   M(psio1::frac32, __VA_ARGS__) \
   /* ... */

FOR_EACH_V2_FMT(V2_ROUND_TRIP_TEST, Validator,      sample_validator())
FOR_EACH_V2_FMT(V2_ROUND_TRIP_TEST, BeaconState,    sample_beacon_state())
```

Adding a format means adding one line to `FOR_EACH_V2_FMT`. Every
existing fixture runs against it automatically.

---

## 5. Architecture

### 5.1 Five layers

```
Layer 5: Sugar                        v1-compat one-liners (phase-later)
           to_ssz(v)  ≡  encode(ssz{}, v)

Layer 4: CPO entry points             psio1::encode / decode / size / validate
           / make_boxed                               ← users stop here

Layer 3: Format dispatch              tag_invoke overloads on format tag
           per-format ~4 hidden friends              ← format authors here

Layer 2: Storage + result types       buffer<T, F, Store>, view<T, F, Store>,
                                      mutable_view<T, F>

Layer 1: Shape concepts + reflection  Primitive, FixedSequence<N>, Record, …
                                      + reflection annotations              ← type authors here
```

Data flow for `psio1::encode(ssz{}, v, sink)`:

```
user code
  → Layer 4: psio1::encode CPO
             finds tag_invoke(encode, ssz, v, sink) via ADL
  → Layer 3: ssz's hidden-friend tag_invoke body
             dispatches on the shape concept T satisfies
  → Layer 1: reflection walk of v's fields via PSIO1_REFLECT
             each field's shape + annotations pick the wire behavior
  → Layer 3: primitive leaves write bytes to sink
  → Layer 2: sink is a storage-policy-aware buffer/stream
```

Every level monomorphizes at compile time. No virtuals, no type
erasure in hot paths.

### 5.2 Customization mechanism: tag_invoke CPOs

The library provides a handful of CPOs. Each is an `inline constexpr`
function object that invokes `tag_invoke(self, fmt, ...)` via
argument-dependent lookup.

```cpp
namespace psio1 {
   namespace detail {
      struct encode_fn {
         template <typename Fmt, typename T, typename Sink>
            requires requires (Fmt f, const T& v, Sink& s) {
               tag_invoke(encode_fn{}, f, v, s);
            }
         constexpr void operator()(Fmt fmt, const T& v, Sink& s) const
            noexcept(noexcept(tag_invoke(encode_fn{}, fmt, v, s)))
         {
            tag_invoke(*this, fmt, v, s);
         }
      };

      struct decode_fn  { /* template operator() → tag_invoke(decode_fn, …) */ };
      struct size_fn    { /* … */ };
      struct validate_fn { /* returns codec_status */ };
      struct make_boxed_fn { /* default-init + decode */ };
   }

   inline constexpr detail::encode_fn     encode{};
   inline constexpr detail::decode_fn     decode{};
   inline constexpr detail::size_fn       size{};
   inline constexpr detail::validate_fn   validate{};
   inline constexpr detail::make_boxed_fn make_boxed{};
}
```

Format authors define `tag_invoke` overloads as hidden friends on
their tag struct (see § 4.3). The hidden-friend form constrains name
lookup — the overloads are only found when the format tag is in the
overload set, so they do not pollute unrelated ADL resolutions.

**Format-scoped sugar via CRTP.** Every format tag inherits from
`psio1::format_tag_base<Derived>`, which provides static-member
wrappers that forward to the generic CPO. Users then write either
form at the call site; both compile to the same code:

```cpp
namespace psio1 {
   template <typename Derived>
   struct format_tag_base {
      template <typename T, typename Sink>
      static constexpr void encode(const T& v, Sink& s)
         noexcept(noexcept(psio1::encode(Derived{}, v, s)))
      { psio1::encode(Derived{}, v, s); }

      template <typename T>
      static constexpr auto encode(const T& v)
      { return psio1::encode(Derived{}, v); }

      template <typename T>
      static constexpr T decode(std::span<const char> b) noexcept
      { return psio1::decode<T>(Derived{}, b); }

      template <typename T>
      static constexpr std::uint32_t size(const T& v) noexcept
      { return psio1::size(Derived{}, v); }

      template <typename T>
      [[nodiscard]] static constexpr codec_status validate(
         std::span<const char> b) noexcept
      { return psio1::validate<T>(Derived{}, b); }

      template <typename T>
      [[nodiscard]] static constexpr codec_status validate_strict(
         std::span<const char> b) noexcept
      { return psio1::validate_strict<T>(Derived{}, b); }

      template <typename T>
      static constexpr std::unique_ptr<T> make_boxed(
         std::span<const char> b) noexcept
      { return psio1::make_boxed<T>(Derived{}, b); }
   };
}

// Non-parametric tag:
struct frac : psio1::format_tag_base<frac> { /* hidden friends … */ };

// Parametric tag:
template <typename W>
struct frac_ : psio1::format_tag_base<frac_<W>> { /* hidden friends … */ };
using frac32 = frac_<detail::frac_w32>;
using frac16 = frac_<detail::frac_w16>;
```

Call sites:

```cpp
// Generic CPO form — format passed as value. Use when writing
// format-polymorphic code:
template <typename F>
auto save(const MyCfg& cfg, F fmt) { return psio1::encode(fmt, cfg); }

// Format-scoped form — shorter when format is known at the call
// site. The common case in application code:
auto bytes = psio1::frac::encode(cfg);
auto cfg2  = psio1::frac::decode<MyCfg>(bytes);
if (auto s = psio1::frac::validate<MyCfg>(bytes); !s) …
```

**Rule**: `psio1::<fmt>` is the format's tag type. Static members of
the type provide the format-scoped form. Instance values of the type
(`psio1::<fmt>{}`) plug into the generic CPO. Both live in the same
identifier; implementation details go in `psio1::detail::<fmt>_impl::`
so no sub-namespace conflict arises.

### 5.2.5 Runtime reflection API (`psio1::reflect<T>`)

Reflection is usable at **both compile time and runtime.** Schema generators run at constexpr time; RPC dispatch runs at runtime; JSON decoders can't know field names at compile time. All are served by the same trait.

Everything below is `constexpr` where possible, so compile-time users see zero overhead and runtime users get inlined inner loops.

```cpp
namespace psio1 {
   template <typename T>
   struct reflect {
      // ── Type-level metadata ────────────────────────────────────────────
      static constexpr std::string_view name;             // "Validator"
      static constexpr std::size_t      member_count;

      // ── Per-member, indexed at compile time ────────────────────────────
      template <std::size_t I> static constexpr auto        member_pointer;     // &T::m
      template <std::size_t I> static constexpr std::string_view member_name;   // "m"
      template <std::size_t I> static constexpr std::uint32_t    field_number;  // from annotation or source order
      template <std::size_t I> using                         member_type = /*…*/;

      // ── Runtime dispatch (also constexpr-callable) ─────────────────────

      // Perfect-hash lookup by name. nullopt if not found.
      static constexpr std::optional<std::size_t>
         index_of(std::string_view name) noexcept;

      // Lookup by field-number annotation.
      static constexpr std::optional<std::size_t>
         index_of_field_number(std::uint32_t n) noexcept;

      // Visit a field by name / number — calls f with the typed member ref.
      template <typename Obj, typename F>
      static constexpr bool
         visit_field_by_name(Obj& o, std::string_view name, F&& f);

      template <typename Obj, typename F>
      static constexpr bool
         visit_field_by_number(Obj& o, std::uint32_t n, F&& f);

      // Iterate every field. `f` is called once per field with
      // (member_index, member_name, member_ref).
      template <typename Obj, typename F>
      static constexpr void for_each_field(Obj& o, F&& f);

      // ── Proxy generation — users derive for their own purposes ─────────
      // `Derived` is a CRTP base that receives dispatch callbacks; library
      // synthesizes a class with one method per field that routes through
      // `Derived`'s handler.
      template <typename Derived> struct make_proxy;
   };
}
```

#### 5.2.5.1 Perfect-hash for name → index

`reflect<T>::index_of(name)` is generated as a **compile-time-built perfect hash** over the field-name strings. Two properties:

1. **O(1) at runtime.** No string comparisons in a loop; one hash + one equality check.
2. **Zero runtime setup.** The hash tables are `constexpr` data baked into the binary.

Small structs (≤ ~8 fields) often compile to a linear if-chain faster than a hash — the library picks automatically based on `member_count` at compile time. Users don't select a strategy.

```cpp
// Example usage — JSON decoder dispatching field names:
bool handle_json_key(Validator& v, std::string_view key, auto& reader) {
   return psio1::reflect<Validator>::visit_field_by_name(v, key,
      [&](auto& field) { reader.read_into(field); });
}
```

#### 5.2.5.2 Proxy generation — pattern for RPC, views, custom accessors

`reflect<T>::make_proxy<Derived>` synthesizes a class that exposes one method per reflected field, routing through `Derived`'s generic handler. The Derived class decides what each call means.

```cpp
// RPC client stub — every field access marshals a call over the wire.
struct rpc_client_base {
   template <typename FieldRef>
   auto dispatch(FieldRef, /*args*/) { /* marshal, send, receive */ }
};

using client_stub = psio1::reflect<PaymentService>::make_proxy<rpc_client_base>;
client_stub c(connection);
c.transfer(from, to, amount);   // marshals Method "transfer" over the wire

// RPC server handler — incoming call name dispatches to the right method.
struct server_dispatch {
   PaymentService& impl;
   bool handle(std::string_view method, auto& args, auto& reply) {
      return psio1::reflect<PaymentService>::visit_field_by_name(impl, method,
         [&](auto& fn) { fn.invoke(args, reply); });
   }
};

// Debug-printer — same machinery, different Derived.
struct printer {
   template <typename FieldRef>
   void handle(std::string_view name, const FieldRef& ref) {
      std::cout << name << " = " << ref << "\n";
   }
};
```

The same `make_proxy` is what view generation uses internally — `view<T, Fmt>`'s field accessors are a proxy whose handler reads from the buffer at the right offset.

#### 5.2.5.3 Schema generation as a client of reflect

Every schema exporter (`to_fbs_schema<T>`, `to_capnp_schema<T>`, `to_wit_schema<T>`, …) is a `consteval` function that walks `reflect<T>` and emits text. No separate schema database; the reflection IS the schema:

```cpp
template <typename T>
consteval std::string to_fbs_schema() {
   std::string out = "table ";
   out += reflect<T>::name;
   out += " {\n";
   for (std::size_t i = 0; i < reflect<T>::member_count; ++i) {
      out += "  ";
      out += reflect<T>::member_name<i>;
      out += ": ";
      out += fbs_type_name_of<reflect<T>::template member_type<i>>();
      if (auto fn = reflect<T>::field_number<i>) {
         out += " (id: ";
         out += std::to_string(fn);
         out += ")";
      }
      out += ";\n";
   }
   out += "}\n";
   return out;
}
```

This composes with `psio1::annotate<&T::m>` queries — the schema walker checks each field's annotations and emits format-specific constraints (bounds, defaults, DWNC markers) accordingly.

#### 5.2.5.4 Relationship to PSIO1_REFLECT vs C++26

`psio1::reflect<T>` is populated one of two ways:

- **Today (C++23):** `PSIO1_REFLECT(T, fields...)` macro emits the specialization, including the perfect-hash table and per-field metadata structs.
- **Post-C++26:** a library primary template uses `std::meta::nonstatic_data_members_of(^T)` + `template for` to synthesize the same data structure, including generating the perfect hash via consteval computation.

**The surface is identical.** Code that queries `reflect<T>::index_of("field")` or iterates `for_each_field(v, f)` does not care which backend populated the trait.

**Visibility:** `reflect<T>` exposes only **public, non-static data members**. Private/protected members and static data are not reflected. This matches current PSIO1_REFLECT behavior and avoids a whole class of language-lawyer questions around friend access and access specifiers in std::meta::.

---

### 5.2.6 Dynamic / type-erased access

Static `reflect<T>` requires T at compile time. But real psiserve/psibase use cases need the same operations when T is only known at runtime:

- Service gateway receives RPC messages whose payload types aren't linked into the gateway binary.
- Schema-aware tooling transcodes between formats driven by a parsed schema file.
- Dynamic validators check untrusted bytes against a schema loaded from disk.
- Introspection UIs let users navigate encoded bytes by field path.

Dynamic access runs on a **runtime schema value** (`psio1::schema`) plus bytes, with no C++ type T involved anywhere.

#### 5.2.6.1 `psio1::schema` — runtime schema value

A schema captures the same information `reflect<T>` has, but as runtime data instead of template metadata:

```cpp
namespace psio1 {
   // Primitive kinds and composite constructors.
   enum class primitive_kind {
      u8, u16, u32, u64, u128, u256,
      i8, i16, i32, i64, i128,
      f32, f64, bool_,
   };

   struct schema;  // forward — types reference schemas recursively

   // A type reference is either a primitive or a pointer to a named schema.
   struct type_ref {
      std::variant<primitive_kind, const schema*> repr;
   };

   struct field_descriptor {
      std::string    name;
      std::uint32_t  field_number;
      type_ref       type;            // primitive, or nested schema, or list<schema>, etc.
      /* Annotations carried as a vector of type-erased spec values. */
      std::vector<dynamic_spec> annotations;
   };

   enum class shape_kind {
      record,          // a struct
      variable_seq,    // vector-like
      fixed_seq,       // array[N]
      optional,
      variant,
      bitfield,
   };

   struct schema {
      std::string                     name;
      shape_kind                      shape;
      std::vector<field_descriptor>   fields;       // for record
      type_ref                        element;      // for sequences/optionals
      std::size_t                     fixed_size;   // for fixed_seq / bitvector
      std::vector<dynamic_spec>       type_annotations;

      // Perfect hash built at schema-construction time:
      std::optional<std::size_t>  index_of(std::string_view name) const noexcept;
      std::optional<std::size_t>  index_of_field_number(std::uint32_t n) const noexcept;
   };

   // Type-erased spec value — carries annotation kind + payload.
   struct dynamic_spec {
      std::string_view                          kind;    // "bytes_spec", "max_size_spec", etc.
      /* value payload — variant or small-buffer-stored */
   };
}
```

#### 5.2.6.2 Bridging static ↔ dynamic

`reflect<T>::schema()` is a `consteval` function returning a `psio1::schema` value constructed from the compile-time reflection. Callable at constexpr time (for consteval schema exporters) or at runtime (to obtain a schema for later dynamic use).

```cpp
// Freeze compile-time reflection into a runtime schema:
constexpr psio1::schema validator_schema = psio1::reflect<Validator>::schema();

// Parse a schema from text (WIT / capnp / protobuf / fbs):
psio1::schema loaded = psio1::parse_wit(wit_text);

// Both are the same kind of value — same operations apply.
```

#### 5.2.6.3 Dynamic codec operations

All format tags that support dynamic operation provide a parallel set of `tag_invoke` overloads that take a `schema` instead of a type parameter:

```cpp
namespace psio1 {
   inline constexpr struct validate_dynamic_fn { /* CPO */ } validate_dynamic{};
   inline constexpr struct encode_dynamic_fn   { /* CPO */ } encode_dynamic{};
   inline constexpr struct decode_dynamic_fn   { /* CPO */ } decode_dynamic{};
   inline constexpr struct transcode_fn        { /* CPO */ } transcode{};
}

// Format-side (e.g. for ssz):
struct ssz : format_tag_base<ssz> {
   // Static path, as defined elsewhere.

   // Dynamic path — takes schema, not T:
   friend codec_status tag_invoke(
      decltype(psio1::validate_dynamic), ssz,
      const schema&, std::span<const char> bytes) noexcept;

   friend void tag_invoke(
      decltype(psio1::encode_dynamic), ssz,
      const schema&, const dynamic_value& value, auto& sink) noexcept;

   friend dynamic_value tag_invoke(
      decltype(psio1::decode_dynamic), ssz,
      const schema&, std::span<const char> bytes) noexcept;
};
```

User-facing one-line operations:

```cpp
// Validate bytes against a schema:
if (auto s = psio1::validate_dynamic(psio1::ssz{}, schema, bytes); !s) …

// Transcode — the killer feature:
std::vector<char> json_out;
auto status = psio1::transcode(
   schema,
   psio1::ssz{}, ssz_bytes,     // from
   psio1::json{}, json_out);    // to
```

`transcode` is a library-provided free function implemented in terms of the format CPOs. It walks the schema: for each field, reads from input format → dispatches to output format's `encode_dynamic` write. **Never materializes a C++ object.** Byte-to-byte transformation driven by the schema.

Byte-identity guarantee: `encode_dynamic(fmt, reflect<T>::schema(), decode_dynamic(fmt, schema, b))` must equal `b` whenever `b` is valid; `encode_dynamic(fmt, reflect<T>::schema(), decode(fmt, b))` equals `encode(fmt, T-value)`. Static and dynamic paths are round-trip interchangeable.

#### 5.2.6.4 Dynamic view — `psio1::dynamic_view`

A runtime-typed view for navigating bytes by path without decoding:

```cpp
psio1::dynamic_view v = psio1::as_dynamic_view(psio1::ssz{}, schema, bytes);

// Field navigation by string name — one hash lookup:
auto eb_view = v["effective_balance"];
std::uint64_t eb = eb_view.as<std::uint64_t>().value();

// Nested paths:
auto pk_byte = v["validators"][10000]["pubkey"][5].as<std::uint8_t>().value();

// Iteration:
for (auto&& [name, field_view] : v.fields()) {
   std::cout << name << ": " << field_view.type_name() << "\n";
}
```

`dynamic_view` wraps a `(schema_ref, span<const char>)` pair. Field access resolves the schema + format offset rules at runtime. For sorted sequences, `operator[]` on a key does binary search (schema tells the view the sequence is sorted).

`dynamic_value` is the owning counterpart — holds its own bytes (for output) and a schema reference.

#### 5.2.6.5 Dynamic support is per-format opt-in

Not every format fits every schema shape at runtime. Format authors declare their dynamic-support scope via a trait:

```cpp
struct ssz : format_tag_base<ssz> {
   static constexpr bool supports_dynamic_validate = true;
   static constexpr bool supports_dynamic_encode   = true;
   static constexpr bool supports_dynamic_decode   = true;
   static constexpr bool supports_dynamic_transcode = true;
};
```

`transcode` between two formats checks both sides support it at compile time; a concept rejects the call otherwise with a readable diagnostic.

#### 5.2.6.6 Use cases

**Gateway RPC service:**
```cpp
// Loaded schema per service at startup:
std::unordered_map<std::string, psio1::schema> services;
services["payment.transfer"] = psio1::parse_wit(load("payment.wit"));

// Incoming request — schema discovered by method name, not linked in:
auto status = psio1::validate_dynamic(
   psio1::frac32{}, services[method_name], incoming_bytes);
if (!status) return reply_error(status.error());
// Transcode to the wire format this gateway's backend speaks:
std::vector<char> ssz_bytes;
psio1::transcode(services[method_name],
                psio1::frac32{}, incoming_bytes,
                psio1::ssz{},    ssz_bytes);
backend.send(method_name, ssz_bytes);
```

**Schema-aware transcoder:**
```cpp
int main(int argc, char** argv) {
   auto schema = psio1::parse_capnp(read_file(argv[1]));
   auto in     = read_file(argv[2]);
   std::vector<char> out;
   psio1::transcode(schema, psio1::capnp{}, in, psio1::json{}, out);
   write_file(argv[3], out);
}
// `capnp2json my.capnp in.bin out.json` — no user C++ types, pure schema-driven.
```

**Runtime validator for untrusted boundaries:**
```cpp
// HTTP handler validating an arbitrary declared body type:
if (auto s = psio1::validate_strict_dynamic(
      psio1::json{}, declared_schema, body); !s)
   return http_400(s.error().what);
```

#### 5.2.6.7 Third-party annotation serialization

Schemas need a generic way to round-trip annotations defined by any third-party
namespace. Specs are data. Behavior belongs to the format or tool that consumes
the spec; the core library does not require a behavior CPO on spec types.

The representation must:

- **Preserve unknown specs** on round-trip. If the loader does not know
  `foo::bar_spec`, emitting the schema back out must reproduce it.
- **Require no central registry.** A schema may carry any namespaced spec.
- **Keep interpretation open.** A format recognizes the specs it understands
  by convention and ignores the rest.

**Representation in the schema:**
```cpp
namespace psio1 {
   struct dynamic_spec {
      std::string_view  kind_name;   // e.g. "myfmt::retry_spec"
      std::vector<char> payload;     // canonical schema encoding
   };
}
```

**Unknown specs pass through unchanged.** Loaders preserve the
`dynamic_spec { kind_name, payload }` verbatim; emitters write it back without
parsing. Open-protocol property holds: a schema round-trips through code that
does not understand its annotations.

**Typed read at the consumer:**
```cpp
for (const auto& ds : field.annotations) {
   if (ds.kind_name == "myfmt::retry_spec") {
      auto spec = myfmt::decode_retry_spec(ds.payload);
      // consume spec here
   }
   // Other kinds: skipped on this code path, preserved in the schema.
}
```

**Compile-time -> runtime bridge:** `reflect<T>::schema()` walks effective
annotations (type + wrapper + member) and emits the first-party specs it knows
how to render. Third-party specs are still pure data; schema exporters can add
adapters for specs they care about without forcing a protocol on every spec
author.

#### 5.2.6.8 Visibility into compile-time shapes

`reflect<T>::schema()` is consteval, so the schema is available at compile time too. This means:

- Consteval schema exporters (`to_wit_schema<T>()`, `to_capnp_schema<T>()`, `to_fbs_schema<T>()`) are trivial wrappers: `return emit_wit(reflect<T>::schema());`.
- Static asserts can compare shapes: `static_assert(reflect<Payment>::schema() == parsed_payment_schema);` at compile time, ensuring the C++ type matches the schema file.
- Cross-language binding generators consume `reflect<T>::schema()` directly.

---

### 5.3 Shape concepts + reflection annotations

**Structural shape concepts** (Layer 1):

| Concept | Matches | Format authors' hook |
|---|---|---|
| `Primitive` | `uint8/16/32/64`, `int8/16/32/64`, `float`, `double`, `bool` | per-primitive write/read |
| `Enum` | `std::is_enum_v<T>` | underlying primitive |
| `FixedSequence<N>` | `std::array<T, N>`, C arrays | element walk |
| `VariableSequence` | types with `size()` + `begin/end` + `push_back`/`resize` | element walk |
| `Optional` | `std::optional`, user types with `has_value`/`value` | selector + payload |
| `Variant` | `std::variant`, user types with `index`/`visit` | tag + payload |
| `Bitfield` | `psio1::bitvector<N>`, `psio1::bitlist<N>`, `std::bitset<N>` | bit-packed |
| `Record` | reflected structs (via PSIO1_REFLECT today, or C++26 reflection) | field walk |

Shapes are purely structural; **semantic variation** (bytes vs text, bounds, default values, custom validators) is expressed as annotations — see below.

**Producer / consumer model.** Annotations are a single-direction protocol:

- **Producers (users)** attach specs to types and members. Specs are plain data structs — designated-init-friendly aggregates with no behavior.
- **Consumers (formats)** read the specs they recognize during encode/decode/validate/schema. A consumer that doesn't recognize a spec kind ignores it. A consumer that does recognize one decides for itself how to apply it (SSZ and JSON may both honor a length bound, but one expresses it in offset tables and the other in string length).
- The library ships a **canonical vocabulary** (`length_bound`, `utf8`, `sorted`, `field_num`, …) that every first-party format agrees on; third parties can introduce their own specs in their own namespaces without coordinating with psio, and the format authors that want to consume them do so in their own code.

Specs are data, not behavior. This keeps the split clean — adding a spec never forces every consumer to be updated, and adding a consumer never requires extending the spec vocabulary.

**Type-level vs member-level — member always wins.** Annotations attach either to a type (`psio1::annotate<psio1::type<T>{}>`) or to a specific data member (`psio1::annotate<&T::field>`). The resolution rule throughout psio is uniform:

> **Member annotation → wrapper-inherent annotation → type annotation → format-default fallback.**

This single precedence rule governs the annotation system, format annotations (§5.3.7), and every format-level dispatch. There are no per-subsystem variations.

#### 5.3.1 `psio1::annotate` — unified annotation trait

Per-field and per-type annotations live in one open-ended variable template. The value is a `std::tuple<...>` of spec structs. Format authors read only the specs they recognize; unknown specs are silently passed through.

```cpp
namespace psio1 {
   // Type wrapper so the same `annotate` name can key on a type too.
   // (C++ NTTPs can't be bare types; member pointers don't need the wrapper.)
   template <typename T> struct type {
      constexpr bool operator==(const type&) const = default;  // structural
   };

   // Primary — no annotations.
   template <auto X>
   inline constexpr auto annotate = std::tuple<>{};

   // Helper to read specs from an annotation tuple. Returns
   // std::optional<Wanted>; nullopt if the spec is not present.
   template <typename Wanted, typename Tuple>
   constexpr auto find_spec(const Tuple&) noexcept -> std::optional<Wanted>;
}
```

Usage:

```cpp
// Field-level — bare member pointer:
template <>
inline constexpr auto psio1::annotate<&Validator::pubkey> = std::tuple{
   psio1::bytes_spec{.size = 48, .field_num = 1},
};

// Type-level — wrap the type:
template <>
inline constexpr auto psio1::annotate<psio1::type<Validator>{}> = std::tuple{
   psio1::definition_will_not_change{},
};
```

#### 5.3.2 Built-in spec types

All spec types live in `psio1::` and are aggregates with designated-init-friendly members. The `length_bound` spec is the unified size-like vocabulary — one struct covers exact, minimum, and maximum length constraints regardless of whether the target shape's length dimension is bytes, elements, or bits. The shape, not the spec, picks which dimension is being bounded:

| Spec | Members | Use |
|---|---|---|
| `psio1::length_bound` | `exact`, `max`, `min` (all `std::optional<std::uint32_t>`) | length constraint; shape-dimension-dependent (byte count for `ByteString`, element count for `VariableSequence`, bit count for `Bitlist`) |
| `psio1::utf8_spec` | `max`, `field_num` | UTF-8 text; semantic validator checks well-formedness |
| `psio1::hex_spec` | `bytes`, `field_num` | bytes encoded as hex text on the wire |
| `psio1::sorted_spec` | `unique`, `ascending` | sequence is sorted; `unique=true` = set semantics |
| `psio1::default_value_spec<T>` | `value` | default when the field is absent on the wire (extensibility) |
| `psio1::default_factory_spec<F>` | (compile-time factory fn) | non-trivial defaults |
| `psio1::field_num_spec` | `value` | protobuf/capnp ordinal |
| `psio1::skip_spec` | `value` | skip this field for the given format |
| `psio1::definition_will_not_change` | *(empty)* | type-level: wire layout frozen; enables DWNC fast paths. **Spelled out, not abbreviated, so the reader sees the commitment.** |

The pre-consolidation `bytes_spec{.size = N}` and `max_size_spec{.max = N}` both fold into `length_bound`:
- `bytes_spec{.size = 48}` → `length_bound{.exact = 48}`.
- `max_size_spec{.max = 256}` → `length_bound{.max = 256}`.

**Static vs runtime spec tagging.** Specs split into two categories:

- **Static specs** influence code generation and emit no runtime work once dispatch is resolved: `field_num_spec`, `skip_spec`, `definition_will_not_change`.
- **Runtime specs** are checked at validate time: `length_bound`, `utf8_spec`, `sorted_spec`.

Each spec type declares its category via a nested tag:

```cpp
namespace psio1 {
   struct static_spec_tag {};
   struct runtime_spec_tag {};

   // Example declarations:
   struct field_num_spec {
      using spec_category = static_spec_tag;
      std::uint32_t value = 0;
   };

   struct length_bound {
      using spec_category = runtime_spec_tag;
      std::optional<std::uint32_t> exact;
      std::optional<std::uint32_t> max;
      std::optional<std::uint32_t> min;
   };
}
```

Formats use the category to split the effective annotation tuple at compile time — static specs drive `if constexpr` dispatch during codegen; runtime specs become a loop in `validate()`. `validate()` never pays for specs that are already resolved.

**Shape applicability — compile-time safeguard.** Each spec declares which shapes it's meaningful on. The library emits a `static_assert` when `effective_annotations` attaches a spec to a member whose shape is not in the spec's `applies_to` set:

```cpp
namespace psio1 {
   struct length_bound {
      using spec_category = runtime_spec_tag;

      // Meaningful only on shapes that carry a length dimension.
      using applies_to = shape_set<
         VariableSequence,
         ByteString,
         Bitlist>;

      std::optional<std::uint32_t> exact;
      std::optional<std::uint32_t> max;
      std::optional<std::uint32_t> min;
   };
}
```

Attaching `length_bound{.max = 256}` to a `Record` or a `Primitive` field is a compile error at the point of annotation, not a silent no-op at the validator.

#### 5.3.3 Semantic validators — the two-phase model

Spec structs may optionally define `static codec_status validate(std::span<const char>) noexcept`. This is the **semantic** check — distinct from structural validation.

| | Structural validation | Semantic validation |
|---|---|---|
| Purpose | Makes noexcept access sound | Enforces content invariants |
| Required before `decode` / view access? | **Yes** — precondition | **No** — access works without it |
| Cost | Bounds arithmetic only | Variable: UTF-8 scan, regex, custom |
| Entry point | `psio1::validate(fmt, bytes)` | `psio1::validate_strict(fmt, bytes)` |
| User-extensible? | No — format-internal | **Yes** — any spec's `validate` member |

- `validate` returns once structural checks pass. Post-validation, field access is noexcept and sound even if semantic invariants are violated.
- `validate_strict` runs structural checks AND invokes every `validate` member on every spec attached to every field, collecting the first failure.

This split lets trust-domain code (WASM guest, intra-process IPC) pay only structural-check cost, while boundary code (network input, cross-trust-domain RPC) pays for semantic enforcement.

```cpp
namespace psio1 {
   struct utf8_spec {
      std::uint32_t max = 0;
      std::uint32_t field_num = 0;

      // Semantic validator — only invoked by validate_strict:
      static codec_status validate(std::span<const char> bytes) noexcept {
         if (max && bytes.size() > max)
            return codec_status{codec_error{"utf8 over max", 0, "utf8_spec"}};
         return is_well_formed_utf8(bytes)
            ? codec_status{}
            : codec_status{codec_error{"invalid UTF-8", 0, "utf8_spec"}};
      }
   };
}
```

Users write their own spec types with their own validators — no library modification required:

```cpp
namespace myapp {
   struct email_spec {
      std::uint32_t max = 256;
      static codec_status validate(std::span<const char> bytes) noexcept {
         return is_valid_email(bytes)
            ? codec_status{}
            : codec_status{codec_error{"invalid email", 0, "email_spec"}};
      }
   };
}

template <>
inline constexpr auto psio1::annotate<&Contact::email> = std::tuple{
   myapp::email_spec{.max = 256},
};
```

#### 5.3.4 Third-party annotation extension

Any third-party format (or non-serialization consumer) can define its own spec types in its own namespace and put them into the annotate tuple. psio does not need to be modified:

```cpp
namespace myfmt {
   struct retry_spec      { std::uint32_t attempts = 1; };
   struct compression_spec{ std::uint32_t level    = 0; };
   struct priority_spec   { std::int8_t   priority = 0; };
}

template <>
inline constexpr auto psio1::annotate<&Message::payload> = std::tuple{
   psio1::bytes_spec{.size = 1024, .field_num = 3},
   myfmt::retry_spec{.attempts = 5},
   myfmt::compression_spec{.level = 9},
};

// In myfmt's encoder:
auto anns = psio1::annotate<&T::m>;
if (auto r = psio1::find_spec<myfmt::retry_spec>(anns)) { /* use r->attempts */ }
```

Unknown specs are ignored by formats that don't recognize them — the annotation system is an **open protocol**, not a closed enum.

#### 5.3.5 Surface forms — today vs C++26

The same logical information can be expressed four ways. They all produce the same post-expansion representation (entries in `psio1::annotate<...>` and `psio1::reflect<...>`).

**Form 1 — `PSIO1_REFLECT` macro (today, most ergonomic for C++23):**

```cpp
struct Validator {
   std::string pubkey;
   std::string withdrawal_creds;
   std::uint64_t effective_balance;
   bool slashed;
};

PSIO1_REFLECT(Validator,
   pubkey           |= psio1::bytes<48> | psio1::field<1>,
   withdrawal_creds |= psio1::bytes<32> | psio1::field<2>,
   effective_balance |= psio1::field<3>,
   slashed          |= psio1::field<4>,
   psio1::definition_will_not_change)
```

**Form 2 — `PSIO1_ATTRS` macro (add annotations to an already-reflected type):**

```cpp
// Reflection declared elsewhere:
PSIO1_REFLECT(Validator, pubkey, withdrawal_creds, effective_balance, slashed)

// Later, in a schema / bindings / app header, add annotations without touching reflection:
PSIO1_ATTRS(Validator,
   pubkey           |= psio1::bytes<48> | psio1::field<1>,
   withdrawal_creds |= psio1::bytes<32> | psio1::field<2>,
   effective_balance |= psio1::field<3>,
   slashed          |= psio1::field<4>,
   type             |= psio1::definition_will_not_change)
```

Multiple `PSIO1_ATTRS` calls can target the same type; each adds to the relevant `psio1::annotate<>` specializations. Useful when one library defines the struct and another adds format-specific hints.

**Form 3 — desugared, no macros (today and post-C++26):**

```cpp
struct Validator { /* fields */ };
PSIO1_REFLECT(Validator, pubkey, withdrawal_creds, effective_balance, slashed)   // or C++26 auto-reflection

template <> inline constexpr auto psio1::annotate<&Validator::pubkey> = std::tuple{
   psio1::bytes_spec{.size = 48, .field_num = 1},
};
template <> inline constexpr auto psio1::annotate<&Validator::withdrawal_creds> = std::tuple{
   psio1::bytes_spec{.size = 32, .field_num = 2},
};
template <> inline constexpr auto psio1::annotate<&Validator::effective_balance> = std::tuple{
   psio1::field_num_spec{.value = 3},
};
template <> inline constexpr auto psio1::annotate<&Validator::slashed> = std::tuple{
   psio1::field_num_spec{.value = 4},
};
template <> inline constexpr auto psio1::annotate<psio1::type<Validator>{}> = std::tuple{
   psio1::definition_will_not_change{},
};
```

What `PSIO1_REFLECT` + annotations (Form 1) and `PSIO1_ATTRS` (Form 2) expand to at compile time. Users may write the desugared form directly.

**Form 4 — post-C++26, no PSIO1_REFLECT at all:**

```cpp
struct Validator {
   std::string pubkey;
   std::string withdrawal_creds;
   std::uint64_t effective_balance;
   bool slashed;
};
// No macro — reflection auto-sourced from `^Validator`. `psio1::reflect<Validator>`
// is populated by a library template that internally calls std::meta::nonstatic_data_members_of.

// Annotate only the fields that need it:
template <>
inline constexpr auto psio1::annotate<&Validator::pubkey> = std::tuple{
   psio1::bytes_spec{.size = 48, .field_num = 1},
};
template <>
inline constexpr auto psio1::annotate<&Validator::withdrawal_creds> = std::tuple{
   psio1::bytes_spec{.size = 32, .field_num = 2},
};
template <>
inline constexpr auto psio1::annotate<psio1::type<Validator>{}> = std::tuple{
   psio1::definition_will_not_change{},
};
// effective_balance, slashed — no annotations, source order gives field numbers.
```

Users migrate incrementally: keep `PSIO1_REFLECT` for now, drop it per-type once their compiler supports C++26 reflection, annotate only the fields they need.

**User types stay clean across all four forms.** Where v1 required `std::string` → `bounded_bytes<48>` wrapper, v2 leaves the user's struct untouched and moves the bound into an annotation.

#### 5.3.6 Rich wrapper types — the opt-in invasive alternative

Some users *want* the invariant baked into the type system — a `bounded<std::string, 48>` value statically rejects constructors that would overflow, a `utf8_string<N>` rejects invalid UTF-8 at set time, a `sorted_set<T, N>` maintains its sort order on insertion. These are ergonomic when the library code works with the value before serialization, not just at wire boundaries.

v2 offers both paths as first-class; users choose per field:

```cpp
// Non-invasive path — attributes, no wrappers:
struct ContactA {
   std::string email;
};
template <> inline constexpr auto psio1::annotate<&ContactA::email> = std::tuple{
   myapp::email_spec{.max = 256},
};

// Invasive path — rich types carry their constraints:
struct ContactB {
   psio1::utf8_string<256> email;   // validates on set; type-level guarantee
};
// No annotation needed — the type carries everything.
```

Both produce identical wire output.

**Library-provided wrapper types** (all non-intrusive on `std::string` / `std::vector` underneath, opt-in for users):

| Wrapper | Equivalent annotation |
|---|---|
| `psio1::bounded<T, N>` | `psio1::max_size_spec{.max = N}` |
| `psio1::utf8_string<MaxN>` | `psio1::utf8_spec{.max = MaxN}` with `validate` |
| `psio1::hex_string<FixedN>` | `psio1::hex_spec{.bytes = FixedN}` |
| `psio1::byte_array<FixedN>` | `psio1::bytes_spec{.size = FixedN}` |
| `psio1::sorted_vec<T, N>` | `psio1::max_size_spec{.max = N}` + `psio1::sorted_spec{}` |

Each wrapper exposes its inherent annotations via a trait so the library can reason about them:

```cpp
namespace psio1 {
   template <typename T>
   struct inherent_annotations { using type = std::tuple<>; };   // default: none

   template <typename T, std::size_t N>
   struct inherent_annotations<bounded<T, N>> {
      using type = std::tuple<max_size_spec>;
      static constexpr auto value = std::tuple{ max_size_spec{.max = N} };
   };

   template <std::size_t N>
   struct inherent_annotations<utf8_string<N>> {
      using type = std::tuple<utf8_spec>;
      static constexpr auto value = std::tuple{ utf8_spec{.max = N} };
   };
   // ... etc
}
```

**Composition and static conflict detection — three-way merge.** A field's effective annotations come from three sources, merged with **member > wrapper > type** precedence. Type-level annotations come from the field's own type, wrapper annotations come from the field type's inherent wrapper facts, and member annotations come from the reflected member pointer. At compile time, the library checks each source for internal contradictions; cross-source duplicates are resolved by precedence:

```cpp
struct Validator {
   psio1::byte_array<48> pubkey;   // wrapper: length_bound{.exact = 48}
};

// OK: member annotation adds field_num without contradicting the wrapper's 48:
template <> inline constexpr auto psio1::annotate<&Validator::pubkey> = std::tuple{
   psio1::field_num_spec{.value = 1},
};

// OK: member annotation explicitly overrides the wrapper's bound for this field.
template <> inline constexpr auto psio1::annotate<&Validator::pubkey> = std::tuple{
   psio1::length_bound{.exact = 32},   // member override wins
   psio1::field_num_spec{.value = 1},
};

// Type-level defaults belong to the field type. They are lower precedence than
// wrapper inherent annotations and member annotations.
template <> inline constexpr auto psio1::annotate<psio1::type<psio1::byte_array<48>>{}> = std::tuple{
   psio1::field_num_spec{.value = 1},    // type-level default
};
template <> inline constexpr auto psio1::annotate<&Validator::pubkey> = std::tuple{
   psio1::field_num_spec{.value = 7},    // member-level override — wins
};
```

The merge helper:

```cpp
namespace psio1 {
   template <typename FieldType, auto MemberPtr>
   struct effective_annotations {
      // Type-level annotations from the field's type.
      static constexpr auto type_anns = annotate<type<FieldType>{}>;

      // Wrapper-inherent annotations from the field's type (e.g. byte_array<48>).
      static constexpr auto wrapper_anns = inherent_annotations<FieldType>::value;

      // Member-level annotations from the member pointer.
      static constexpr auto member_anns = annotate<MemberPtr>;

      // Last matching spec wins, so tuple order encodes the precedence.
      static constexpr auto value =
         std::tuple_cat(type_anns, wrapper_anns, member_anns);
   };
}
```

Format authors query `effective_annotations` (never `annotate` directly) so that the three sources unify into a single spec tuple by the time wire code sees it.

**Why wrapper sits between member and type.** A wrapper is a type-level fact about the *field's* type, not about the enclosing record. A user-supplied type-level annotation on the enclosing record shouldn't silently override a wrapper's inherent bound — the wrapper is closer to the data. But a user-supplied *member*-level annotation is an explicit per-field decision and wins over both.

**Rule of thumb:**
- Use **wrappers** (rich types) when the invariant matters in business logic before serialization — e.g. you want the email to be rejected at construction time, not just at wire-validation time.
- Use **annotations** (attributes) when the constraint is purely a wire-boundary concern — e.g. "this field is 48 bytes on the wire; I don't care if the in-memory `std::string` is longer before serialization" (it'd be truncated or rejected at encode time).
- Mix freely. Within one annotation source, contradictions are compile-time errors; across sources, the explicit precedence rule decides which spec is effective.

#### 5.3.7 Format annotations — semantic leaves with alternate encodings

Reflection walks records, sequences, optionals, variants, and bitfields through
shape dispatch. Some domain types should instead be treated as semantic leaves:

- `name_id` stores a `uint64_t`, but JSON uses the canonical string `"alice"`.
- A hash stores bytes, but text may use hex or base58.
- A timestamp stores a numeric count, but text may use RFC3339.
- A strong wrapper may collapse to the wrapped value on the wire.
- A rich reflected object may render as an XML string for one JSON field.
- A reflected object may render as bin bytes inside a fracpack structure.

These leaf encodings are ordinary annotations:

```cpp
psio1::binary_format<Adapter>{}
psio1::text_format<Adapter>{}
```

The annotation is data: it names an adapter. Interpretation is behavior owned
by the consuming format. JSON asks effective annotations for `text_format`.
Binary-oriented formats ask for `binary_format`. Other categories can be added
later using the same producer/consumer rule.

**Serialization order when a format annotation is in play:**

```text
logical C++ type
  -> selected format annotation
  -> adapter shape (uint64_t, string, fixed_bytes<N>, ...)
  -> format encoder / decoder / validator for that shape
```

If no matching format annotation exists, psio continues normal shape dispatch.

**Adapter declaration:**

```cpp
struct name_binary
{
   using shape_type = std::uint64_t;
   static constexpr shape_type to(name_id n) noexcept { return n.value; }
   static constexpr name_id from(shape_type v) noexcept { return name_id{v}; }
};

struct name_text
{
   using shape_type = std::string_view;
   static std::string to(name_id n) { return n.str(); }
   static name_id from(std::string_view s) { return name_id::parse_or_abort(s); }
   static psio1::codec_status validate(std::string_view s) noexcept;
};
```

Required adapter members are `shape_type`, `to`, and `from`. `validate` is
optional and is used when the shape domain is wider than the logical type.

Adapters may also represent a nested encoded subtree. The simple implemented
form uses `format_adapter<T, Fmt>`, a byte-shaped adapter:

```cpp
using payload_bin = psio1::format_adapter<payload, psio1::bin>;
```

When consumed by a delimiter-aware parent format such as fracpack, SSZ, pssz,
Avro, Borsh, Bincode, key, a native FlatBuffers table field, Cap'n Proto, or
WIT canonical ABI, the parent frames the bytes and the adapter validates the
delegated format. The optimized Adapter contract is the same boundary without
mandatory materialization:
`packsize(value)`, streamed `encode(value, sink)`, `decode(span)`,
`validate(span)`, and `validate_strict(span)`. The staged C++ implementation
has this direct byte-shaped path for `bin`, `frac`, `ssz`, `pssz`, `key`,
`avro`, `borsh`, `bincode`, native `flatbuf`, `capnp`, and `wit`; `key` keeps
ownership of null escaping and validates/constructs from the unescaped span.
Native `flatbuf`, `capnp`, and `wit` named view accessors now expose
Adapter-annotated leaves as logical values, while their format-specific schema
text generators emit the selected Adapter wire shape (`[ubyte]`, `Data`,
`list<u8>`, etc.). Runtime SchemaBuilder records `binary-format` /
`text-format` attributes with the selected Adapter shape. Dynamic `frac`
validation/decode/encode and same-format transcode compile runtime schemas
through `binary-format` wire shapes. Cap'n Proto dynamic views expose Adapter
fields by wire shape and can materialize logical fields via `dynamic_view::as<T>()`.
Cross-format dynamic transcode and mutable views still need to consume that
metadata without changing their public APIs.

**Type-level defaults:**

```cpp
template <>
inline constexpr auto psio1::annotate<psio1::type<name_id>{}> = std::tuple{
   psio1::binary_format<name_binary>{},
   psio1::text_format<name_text>{},
};
```

The non-intrusive spelling is:

```cpp
PSIO1_ATTRS(psio1::type<app::name_id>{},
           psio1::binary_format<app_name_binary>{},
           psio1::text_format<app_name_text>{});
```

**Member override:**

```cpp
struct transaction
{
   app::name_id account;
};

PSIO1_REFLECT(transaction, account)

PSIO1_ATTRS(&transaction::account,
           psio1::text_format<app_name_base58>{});
```

Resolution uses the same `effective_annotations` tuple as every other spec:
type-level annotations first, wrapper inherent annotations next, member
annotations last. Last matching `text_format` or `binary_format` wins, so the
precedence is member > wrapper > type.

This applies to rich objects too. Encoding `payload` directly as JSON may produce
`{"id":7,"label":"seven"}`, while a member annotated with
`text_format<payload_xml>` produces an escaped XML string at that field. Likewise
a type-level or member-level `binary_format<payload_bin>` can make a reflected
payload appear as a byte leaf when a binary-oriented format asks for binary.

**Packsize** is computed on the selected adapter shape, not on the logical type.
For `name_id` under bin, the selected shape is `uint64_t` and the size is 8
bytes. For JSON, the selected shape is a string and the size is the JSON string
size including quotes and escaping.

**Validation** follows the same path:

```text
wire bytes
  -> format validates shape
  -> optional adapter validate(shape)
  -> adapter from(shape)
```

For JSON `name_id`, shape validation proves the field is a string; adapter
validation proves it is a valid canonical name. For binary `name_id`, shape
validation proves the expected integer width exists; adapter validation can
reject reserved numeric values.

For nested byte leaves, validation is fractal: the parent format validates the
container boundary, then the adapter validates the delegated encoded subtree.

**Customizing external types** requires no format-specific serializer:

```cpp
namespace app {
   struct name_id {
      std::uint64_t value;
      std::string str() const;
      static name_id parse(std::string_view);
   };
}

struct app_name_binary
{
   using shape_type = std::uint64_t;
   static shape_type to(app::name_id n) noexcept { return n.value; }
   static app::name_id from(shape_type v) noexcept { return {v}; }
};

struct app_name_text
{
   using shape_type = std::string_view;
   static std::string to(app::name_id n) { return n.str(); }
   static app::name_id from(std::string_view s) { return app::name_id::parse(s); }
};

PSIO1_ATTRS(psio1::type<app::name_id>{},
           psio1::binary_format<app_name_binary>{},
           psio1::text_format<app_name_text>{});
```

Binary formats see a `uint64_t`. JSON sees a string. The type author wrote one
customization.

**Schema and dynamic use.** Schemas retain the logical type, selected format
annotation, and adapter shape. Dynamic validators and transcoders therefore know
that a JSON string is `name_id` using its `text_format`, not merely "some
string".

**Rule of thumb:**
- Use `binary_format` / `text_format` when the wire encoding is a domain choice
  distinct from the type's storage layout.
- Use rich wrappers when the invariant itself matters in the C++ type system and
  the wire form remains the wrapped container.
- Use ordinary annotations such as `length_bound`, `field_num`, and `sorted`
  when the field is still walked as its normal shape and the spec only colors in
  a wire-boundary detail.

### 5.4 Error model

**`validate` is the only operation that returns errors.** Every
other operation is `noexcept` and assumes its input was validated.

Return type:

```cpp
namespace psio1 {
   struct codec_error {
      const char*   what;        // static message
      std::uint32_t byte_offset; // where in the buffer the problem was
      const char*   format_name; // "ssz", "frac", etc.
   };

   class [[nodiscard]] codec_status {
      const codec_error* err_;   // nullptr = success

    public:
      constexpr codec_status() noexcept : err_(nullptr) {}
      constexpr explicit codec_status(const codec_error& e) noexcept : err_(&e) {}

      constexpr bool ok()    const noexcept { return !err_; }
      constexpr explicit operator bool() const noexcept { return ok(); }

      constexpr const codec_error& error() const noexcept { return *err_; }

#if PSIO1_EXCEPTIONS_ENABLED
      // Opt-in throwing helper — must be called explicitly.
      void or_throw() const {
         if (err_) throw *err_;
      }
#endif
   };
}
```

Contract:

1. `psio1::validate(fmt, bytes)` returns `codec_status`. Marked
   `[[nodiscard]]` at both the CPO level and the `codec_status`
   type level.
2. Project CI enables `-Werror=nodiscard`. Unchecked status values
   fail to compile. This holds under both exception modes.
3. With exceptions enabled, users can opt into throwing via
   `.or_throw()`. Without it, they must check via `if (!status) …`.
4. `decode`, `encode`, `size`, `make_boxed`, and `view` accessors
   are **`noexcept`**. Passing unvalidated bytes to them is
   undefined behavior. A build flag `PSIO1_STRICT_VALIDATE=1` turns
   on redundant validation in debug builds for belt-and-suspenders
   testing, but release builds trust their input.
5. `psio1::codec_error` carries a static `what` string (no allocation)
   + a byte offset + a format-name string. Small, cheap to pass.
6. Under `-fno-exceptions`, `or_throw()` is not compiled. `codec_status`
   still works fully; only the opt-in throw path disappears.

This makes "did you validate?" a compile-time invariant. No silent
drops, no runtime double-checks in hot paths, no exception coupling
for WASM builds.

### 5.5 Result types

```cpp
namespace psio1 {
   enum class storage { owning, mut_borrow, const_borrow };

   template <typename T, typename Fmt, storage S = storage::owning>
   class buffer {
      /* holds bytes, typed with (T, Fmt). Read ops on all S;
         write ops require S != const_borrow. All storage
         operations are provided via free functions — see below. */
   };

   template <typename T, typename Fmt, storage S = storage::const_borrow>
   class view {
      /* non-owning (or borrow-from-buffer) view with any per-format
         indexing metadata. Public surface is exactly the reflected
         fields of T (generated from PSIO1_REFLECT); no other
         methods. Operators [] / * / bool / begin / end are provided
         when the shape calls for them (sequence, optional,
         iterable). */
   public:
      /* Reflection-generated accessors for T's fields live here.
         No name-collision risk with library methods because no
         library methods are exposed on `.`. */
   };

   template <typename T, typename Fmt>
   class mutable_view {
      /* arena-backed editor. Same access-surface rule as view —
         fields on `.`, storage ops via free functions. */
   };

   // Free-function storage API — works on buffer, view, mutable_view
   // uniformly. ADL-findable via the codec type tag.
   template <typename T, typename F, storage S>
   std::span<const char> bytes(const buffer<T, F, S>&) noexcept;
   template <typename T, typename F, storage S>
   std::span<const char> bytes(const view<T, F, S>&) noexcept;

   template <typename T, typename F, storage S>
   std::uint32_t size(const buffer<T, F, S>&) noexcept;
   template <typename T, typename F, storage S>
   std::uint32_t size(const view<T, F, S>&) noexcept;

   template <typename T, typename F, storage S>
   constexpr F format_of(const buffer<T, F, S>&) noexcept { return {}; }
   template <typename T, typename F, storage S>
   constexpr F format_of(const view<T, F, S>&)   noexcept { return {}; }

   template <typename T, typename F, storage S>
   view<T, F> as_view(const buffer<T, F, S>&) noexcept;   // cheap for some Fmts, lazy for others
   template <typename T, typename F, storage S>
   buffer<T, F, storage::owning> to_buffer(const view<T, F, S>&);
}
```

**Access-surface rule.** Nothing appears on the `.` side of
`buffer`, `view`, or `mutable_view` except what reflection puts
there (field accessors for records, or shape-appropriate operators
for sequences/optionals/variants/bitfields). Storage, format probes,
and materialization helpers are all free functions in `psio1::`. This
makes "does `v.foo()` call a library method?" always answerable
with "no" — it either calls a reflected field accessor or it's a
compile error.

All three types are fully monomorphized. The `storage` tag selects
the internal representation at compile time; no runtime
discriminator in hot paths.

---

## 6. Naming

- Format tags: `psio1::ssz`, `psio1::pssz` (auto-selected width),
  `psio1::pssz16`, `psio1::pssz32`, `psio1::frac`, `psio1::frac16`, `psio1::frac32`,
  `psio1::json`, `psio1::bin`, `psio1::key`, `psio1::avro`, `psio1::borsh`,
  `psio1::bincode`, `psio1::flatbuf`, `psio1::capnp`, `psio1::wit`. Plain types
  at the top level — no `_fmt` suffix, no nested `fmt::`. Comparisons against
  the upstream FlatBuffers library live in demo/benchmark code, not the
  official format tag list.
- Result types: `psio1::buffer<T, Fmt, Store>`, `psio1::view<T, Fmt,
  Store>`, `psio1::mutable_view<T, Fmt>`. Storage default is
  `psio1::owning_vec`.
- Generic operations: `psio1::encode`, `psio1::decode`, `psio1::size`,
  `psio1::validate`, `psio1::make_unique`, `psio1::make_shared`.
- Field access: `v->field_name()`. View-local ops: `v.data()`,
  `v.size()`, `v.format()`, `v.buffer()`, `v.as_view()`,
  `v.to_owned()`.
- Rust mirrors: `psio1::Ssz`, `psio1::Frac32`, etc. as zero-sized
  marker types; `psio1::encode::<Ssz, _>(v)`.

Legacy v1 identifiers that conflict (e.g. anything declaring
`namespace psio1::ssz { ... }`) get relocated when v2 lands — either
moved under `psio1::detail::` if implementation-internal, or moved to
`psio1::v1::` if user-facing compatibility is preserved.

---

## 7. Coexistence / migration

- v2 is staged in `libraries/psio2` under `psio2::` / `PSIO2`, with no nested
  `v2` namespace. Users of the staging tree opt in with
  `#include <psio2/codec.hpp>` and qualified `psio2::` calls.
- Each format lands as one PR: scaffold + shape concepts for format,
  byte-parity tests, bench confirmation (§ 3.2 N1 perf gate — meet
  or exceed v1 within ±2% on the anchor workloads).
- Once all official formats are green, a follow-up PR promotes v2 to the
  primary namespace and demotes v1 under `psio1::v1::`. Sugar layer
  (`psio1::to_ssz`, etc.) becomes one-line `inline` forwarders to the
  v2 implementations.
- `convert_to_ssz` / `convert_from_ssz` / `fracpack_size` /
  `validate_frac` / `format_json` / `to_json_fast` and similar
  inconsistent legacy names become `[[deprecated]]` aliases that
  forward to v2 for one release, then get deleted.

---

## 8. Success criteria

v2 ships when:

1. **Byte parity** — every v1 fixture round-trips byte-identically
   through v2 for every format.
2. **Perf parity** — bench throughput meets or exceeds v1 within ±2%
   on the anchor workloads (mainnet-genesis BeaconState encode /
   decode / validate for SSZ, pssz, frac; 21 063-Validator list for
   all formats that support it). Measured by the existing bench
   harness; no asm-level lockstep required.
3. **Format coverage** — all official formats (§ 1 of
   `psio-format-feature-matrix.md`) have their `tag_invoke`
   hidden-friend overloads on a tag struct deriving from
   `format_tag_base`. Static path + dynamic path both implemented
   (or format declares `supports_dynamic_* = false` with
   justification).
4. **Format-neutral conformance harness** covers every (format,
   fixture) pair with one macro invocation (§ 4.4). Every preserved
   or upgraded test in § 8.5 is reachable through the harness.
5. **Cross-format conversion** — `encode<ToFmt>(view<T, FromFmt>)`
   matches `encode<ToFmt>(decode<FromFmt, T>(bytes))` for every
   (FromFmt, ToFmt) pair that supports the same shape. Same property
   holds through the dynamic-transcode path.
6. **Static/dynamic byte-identity** —
   `encode_dynamic(fmt, reflect<T>::schema(), dynamic_value)`
   produces byte-identical output to `encode(fmt, T_value)` for the
   same logical value.
7. **Runtime reflection coverage** — `reflect<T>::index_of(name)`,
   `index_of_field_number`, `visit_field_by_name`,
   `visit_field_by_number`, `for_each_field`, and `make_proxy` have
   tests for every reflection-carrying fixture. Perfect-hash
   correctness validated across every name in every fixture.
8. **Third-party annotation round-trip** — schemas containing
   unknown-kind `dynamic_spec` entries round-trip byte-identically
   through parse → emit (§ 5.2.6.7).
9. **Rich-wrapper consistency** — `psio1::byte_array<48>` on a field
   annotated with `bytes_spec{.size = 32}` is a compile-time error
   with a readable message naming the mismatch (§ 5.3.6).
10. **Custom-container smoke** — one user-defined `custom_vector<T>`
    class satisfies `VariableSequence` and works across every format
    without code changes to psio.
11. **Documentation** — this doc stays current; a user-facing README
    shows the four code patterns from § 4; the schema-import /
    contract-check DX workflow (philosophy § 0 in
    `psio-language-principles.md`) has an end-to-end walkthrough.

---

## 8.5 Inventory — tests, benches, and validation that must survive

v2 is a template-layer reshuffle, not a ground-up rewrite. Every
existing test, benchmark, fuzz fixture, and validation corpus has to
keep working during development and after v2 ships. This section
inventories what exists and specifies what happens to each under the
migration.

### 8.5.1 C++ tests (`libraries/psio1/cpp/tests/`)

| File | What it covers | Action under v2 |
|---|---|---|
| `ssz_tests.cpp` | SSZ encode/decode round-trip, view, validate | **Preserve** — add a parallel psio2 test that runs the same assertions through the staged `psio2::` wrappers. Merge into the format-neutral conformance harness once SSZ lands in v2. |
| `pssz_tests.cpp` | pssz equivalent | Same as ssz_tests — parallel v2 file, eventual merge. |
| `frac_ref_tests.cpp` | fracpack view/ref tests | Preserve; fracpack is last into v2, tests stay as v1 until the migration PR. |
| `frac16_tests.cpp`, `frac16_mutation_tests.cpp`, `frac16_fuzz_generated.cpp` | frac16 format + mutation + generated fuzz vectors | Preserve; frac16 in v2 is a tag-param on `frac<W>`. Fuzz-generated vectors stay valid. |
| `validate_frac_tests.cpp` | Structural validation for frac | Preserve; becomes one of the inputs to v2's `validate_strict` harness. |
| `beacon_state_tests.cpp` | Full BeaconState round-trip, Validator + BLSPubkey fixtures | **Upgrade** — becomes a tier-1 conformance fixture across every format v2 supports. |
| `bin_extensibility_tests.cpp` | Binary format extensibility boundary cases | Preserve; bin gets v2 wrappers late in migration. |
| `bitset_tests.cpp`, `bounded_tests.cpp`, `ext_int_tests.cpp` | Type-level tests for helper types | Preserve as-is — these test types, not formats. |
| `capnp_parser_tests.cpp`, `capnp_schema_tests.cpp`, `capnp_view_tests.cpp` | capnp schema parsing + view generation | Preserve; parser output feeds `psio1::schema` in v2. |
| `fbs_parser_tests.cpp` | flatbuffers schema (.fbs) parser | Preserve. |
| `json_tests.cpp` | JSON encode/decode/format | Preserve; becomes conformance-harness-driven in v2. |
| `multiformat_tests.cpp` | Already tests one value across multiple formats (ssz, pssz, frac) | **Anchor** — this is the closest existing test to the v2 conformance harness. Expand to cover every v2 format. |
| `psio_attribute_tests.cpp` | PSIO1_REFLECT attribute semantics | **Upgrade** — expand to cover the new annotation spec types and the static-consistency checks between wrappers and annotations. |
| `psio_structural_tests.cpp` | Reflection structural queries | **Upgrade** — expand to cover `reflect<T>::index_of(name)` perfect-hash lookup, `visit_field_by_name`, `for_each_field`, `make_proxy`, runtime `schema()` generation. |
| `schema_bounded_tests.cpp` | Schema + bounded types interaction | Preserve; absorbs into `psio1::schema` round-trip tests. |
| `view_tests.cpp` | Unified `view<T, Fmt>` framework | **Upgrade** — becomes the primary home of the "access-surface rule" tests (`v.field()` works, `v.library_method()` is a compile error). |
| `wit_attribute_tests.cpp`, `wit_resource_tests.cpp`, `wit_variant_tests.cpp` | WIT canonical-ABI edge cases | Preserve; wit lands in v2 late; its existing coverage comes along. |

### 8.5.2 C++ benchmarks (`libraries/psio1/cpp/benchmarks/`)

| File | What it covers | Action under v2 |
|---|---|---|
| `bench_ssz_beacon.cpp` | SSZ + fracpack on mainnet BeaconState; head-to-head with sszpp | **Anchor perf-parity test.** During v2 development, a parallel `bench_ssz_beacon_v2.cpp` runs the same workload through v2; CI compares medians. v2 ship-gate requires parity. |
| `bench_fracpack.cpp`, `bench_fracpack16.cpp` | frac32 / frac16 perf | **Anchor** — parallel v2 runs; parity required before frac lands in v2. |
| `bench_capnp.cpp`, `bench_cp_view.cpp` | capnp encode / view | **Anchor** for capnp v2 migration. |
| `bench_flatbuf.cpp`, `bench_standalone_flatbuf.cpp`, `bench_reflect_flatbuf.cpp` | native flatbuf plus upstream-library comparison benches | **Anchor** — native flatbuf needs v2 parity; upstream FlatBuffers numbers remain demo/benchmark comparison data, not an official codec target. |
| `bench_wit.cpp` | wit canonical-ABI perf | **Anchor** for wit v2 migration. |
| `bench_msgpack.cpp`, `bench_protobuf.cpp` | Head-to-head against external msgpack / protobuf | **Preserve** — these are the competitor benches from principle 5 of the language doc. Become CI-reported in v2 as "psio vs competitor" numbers per release. |
| `bench_modern_state.cpp`, `bench_shadow_index.cpp`, `bench_zero_compress.cpp` | Secondary workloads (non-BeaconState) | Preserve; v2 parity required but not the ship gate. |
| `bench_fuzz_generated.cpp` | Fuzz-generated input perf | Preserve as a regression-catcher. |
| `bench_schemas.capnp`, `.fbs`, `.proto`, `bench_schemas_generated.h` | Shared schema inputs for the comparison benches | **Preserve; expand** — these schema files become inputs for the v2 schema-round-trip conformance tests (parse → schema → emit → diff). |

### 8.5.3 C++ cross-validation emitters (`libraries/psio1/cpp/tools/`)

| File | What it emits | Action |
|---|---|---|
| `emit_xval_fixtures.cpp` | Baseline bytes for primitive + string + vec + optional | **Preserve** — these are the canonical fixtures consumed by Rust tests today, and will be consumed by TS / Python / Go tests in the future. Commit the output bytes to the repo. |
| `emit_xval_arrays.cpp` | Array fixtures | Preserve. |
| `emit_xval_bits.cpp` | Bitvector / bitlist fixtures | Preserve. |
| `emit_xval_extint.cpp` | 128/256-bit integer fixtures | Preserve. |
| `emit_xval_validator.cpp` | Validator struct 121-byte fixture | Preserve; upgrade to also emit full BeaconState once v2 is stable. |

v2 adds **cross-language fixture emission**: beyond C++→Rust, also Rust→C++, and eventually each tier-1 language emits canonical fixtures consumed by every other tier-1 language. Conformance harness ensures byte-identity across all pairs.

### 8.5.4 Rust tests (`libraries/psio1/rust/psio/src/*_tests.rs` + inline `#[cfg(test)]`)

| Location | Coverage | Action |
|---|---|---|
| `ssz.rs` inline tests | SSZ primitives, String, Vec, Option, ext-int, bitvector, bitlist, array, bounded, views | **Preserve** — parallel v2 tests introduced with the Rust v2 port. |
| `pssz.rs` inline tests | pssz equivalent | Same. |
| `fracpack.rs` inline tests | Fracpack encode/decode, prevalidated, structural walks | Same. |
| `ssz_derive.rs`, `pssz_derive.rs` inline | Derive macro outputs | Upgrade to test annotation-driven codegen once v2 annotation DSL is settled. |
| `cross_validation_tests.rs` | C++-emitted fixtures → Rust decode, round-trip | **Anchor** — expand to every format and every tier-1 language pair. |
| `beacon_state_tests.rs` | Full BeaconState round-trip, mainnet genesis decode, byte-identical re-encode | **Anchor** perf-parity fixture. |
| `json.rs` tests | JSON canonical form | Preserve. |
| `nested.rs` tests | Nested type decode | Preserve. |
| `schema.rs`, `schema_export/`, `schema_import/` | Schema value, parsers (fbs/capnp/wit), emitters | **Upgrade** — these already prototype the `psio1::schema` path v2 formalizes. Tests become conformance fixtures. |
| `capnp/`, `flatbuf/`, `wit/` inline tests | Per-format codec tests | Preserve; migrate into v2's conformance harness. |

### 8.5.5 Rust benchmarks (`libraries/psio1/rust/psio/benches/`)

| File | Coverage | Action |
|---|---|---|
| `fracpack_bench.rs` | Rust fracpack encode/decode | **Anchor** perf-parity bench. |
| `ssz_validator_bench.rs` | 21k-Validator list, head-to-head vs ethereum_ssz + ssz_rs | **Anchor** — head-to-head numbers ship in every release note. |
| `ssz_beacon_bench.rs` | Full mainnet BeaconState, 4-way (psio SSZ, psio pssz, ethereum_ssz, ssz_rs) | **Anchor** — the flagship perf demonstration. |

### 8.5.6 Fuzzing corpora

- `libraries/psizam/tests/fuzz/` — WASM engine fuzz cases; unrelated to psio but inventoried here for completeness (not affected by v2).
- `frac16_fuzz_generated.cpp` + `gen_frac16_fuzz.py` — generated fracpack fuzz vectors. **Preserve** — feeds v2's `validate` harness as input.
- No dedicated psio fuzzer today; v2 step-2 follow-up: run AFL/libfuzzer against `psio1::validate(fmt, bytes)` for every format.

### 8.5.7 Action matrix

| Category | Preserve as-is | Upgrade | Retire (merged into harness) | Add new |
|---|---|---|---|---|
| C++ format round-trip tests | 0 | all (migrate to conformance harness) | all individual `*_tests.cpp` format files, eventually | conformance-harness-driven tests |
| C++ benches | 0 | all (add v2 parallel) | 0 (v1 benches retired only after full v2 migration) | v2-flavored parallels for each during migration |
| C++ xval emitters | all | `emit_xval_validator.cpp` → add BeaconState | 0 | emitters for new fixture types (Schema round-trip, dynamic transcode) |
| Rust inline tests | most | derive-macro tests (annotation DSL) | per-format round-trip tests | conformance-harness-driven |
| Rust benches | 0 | all (add v2 parallel) | 0 | v2 parallels |
| Fuzz | all | `validate` over every v2 format | 0 | AFL/libfuzzer targets |

### 8.5.8 Ship-gate tied to inventory

v2.0 ships when every row in the action matrix has:

1. A CI-green v2 parallel or v2 migration (whichever the action says).
2. Bench parity numbers within the 2% envelope (§ 3.2 N1) on
   every anchor workload.
3. Conformance harness coverage of every preserved or upgraded fixture.

No v1 test, bench, or fuzz corpus is retired until v2 covers it
verifiably. v1 stays compilable in parallel for at least one release
after v2 reaches full parity, to give users a regression comparison
point.

---

## 9. Out of scope for v2.0

- **Mutable view arena representation choice** (in-place mutation vs
  edit-tree overlay). v2.0 ships with an arena that stores a growable
  buffer + edit log; the representation is an implementation detail
  that doesn't affect the API.
- **Per-pair optimized converters** (byte surgery for frac32 → frac16,
  ssz ↔ pssz). Fallback is the shape-polymorphic encode/transcode
  path (§ 5.2.6.3); we add specializations later as profiling
  warrants.
- **AFL/libfuzzer harness for `psio1::validate`.** Design allows it
  (structural `validate` is noexcept + total-function on span); the
  fuzz targets themselves are a step-2 follow-up.
- **Non-C++ language ports.** Rust v2, TS, Python, Go, Zig ports
  proceed on their own schedules per `psio-language-principles.md`.
  C++ v2 is the anchor; other languages follow as separate projects.
- **Schema diff/merge tooling.** The philosophy (§ 0 in the language
  doc) promises schema evolution is a human/AI refactor; dedicated
  tooling to produce the diff or propose patches is a future
  concern — build-time diagnostics are enough for v2.0.

---

## 10. Resolved (record of decisions)

- **Customization mechanism** — tag_invoke CPOs with hidden-friend
  overloads on the format tag. No class-template specialization in
  `psio1::`. (§ 5.2)
- **Error model** — validate-is-checkpoint; `codec_status` with
  `[[nodiscard]]` enforced by `-Werror=nodiscard`; `or_throw()` as
  opt-in under exceptions; all other ops `noexcept`. (§ 5.4)
- **Shape vs representation** — structural shape concepts + per-field
  reflection annotations. User types unchanged; serialization concerns
  live on the reflection. (§ 5.3)
- **Perf gate** — bench-parity, not asm-parity. (§ 3.2 N1)
- **Exception optionality** — `-fno-exceptions` is first-class. (§ 3.2 N5)
- **Namespacing** — format tags at `psio1::` top level; implementation
  details under `psio1::detail::`; per-format helpers as nested member
  types of the tag. No `psio1::fmt::` namespace.
- **View access surface** — nothing on `v.`'s public surface except
  reflected field accessors and shape-appropriate operators. All
  storage / format / materialization operations are free functions in
  `psio1::` (`bytes`, `size`, `format_of`, `to_buffer`, `as_view`).
  Views therefore feel identical to the reflected type — there is no
  second-class-wrapper tax. No `operator->` used.
- **Annotation trait** — single `psio1::annotate<X>` variable template
  holding a `std::tuple` of spec structs. `X` is a bare member
  pointer for field annotations; `psio1::type<T>{}` wrapper for
  type-level. Third-party formats define their own spec types in
  their own namespace; the tuple is an open protocol, not a closed
  enum. PSIO1_REFLECT's `|=` syntax is sugar that expands to these
  specializations.
- **`PSIO1_ATTRS` macro** — separate macro that adds annotations to
  an already-reflected type (without replaying `PSIO1_REFLECT`).
  Useful for schemas and bindings layered over other libraries'
  types.
- **Two-phase validation** — `validate` is structural only (fast,
  required for noexcept access). `validate_strict` runs structural
  checks plus any `static validate(span)` members defined on spec
  structs (semantic: UTF-8 well-formedness, regex, user-defined).
- **Custom validators** — spec structs may carry a `static
  codec_status validate(std::span<const char>) noexcept` member.
  Third-party spec authors get semantic validation for free via
  `validate_strict`.
- **Named spec attributes, spelled out.** Acronyms hide commitments;
  `psio1::definition_will_not_change` is never shortened to `dwnc` in
  public names.
- **Rich wrapper types opt-in.** `psio1::bounded<T, N>`,
  `psio1::utf8_string<N>`, `psio1::byte_array<N>`, etc. carry their
  invariants in the type system for users who want them. Their
  inherent annotations merge with `psio1::annotate` at compile time;
  contradictions are `static_assert` errors.
- **Runtime reflection is first-class.** `psio1::reflect<T>` exposes
  a constexpr-callable API covering: compile-time member info
  (pointer/name/type/field_number), runtime name→index (perfect
  hash, O(1)), runtime field-number→index, visitor dispatch by
  name and by number, `for_each_field` iteration, and
  `make_proxy<Derived>` for RPC stubs and custom accessors. Schema
  exporters consume this same trait at consteval time. Reflection
  covers public non-static members only.
- **Dynamic / type-erased access is first-class.** `psio1::schema` is
  the runtime counterpart of compile-time reflection. Bridged via
  `reflect<T>::schema()` (consteval). Per-format dynamic CPOs
  (`validate_dynamic`, `encode_dynamic`, `decode_dynamic`) plus
  `psio1::transcode` and `psio1::dynamic_view`/`dynamic_value` cover
  schema-driven bytes operations without requiring a compile-time
  C++ type. Format-author opt-in via
  `supports_dynamic_{validate,encode,decode,transcode}`. Reflection
  IS the schema — static and dynamic paths produce byte-identical
  wire results.
- **Third-party annotation serialization** — `dynamic_spec
  { kind_name, payload }` preserves pure-data specs. Formats and tools
  consume the spec names they recognize by convention and ignore the rest.
  Unknown-kind specs round-trip opaquely (open protocol). (§ 5.2.6.7)
- **Annotation DSL (macro level)** — `field |= annotation | annotation`
  inside `PSIO1_REFLECT`, and `PSIO1_ATTRS(Type, …)` separately. Macro
  syntax is sugar that expands to direct `psio1::annotate<…> =
  std::tuple{…}` specializations. Users may write the desugared form
  directly. (§ 5.3.5 Form 1 & Form 2)
- **v1 wrapper migration path** — `bounded<T, N>`, `utf8_string<N>`,
  `byte_array<N>`, `bitlist<N>` etc. are first-class in v2. Users
  keep them unchanged; their inherent annotations merge with any
  additional `psio1::annotate<…>` annotations on the same field,
  with compile-time consistency checks that reject contradictions.
  No forced migration off wrappers; no forced adoption of
  annotations. (§ 5.3.6)
- **Schema-is-contract philosophy** — across every tier-1 language:
  `schema-import` emits a one-time initial code file that the user
  owns and edits; `reflect<T>::schema() == parse_schema(path)`
  static check enforces the wire contract; schema evolution is a
  human/AI refactor, not a regenerate-and-merge. (see
  `psio-language-principles.md` § 0)

## 11. Open questions

1. **Annotation set scope for v2.0.** Core ship set is locked:
   `bytes_spec`, `utf8_spec`, `hex_spec`, `max_size_spec`,
   `sorted_spec`, `default_value_spec<T>`, `default_factory_spec<F>`,
   `field_num_spec`, `skip_spec`, `definition_will_not_change`
   (see § 5.3.2). Open: which additional specs make v2.0 vs later?
   Candidates: `serialize_as<U>` (custom wire-type mapping),
   `skip_if<Pred>` (conditional omission), `const_value<Expr>`
   (always-this-value field). Decide per-format demand.
2. **Mutable view arena representation** — in-place vs
   overlay-with-replay. API agnostic (same surface either way);
   purely an implementation choice. Pick at step 1 code review.
3. **Storage tag naming** — `storage::owning` /
   `storage::mut_borrow` / `storage::const_borrow`, or shorter
   (`own` / `mut_ref` / `ref`). Code-review bikeshed; no functional
   impact.
4. **Per-format `operator->` escape** — current rule (§ 5.5) is "no
   operator->, field access via `.`". Revisit only if a format
   emerges where the access pattern requires pointer-like semantics
   (unlikely; flag only if encountered during step 1).
5. **AI-copilot diagnostic format** — per
   `psio-language-principles.md` § 0, schema-mismatch diagnostics
   should be machine-readable enough that an AI plugin can patch
   the source. Format TBD: structured JSON alongside the
   human-readable message? A `psio-diagnostic-schema.json` that
   tooling can consume? Not a ship-blocker; design in parallel
   during step 1.

---

## 12. Pointer back

When v2 ships or a decision above lands, update:
- This doc (§ 2, 6, 10, 11).
- `.issues/psio-format-feature-matrix.md` § 6A (current "format-neutral
  API" sketch) to reference this doc as the authoritative design.
- `.issues/psio-language-principles.md` (cross-language checklist;
  schema-import + contract-check workflow lives there).
- `libraries/psio1/README.md` (end-user docs — add the four DX patterns
  from § 4 plus the schema-evolution walkthrough).
