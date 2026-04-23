---
id: psiserve-onchain-library-linking
title: On-chain library linking — libc / libc++ / libpsibase as content-addressed wasm modules
status: backlog
priority: high
area: psizam, psiserve, source-to-pzam
agent: ~
branch: ~
created: 2026-04-22
depends_on: []
blocks: []
---

## Description

Today every guest wasm statically links its own copy of libc, libc++, any
framework libraries it uses. A typical service with STL ends up 50–500 KB;
the code-section bytes are dominated by libc/libc++. Every contract
uploaded to chain pays for another copy of this code in storage, in JIT
work at prepare time, and in host code memory after JIT caching.

We want the canonical pattern:

- System libraries (libc, libc++, optionally libpsibase implementation)
  are uploaded once as content-addressed wasm modules.
- Contract wasms import symbols from those libraries instead of linking
  their code in.
- The runtime resolves those imports once at `prepare()` time per
  contract, not per instance.

This is the wasm-world analogue of ELF shared libraries, adapted to the
pzam JIT cache. It is NOT wasm-level dynamic linking (Emscripten main-
module/side-module) at instantiation time — the resolution happens
during the native-code compile step, not at linear-memory-instantiation.

## Prior art in this repo

- `psizam::runtime::prepare(wasm)` → `module_handle`, caches the JIT'd
  pzam internally. Today each pzam is self-contained.
- `reloc_symbol` (see `psizam/pzam_cache.hpp`) lists the relocation
  classes the loader resolves. Today they cover runtime helpers only
  (`call_host_function`, `grow_memory`, trap handlers, float-trunc
  helpers). No "external pzam symbol" class exists.
- `programs/source-to-pzam/` drives the wasi-sdk toolchain (clang.pzam
  + wasm-ld.pzam + native pzam-compile) to produce .pzams.
- psibase's `libraries/psibase/CMakeLists.txt` already demonstrates the
  libc surgery pattern (`cp libc.a; llvm-ar d dlmalloc.o; llvm-ar rs
  simple_malloc.o`) we'd reuse for our bump variant.

## Design — runtime layer

### Canonical artifact = .wasm

Users only interact with .wasm files. The `.pzam` is a runtime cache
artifact, not a user-visible format. A library is registered by its
.wasm bytes:

```cpp
library_id psizam::runtime::register_library(
   std::string  name,        // e.g. "libc"
   wasm_bytes   wasm,        // the side-module wasm
   build_profile profile);   // must match contracts that link against it
```

Internally the runtime JIT-compiles to a `.pzam` (caches), keeps its
symbol table, and assigns a stable native code address range.

### Linking at prepare, not at instantiate

`prepare(contract_wasm)` runs exactly once per contract-wasm hash. It:

1. Parses the wasm; enumerates imports.
2. For each import whose `(module, field)` matches a registered library,
   records an `external_library_symbol` relocation carrying
   `(library_hash, symbol_id)`. NO concrete address is baked in.
3. JIT-compiles the contract's own functions.
4. Emits a PIC pzam: relocation table + code blob. Code blob has zero
   embedded absolute addresses for external symbols; all go through
   the relocation table.

`instantiate()` does zero linking work:

1. Allocate linear memory.
2. memcpy the initial-state image.
3. Return instance.

Relocations resolve once at pzam LOAD (not at each instance). The
loader walks the reloc table, fills a per-pzam GOT-style table from
the currently-registered library's symbol addresses, and either
patches call sites (eager bind) or leaves them indirect (lazy bind).

Both approaches have been validated in decades of ELF linker work;
we'd pick eager bind for hot pzams and lazy for cold.

### Cache key

A contract pzam depends on every library it links against. The pzam
cache key becomes:

```
pzam_key = hash(
   contract_wasm_hash,
   [ (library_name, library_wasm_hash) for each imported library ],
   runtime_policy = { jit_backend, float_mode, deterministic,
                      gas_metering, arch },
   opt_tier
)
```

A library upgrade (new bytes) → new hash → all dependent pzams
cache-invalidate. No stale execution possible.

### New reloc class

Add to `reloc_symbol` (per arch):

```
external_library_symbol = (library_hash_short, symbol_id)
```

The short hash (12-16 bytes of the library's full hash) plus a
per-library symbol id lets the loader look up the target address in
whichever library pzam is currently registered at that hash. Veneers
on aarch64, direct rel32 patch on x86_64.

## Design — build layer

### Profile tuple

Build-time axes that affect .wasm bytes (CMake-visible):

```
profile = (memory_class,   // wasm16 / wasm32 / wasm64
           allocator,      // bump / full
           exec_model,     // command / reactor
           opt_tier)       // debug / O2 / Oz
```

Runtime axes (softfloat, determinism, jit backend, arch, gas metering)
are NOT in the profile. They live in the pzam cache key only.

Three practical profiles cover nearly all uses:

```
PSI_PROFILE_SERVICE = (wasm32, bump, reactor, O2)
PSI_PROFILE_CHAIN   = (wasm32, full, command, O2)
PSI_PROFILE_TINY    = (wasm16, bump, reactor, Oz)
```

### Tiers

| tier | what | how it ships | CMake helper |
|---|---|---|---|
| 1 | System on-chain library | side-module .wasm + pzam | `psi_wasm_library` |
| 2 | User static library | `.a` archive + headers | `psi_static_library` |
| 3 | Application (service/contract) | plain .wasm | `psi_service` |

Tier 1: libc, libc++. PIC, side-module layout, fixed linear-memory
data zone. Built per profile. One CMake target per (library, profile).

Tier 2: libpsibase implementation, any framework code that's
template-heavy or mixed-template. Static archive linked into the
contract's wasm at build time. Can declare transitive LINKS to tier
1 libraries; those propagate as imports in the final wasm.

Tier 3: the user's service. Three-line CMake:

```cmake
psi_service(my_contract
   SOURCES src/service.cpp
   PROFILE PSI_PROFILE_SERVICE
   LINKS   psi-libpsibase)        # transitive: pulls libc/libc++
```

The service author never writes `-mexec-model`, `-Wl,--global-base`,
`-Wl,--allow-undefined`. Profile + LINKS is enough to derive all flags.

### Linear-memory layout

Each tier-1 library claims a fixed zone of linear memory for its
`.rodata`/`.data`/`.bss`. The zone is pinned per library version and
exposed via a header:

```c
// psi-libc-layout.h — shipped alongside libc.wasm
#define PSI_LIBC_DATA_BASE   0x1000
#define PSI_LIBC_DATA_END    0x4000   // 12 KB reserved
```

Contracts link with `-Wl,--global-base=$(sum of tier-1 library zones)`
so their own data lands above. Mismatch → link error at CMake time.

A new library version = new layout constants = new content hash =
independent of the previous version. Old contracts unaffected.

## Design — toolchain layer

### On-chain linking via wasm-ified clang/wasm-ld

The `source-to-pzam` infrastructure already runs `clang.pzam` and
`wasm-ld.pzam` inside psizam. With an in-memory VFS WASI host
(see issue `psiserve-user-filesystem.md` — same shape), the full
compile+link pipeline runs entirely from memory:

1. Host populates VFS: user source bytes + library bytes from the
   chain's content store + pinned sysroot.
2. Run clang.pzam: source → .o.
3. Run wasm-ld.pzam: .o + libraries → .wasm.
4. Run pzam-compile (native): .wasm → .pzam.

Determinism: clang and wasm-ld are integer-only workloads (no FP in
their codegen path), so all engines produce byte-identical output.
The pipeline's only inputs are content-hashed bytes (source +
sysroot + toolchain pzams + deps). Output is a deterministic
function of those hashes.

Browser: same pipeline, with V8/SpiderMonkey/JSCore executing
clang.wasm (not clang.pzam) and a JS-side WASI shim backed by
an in-memory VFS. Byte-identical output to the server pipeline.

### Pinned toolchain

Chain governance records the content hashes of:

- clang.wasm
- wasm-ld.wasm
- wasi-sysroot bundle (fracpack-packaged)

These are consensus-critical artifacts. Upgrading any of the three is
a governance event; old contracts keep pinning to the version they
were tested against.

## Acceptance criteria

### Phase 1 — runtime linking

- [ ] `psizam::runtime::register_library()` API implemented.
- [ ] `external_library_symbol` relocation class on both arches.
- [ ] PIC pzam emission (current pzam format is compatible; PIC flags
      at codegen enforced).
- [ ] Pzam-load patches relocations once per pzam load, not per
      instance.
- [ ] Cache key includes linked-library hashes.
- [ ] End-to-end toy: register a `libmath.wasm` with `square`/`cube`
      exports; a `user.wasm` that imports them; instantiate; confirm
      correct behavior.

### Phase 2 — build-system layer

- [ ] `psi_declare_profile()` macro in CMake.
- [ ] `psi_wasm_library(... PROFILES ...)` helper emits side-module
      wasm with the layout header generation.
- [ ] `psi_static_library(... PROFILE ... LINKS ...)` helper for
      tier-2 archives with transitive LINKS.
- [ ] `psi_service(... PROFILE ... LINKS ...)` three-line service
      recipe; derives all flags from profile + transitive LINKS.
- [ ] Profile mismatch between source and its LINKS fails at
      configure time with a clear message.

### Phase 3 — system libraries

- [ ] `libraries/psi-libc/` — bump allocator + WASI polyfill + crt
      variants (command + reactor). Reproduces the psibase recipe
      (patched libc.a + libc++abi.a + polyfill).
- [ ] `libraries/psi-libcxx/` — wasi-libc's libc++ wrapped as a
      tier-1 side module. Exports the functions our common services
      need; small set, not the whole STL.
- [ ] `libraries/psi-libpsibase/` — the psibase service framework
      split into (a) tier-2 static archive, (b) header-only template
      code. Uses libc/libc++ as tier-1 deps.
- [ ] Published `psi-libc-layout.h` / `psi-libcxx-layout.h` for
      consumers.

### Phase 4 — on-chain toolchain

- [ ] In-memory VFS WASI host (shared with snapshot infra).
- [ ] `psi-build` library: portable driver that runs clang.wasm +
      wasm-ld.wasm over a VFS. Built both natively (server) and for
      browser use (compiled to wasm OR exposed via JS bindings to
      V8's wasm engine).
- [ ] Chain governance: pinned content hashes for clang.wasm,
      wasm-ld.wasm, sysroot. Upgrades are explicit governance acts.
- [ ] Reference browser editor that uses `psi-build` to produce
      contracts locally; chain re-runs the same pipeline; bytes
      match byte-for-byte.

## Non-goals

- **Not using kernel COW**. Instance linear memory is private, no
  shared mappings. Sharing happens at the pzam code layer via
  registered libraries; static data in linear memory stays private
  per instance. See design note at the top of
  `plans/psiserve-design.md`.
- **Not implementing wasm dynamic linking (Emscripten side-module at
  instantiate time)**. Resolution happens at pzam-compile (prepare),
  not at wasm-instantiate.
- **Not shipping native lld as part of the chain**. Linking is done
  via `wasm-ld.wasm` executed inside psizam, pinned by content hash.
  Determinism comes from wasm execution semantics, not lld's internal
  stability across versions.
- **Not solving cross-language library reuse** yet. Each language's
  stdlib is its own set of tier-1 wasms (Rust std.wasm, TinyGo
  runtime.wasm, etc.). They coexist on chain but don't share code.
