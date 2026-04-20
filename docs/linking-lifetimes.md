# Linking and Execution Context Lifetimes

Companion to `wasm-managment.md`. Answers the question: *once we have
multi-module linking and cross-account RPC, how long do things live,
and what gets cached?*

The short answer: **linking is once-per-pzam (content-addressed,
chain-global), execution-context wiring is once-per-composition (scope
depends on host mode).** The per-call hot path is two lookups.

---

## Two layers, sharply separated

### 1. Long-lived global cache (chain-scope or server-scope)

Things that are content-addressed and never change for a given hash.
Compile-time artifacts.

| Cache                | Key                                        | Contents                                                     |
| -------------------- | ------------------------------------------ | ------------------------------------------------------------ |
| **Compiled module**  | `(pzam-hash, backend-flags)`               | Native JIT output from the pzam                              |
| **Bridge table**     | `(callee-pzam-hash, interface-hash, u64 action)` | Pre-JIT'd marshaling function specialized for that method    |
| **Name registry**    | `account-name`                             | Current pzam-hash for that account (mutable, version-bumped) |

Keyed on content hash, so `SetCode`-style operations never need to
*invalidate* anything — they just produce a new hash, and callers
naturally miss into the new entries. Old entries age out via LRU.

Bridges live with the callee (one table per callee pzam, indexed by the
u64 action name). Pre-AOT compiled by the callee's toolchain at
pzam-build time: zero codegen on the caller's hot path, no cross-caller
specialization needed because a bridge is only a function of
`(callee, interface, method)` — the caller contributes only a base
pointer at dispatch time.

### 2. Composition / ExecutionContext (scope depends on host mode)

Things that hold runtime state. Lazily populated on first call to a
service, reused for every subsequent call to that service **within the
same composition**, torn down at composition end.

- Per-service `ExecutionContext` (backend instance + linear memory).
- Identity/permission state (who's calling as whom).
- Call trace / gas meter / whatever bookkeeping the host wants.

Linear memory is acquired from a `MemoryPool` at first use, not on
every call. On a recursive chain `A → B → A`, the second `A` call sees
the same linear memory as the first (with any state the outer frame
wrote still intact) — matches psibase's behavior and is usually what
you want for reentrancy semantics.

---

## Scope rule: objective vs subjective

**Objective / blockchain services (deterministic, consensus):**
*composition lifetime = transaction.*

- Created lazily when the tx's first cross-contract call happens.
- Torn down at tx end; all linear memories returned to pool.
- Clean reset semantics — no state leaks between transactions.
- Matches psibase's exact model.

**Subjective / host-side services (native float, max perf, long-lived):**
*composition lifetime = the hosted session (request, fiber, or
service-long).*

- Created at server startup or on first use, reused for the life of
  the session or the server.
- Avoids MemoryPool churn on chatty RPC-style workloads.
- Acceptable because there's no consensus boundary forcing resets.
- Explicit reset available for logical "work unit" boundaries
  (e.g., "drop the composition, start fresh" for a dev-mode reset).

The bridge and module caches are identical in both modes — long-lived
and global. Only the *composition* scoping differs.

---

## Per-call hot path

In steady state, a cross-account call costs:

1. Resolve the typed handle's `(callee, action_u64)`.
2. `composition.contexts.find(callee)` — O(log n) map lookup; hit in
   steady state.
3. Identity push (two pointer stores, see wasm-management.md §permissions).
4. `bridge_table[action].invoke(sender, receiver, caller_arg_ptr)` —
   indirect call into pre-JIT'd marshaling code.
5. Marshaling: direct caller-mem → callee-mem copies per indirect
   field, guarded-page safety.
6. Invoke callee's universal dispatcher with `(sender, receiver,
   action, arg_region)`.
7. Identity pop on return.

No linking, no compilation, no import resolution, no WIT parsing. One
map lookup, one table lookup, one indirect call. Approaches memcpy-limited
throughput.

---

## Invalidation and upgrades

A pzam upgrade (new code for an account) changes the name-registry's
`account-name → pzam-hash` binding. Any held handles carry the old
pzam-hash and will trap on next dispatch (hash mismatch), forcing
callers to re-`.as<T>()`. The bridge cache and module cache are
unaffected — old entries age out via LRU, new pzam hits produce new
cache keys naturally.

Permission revocation: handles minted via `actor.from(other)` re-check
privilege at each dispatch against the current CodeRow flags, so
revocation takes effect immediately without needing to track per-handle
invalidation. (Alternative: clamp privilege at handle-mint time, with
an explicit bump-version escape; slightly faster, slightly less safe.
Default to per-call re-check.)

---

## Comparison to psibase (reference)

| Aspect                | Psibase                                 | Psiserve                                           |
| --------------------- | --------------------------------------- | -------------------------------------------------- |
| Module cache          | `(codeHash, vmOptions)` → backend, LRU   | `(pzam-hash, backend-flags)` → backend, LRU        |
| ExecutionContext      | Per-service per-transaction              | Per-service per-composition                        |
| Composition scope     | Fixed: transaction                       | Objective: tx. Subjective: session-long.           |
| Dispatch model        | Universal dispatcher, opaque u64 action  | Same — universal dispatcher, u64 action            |
| Cross-call marshaling | Fracpack action bytes (serialize/copy)   | Pre-JIT'd bridges (single-copy per indirect field) |
| Identity              | `(sender, receiver)` packed in Action   | Same — carried on typed handle, no stack           |
| Permission check      | At host `call()`, per call               | Same — at host dispatch primitive, per call        |
| Privileged-account set | `isPrivileged` bit on `CodeRow`          | Same — flag on account record                      |

The design heritage from psibase is intentional. The two changes worth
the complexity:

1. **Pre-JIT'd bridges** cut the per-call marshaling cost from
   "serialize + bytewise copy + deserialize" to "direct mem→mem copy
   per indirect field." Big win for chatty inter-service calls.
2. **Subjective-mode long-lived compositions** avoid the per-tx
   MemoryPool churn when there's no consensus reason to reset. Matters
   for server-side hosted workloads; irrelevant for blockchain.
