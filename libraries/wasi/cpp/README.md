# wasi-cpp

PSIO-reflected C++ bindings for WASI interfaces.

## What's here

Per-interface headers under `include/wasi/<version>/` — one per WASI
interface or tightly related group. Each carries:

- `PSIO_PACKAGE(wasi, "<version>")` — so `psio::package_of<T>` resolves
  to `wasi` for any type declared here, which makes `emit_wit` produce
  `use wasi:io/streams.{...}` in downstream bindings rather than
  re-declaring these types.
- `PSIO_INTERFACE(<name>, types(...), funcs(...))` — groups the types
  and functions that together form one WIT interface.
- `PSIO_REFLECT(T, ...)` on every record / variant / resource.

## Contract with upstream

The vendored canonical WIT lives in `../wit/<version>/`. The
`tests/round_trip` suite reflects the headers into a `psio::schema`,
runs `emit_wit`, and diffs the output against the vendored `.wit`.
Any drift fails the test — that is the enforcement mechanism.

Today these headers are hand-written. Phase D of PSIO's WIT
integration (see `libraries/psio/doc/wit-integration.md`) delivers
`psio-gen-headers <pkg.wit> -o include/`, at which point the headers
become regenerated output and hand-maintenance goes away.

## Naming for consumed-WIT headers

These headers invert psiserve's usual C++ → WIT direction — the WIT is
external and fixed, and the C++ is authored to reflect onto it. Two
rules keep the authored C++ clean across both clang and GCC:

1. **Use the WIT bare name for types when it's unambiguous in the
   header's translation unit.** WIT scopes type names per interface;
   C++ does not. If no other type in the same TU claims the bare name,
   drop any interface prefix and let reflection emit the spec-correct
   WIT identifier. A WIT type and a WIT function in the same interface
   can share a bare name (e.g. `error-code` and `network-error-code`)
   — mapping each to its bare C++ identifier (`error_code` and
   `network_error_code`) avoids a C++ name collision naturally.

   If a future interface in the same TU would also need `error_code`,
   fall back to a per-interface nested namespace (e.g.
   `wasi_sockets_network::error_code`). Reflection strips `::` scope
   qualifiers before kebab-lowering, so the emitted WIT name is
   unaffected.

2. **Typed literals for WASI numeric constants.** Multiplications and
   divisions against `wasi_duration` (and similar typedefs) use
   `wasi_duration{N}`, not raw `ULL`/`UL` suffixes. `uint64_t` has
   different underlying widths on Linux (`unsigned long`) and macOS
   (`unsigned long long`); type deduction through helpers like
   `sock_detail::ok<T>` picks up the wider type from the suffix and
   breaks the declared return variant.

## Usage from psiserve

```cpp
#include <wasi/0.2.3/io.hpp>
#include <wasi/0.2.3/sockets.hpp>

// Psiserve implements these via PSIO_HOST_MODULE'd classes;
// Linker<world> wires impls to imports at composition time.
```

## Not here

- Host implementations — those live in psiserve.
- Rust / Go / Python / JS bindings — those live in their own
  ecosystems, generated from `../wit/` via each ecosystem's tooling.
