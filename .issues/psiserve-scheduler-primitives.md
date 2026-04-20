---
id: psiserve-scheduler-primitives
title: psiserve scheduler primitives — async dispatch, preempt-lock, root-cause gas
status: ready
priority: high
area: psiserve,psizam
agent: ~
branch: ~
created: 2026-04-20
depends_on: [psizam-gas-metering, psizam-gas-state-redesign, psizam-gas-yield-handler]
blocks: [psiserve-wit-host-guest-api]
---

## Motivation

psiserve needs a general-purpose cooperative-scheduling substrate that lets a
guest WASM author:

- Dispatch work across strands (serialization domains) and instances (stateful
  or stateless), with explicit thread-placement hints.
- Bill compute against a **root cause** (IP, authenticated user) carried
  implicitly through the spawn tree.
- Declare atomic critical regions without relying on host-specific quantum
  sizes, without deadlocking, and without entangling gas with consensus.
- Be preempted for fairness while still letting the runtime bound strand
  monopoly by gas and wall-clock time.

The design sits on top of psizam's gas-state redesign
(`psizam-gas-state-redesign`) and yield handler
(`psizam-gas-yield-handler`). Those provide the VM-level mechanism
(probe-site call, gas deadline, yield). This issue is the policy and guest
ABI built on top of them. Gas remains subjective — consensus-visible bills
are the builder's attested oracle number, clamped by a user-signed
`max_gas`; validators need not meter.

## Design

### 1. `async` dispatch intrinsic

One primitive covers all spawn patterns in psiserve — listeners, per-connection
handlers, context-free verify workers, builder dispatch, etc.

```
async(
  instance : Instance::New(code_hash, strand: This|Any|Fresh)
           | Instance::Existing(instance_id),
  entry    : ExportName,
  args     : bytes,                    // WIT-lowered
  wait     : Wait::Block | Wait::Fire,
) -> Option<Result>
```

- `Instance::Existing` dispatches to the instance's bound strand.
- `Wait::Block` — caller suspends; 1 memcpy across the boundary (caller linear
  memory frozen while callee runs).
- `Wait::Fire` — fire-and-forget; 2 memcpies (args staged through a native
  queue).

Higher-level roles (CF-verify pool, builder, N-listener fanout) are
combinations of these parameters. The runtime knows no domain-specific roles.

### 2. Strand model

- Strand = serialization domain; serializes handler execution on its fibers.
- Long-lived instances are 1:1 with a strand — the strand is that instance's
  implicit mutex.
- Strands migrate across OS threads via work-stealing; fibers are not pinned
  to threads.
- Serialization is **asio-style release-on-await**: fibers release the strand
  on suspension; other fibers on the strand may run. Single-flight
  (hold-across-await) is rejected because it deadlocks on any dispatch cycle
  (caller strand S → callee → anything downstream that needs S).
- Reentrancy discipline is the same one `wasm → host → wasm` already requires:
  invariants must hold at any host-call boundary.

### 3. Root-cause gas attribution

- Every fiber carries `root_cause: RootId`, a 64-bit capability minted by
  `register_root(origin)`. Origin is a string the guest chooses
  (IP + port, cert fingerprint, authenticated user, etc.).
- Children inherit the parent's `RootId` on every `async` spawn; cannot be
  forged from guest code.
- Native maintains `gas_spent[RootId]` and a declared ceiling per root;
  exhaustion traps the current fiber at its next probe.
- Gas is **subjective**: consensus-visible bills are the builder's oracle
  number clamped by a user-signed `max_gas`. Validators may skip metering
  entirely (they still verify outcomes, just not cost).

### 4. Injected probes (non-yielding)

Probes already fire at function entry and loop back-edges (per
`psizam-gas-metering`). Each probe does, in order:

1. Decrement root-cause gas (via the new `gas_state` deadline model from
   `psizam-gas-state-redesign`); trap on underflow.
2. Check fiber cancel flag; unwind if set.
3. Sample `__preempt_lock`:
   - If `0`: clear `lock_held_since`; may invoke scheduler for fairness
     preemption.
   - If `1`: if `lock_held_since == None`, set it to `now()`. Else if
     `now() - lock_held_since > MAX_LOCK_WALL_TIME`, trap.

**Probes never introduce reentry beyond what `wasm → host → wasm` already
permits.** A probe under a held lock does not call the scheduler — it only
updates timers, enforces bounds, and returns.

### 5. `__preempt_lock` — guest memory flag

- A WASM module-level `(global (mut i32))`, exported under the name
  `__preempt_lock`. The host samples it at probe firings and at
  `yield_if_needed`.
- Guest writes `1` to enter an atomic region, `0` to exit. Stores are plain
  WASM; no intrinsic call required.
- Suspending intrinsics (`async<Block>`, blocking I/O, `yield_if_needed`)
  **trap** when the flag is `1`.
- A `1 → 0 → 1` toggle within a single basic block is **invisible** to the
  runtime — the runtime samples only at probes and at `yield_if_needed`.

**Using a module global (not a linear-memory address)** prevents corruption
by stray guest pointer arithmetic and lets the host cache a stable export
index at instantiation time.

### 6. `yield_if_needed()` intrinsic

The only reliable progress signal. Semantics:

1. Trap if `__preempt_lock == 1`.
2. Charge a fixed root-cause gas cost.
3. Check cancel flag; unwind if set.
4. If any fiber is queued on this strand, suspend and let the scheduler
   run it; else fall through.
5. Return.

Injected probes fire at compiler-chosen points the guest can't control.
`yield_if_needed` is the only point the **guest** can choose. This matters
for long critical regions that need chunking (see `checkpoint()` RAII below).

### 7. Wall-clock lock-time bound

- `MAX_LOCK_WALL_TIME` bounds strand monopoly independent of gas budget.
- Starts when a probe first observes `__preempt_lock == 1`; clears when a
  probe observes `0` (which requires the guest to release the lock **and**
  for a probe or `yield_if_needed` to fire afterward — basic-block toggles
  are invisible).
- Trap if exceeded. Recovery: chunk the region with `checkpoint()`.
- Independent of gas: a well-funded caller with large `max_gas` still cannot
  freeze a strand longer than this bound.

### 8. Cancellation

- Per-fiber cancel flag, set by: parent fiber death, root-cause gas
  exhaustion, explicit kill.
- Observed at every probe and at entry to every suspending host call.
- Propagates through the spawn tree: kill the root → every descendant
  unwinds at its next probe.
- **Awaiter sets are refcounted**: two fibers `async<Block>`ing the same
  `(code_hash, args_hash)` share one in-flight task; cancel fires only when
  the awaiter count drops to zero.
- **Completed tasks are always cached**, even if cancelled after completion —
  the work is sunk, storing the result is free.

## Guest-facing ABI

Intrinsics imported from module `psiserve.core`:

```
(func $async            (param i32 i32 i32 i32 i32 i32) (result i32))
(func $yield_if_needed                                     )
(func $register_root    (param i32 i32)              (result i64))
```

Guest-exported global:

```
(global $__preempt_lock (mut i32) (i32.const 0))
(export "__preempt_lock" (global $__preempt_lock))
```

## C++ RAII helpers

New header (location TBD by the implementer; `scheduler.hpp` currently
re-exports `psiber::Scheduler`, so the scheduler primitives likely belong
alongside that — either in `libraries/psiber/include/psiber/` or a new
`psiserve/scheduler_primitives.hpp`):

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <variant>
#include <vector>

namespace psiserve {

// ── Guest-exported flag the host samples at probes ────────────────────
extern "C" {
    extern volatile std::uint32_t __preempt_lock;
}

namespace detail {
    extern "C" void          __psiserve_yield_if_needed() noexcept;
    extern "C" std::uint64_t __psiserve_register_root(
        const char* origin_ptr, std::uint32_t origin_len) noexcept;
    extern "C" std::int32_t  __psiserve_async(
        std::uint32_t instance_ref,
        const char* entry_ptr,        std::uint32_t entry_len,
        const std::uint8_t* args_ptr, std::uint32_t args_len,
        std::uint32_t wait_mode) noexcept;
}

// ── Explicit safepoint ────────────────────────────────────────────────
// Traps if __preempt_lock == 1. The only reliable progress signal.
inline void yield_if_needed() noexcept {
    detail::__psiserve_yield_if_needed();
}

// ── preempt_guard: RAII critical section ──────────────────────────────
//
// While alive:
//   - fairness preemption is suppressed
//   - any suspending intrinsic traps
//   - wall-clock hold time bounded by MAX_LOCK_WALL_TIME; trap if exceeded
//
// For long regions, call checkpoint() between self-contained chunks.
class preempt_guard {
public:
    preempt_guard() noexcept  { __preempt_lock = 1; }
    ~preempt_guard() noexcept { __preempt_lock = 0; }

    preempt_guard(const preempt_guard&)            = delete;
    preempt_guard& operator=(const preempt_guard&) = delete;
    preempt_guard(preempt_guard&&)                 = delete;
    preempt_guard& operator=(preempt_guard&&)      = delete;

    // Release, yield, re-acquire.
    void checkpoint() noexcept {
        __preempt_lock = 0;
        yield_if_needed();
        __preempt_lock = 1;
    }
};

// ── root_scope: establish root-cause attribution for this fiber ───────
class root_scope {
public:
    explicit root_scope(std::string_view origin) noexcept
        : id_(detail::__psiserve_register_root(
              origin.data(), static_cast<std::uint32_t>(origin.size()))) {}

    root_scope(const root_scope&)            = delete;
    root_scope& operator=(const root_scope&) = delete;
    root_scope(root_scope&&)                 = delete;
    root_scope& operator=(root_scope&&)      = delete;

    std::uint64_t id() const noexcept { return id_; }

private:
    std::uint64_t id_;
};

// ── async dispatch ────────────────────────────────────────────────────
enum class wait_mode : std::uint32_t { block = 0, fire = 1 };

struct instance_new {
    std::uint64_t code_hash;
    enum strand_hint : std::uint32_t { this_, any_, fresh_ } hint;
};
struct instance_existing {
    std::uint64_t instance_id;
};
using instance_target = std::variant<instance_new, instance_existing>;

// Block-until-return. Traps if __preempt_lock == 1.
std::vector<std::uint8_t> call(instance_target target,
                               std::string_view entry,
                               std::span<const std::uint8_t> args);

// Fire-and-forget. Safe under preempt_guard (no suspension).
void post(instance_target target,
          std::string_view entry,
          std::span<const std::uint8_t> args);

} // namespace psiserve
```

### Usage

```cpp
// Connection accept — establish root, fire handler
void blockchain::on_connection(sock_handle s) {
    psiserve::root_scope root{describe_origin(s)};
    psiserve::post(psiserve::instance_new{connection_handler_hash(),
                                          psiserve::instance_new::any_},
                   "handle", serialize(s));
}

// Small critical section
void builder::apply_tx(const tx_t& tx) {
    psiserve::preempt_guard g;
    state_.apply(tx);
    block_in_progress_.append(tx);
}

// Long critical section, chunked
void builder::finalize_block() {
    psiserve::preempt_guard g;
    for (auto& tx : block_in_progress_) {
        commit_index(tx);
        g.checkpoint();     // release, yield, re-arm
    }
    seal_header();
}
```

## Host implementation notes

- **Probe hot path**: `gas--; if (gas<0) trap; if (cancel) unwind;
  sample_lock();`. Target ~5 cycles; inline the stub at each probe site.
  Leverage `psizam-gas-state-redesign`'s deadline model so the common
  "non-interrupt" mode keeps the DTD register-pinned fast-path.
- **Lock sampling**: read the exported `__preempt_lock` global by stable
  index; cache index at instantiation.
- **`MAX_LOCK_WALL_TIME`**: default 5 ms, configurable. Emit telemetry
  distinguishing gas-trap, lock-trap, and cancel-unwind.
- **Cancel-during-host-call**: suspending host operations (epoll/kqueue
  wait, thread-pool wait, mailbox dequeue) must register a cancel hook
  so a set cancel flag unparks them promptly.
- **Root-cause ledger**: `unordered_map<RootId, RootState>` with striped
  locking; bias for probe-path reads.
- **Dedup**: task key `(code_hash, blake3(args))`; awaiter refcount in
  task record; result cached on completion regardless of cancellation.

## Acceptance Criteria

- [ ] `async` intrinsic exposed to guests with all four `wait`/`strand-hint`
      combinations reachable from the C++ helpers.
- [ ] `yield_if_needed` intrinsic lowered across all psizam backends
      (interp, jit, jit2, jit_llvm, aarch64).
- [ ] `register_root` intrinsic + per-fiber `RootId` inheritance on spawn.
- [ ] `__preempt_lock` module global recognized and sampled by the probe
      hot path.
- [ ] Probes always perform gas + cancel + lock-time checks in the
      stated order; never invoke the scheduler under a held lock.
- [ ] C++ RAII helpers (`preempt_guard`, `root_scope`, `yield_if_needed`,
      `call`, `post`) compiling for guest WASM and covered by unit tests.
- [ ] **Test — spawn-tree accounting**: N children's gas sums to parent's
      root debit.
- [ ] **Test — cancel propagation**: killing a parent unwinds all
      descendants at their next probe.
- [ ] **Test — preempt-lock atomicity**: two fibers race on shared state;
      one holds `preempt_guard`; the other observes only consistent states.
- [ ] **Test — lock wall-time trap**: fiber holds lock past
      `MAX_LOCK_WALL_TIME` with no `checkpoint()`; traps; strand released.
- [ ] **Test — suspend-under-lock trap**: `yield_if_needed()` /
      `async<Block>` under `preempt_guard` traps cleanly.
- [ ] **Test — awaiter dedup**: two `async<Block>` calls with the same
      `(code_hash, args)` run once; both resume with the same result;
      cancelling one does not cancel the other.
- [ ] **Test — gas-exhaustion under lock**: hostile fiber holds lock,
      burns gas in tight loop; traps on gas before lock-time (either
      trap acceptable; both must release strand).
- [ ] **Test — basic-block toggle is invisible**: `__preempt_lock = 0;
      __preempt_lock = 1;` with no probe or intrinsic between does **not**
      reset `lock_held_since`.
- [ ] **Benchmark**: probe overhead per firing, preempt_guard
      enter/exit cost, `yield_if_needed` cost on each backend.

## Open Questions

1. **Flag storage**: exported module global (proposed) vs. fixed
   linear-memory slot vs. host-managed per-fiber field exposed via load
   intrinsic. Global is cleanest; confirm the WIT toolchain can emit and
   re-import it across components.
2. **`RootId` lifetime**: if the originating connection dies but work
   continues, does the `RootId` stay attached or detach to a default
   root? Proposal: `register_root` returns a handle whose destruction
   implies "no more spawns against this root, but existing children
   finish billing to it."
3. **Strand fairness default**: pure round-robin vs. time-quantum-aware.
4. **Cost of `checkpoint()`**: fixed gas charge per call (current
   proposal) vs. free. Free risks guests calling it in hot loops to
   avoid preemption accounting; fixed is more honest.
5. **Cancellation gas**: how much gas is billed between cancel-flag-set
   and cancel-observed? Proposal: everything up to the observing probe,
   no further.
6. **Instance pool sizing** for `Instance::New`: per `code_hash` LRU vs.
   global LRU. Affects hot-path instantiation cost for stateless
   workers.

## Non-Goals

- Cross-process / cross-node async.
- Shared-memory between fibers beyond what a strand's serialization
  already provides.
- Consensus-visible gas. Gas stays subjective; builder attestation is
  the consensus-visible number.
- Replacing psibase's existing transaction pipeline.

## Notes

- Builds on `psizam-gas-state-redesign`'s `gas_state { consumed, atomic
  deadline }` layout — the probe's gas decrement and deadline-refresh
  logic is already shaped correctly for this design.
- Composes with the existing `psiserve::fiber_lock_policy` (from the
  closed `psitri-fiber-lock-policy`); psitri operations remain
  fiber-yield-aware and don't need separate scheduling treatment.
- The `yield_if_needed` / `preempt_guard` pair subsumes earlier
  "runway-after-yield" proposals that would have made critical-section
  size depend on node quantum settings — rejected because it would
  entangle gas accounting with outcome determinism.
