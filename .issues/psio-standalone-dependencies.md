# psio standalone dependency analysis

**Status:** survey of the dependency surface that the new
`gofractally/psio` repo (vendored at `external/psio` here) carries.
Goal: enumerate exactly what a downstream project must provide to
build and use psio as a standalone library, and call out which
dependencies are blockers vs. clean per-feature opt-ins.

Submodule HEAD surveyed: `863ede1` (`Add native format and view_of
concept`, on top of `09eaae5 Seed from gofractally/psiserve@0c2004e:
libraries/psio/cpp`).

## TL;DR

| layer | hard-required | optional (per-feature) |
|---|---|---|
| **Library** (`include/`)        | C++23 stdlib · Boost.Preprocessor (headers-only) · xxhash (single-header) · ucc (header-only utility lib) | simdjson · libflatbuffers |
| **Tests** (`tests/`)            | Catch2 v2 (single-header) | msgpack-cxx · libprotobuf · libcapnp · libflatbuffers · psio1 |
| **Benchmarks** (`benchmarks/`)  | — | libcapnp · libflatbuffers · msgpack-cxx · libprotobuf · simdjson · psio1 |

Two **standalone-blockers** today:

1. The CMakeLists wires xxhash + ucc directly from `external/psitri/`
   paths. A consumer that doesn't have psitri in their tree can't build
   psio without first patching those paths. **Mitigation needed before
   psio ships standalone** — either vendor those into psio's own
   external/ tree, or expose them as CMake-find-package targets.
2. ~13 test files `#include <psio1/...>` for v1-parity oracle tests.
   psio1 is psiserve-internal; psio standalone has no way to fetch it.
   **Mitigation:** move v1-parity tests up to `psiserve` (out of psio's
   test suite), or gate them in psio behind `PSIO_HAVE_PSIO1=ON`.

## Library — hard requirements

### 1. C++23 standard library

Per `target_compile_features(psio PUBLIC cxx_std_23)` in psio's
`CMakeLists.txt`. Standard headers used (≥30 of them; partial list):
`<algorithm>`, `<array>`, `<bit>`, `<charconv>`, `<chrono>`,
`<compare>`, `<concepts>`, `<cstdint>`, `<expected>`, `<map>`,
`<optional>`, `<set>`, `<span>`, `<string_view>`, `<tuple>`,
`<type_traits>`, `<unordered_map>`, `<utility>`, `<variant>`,
`<vector>`. Notable C++23 features in use: `std::expected`,
`std::span`, ranges-style `<ranges>` constructs.

### 2. Boost.Preprocessor (header-only)

```cmake
find_package(Boost REQUIRED)
target_link_libraries(psio PUBLIC Boost::headers)
```

Used by `PSIO_REFLECT(...)` and friends — `BOOST_PP_CAT`,
`BOOST_PP_SEQ_FOR_EACH*`, `BOOST_PP_VARIADIC_*`,
`BOOST_PP_CHECK_EMPTY`, `BOOST_PP_TUPLE_*`, etc. for the keyword
dispatch and per-field codegen. **Header-only**; `Boost::headers`
target is enough. Tested against Boost 1.79+.

### 3. xxhash (single-header)

```cmake
target_include_directories(psio PUBLIC
   ${CMAKE_SOURCE_DIR}/external/psitri/libraries/hash/include)
```

Used by `psio/pjson.hpp`, `psio/pjson_view.hpp`, `psio/psch.hpp`
through `#include <hash/xxhash.h>` with `#define XXH_INLINE_ALL`.
Powers the 8-bit key prefilter in pjson's object lookup and
canonical-form hash template baked into `is_canonical_for<T>`.

**Standalone gap:** the CMake hard-codes the `external/psitri/...`
path. A consumer without psitri will fail at configure unless they
either (a) vendor the file at the same relative path, or (b) patch
the include directory. The fix: drop the file (or a thin wrapper)
into `external/psio/external/hash/` and update the CMake path.

### 4. ucc — psitri's small-utility library (header-only)

```cmake
target_include_directories(psio PUBLIC
   ${CMAKE_SOURCE_DIR}/external/psitri/libraries/ucc/include)
```

Used by `pjson_view.hpp` through `#include <ucc/lower_bound.hpp>`.
Provides `ucc::find_byte` — SWAR 8-byte-at-a-time scalar byte scan
with NEON / AVX2 fast paths. Powers the hash-prefilter scan in
pjson's object key lookup.

**Standalone gap:** same shape as xxhash — the path points at
psitri. The dependency is **a single header providing one function**
(`find_byte`); the simplest fix is to vendor that one header into
psio's own external/ tree, or replace the call with a portable
`memchr`-based fallback.

## Library — optional / per-feature

### 5. simdjson (optional)

```cmake
find_package(simdjson QUIET)
if(simdjson_FOUND)
   target_link_libraries(psio PUBLIC simdjson::simdjson)
   target_compile_definitions(psio PUBLIC PSIO_HAVE_SIMDJSON=1)
endif()
```

Used by `pjson_json.hpp` / `pjson_json_typed.hpp` for
`pjson::from_json` (JSON text → pjson binary). **Cleanly gated** via
`PSIO_HAVE_SIMDJSON`; consumers without simdjson keep everything
else and just lose JSON-text input. Already standalone-clean.

### 6. libflatbuffers (optional, only via `flatbuf_lib.hpp`)

`flatbuf_lib.hpp` `#include <flatbuffers/flatbuffers.h>` for the
`psio::flatbuf_lib` format tag — a bridge to Google's canonical
FlatBuffers runtime, kept alongside psio's own zero-dep
`psio::flatbuf` for byte-parity verification.

Currently NOT gated in psio's CMakeLists — the header references
`flatbuffers/flatbuffers.h` unconditionally, but the file is only
compiled by users who include `psio/flatbuf_lib.hpp`. Header-only
psio works without libflatbuffers as long as nothing pulls in
`flatbuf_lib.hpp`.

**Recommendation for standalone:** gate the include in `flatbuf_lib.hpp`
behind `#if __has_include(<flatbuffers/flatbuffers.h>)` or expose a
`PSIO_HAVE_FLATBUFFERS` define so consumers know whether the bridge
is available.

## Tests — additional dependencies

`tests/` is gated behind `PSIO_ENABLE_TESTS=ON` (default OFF).

### 7. Catch2 v2 (single-header)

```cmake
set(PSIO_CATCH2_INCLUDE
   ${CMAKE_SOURCE_DIR}/libraries/psizam/external/Catch2/single_include/catch2)
```

Used by every `*_tests.cpp`. **Standalone gap:** path hard-codes
`libraries/psizam/external/Catch2/...`. For psio standalone, consumers
need to either fetch psizam (heavy — psizam is a full WASM engine) or
have the path patched. Easy fix: point at a system `Catch2` package
via `find_package(Catch2 2 QUIET)` with the in-tree path as fallback.

### 8. psio1 (psiserve-internal — currently a hard test dep)

13 test files `#include <psio1/...>` for v1-parity oracle tests:

```
psio1/bitset.hpp · psio1/capnp_view.hpp · psio1/compress_name.hpp ·
psio1/ext_int.hpp · psio1/flatbuf.hpp · psio1/fracpack.hpp ·
psio1/from_avro.hpp · psio1/from_bin.hpp · psio1/from_bincode.hpp ·
psio1/from_borsh.hpp · psio1/from_key.hpp · psio1/from_pssz.hpp ·
psio1/from_ssz.hpp
```

These tests confirm psio's wire-format outputs are byte-identical to
psio v1 — but they only compile when `libraries/psio1/` exists, which
is psiserve-only.

**Standalone gap:** psio1 is not available outside psiserve. **Action
items:** either (a) move these tests up to `psiserve/tests/`, or (b)
add `PSIO_HAVE_PSIO1` gate in `tests/CMakeLists.txt` so they only
compile when psio1 is reachable.

### 9. msgpack-cxx (one test)

One test file (`tests/msgpack_tests.cpp`) `#include <msgpack.hpp>` for
adapter conformance vs. the canonical library. Cleanly gated via
`PSIO_HAVE_MSGPACK` if the parent CMake exposes it; standalone-fine.

### 10. libprotobuf, libcapnp (used in tests via adapter shapes)

Tests/benchmarks include adapters that use the canonical libraries.
Already gated behind their respective `find_package` checks in
`benchmarks/CMakeLists.txt`. Standalone-fine.

## Benchmarks — additional dependencies

`benchmarks/` is gated behind `PSIO_ENABLE_BENCHMARKS=ON` (default
OFF). Includes adapter-bridge code that brings in (all optional via
`find_package`):

- **libcapnp** (`<capnp/message.h>`, `<kj/array.h>`) — used by
  `bench_psio_vs_externals.cpp` for the `psio::capnp` view ground-truth.
- **libflatbuffers** — same shape, for `psio::flatbuf` parity.
- **msgpack-cxx** — same.
- **libprotobuf** — same.
- **simdjson** — for the json bench.
- **psio1** — for v1↔v3 head-to-head wire/perf comparison.

All cleanly gated; absence of any one disables the column for that
external library, the bench still runs against psio's own formats.

## Recommended path to "psio is standalone-buildable"

In rough priority order:

### Must-fix (block standalone build)

1. **Vendor `hash/xxhash.h` into `external/psio/external/hash/`** and
   change the CMake path. The single-header file is ~3 KLOC and BSD-2;
   psitri's copy is itself vendored from the upstream xxHash repo.
2. **Vendor `ucc::find_byte` into `external/psio/external/ucc/`** OR
   rewrite the one call site to use a portable `memchr` / loop fallback
   (the SWAR optimization is nice-to-have but not load-bearing). The
   one accessor used is a few hundred lines.
3. **Switch Catch2 path** to `find_package(Catch2 2 QUIET)` first, with
   the psizam path as a fallback only when building inside psiserve.

### Should-fix (cleaner standalone test surface)

4. **Gate `psio1` tests** behind `if(TARGET psio1)` or
   `option(PSIO_TEST_AGAINST_PSIO1 OFF)`. The tests stay in
   `external/psio/tests/` for psiserve to opt into; psio standalone
   skips them.
5. **Gate `flatbuf_lib.hpp`** behind `#if __has_include(...)` so the
   header doesn't fail to parse when libflatbuffers is absent.

### Nice-to-have

6. **Export a CMake package** (`psioConfig.cmake`) so consumers can do
   `find_package(psio)` instead of the subdirectory pattern psiserve
   uses today.
7. **Document the build matrix** in psio's README: which feature
   needs which dep, default ON/OFF, link command.

## Status of this consumer (psiserve)

The change in this commit is just `add_subdirectory(external/psio)`
in place of `add_subdirectory(libraries/psio)`. Tests still pass:
- `psio_annotate_tests`: 26 cases / 112 assertions — green
- `psio_caps_parity_tests`: 2 / 15 — green
- `psio_pssz_tests`: 32 / 65 (4 pre-existing failures unchanged)
- `psio_bench_vs_externals`: exit 0, 1914 rows

Because psiserve has psitri checked out at `external/psitri/`, the
hard-coded paths in psio's CMake resolve naturally — psiserve hides
the standalone-blocker. **Consumers without psitri will need the
fixes above before they can use psio without patching.**

`libraries/psio/` (the legacy in-tree copy) is still present but no
longer reachable from the build. It can be deleted in a follow-up
once we're satisfied with the swap.
