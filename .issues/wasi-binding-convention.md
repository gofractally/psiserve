---
id: wasi-binding-convention
title: WASI C++ bindings — convention for type/method name collisions + literal-type portability
status: resolved
priority: medium
area: wasi
agent: ~
branch: ~
created: 2026-04-20
resolved: 2026-04-20
depends_on: []
blocks: []
---

## Resolution (2026-04-20)

Landed option (1) — hand-written bindings with a codified convention.
Two rules recorded in `libraries/wasi/cpp/README.md`:

1. **Use the WIT bare name for types when unambiguous in the TU.**
   Applied to sockets: `enum class network_error_code` renamed to
   `enum class error_code`. This aligns reflection's WIT emission with
   the spec (`error-code`) and breaks the collision with the
   `network_error_code` function since the type and function now have
   different C++ bare names. No reflection change needed.

2. **Typed literals for WASI numeric constants.** `wasi_duration{N}`
   replaces raw `ULL`/`UL` suffixes in `sockets_host.hpp` (5 sites).
   Deducing helpers like `sock_detail::ok<T>` now see the expected
   `wasi_duration` type on both Linux and macOS.

The other 11 WASI headers are unchanged — no other interface has a
WIT-type/WIT-function name collision today. If one appears later, the
README notes per-interface nested namespacing as the next fallback
(reflection already strips `::` scope qualifiers before kebab-
lowering, so no macro changes are needed).

A round-trip test extension for `wasi:sockets/network` asserting
`iface.type_names` contains `"error-code"` is open as follow-up work.


## Background

`libraries/wasi/cpp/include/wasi/0.2.3/` contains hand-written C++
bindings for WASI Preview 2 (sockets, clocks, filesystem, http, cli,
io, random). They map the WIT IDL to C++ types + static methods.

What the psio macros do (and what they do not):
- `PSIO_REFLECT(T, ...)` — records the shape of hand-authored enums
  and structs for fracpack / JSON / schema emission.
- `PSIO_PACKAGE(name, version)` — registers a package tag.
- `PSIO_INTERFACE(name, types(...), funcs(...))` — expands (via
  Boost.PP) into a `psio::detail::interface_info` specialization
  that captures `decltype(&name::method)`, method names, param
  names, and synthesizes a proxy template. It does not emit the
  host-side method declarations or the types.
So the types, enums, and method signatures in these 12 headers are
100% hand-written; the macros are reflection plumbing around them.

`libraries/wit-macro/` exists but is a **Rust proc-macro** (pulls
`wit-parser` / `wit-component`). Its direction is Rust → WIT IDL +
WASM component metadata — for guest code. It does not emit C++. No
WIT → C++ generator exists in the tree today.

Two build breaks on 2026-04-20 (GCC 15, Ubuntu glibc) exposed design
weaknesses in how the bindings map WIT to C++:

### Case 1 — type/method name collision

`sockets.hpp` defines `enum class network_error_code` and then, inside
`struct wasi_sockets_network`, a static method named
`network_error_code` that returns `std::optional<network_error_code>`.
GCC 15's `-Wchanges-meaning` flags this as an error: the method name
shadows the enum type at the point of the declaration. Per WIT the
method is named `network-error-code` on the `wasi:sockets/network`
interface, and the existing convention here lowers WIT kebab-case to
C++ snake_case uniformly, so it overlaps with the WIT type of the same
name.

Reproducer: any WASI module that imports `wasi:sockets/network` and
includes `sockets.hpp` at the top level (e.g. `wasi_tcp_echo`
example).

### Case 2 — integer-literal portability

`sockets_host.hpp:434/477` converts a WASI `duration` to nanoseconds
via `sock_detail::ok(static_cast<wasi_duration>(val) * 1000000000ULL)`
where `wasi_duration = uint64_t`. On x86_64 Linux glibc `uint64_t`
lowers to `unsigned long`; the `ULL` literal forces
`unsigned long long`; the `*` deduces the wider type; the wrapping
`sock_detail::ok<T>(T)` infers `T = unsigned long long`, so the
returned `variant<unsigned long long, network_error_code>` cannot
convert to the declared return type
`variant<unsigned long, network_error_code>`. On macOS
(`uint64_t = unsigned long long`) the deduction matches and the bug
is invisible, so this breaks only on Linux.

## Immediate fix (short-term)

Done at the build-unblock level (see companion commit): rename the
colliding static to something unambiguous (e.g.
`get_network_error_code` or move it to a free function outside the
interface struct), and change the nanosecond multiplier to
`wasi_duration{1'000'000'000}` (or drop the `ULL` suffix and rely on
implicit narrowing-free arithmetic).

## Deeper question

Whether the long-term WASI binding story is:

1. **Keep hand-written bindings + codify conventions** in a
   `README.md` / style guide alongside `libraries/wasi/cpp/include/`:
   * WIT kebab→snake_case lowering rule.
   * When a WIT function's name equals a WIT type's name, the C++
     function gets a `get_` / `query_` / `make_` prefix (match
     wit-bindgen-cpp output).
   * Numeric literals for WIT durations/sizes use typed constructors:
     `wasi_duration{1'000'000'000}`, `u64_size{N}`, never raw
     `ULL`/`UL` suffixes.
   * Required compiler warnings for the bindings:
     `-Wchanges-meaning -Wnarrowing -Wconversion` at least on the
     binding headers.

2. **Adopt or build a WIT → C++ generator** — a C++-side analogue
   of `wit-macro`. Parse `.wit`, emit the enums, the struct +
   `static inline` method signatures, and the
   `PSIO_PACKAGE` / `PSIO_INTERFACE` registration calls. Options:
   a. Upstream `wit-bindgen-cpp` — needs a post-pass to emit
      PSIO macros alongside the generated types.
   b. A psiserve-native Rust tool that reuses the `wit-parser`
      dependency already in `wit-macro`'s Cargo.toml but targets
      C++ output.
   c. A C++/Python build-step script that parses WIT and runs at
      configure time.
   Generator-emitted headers carry a `DO NOT EDIT` banner and
   regenerate from `.wit` source on build.

The trade-off is familiar: hand-written headers are easier to read
and locally tweak but rot over time and surface portability bugs only
on the compiler that first disagrees; generators front-load the
infrastructure cost and lock in the convention rigidly. We have 12
hand-written `.hpp` files in `libraries/wasi/cpp/include/wasi/0.2.3/`
today (7 client + 5 host). That's the scale at which "just write a
generator" becomes reasonable if WASI 0.3 drifts materially from
0.2.3 and we need to re-emit them.

## Related

- `wit-macro` in `libraries/wit-macro/` — existing psiserve code for
  WIT-driven bindings at the psio layer. Could be the foundation for
  option (2).
- `psio-wit-resource-drop-specializations.md` — touches the same
  WIT → psio specialization surface.
- `libraries/wit-test-component/` — fixture for exercising
  WIT/component bindings.

## Acceptance criteria

- [ ] Decision recorded: option (1) convention or option (2) generator.
- [ ] If (1): `libraries/wasi/cpp/README.md` with the lowering rules,
      prefix convention for name collisions, typed-literal rule, and
      required warnings added to the CMake target.
- [ ] If (2): generator spec + initial pass over one interface (e.g.
      `wasi:clocks/wall-clock`) landed as a proof-of-concept, with the
      old hand-written headers kept as a fallback until parity.
- [ ] A lint/CI step that would have caught both breaks (GCC 15
      `-Wchanges-meaning` at minimum; ideally a 32-bit / macOS cross-
      build rotation to catch the `unsigned long` vs `unsigned long long`
      case).

## Notes

This issue is design-scope; the immediate two-line fix is
intentionally out of scope. File against the WASI binding subsystem
so the convention choice isn't blocked on engine work.
