---
id: psizam-runtime-api-maturation
title: Bring psizam::runtime up to functional parity with composition<> and pzam-run
status: ready
priority: high
area: psizam
agent: ~
branch: ~
created: 2026-04-19
depends_on: []
blocks: [psizam-unify-host-registration-under-runtime]
---

## Summary

Turn `psizam::runtime` from a skeleton into the library's one real WASM
load + compile + instantiate + bind + invoke surface. Today it sits in
front of two mature but separate systems — `backend<Host, Impl>` and
`pzam-run`'s hand-rolled pzam loader — with `composition<Host, BackendKind>`
forming a third nearly-complete surface glued to `backend<>`. This issue
is the prerequisite for `psizam-unify-host-registration-under-runtime`;
without it, "port pzam-run to the runtime API" and "port
composition_tests.cpp to the runtime API" are hollow instructions.

The DX has been designed and approved in conversation. The spec below
captures the full configuration surface, the typed-int + bitflag
primitives it composes from, the compile/instance/runtime scope split,
and the cache-key / .pzam container contract. Implementation work is
split into a **gas-independent** track that can land immediately and a
**gas-dependent** carve-out that waits on `psizam-gas-state-redesign`
Phase B.

## Progress

### Completed (2026-04-19 / 2026-04-20)

- **Foundations**
  - `psizam/runtime_limits.hpp` typed-int aliases (`wasm_pages`,
    `stack_bytes`, `call_depth`, `table_entries`, `ms_duration`,
    `us_duration`, `host_bytes`, `host_gb`) + UDLs. Conditional `ucc`
    link in `libraries/psizam/CMakeLists.txt`. Verified compile +
    cross-tag rejection. — commit `e9b9a12`
- **Step 1 — backend-kind dispatch in `instance_impl`**
  - New header `psizam/detail/instance_be.hpp`: abstract base +
    `backend_kind` enum + `make_instance_be()` factory declaration.
  - Per-backend `detail::instance_be_impl<Impl>` defined in
    `runtime.cpp` for interpreter / jit / jit2 / jit_llvm. Factory
    gates jit/jit2 on `__x86_64__`/`__aarch64__` and jit_llvm on
    `PSIZAM_ENABLE_LLVM_BACKEND` with clear runtime-rejected
    diagnostics for unbuilt backends.
  - `instance_impl` now owns `unique_ptr<instance_be>`;
    `linear_memory()`, `memory_size()`, `backend_ptr()` route through
    the abstract interface.
  - `runtime::instantiate` maps `instance_policy::compile_tier` →
    `detail::backend_kind` via `tier_to_backend_kind()` helper.
  - `examples/runtime_resource` updated to explicitly request
    `compile_tier::interpret` (its direct `backend_ptr()` cast
    relies on Step 10 for cross-backend support).
  — commit `41c7153`
- **Step 5 — `instance::run_start()` convenience**
  - Added `int run_start(void*)` virtual to `instance_be`; per-backend
    impl runs `module.start` (if `!= UINT32_MAX`) then resolves and
    calls `_start`; catches `wasi_host::wasi_exit_exception` and
    returns its code; returns 0 on normal exit.
  - Public `int instance::run_start()` declared in `runtime.hpp`,
    forwards to the impl with the stored `host_ptr`.
  — commit `5edf26f`

### In progress

- **Step 3 — `runtime::load_cached` absorbs `pzam_run` loader**
  (next; ~350 lines from `tools/pzam_run.cpp` 87–344 moves into
  `runtime::load_cached`).

### Blocked / deferred

- **`hello_runtime` / `psizam_runtime_resource` examples** — fail to
  build at HEAD due to the pre-existing `gas_handler_t` redefinition
  between `gas.hpp` (`void(*)(void*)`) and `runtime.hpp`
  (`void(*)(gas_state*, void*)`). This is the exact collision
  `psizam-gas-state-redesign` exists to resolve. Verified pre-existing
  on `origin/main`, not introduced by this work. Track A code is
  syntactically valid and would build cleanly once Phase B of that
  issue lands.
- **Track B** (gas-dependent): Step 2 (prepare with injector),
  Step 4 (instantiate completes with gas_state population),
  cache-key / `.pzam` v2 — carve out into follow-up
  `psizam-runtime-gas-integration` once `psizam-gas-state-redesign`
  Phase B ships.
- **Step 10** (`instance::as<Tag>` proxy across backend kinds) —
  awaits the call-erasure extension to `instance_be`. Until then,
  `as<Tag>()` keeps the interpreter-hardcoded cast; callers that need
  jit/jit2/jit_llvm must use `backend_ptr()` directly with knowledge
  of the kind.

### Verified clean

- `composition_tests` and `pzam-run` build successfully against the
  Step-1 + Step-5 changes (no regressions).
- `runtime_limits.hpp` compiles standalone and produces clear
  diagnostics for cross-tag arithmetic.
- All commits on `origin/main`.

## Approved DX spec

### Three configuration scopes

Every dial in the runtime lives in exactly one of three structs, scoped
by lifetime:

| Scope | Lifetime | Examples |
|---|---|---|
| `runtime_config` | Once at `runtime` construction | arena size, signal-handler install, JIT code cache size, gas-pool registry sizing |
| `compile_policy` | Baked at `prepare()` / `load_cached()`; immutable on `module_handle` | tier, float mode, stack-limit kind, `meter_cap`, `check_spacing`, gas schedule, mem safety |
| `instance_policy` | Per `instantiate()` | per-instance `meter_cap` subset, gas budget, wall/runtime budgets, gas-pool handle, max pages, trust |

**Host-owned state** (WASI args/env/preopens, host counters, etc.) lives
on the `Host` type registered via `provide<>`, not on any policy struct.

**No callbacks anywhere in the public config.** Behavior selection is
fully declarative — gas/wall/abort handlers are runtime-internal and
keyed off `meter_cap` bits + policy state. This keeps the API
WASM-callable: a psiserve module orchestrating child instances passes
plain handles and bitflags, never function pointers.

### Typed primitives

Every numeric quantity flowing through the policy surface is a
`ucc::typed_int<T, Tag>`, so the compiler rejects mixing pages with
gas units, ms with µs, etc.

| Primitive | Header | Status |
|---|---|---|
| `meter_cap`, `codegen_shape`, `meter_presets::*`, `compatible()`, `missing()`, `unused()`, `shape_for()`, `gas_costs` | `psizam/gas.hpp` | **landed** (origin `f1cc307`) |
| `gas_units` | `psizam/gas.hpp` | lands in `psizam-gas-state-redesign` Phase B |
| `wasm_pages`, `stack_bytes`, `call_depth`, `table_entries`, `ms_duration`, `us_duration`, `host_bytes`, `host_gb` + UDLs | `psizam/runtime_limits.hpp` | **landed** (this branch, `e9b9a12`) |

### `compile_policy` (baked into `module_handle`)

```cpp
struct compile_policy {

   struct tier_config {
      enum class level { interpret, jit1, jit2, jit_llvm };
      level initial          = level::jit1;
      level optimized        = level::jit_llvm;
      bool  tier_up_allowed  = true;
   } tier;

   struct float_config {
      enum class mode { soft, native_canonical, native_fast };
      mode value = mode::soft;
   } floats;

   struct stack_config {
      enum class kind { call_count, bytes };
      kind        limit_kind    = kind::bytes;
      stack_bytes compile_limit = 64_kb;   // upper bound baked into prologue
   } stack;

   struct metering_config {
      // Capability bits the injector + codegen must support.
      // Runtime gas_policy must be compatible(caps, runtime.caps).
      meter_cap            caps          = meter_presets::trap;
      // Spacing between baked check sites (frequency, static).
      // Runtime cadence (yield_slice, poll_slice) must be a multiple.
      gas_units            check_spacing = gas_units{1'000};
      // Gas-cost model. nullptr only valid when caps == meter_cap::none.
      const gas_schedule*  schedule      = &gas_schedules::standard_v1;
   } metering;

   struct memory_config {
      enum class safety { guarded, checked, unchecked };
      safety     kind            = safety::checked;
      wasm_pages max_pages_hint  = 256_pages;
      // NOTE: `guarded` enables signal-based abort when
      //       runtime_config.signals.install_sigsegv_handler is on.
   } memory;

   struct compile_meta {
      enum class trust { native, sandboxed };
      trust       compiler_trust       = trust::native;
      ms_duration compile_timeout_ms   = 5'000_ms;
      bool        capture_wit_sections = true;
   } compile;
};
```

### `instance_policy` (per `instantiate()`)

```cpp
struct instance_policy {

   struct memory_config {
      wasm_pages initial_pages = 0_pages;   // 0 = honor module's min
      wasm_pages max_pages     = 0_pages;   // 0 = honor compile_policy hint
   } memory;

   struct stack_config {
      // Units determined by compile_policy::stack.limit_kind.
      // stack_bytes when bytes; call_depth when call_count.
      uint32_t limit_value = 64u * 1024u;
   } stack;

   struct gas_config {
      // Subset of compile-time meter_cap; must satisfy
      //   compatible(compile.metering.caps, gas.caps).
      meter_cap                 caps = meter_presets::trap;

      // Fields populated conditionally by which caps bits are set.
      gas_units                 budget          = gas_units{1'000'000};
      ms_duration               wall_budget     = 0_ms;     // 0 = no wall ceiling
      ms_duration               runtime_budget  = 0_ms;     // 0 = no on-CPU ceiling
      gas_units                 yield_slice     = gas_units{0};   // 0 = no yield
      gas_units                 poll_slice      = gas_units{10'000};
      gas_pool_handle           pool            = {};       // empty = private counter
   } gas;

   struct trust_config {
      enum class level { untrusted, trusted };
      level value = level::untrusted;
   } trust;

   struct fork_config {                   // Layer 3+ COW
      const instance* snapshot_from = nullptr;
   } fork;
};
```

### `runtime_config` (set at construction)

```cpp
struct runtime_config {

   struct memory_config {
      host_gb     arena_size         = 256_gb;   // checked / unchecked
      std::size_t guarded_pool_size  = 64;       // # guarded sandboxes
      host_bytes  jit_code_cache_max = 512_mb;
   } memory;

   struct signals_config {
      // SIGSEGV → guarded-memory abort path; OOB traps.
      bool install_sigsegv_handler  = true;
      // SIGALRM → wall deadlines without gas polls; cross-thread abort.
      bool install_sigalrm_handler  = false;     // off: shares process timer
      // SIGILL → JIT gas-trap pattern (ud2 / brk).
      bool install_sigill_handler   = true;
      bool jit_signal_diagnostics   = false;
   } signals;

   struct cache_config {
      std::size_t max_cached_modules     = 1024;
      host_bytes  max_cached_library_max = 128_mb;
   } cache;

   // Gas dispatch is *internal*. No callback fields; behavior is keyed
   // off meter_cap bits + per-instance gas_config state.
};
```

### Compatibility rules

#### Compile ↔ instance (gas)

```cpp
// At runtime::instantiate(tmpl, policy):
auto miss = missing(tmpl.compile_policy().metering.caps,
                    policy.gas.caps);
if (any(miss))
   throw policy_mismatch{"module compiled without", miss};

// Plus: policy.gas.yield_slice and poll_slice must be positive
// integer multiples of tmpl.compile_policy().metering.check_spacing.
```

#### Bind compatibility (consumer ↔ provider)

| Field | Rule | On mismatch |
|---|---|---|
| `floats.mode` | Must match exactly | reject |
| `stack.limit_kind` | Must match | reject |
| `metering.caps` (intersection) | Compatible (subset rule) | reject if either declares modes the other can't speak |
| `trust.value` | provider ≥ consumer | reject unless `allow_trust_downgrade` |
| `gas.pool` | OK to share or not | tracked, never rejected |
| `tier.level` | Independent | allow |
| `memory.kind` | Independent per instance | allow |

#### Interruption capability (derived, exposed on `module_handle`)

Computed at `prepare()` from `(runtime_config.signals,
compile_policy.metering.caps, compile_policy.memory.kind)`:

```cpp
struct interruption_caps {
   bool gas_ceiling_enforceable;      // has gas_budget bit
   bool wall_deadline_enforceable;    // has wall_budget OR SIGALRM
   bool runtime_deadline_enforceable; // has gas_budget bit
   enum class abort_latency {
      prompt,        // ~check_spacing instructions or memory-access
      bounded,       // next frequent memory write
      cooperative,   // only at import boundaries
   } abort;
};
```

`unchecked + meter_cap::none + no SIGALRM` is a legal but
**uninterruptible** combination. Documented in the header; not rejected.

### Cache-key composition

Every field that influences emitted code or artifact layout feeds the
compile-policy hash. Two modules with the same WASM but different
compile policies are two cache entries.

```cpp
struct module_cache_key {
   uint64_t wasm_hash;     // xxh3(raw wasm bytes)
   uint64_t compile_hash;  // xxh3(canonical compile_policy encoding)
   uint64_t env_hash;      // arch | codegen_version | page_size

   uint64_t digest() const;
   std::string hex() const;       // for log lines / .pzam filenames
};
```

`compile_hash` inputs:

| Field | Reason |
|---|---|
| `tier.initial` | different backends emit different code |
| `floats.value` | FP codegen differs per mode |
| `stack.limit_kind` + `compile_limit` | prologue shape |
| `metering.caps` | check-site emission |
| `metering.check_spacing` | frequency baked into JIT prologues |
| `metering.schedule->identity()` | gas costs baked into decrement constants |
| `memory.kind` | bounds-check codegen |
| `memory.max_pages_hint` | bounds-check elision |
| `compile.compiler_trust` | sandboxed vs native compiler |
| `compile.capture_wit_sections` | whether WIT blobs stored |

`env_hash` inputs: host arch (x86_64 / aarch64), JIT codegen version
tag (bumped when codegen changes), runtime page size.

NOT in the key: any `instance_policy` field, `runtime_config::signals`
choices, arena sizing.

### `.pzam` container v2

`pzam_compile_state` (currently `{arch, opt_tier, stack_limit_mode,
page_size, softfloat}`) extends to:

```cpp
struct pzam_compile_state_v2 {
   uint32_t format_version;       // bump 1 → 2
   uint32_t arch;
   uint32_t tier;                 // was opt_tier
   uint32_t stack_limit_kind;     // call_count | bytes
   uint32_t page_size;
   uint32_t float_mode;           // was softfloat bool; now enum
   uint16_t meter_caps;           // bitmask (uint16_t per gas.hpp)
   uint64_t check_spacing;
   uint64_t gas_schedule_identity;
   uint32_t memory_kind;
   uint32_t compile_hash_lo;
   uint32_t compile_hash_hi;
   uint32_t wit_sections_present;
};
```

`load_cached()` validates every field. format_version < 2 ⇒ reject.
`compile_hash` mismatch ⇒ specific error: `"pzam compiled with hash
0xABCD…, current request hashes to 0x1234…; re-prepare or load matching
artifact"`.

### Status queries / mutability on `instance`

```cpp
class instance {
public:
   // Monotonic queries
   gas_units    consumed_gas()    const;
   ms_duration  wall_elapsed()    const;
   ms_duration  runtime_elapsed() const;   // wall − yielded
   ms_duration  yielded_time()    const;

   // Remaining capacity (nullopt when policy has no such ceiling)
   std::optional<gas_units>   remaining_gas()     const;
   std::optional<ms_duration> remaining_wall()    const;
   std::optional<ms_duration> remaining_runtime() const;

   struct snapshot { /* all of the above + caps + aborted + yielded */ };
   snapshot snap() const;

   // Adjustments — wrong-policy calls throw policy_mismatch.
   void add_gas            (gas_units   delta);
   void set_gas_budget     (gas_units   absolute);   // must be >= consumed
   void extend_wall        (ms_duration delta);
   void set_wall_budget    (ms_duration absolute);   // must be >= elapsed
   void extend_runtime     (ms_duration delta);
   void set_runtime_budget (ms_duration absolute);

   // Abort — sets latched flag; instance traps at next safe point.
   enum class abort_reason : uint32_t {
      host_requested, gas_exhausted, wall_timeout, runtime_timeout,
      resource_limit, fiber_cancelled, wasm_trap, unresolved_import,
      unknown,
   };
   void abort(abort_reason r = abort_reason::host_requested);
};
```

Pool API on `runtime` (handle-based, WASM-safe):

```cpp
gas_pool_handle create_gas_pool(gas_pool_config cfg);
void            destroy_gas_pool(gas_pool_handle);
gas_units       pool_consumed (gas_pool_handle) const;
gas_units       pool_remaining(gas_pool_handle) const;
void            add_gas       (gas_pool_handle, gas_units delta);
void            set_gas_budget(gas_pool_handle, gas_units absolute);
```

### DX — what user code looks like

```cpp
using namespace psizam;
using namespace psizam::literals;

runtime rt{{
   .memory  = { .arena_size = 32_gb },
   .signals = { .jit_signal_diagnostics = true },
}};

auto pool = rt.create_gas_pool({.budget = gas_units{10'000'000}});

compile_policy cp{
   .tier     = { .initial = compile_policy::tier_config::level::jit1 },
   .floats   = { .value   = compile_policy::float_config::mode::soft },
   .metering = { .caps    = meter_presets::trap,
                 .check_spacing = gas_units{1'000} },
   .memory   = { .kind    = compile_policy::memory_config::safety::guarded,
                 .max_pages_hint = 512_pages },
};

auto tmpl = rt.prepare(wasm_bytes{w}, cp);
rt.provide(tmpl, host);
rt.bind<clock_api>(tmpl, clock_inst);   // bind compat checked here

for (auto& req : incoming) {
   auto inst = rt.instantiate(tmpl, {
      .memory = { .max_pages = 64_pages },
      .gas    = { .caps   = meter_presets::trap,
                  .budget = gas_units{1'000'000},
                  .pool   = pool },
      .trust  = { .value = instance_policy::trust_config::level::untrusted },
   });
   inst.as<greeter>().handle(req);
}
```

## Work plan — split by gas dependency

### Track A — gas-independent (this issue)

These steps land against the current `runtime.hpp` skeleton without
touching gas wiring. They unlock the parallel runtime API surface for
everything except metering, and prepare the ground for Track B.

#### Step 1 — backend-kind dispatch in `instance_impl`

Replace hardcoded `using backend_t = backend<std::nullptr_t,
interpreter>` with an abstract base `instance_be` exposing
`raw_backend()`, `linear_memory()`, `run_start()`, and a non-template
`call_with_return_erased()`. Per-backend `instance_be_impl<Impl>`
holds the typed `backend<>`. `instance::as<Tag>` dispatches on the
stored kind via the abstract interface (Step 10 finishes the
typed-proxy plumbing for non-interpreter backends).

#### Step 3 — `runtime::load_cached` absorbs `pzam_run` loader

Move the ~350 lines of `tools/pzam_run.cpp` (lines 87–344) covering
pzam container parse + per-arch code section pick + symbol-table
build + pzam-relocation-to-runtime-relocation conversion + aarch64
veneer generation + executable-memory allocation + RX flip + icache
flush + module.code population + element-segment code_ptr fixup +
trampoline-direction selection (single source: `cs->opt_tier`,
asserted) into `runtime::load_cached`. The 96cba71-style assert
lives at the single new write site for `_host_trampoline_ptrs`.

`compile_page_size`, `max_stack`, `stack_limit_is_bytes` are stored on
`module_handle_impl` for `instantiate` to use.

#### Step 5 — `instance::run_start()` convenience

Run module.start (if present), then resolve and call `_start`. Catch
`wasi_exit_exception` → return code; catch `psizam::exception` → per-
policy handling. `pzam-run`'s `main` becomes `return inst.run_start()`.

#### Step 6 — `runtime::bind` dynamic (WIT-driven) variant

Implement `bind(consumer_mod, provider, iface_name)` by reading the
`component-wit` custom section, locating the named interface, and
generating bridge entries from WIT-declared methods. Multi-language
composition (Rust consumer, MoonBit provider) is the motivating use
case.

#### Step 7 — `register_library` / `cache_stats` / `evict` / `clear_cache`

- `register_library(name, archive_bytes)` — parse `.a`, cache member
  `.o` files under a named registry.
- `register_library(name, wasm_bytes)` — cache standalone `.wasm`
  library.
- `cache_stats` — real counts.
- `evict(wasm_bytes)` and a new `evict(module_cache_key)` overload.
- `clear_cache()` — drop everything.

Library cache entries key on `{archive_hash, compile_hash}` so the
same library compiled under two policies is two entries.

#### Step 8 — Memory-injection hook on the fast trampoline

In `host_function_table.hpp`:

```cpp
template<auto Func, typename Cls, typename R, typename Args>
native_value fast_void_trampoline(void* host, native_value* args, char* memory) {
   if constexpr (requires(Cls* h) { h->memory = memory; })
      static_cast<Cls*>(host)->memory = memory;
   return fast_trampoline_fwd_impl<Func, Cls, R, Args>(...);
}
```

Same for `_rev`. Once landed, `wasi_trampoline` / `wasi_trampoline_rev`
in `wasi_host.hpp` are redundant — they exist only to mutate
`host->memory`. WASI then registers through `provide<wasi_host>` with
no special case (downstream issue
`psizam-unify-host-registration-under-runtime`).

#### Step 9 — Decide fate of `composition<>` and siblings

After Steps 1–8, `composition<Host, BackendKind>` is structurally
redundant with `runtime::prepare + provide + bind + instantiate`.
Recommended: **retire `composition.hpp`** publicly, **rename
`bridge_executor.hpp` to `detail/bridge_executor.hpp`** (keep its
canonical-ABI lift/lower + 16-wide dispatch as runtime-internal
plumbing), **delete `canonical_dispatch.hpp`** if nothing else
references it after the move. Migrate `composition_tests.cpp` callers
to the runtime API as part of `runtime_parity_tests.cpp`.

#### Step 10 — `instance::as<Tag>` proxy across backend kinds

Today the proxy adapter calls `backend_t::call_with_return(...)` with
`backend_t` hardcoded to interpreter. Replace with a non-template
`call_with_return_erased` on `instance_be` that the variant backends
implement. Step 1's abstract base provides the seam.

### Track B — gas-dependent (carved out, follow-up issue)

The steps below need `gas_state`, `gas_handler_t` single-sourced in
`gas.hpp`, and the per-mode codegen shapes from
`psizam-gas-state-redesign` Phase B. Carve to a follow-up issue
`psizam-runtime-gas-integration` once Phase B lands.

#### Step 2 — `runtime::prepare` actually compiles

Parse + typecheck + validate, then invoke the backend compiler
selected by `compile_policy.tier.initial`, applying the gas injector
with `compile_policy.metering.caps` + `check_spacing` +
`schedule`. Populate real `compile_time_ms`, `native_code_size`,
`wasm_size`. Wire `wit_sections()` to the `component-wit` custom
section. Compute `interruption_caps` and attach to `module_handle`.
Compute `module_cache_key`; store in the runtime's cache; return
existing entry on re-prepare with same key.

Failure modes translate to `psizam::exception` subclasses with the
stage recorded (`compile_exception`, `validate_exception`, etc.).

#### Step 4 — `runtime::instantiate` completes

For prepare-path modules: construct the selected backend with
`mod->wasm_copy`, `std::move(mod->table)`, `mod->host_ptr`, fresh
`wasm_allocator` — parameterized by tier from policy.

For load_cached-path modules: construct `jit_execution_context<>`
over the restored module with fresh `wasm_allocator`. Apply page-
size conversion if `compile_page_size != runtime_page_size`.
Populate `_host_trampoline_ptrs` from `host_function_table` using the
single stored `reverse_host_args` flag, with the 96cba71-style assert.
Populate `jit_func_ranges` + `jit_func_range_count` if
`PSIZAM_JIT_SIGNAL_DIAGNOSTICS` is defined.

Run the compile/instance gas-cap compatibility check
(`compatible(tmpl.compile_policy().metering.caps, policy.gas.caps)`)
and the `check_spacing` invariant (`yield_slice % check_spacing == 0`).

Initialize `gas_state` from `instance_policy::gas_config` (private
counter or shared pool handle), wire the appropriate handler closure
based on the runtime caps subset.

Return an `instance` wrapping the constructed context.

## Acceptance criteria

### Track A

- [ ] `instance::as<Tag>()` works regardless of backend kind
      (interpreter / jit / jit2 / jit_llvm).
- [ ] `runtime::load_cached(pzam)` fully loads a `.pzam` file,
      including aarch64 veneers, page-size conversion, and
      trampoline-direction selection (derived from `cs->opt_tier`
      once, asserted, stored).
- [ ] `instance::run_start()` runs start + `_start` and returns the
      WASI exit code.
- [ ] `runtime::bind(consumer, provider, name)` (dynamic) resolves
      from the consumer's `component-wit` section.
- [ ] `fast_void_trampoline` / `fast_void_trampoline_rev` inject
      `host->memory` when the host class has that field.
- [ ] The 96cba71-style trampoline-direction assert is at exactly one
      site (the single `_host_trampoline_ptrs` writer in `load_cached`
      / `instantiate`).
- [ ] `composition.hpp` deleted (or moved to `detail/`); migrated
      tests pass against the runtime API.
- [ ] `register_library` / `cache_stats` / key-based `evict` /
      `clear_cache` work end-to-end.
- [ ] `module_handle::cache_key()` returns a stable key for a given
      `(wasm, compile_policy)` pair.
- [ ] `runtime_parity_tests.cpp` exercises every scenario
      `composition_tests.cpp` covers today, against
      interpreter / jit / jit2 / jit_llvm (gas-policy-free subset).
- [ ] `hello_runtime` example continues to pass without changes to its
      usage pattern.

### Track B (follow-up `psizam-runtime-gas-integration`)

- [ ] `runtime::prepare(wasm, cp)` compiles per `cp.tier.initial`
      with the gas injector wired to `cp.metering.{caps,check_spacing,schedule}`.
- [ ] `native_code_size()`, `compile_time_ms()`, `wasm_size()` all
      return real values.
- [ ] `runtime::instantiate` works for all four backends and
      pzam-loaded modules.
- [ ] Compile/instance gas-cap compatibility checked at
      `instantiate()` with specific error messages naming the missing
      bits.
- [ ] `instance::add_gas` / `set_gas_budget` / `extend_wall` /
      `set_wall_budget` / `extend_runtime` / `set_runtime_budget` /
      `abort()` / `consumed_gas()` / `remaining_*()` / `snap()` all
      function per the spec.
- [ ] Pool API (`create_gas_pool`, `add_gas(pool, ...)`, etc.)
      end-to-end.
- [ ] `.pzam` container v2 with full compile_hash; load_cached
      rejects v1 and policy-hash mismatches.
- [ ] `runtime_parity_tests.cpp` exercises gas-policy variants across
      all backends.

## Non-goals

- Porting `pzam_run.cpp`, `psizam_wasi.cpp`, or `composition_tests.cpp`
  onto the runtime API — those are
  `psizam-unify-host-registration-under-runtime` Steps 3/4.
- Defining WASI Preview 1 as `PSIO_INTERFACE` — downstream issue
  Step 1.
- Designing / implementing psix as `PSIO_INTERFACE` — downstream
  issue Step 2.
- Deleting the legacy WASI helpers — downstream issue Step 5.
- Implementing `gas_state`, `gas_handler_t` single-sourcing, or
  per-mode codegen — `psizam-gas-state-redesign`.

## Files that will be touched

### Landed (this branch)

- `libraries/psizam/include/psizam/runtime_limits.hpp` — typed-int
  aliases for runtime bounds (commit `e9b9a12`).
- `libraries/psizam/CMakeLists.txt` — conditional `ucc` link
  (commit `e9b9a12`).

### New (this issue, Track A)

- `libraries/psizam/include/psizam/detail/instance_be.hpp` — abstract
  backend holder + per-backend impls (Step 1 / Step 10).
- `libraries/psizam/tests/runtime_parity_tests.cpp` — backend-
  parametrized runtime tests (Track A subset).

### Modified (this issue, Track A)

- `libraries/psizam/src/runtime.cpp` — grows substantially (was
  ~185 lines, will be ~600–800 in Track A; ~1000–1200 with Track B).
- `libraries/psizam/include/psizam/runtime.hpp` — `instance::as<Tag>`
  becomes backend-kind-agnostic; add `run_start()`, status queries,
  abort surface; drop the duplicate stub `gas_state` and
  `gas_handler_t` (defer the gas wiring to Track B); add
  `compile_policy`, `instance_policy::{memory,stack,trust,fork}`,
  `runtime_config`, `gas_pool_handle`.
- `libraries/psizam/include/psizam/host_function_table.hpp` — fast
  trampoline memory-injection hook (Step 8).

### Modified (Track A, possibly)

- `libraries/psizam/include/psizam/composition.hpp` — retire / move
  to `detail/` (Step 9).
- `libraries/psizam/include/psizam/bridge_executor.hpp` — move to
  `detail/` (Step 9).
- `libraries/psizam/include/psizam/canonical_dispatch.hpp` — possibly
  delete (Step 9).

### New (Track B follow-up)

- `libraries/psizam/include/psizam/cache_key.hpp` — `module_cache_key`
  + canonical compile-policy encoder.
- `.pzam` container v2 layout (in existing pzam header).
- `libraries/psizam/include/psizam/interruption_caps.hpp` — derived
  capability struct exposed on `module_handle`.

## Background reading / context

- `libraries/psizam/include/psizam/gas.hpp` — landed `meter_cap` /
  `codegen_shape` / `meter_presets` / `gas_costs` (canonical source).
- `libraries/psizam/include/psizam/runtime_limits.hpp` — landed
  typed-int aliases (this branch).
- `libraries/psizam/include/psizam/runtime.hpp` — public surface
  (still skeletal); template bodies for `provide<>` / `bind<>`.
- `libraries/psizam/src/runtime.cpp` — stub implementations to grow.
- `libraries/psizam/include/psizam/composition.hpp` — most complete
  parallel implementation; reference for what `runtime::prepare +
  instantiate + bind` needs to cover.
- `libraries/psizam/include/psizam/bridge_executor.hpp` — canonical
  ABI lift/lower, 16-wide dispatch. Runtime's `bind<>` already uses
  these primitives indirectly.
- `libraries/psizam/tools/pzam_run.cpp` — the 530-line hand-rolled
  loader to be absorbed into `load_cached + instantiate`.
- `libraries/psizam/include/psizam/backend.hpp` lines 340–475 — the
  trampoline-direction selection + asserts (commit `96cba71`) that
  `runtime::instantiate` / `load_cached` need to inherit as the single
  site.
- `libraries/psizam/examples/runtime/hello_runtime.cpp` — minimal
  happy-path usage; should continue to work.
- `libraries/psizam/tests/composition_tests.cpp` — the template-
  parameterized test suite that `runtime_parity_tests.cpp` must
  reproduce through the `runtime` API.
- `.issues/psizam-gas-state-redesign.md` — Phase B unblocks Track B
  of this issue.
- `.issues/psizam-unify-host-registration-under-runtime.md` —
  downstream issue this one unblocks; read its "Pre-audit findings"
  section for the full map of gaps this issue closes.
- Commit `f1cc307` — landed `meter_cap` bitflags + `codegen_shape` +
  `meter_presets`. Canonical source for the metering capability bits.
- Commit `e9b9a12` — landed `runtime_limits.hpp` typed-int aliases.
- Commit `3b344cd` — the "runtime Phase 2" commit that introduced the
  current skeletal surface. This issue is roughly Phase 3.
- Commit `960c425` — the `psizam-llvm-aot-runtime-crash` fix that put
  trampoline-direction selection into `pzam_run.cpp`. That code moves
  into `runtime::load_cached` (Step 3).
- Commit `96cba71` — the asserts. The single trampoline-write site
  after this issue must carry them.

## Decisions locked in (during DX design)

These were resolved in the design conversation; recording so reviewers
don't relitigate:

- **Three-scope split** (runtime / compile / instance), not flat or
  two-scope.
- **Nested config structs**, not flat fields. Aliases published in
  `runtime.hpp` so call sites can write `tier::jit1` rather than the
  full path.
- **Hard-reject** on wrong-policy adjust calls (e.g. `add_gas` when
  policy has `meter_cap::none`) — silent no-ops hide bugs.
- **WASI / argv / preopens** live on the `Host` type, not on any
  policy struct. Implication for downstream issue.
- **`abort_reason` is a closed enum** for now. Extensible variant
  considered if psiserve later needs domain-specific reasons.
- **Default `compile_policy::metering.caps = meter_presets::trap`** —
  matches the most common untrusted case. Users opt into broader
  shapes (`both_poll`) explicitly when they need policy flexibility.
- **`expose interruption_caps` as a public query** on `module_handle`.
- **`evict` gains a `module_cache_key` overload** alongside the
  `wasm_bytes` one.
- **Gas pool ownership: runtime-owned, opaque handle.** No
  `shared_ptr<gas_state>` exposed to callers — keeps the API
  WASM-callable.
- **No callbacks anywhere in public config.** Behavior selection is
  declarative; gas/abort handlers are runtime-internal.
- **LLVM tier rejected at runtime** with clear message if
  `PSIZAM_ENABLE_LLVM_BACKEND` is OFF, rather than compile-gated.
- **`runtime_budget` is *not* its own `meter_cap` bit.** The wall vs.
  on-CPU distinction lives in `instance_policy::gas` runtime data,
  not in the cap-bit ABI. Keeps the bitflag surface stable post-land.
- **Gas schedule is immutable once constructed**, identity computed in
  the constructor. Schedules are versioned by symbol
  (`standard_v1`, future `_v2`), not by mutation.
