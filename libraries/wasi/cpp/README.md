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

## Usage from psiserve

```cpp
#include <wasi/0.2.3/io.hpp>
#include <wasi/0.2.3/sockets.hpp>

// Psiserve implements these via PSIO_IMPL'd classes;
// Linker<world> wires impls to imports at composition time.
```

## Not here

- Host implementations — those live in psiserve.
- Rust / Go / Python / JS bindings — those live in their own
  ecosystems, generated from `../wit/` via each ecosystem's tooling.
