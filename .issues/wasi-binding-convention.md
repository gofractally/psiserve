---
id: wasi-binding-convention
title: WASI C++ bindings — convention for type/method name collisions + literal-type portability
status: open
priority: medium
area: wasi
agent: ~
branch: ~
created: 2026-04-20
depends_on: []
blocks: []
---

## Background

`libraries/wasi/cpp/include/wasi/0.2.3/` contains hand-written C++
bindings for WASI Preview 2 (sockets, clocks, filesystem, http, cli,
io, random). They map the WIT IDL to C++ types + static methods.
There is no generator today.

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

2. **Adopt or build a WIT → C++ generator** that handles the
   disambiguation + typed literals automatically. Upstream
   `wit-bindgen-cpp` is one option; a psiserve-native generator that
   integrates with `wit-macro` / `PSIO_REFLECT` is another.
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
