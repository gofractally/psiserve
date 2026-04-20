---
id: psizam-unify-host-registration-under-runtime
title: Unify WASI / psix / WIT host registration under a single runtime API
status: blocked
priority: high
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: [psizam-llvm-aot-runtime-crash, psizam-runtime-api-maturation]
blocks: []
---

> **2026-04-19 update — blocked on `psizam-runtime-api-maturation`.**
> First-pass attempt discovered the target `runtime` API is still
> skeletal (see "Pre-audit findings" below). This issue stays as the
> *destination* plan; the prerequisite work of bringing `runtime` up
> to functional parity with `composition<>` and with pzam-run's hand-
> rolled loader lives in `.issues/psizam-runtime-api-maturation.md`.
> Once that issue lands, every step below is tractable as originally
> written.

## Summary

Collapse the 4+ hand-rolled "load a WASM module and wire its imports"
code paths into a single library entrypoint built on `psizam::runtime`.
Eliminate `host_function_table` and `wasi_host` (the legacy, name-string-
based binding system) in favor of `PSIO_INTERFACE`-driven registration
through `runtime::provide<>`.

This is the structural fix for the class of bug tracked in issue #0016
— the recurring "trampoline arg-order wired wrong in N different
places" family. The specific fix in #0016 unblocked clang.pzam /
wasm-ld.pzam execution; #0018 makes that class of bug impossible by
removing the hand-rolled wiring entirely.

## Motivation — why we keep hitting this

Between 2026-04-16 and 2026-04-19 the repo took **six separate commits**
in the "host-call trampoline arg-order" family:

```
604bcb5  Pick host-call trampoline direction per backend
189565b  Fix JIT1/JIT2 composition support: trampoline arrays + arg reversal
fb1e719  Fix aarch64 JIT host-call arg repack producing forward-ordered buffer
6259a35  Fix jit_llvm precondition crash: add slow_trampoline_fwd for forward-order args
96cba71  Add debug asserts for trampoline arg-order fallback in backend.hpp
6eed38f  pzam-run: fix page_size hardcode, rev trampolines, code_size for signal handler
960c425  pzam-run: pick forward trampoline for LLVM AOT .pzam (#0016 fix)
```

Root cause: two calling conventions for host-call args coexist for real
perf reasons —
- **reverse** (args[0] = LAST WASM param): `jit`, `jit2` use this to
  avoid copying — the WASM operand stack already has args in LIFO
  order
- **forward** (args[0] = FIRST WASM param): interpreter, `jit_llvm`,
  and canonical-ABI calls use this

…and every code path that loads a module and wires imports has
independently reinvented the "pick the right trampoline variant" dance:

| Code path | Wires trampolines? | Has asserts? |
|---|---|---|
| `backend<>::construct` (library) | ✅ | ✅ (commit 96cba71) |
| `pzam-run` CLI | ✅ hand-rolled | ❌ |
| `composition_tests.cpp` | ✅ hand-rolled | ❌ |
| Future `typed_function`, etc. | Unknown | Unknown |

When a hand-rolled path picks the wrong variant, the WASI call silently
returns garbage — the fallback `entry.rev_trampoline ? entry.rev_trampoline : entry.trampoline`
is unconditional. Every manifestation of the bug looks like a different
codegen-level symptom (register clobber, OOB load, string bytes used as
pointer) because the corruption happens deep in the callee's consumption
of the mis-ordered args. That is why we kept chasing phantom codegen
bugs through #0016.

## What we already have (the right pieces, not yet connected)

The `psizam::runtime` system introduced in commit `3b344cd` ("Wire
runtime Phase 2: prepare, provide, instantiate, typed proxy calls") is
the correct library API:

```cpp
psizam::runtime rt(cfg);
rt.provide<HostInterface>(module, host_impl);
auto handle = rt.prepare(wasm_bytes, policy);       // OR
auto handle = rt.load_cached(pzam_bytes);
auto inst   = rt.instantiate(handle);
inst.as<GuestInterface>().some_method(...);         // typed calls
inst.run_start();                                    // WASI-style start
```

From the commit message:
> "runtime::provide<Host>(mod, host) walks PSIO_HOST_MODULE interfaces
> and registers methods with the module's host_function_table —
> **scalar methods get fast trampolines, canonical methods get
> type-specialized 16-wide handlers with compile-time lift/lower
> (same perf as hosted<>)**."

So `runtime` already handles *both* calling conventions transparently,
picking the right trampoline + direction at compile time based on the
method's signature and the resolved backend kind. The asserts from
`96cba71` fire on the one canonical path.

## The conceptual model — three host flavors, two calling conventions

Every "host" is just a provider of one or more interfaces. The
differences are only in the *shape* of the signatures, not in the
registration mechanism.

| Host flavor | Import namespace | Calling convention | Example audience |
|---|---|---|---|
| **WASI Preview 1** | `wasi_snapshot_preview1.*` | Scalar / C-ABI (i32/i64/f32/f64, u32 linmem pointers, errno returns) | wasi-sdk-compiled tools: clang.wasm, wasm-ld.wasm, ImageMagick, etc. |
| **psix** (new) | `psiserve:io.*` (or chosen ns) | Scalar / C-ABI — raw syscall shape but with psiserve-specific semantics (ring-buffer I/O, process-per-instance scheduler, message passing) | services built with `psi_sdk` |
| **WIT / component** | `package:interface@version.*` | Canonical ABI (`string`, `list<T>`, `record`, `option`, `result`, `resource`) — compile-time lift/lower | psibase contracts, multi-language components (Rust/Go/MoonBit/Zig/TS) |

So really only **two** calling conventions exist:
1. **Scalar / C-ABI** — WASI + psix. Fast path: args flow
   register-to-register with only the `i32 ↔ i64` widening the trampoline
   already does.
2. **Canonical ABI** — WIT. Per-type lift/lower through linear memory,
   already handled by `canonical_trampoline<method>` registered by
   `runtime::provide<>`.

`runtime::provide<T>` dispatches at compile time: if all args/returns
on a method are scalar, it registers a fast trampoline (both forward
and reverse variants); otherwise it registers a canonical-ABI handler.
The backend/pzam-opt-tier determines which variant gets selected at
instantiation time.

## Target architecture

After this issue is done, the whole host-binding picture is:

```
                     guest WASM binaries
                            │
           ┌────────────────┼─────────────────┐
           │                │                 │
  [wasi-sdk compiled]  [psi_sdk services]  [WIT components]
   clang.wasm,           psibase contracts,   Rust/Go/MoonBit
   wasm-ld.wasm          HTTP services         components
           │                │                 │
           ▼                ▼                 ▼
     wasi_preview1       psix            user-defined
     PSIO_INTERFACE      PSIO_INTERFACE   PSIO_INTERFACE
     (scalar ABI)        (scalar ABI)     (canonical ABI)
           │                │                 │
           └────────┬───────┴─────────────────┘
                    ▼
             runtime::provide<T>
             runtime::load_cached / prepare
             runtime::instantiate
                    ▼
              backend<BackendKind>
                    ▼
              psizam execution
```

**One registration mechanism, one load mechanism, one instantiate
mechanism.** `host_function_table` and `wasi_host` disappear. No
executable or test ever touches `trampoline` / `rev_trampoline`
directly.

## Example of what the end state looks like

`pzam-run`'s entire main becomes approximately this (was ~350 lines
with the hand-rolled wiring):

```cpp
int pzam_run_main(int argc, char** argv) {
   auto args = parse_cli(argc, argv);

   psizam::runtime rt(runtime_config{/* guarded pool etc */});

   wasi_shim wasi;
   wasi.args     = std::move(args.wasm_args);
   wasi.env      = current_env();
   for (auto& [guest, host] : args.preopens) wasi.add_preopen(guest, host);

   auto mod = rt.load_cached(psizam::pzam_bytes{read_file(args.pzam_path)});
   rt.provide<wasi_preview1>(mod, wasi);

   auto inst = rt.instantiate(mod);
   return inst.run_start();
}
```

No trampoline selection. No page-size conversion dance. No
`_host_trampoline_ptrs` array construction. No jit_func_ranges
population (that's a runtime concern — it moves inside
`runtime::instantiate`). No manual relocation application.

Similarly, `composition_tests.cpp` binds two instances through the
runtime bind API (`rt.bind<InterfaceTag>(consumer, provider)`) rather
than hand-constructing host_function_table entries.

## Concrete work plan

### Step 1 — Define WASI Preview 1 as a `PSIO_INTERFACE` host
Re-express the ~60 WASI Preview 1 functions currently in
`wasi_host.hpp` as a `struct wasi_snapshot_preview1 { ... }` with
`PSIO_INTERFACE(...)` reflection. Keep the signatures scalar (i32/i64);
the implementation still calls the existing POSIX-backed handlers. The
rename matters: `wasi_host` becomes the host-side *implementation*
(the struct whose methods get bound), no longer a special binding
layer.

Out of scope for this step: replacing the POSIX-backed implementation
with psix. Just move the ABI surface onto the typed interface system.

**Files**: `libraries/psizam/include/psizam/wasi/preview1.hpp` (new);
keep `wasi_host.hpp`'s argv/env/preopens state but strip its
`host_function_table` registration helpers.

### Step 2 — Express `psix` as a `PSIO_INTERFACE`
Following `plans/psix-ipc-design.md`, design the initial psix host
interface as a `PSIO_INTERFACE`. Scalar signatures where the spec
calls for them (fds, byte offsets), rich types via canonical ABI where
they fit (message envelopes, preopened descriptors).

This is a new codebase artifact; no legacy to migrate. It should
reuse the same registration helper that WASI uses.

**Files**: `libraries/psiserve/include/psiserve/psix/interface.hpp` (new),
`libraries/psiserve/src/psix/*.cpp` (implementation).

### Step 3 — Port `pzam-run` to the runtime API
Rewrite `libraries/psizam/tools/pzam_run.cpp` main around
`runtime::load_cached + provide<wasi_preview1> + instantiate +
run_start`. Delete the hand-rolled trampoline selection, the
page-size conversion block, and the manual `_host_trampoline_ptrs`
wiring.

Acceptance: `pzam-run /tmp/wasm-ld.pzam -- --version` and
`pzam-run /tmp/clang.pzam --version` still produce the same output
they do today (after the #0016 fix). Argument-passing no longer
depends on CLI-level code.

### Step 4 — Port `composition_tests.cpp` similarly
Drop the direct `host_function_table` usage; use
`runtime::bind<InterfaceTag>` and/or typed `provide<>`.

### Step 5 — Delete the legacy layer
Once nothing references them, remove:

- `libraries/psizam/include/psizam/host_function_table.hpp`
- `libraries/psizam/include/psizam/detail/wasi_host.hpp`
  (the `host_function_table`-based registration; the data holding
  `env`, `args`, `preopens` lives on the new `wasi_shim` struct)
- `add_wasi_func`, `register_wasi`, `wasi_trampoline`,
  `wasi_trampoline_rev` helpers
- The `rev_trampoline` / `trampoline` field split on
  `host_function_table::entry` is kept (still needed internally by
  `backend<>::construct`), but the entry struct itself stays internal
  to the library.

### Step 6 — Lock it in
Add the assert from commit `96cba71` to **every** remaining place
that writes to `_host_trampoline_ptrs`. Expectation: there should be
exactly one such place — inside the runtime / backend construct path.
A grep like
`grep -rn '_host_trampoline_ptrs\|\brev_trampoline\b' libraries/ tools/ tests/`
should show only internal library files.

## Acceptance criteria

- [ ] WASI Preview 1 surface lives in a `PSIO_INTERFACE` declaration;
      no `host_function_table::entry` is constructed outside
      `runtime::provide<>`.
- [ ] `psix` is registered through the same `provide<>` mechanism.
- [ ] `pzam-run`'s main function is under ~40 lines.
- [ ] `composition_tests.cpp` uses `runtime` APIs only.
- [ ] `grep -rn 'rev_trampoline' libraries/psizam/tools/ libraries/psizam/tests/`
      returns zero matches.
- [ ] `grep -rn '_host_trampoline_ptrs' libraries/psizam/tools/`
      returns zero matches.
- [ ] All existing tests pass.
- [ ] `pzam-run /tmp/wasm-ld.pzam -- --version` and
      `/tmp/clang.pzam --version` still print the expected output
      (matching the interpreter).
- [ ] `libraries/psizam/include/psizam/host_function_table.hpp` and
      the legacy `wasi_host.hpp` registration helpers are deleted.

## Files that will be touched

New:
- `libraries/psizam/include/psizam/wasi/preview1.hpp` — typed WASI P1 interface
- `libraries/psiserve/include/psiserve/psix/interface.hpp` — typed psix interface

Modified:
- `libraries/psizam/tools/pzam_run.cpp` — rewritten around `runtime`
- `libraries/psizam/tests/composition_tests.cpp` — rewritten around `runtime`
- `libraries/psizam/include/psizam/runtime.hpp` — may need small
  additions (e.g. a `run_start()` convenience, or a `load_cached` that
  resolves the backend kind from the .pzam's `opt_tier`)
- `libraries/psizam/src/runtime.cpp` — implementation

Deleted:
- `libraries/psizam/include/psizam/host_function_table.hpp`
- Legacy parts of `libraries/psizam/include/psizam/detail/wasi_host.hpp`

## Background reading / context the next agent will want

- `libraries/psizam/include/psizam/runtime.hpp` — current runtime API
  surface. Already has `load_cached(pzam_bytes)`, `provide<Host>`,
  `instantiate`, `bind<Interface>`.
- `libraries/psizam/src/runtime.cpp` — runtime implementation.
- `libraries/psizam/examples/runtime/hello_runtime.cpp` — working
  end-to-end example of the new API with typed interfaces.
- `libraries/psizam/examples/hello_world/host.cpp` — example host
  binding via `PSIO_INTERFACE`.
- `libraries/psizam/include/psizam/backend.hpp` — where the
  trampoline-direction dispatch currently lives (`reverse_host_args`
  flag per backend + `96cba71`'s asserts).
- `libraries/psizam/include/psizam/host_function_table.hpp` — legacy
  table, to be removed.
- `libraries/psizam/include/psizam/detail/wasi_host.hpp` — legacy
  WASI binding, to be migrated.
- `libraries/psizam/tools/pzam_run.cpp` — the worst offender on
  hand-rolled wiring, including the trampoline-direction fix from
  `960c425` that motivated this issue.
- `plans/psix-ipc-design.md` — psix design doc; informs how psix
  should be exposed as a typed interface.
- Commit `3b344cd` message — describes the scalar-vs-canonical
  compile-time dispatch inside `runtime::provide`.
- Commit `960c425` message — the #0016 fix that exposed this issue.
- Commit `96cba71` — the asserts that catch the trampoline-direction
  mismatch. Whatever assertions pzam-run was missing should be
  impossible to miss after this issue lands, because the relevant
  code moves into the runtime.

## Notes for the implementer

- WASI's ~60 functions are mostly mechanical — there's a natural
  pattern (fd ops, path ops, clock ops, env/args, random, poll). If
  the PSIO macros don't yet support raw linear-memory pointer types
  cleanly, you may need a small `wit::linmem_ptr<T>` helper. Keep the
  scalar ABI — don't "upgrade" WASI to rich types, it needs to stay
  wire-compatible with wasi-libc.
- `runtime::load_cached` currently takes `pzam_bytes` but needs to
  decide trampoline direction from `cs->opt_tier` the same way the
  #0016 fix does. That logic moves from `pzam_run.cpp` into the
  runtime. Keep the existing `jit2 / jit / jit_profile / jit_llvm →
  reverse / forward` mapping visible in one place, next to the
  asserts.
- `jit_func_ranges` population (currently in pzam_run.cpp) is needed
  for signal-handler diagnostics under `PSIZAM_JIT_SIGNAL_DIAGNOSTICS`.
  Move that into `runtime::instantiate` so every executable benefits
  automatically.
- Page-size conversion (the `compile_ps != runtime_ps` block) is
  also a runtime concern — move it inside `load_cached` /
  `instantiate`.

## Pre-audit findings (2026-04-19)

A full exploration pass surfaced the following issues with the plan
as originally written — the *destination* API isn't yet capable of
receiving the migration. These are captured here so the next pass
doesn't rediscover them.

### 1. `runtime::prepare` / `instantiate` are not real yet

`libraries/psizam/src/runtime.cpp`:

- `prepare(wasm_bytes, policy)` stores the bytes and records a
  compile time. It does **not** parse, validate, compile, or build a
  `backend`. Any tool that calls `provide<>` + `instantiate` on a
  prepared WASM module gets an empty host function table by the
  time instantiate runs — fine for the hello_runtime example
  because instantiate re-builds the backend with a std::move'd
  copy of the table, but the whole path assumes one hardcoded
  backend:

  ```cpp
  struct instance_impl {
     using backend_t = backend<std::nullptr_t, interpreter>;
     ...
  };
  ```

  `composition_tests.cpp` runs across `interpreter`, `jit`, `jit2`,
  `jit_llvm` via TEMPLATE_TEST_CASE, so porting it onto this
  instance_impl is a non-starter without type erasure.

- `load_cached(pzam_bytes)` returns `module_handle{std::make_shared<module_handle_impl>()}`
  — a blank module. No pzam parsing, no relocation, no executable
  code allocation. Every piece of pzam-run's ~350-line loader has
  to move into this function before pzam-run can be rewritten.

- `check(wasm)` returns `{}`. `register_library(...)` is a no-op.

- `instance::as<Tag>()` also hardcodes
  `backend<std::nullptr_t, interpreter>`, so even once instantiate
  supports other backends, proxy calls fall back to interp.

### 2. `composition<Host, BackendKind>` duplicates most of `runtime`

`libraries/psizam/include/psizam/composition.hpp` (~400 lines) +
`bridge_executor.hpp` (~633 lines) + `canonical_dispatch.hpp` (~366
lines) implement:

- `composition::add(bytes)` ≡ `runtime::prepare`
- `composition::register_host<T>(mod)` ≡ `runtime::provide<T>(mod, host)`
- `composition::link<T>(consumer, provider)` ≡ `runtime::bind<T>(consumer, provider)`
- `composition::instantiate()` ≡ per-module `runtime::instantiate`

Step 4 of this issue says composition_tests should use `runtime` only,
which means `composition<>` is the one that retires. But the tests are
templated over the backend kind, so `runtime` needs to accept a backend
parameter (policy-driven or templated) *before* the port can happen.

### 3. WASI state is entangled with a hand-rolled trampoline

`wasi_host::memory` is mutated by `wasi_trampoline` / `wasi_trampoline_rev`
before each dispatch. The generic `fast_void_trampoline` in
`host_function_table.hpp` does not do this. Registering WASI through
`provide<wasi_host>` as-is would leave `host->memory` stale. Two
options for the eventual migration:

- (a) Add a detect-and-set-memory step to `fast_void_trampoline` /
  `fast_void_trampoline_rev` via `requires(Cls* h, char* m){ h->memory = m; }`
  so any host with a `memory` field Just Works. Minimal blast
  radius, but puts a WASI-shaped concern in the generic trampoline.
- (b) Change WASI function signatures to take a memory handle
  (`wit::linmem_ptr<T>` or a thin `linear_mem` view) as an explicit
  parameter, matching how canonical-ABI host fns are written.
  Cleaner long-term; ~40 signatures to update.

Recommend (a) for the port, (b) later if the trait becomes awkward.

### 4. Assert coverage is currently single-site

Commit 96cba71 put the trampoline-direction assert in
`backend<>::construct` (both constructors, ~lines 374–395 and
448–473 of `backend.hpp`). `pzam-run`'s equivalent wiring (the
`is_llvm_pzam ? fwd : rev` block around lines 353–380 of
`pzam_run.cpp`) has **no assert** — the fallback
`e.rev_trampoline ? e.rev_trampoline : e.trampoline` will silently
pick the wrong direction if only one is populated. This is exactly
the class of bug that motivated this issue. Any port needs to carry
the assert with it.

### 5. Callers of legacy WASI/table API

`grep` for the old helpers returns four call sites:

- `libraries/psizam/tools/pzam_run.cpp` — `register_wasi(table)` + manual `_host_trampoline_ptrs`
- `libraries/psizam/tools/psizam_wasi.cpp` — `register_wasi(table)` + backend ctor with `host_function_table`
- `libraries/psizam/include/psizam/detail/wasi_host.hpp` — the definitions
- Issue #0018 itself

No other tests/tools/headers use them, so the deletion blast radius
is narrow once those two tools are ported.

### 6. Package / interface naming for WASI

`PSIO_INTERFACE(wasi_snapshot_preview1, ...)` implies a package
declaration. `PSIO_PACKAGE(wasi, "0.1.0");` is a reasonable default
but we should confirm psio's reflection machinery handles the
colon-delimited `wasi_snapshot_preview1` name (it already does for
`psiserve:io.*`-shaped names — check `plans/psix-ipc-design.md` for
conventions before committing to a spelling).

### 7. ~60 functions is actually 42

Current `wasi_host` implements exactly 42 functions (counted from
the `register_wasi` body). The `func(...)` macro in PSIO_INTERFACE
takes a comma-separated name + param list per function, so the
PSIO_INTERFACE block for WASI P1 will be ~42 entries, not ~60.

## Suggested order of attack (post-psizam-runtime-api-maturation)

1. Land psizam-runtime-api-maturation first — brings `runtime::prepare`, `instantiate`,
   `load_cached`, and instance_impl up to the level the tests and
   tools need. Includes backend-kind dispatch, pzam loader migration,
   and the memory-injection hook on the fast trampoline.
2. Add `PSIO_INTERFACE(wasi_snapshot_preview1, ...)` + `PSIO_HOST_MODULE`
   on `wasi_host`. Register WASI through `provide<wasi_host>` in both
   `psizam_wasi.cpp` and `pzam_run.cpp`.
3. Delete `register_wasi`, `add_wasi_func`, `wasi_trampoline`,
   `wasi_trampoline_rev`. Move `wasi_host` out of
   `include/psizam/detail/` and into a public WASI-oriented location
   (`include/psizam/wasi/preview1_host.hpp`?) since external tools
   now instantiate it by name.
4. Port `composition_tests.cpp` onto `runtime::prepare + provide + bind
   + instantiate`. Delete `composition.hpp` and friends if nothing
   else references them.
5. Port `pzam_run.cpp` to the runtime API (~40 lines).
6. Add the 96cba71-style assert at the remaining trampoline-direction
   write sites — expectation: only one, inside `runtime::instantiate`.
7. Design + define psix as `PSIO_INTERFACE`. This is a new interface,
   not a migration — see `plans/psix-ipc-design.md`.
