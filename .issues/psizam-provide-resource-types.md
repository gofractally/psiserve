---
id: psizam-provide-resource-types
title: "runtime::provide<> cannot handle own<T>/borrow<T> resource types"
status: open
priority: high
area: psizam, psiserve
agent: ~
branch: feature/runtime-migration
created: 2026-04-21
depends_on: []
blocks: [psiserve-wit-host-guest-api]
---

## Summary

`runtime::provide<Host>(mod, host)` fails to compile when `Host` has
methods returning `psio::own<T>`, `psio::borrow<T>`, or
`std::expected<own<T>, error>`. This blocks registering `DbHost` (and
any future host with WIT resource types) through the runtime API.

The root cause is that `is_scalar_wasm_type_v` explicitly returns
`false` for `own<T>` and `borrow<T>` (correct — they ARE NOT scalars,
they are resource handles requiring lift/lower). But the non-scalar
`slow_dispatch` path then tries to construct/copy these move-only
types, which fails.

The real fix is not to make them scalar. It's to teach the canonical
lift/lower path how to marshal resource handles across instance
boundaries, the same way it already marshals strings and lists.

## What resource lift/lower means

A `u32` handle index at the WASM boundary is NOT a raw value — it's an
index into a per-instance handle table. Crossing instance boundaries
requires:

### Lift (caller side, on caller's strand)

For `own<T>` argument:
1. Read the u32 handle from the caller's flat args
2. Look up the host object in the caller's handle table
3. **Remove** the entry (ownership transfers — caller loses access)
4. The host object travels to the callee's strand

For `borrow<T>` argument:
1. Read the u32 handle from the caller's flat args
2. Look up the host object in the caller's handle table
3. **Keep** the entry (borrow — caller retains ownership)
4. A reference to the host object travels to the callee's strand

### Lower (callee side, on callee's strand)

For `own<T>` parameter:
1. Receive the host object
2. **Insert** into the callee's handle table → new u32 handle
3. Pass the new handle to the callee's export

For `borrow<T>` parameter:
1. Receive the reference
2. **Insert temporarily** into the callee's handle table → new u32
3. Pass the new handle to the callee's export
4. On call return: **remove** the temporary entry (borrow expires)

### Return values

For `own<T>` return:
1. Callee's export returns a u32
2. **Lift on callee's strand**: extract host object from callee's table
3. Transport to caller's strand
4. **Lower on caller's strand**: insert into caller's table → new u32

For `std::expected<own<T>, error>` return:
1. The flat representation is (discriminant, payload) — same as any
   variant in canonical ABI
2. On success: payload is a u32 handle → lift/lower as above
3. On error: payload is the error enum → pass through as scalar

## Threading invariant

**A fiber never touches two handle tables simultaneously.** Lift always
happens on the strand that owns the source table. Lower always happens
on the strand that owns the target table. The host object travels
between strands via:

- **Fiber migration**: the calling fiber joins the target strand, does
  the lower there, runs the call, lifts the return, migrates back. The
  host object lives on the fiber's stack — no allocation.

- **Lambda capture**: the dispatch lambda captures the host object by
  value (it's typically a pointer or shared_ptr — small). The lambda
  runs on the target strand's fiber. Return travels back via
  fiber_promise.

Both patterns are safe. The bridge executor already uses the lambda
pattern for string/list marshalling across linear memories.

## Where this fits in the existing code

### Canonical ABI marshalling (bridge executor)

`libraries/psizam/include/psizam/bridge_executor.hpp` already has the
pattern for cross-instance marshalling:

- `bridge_program` — a sequence of opcodes that describe how to
  lift/lower each argument and return value
- `compile_bridge<FnPtr>()` — introspects the C++ function signature
  at compile time, emits opcodes for each arg/return
- `execute_bridge_erased()` — runs the program against consumer/provider
  linear memories

Resource lift/lower would add new opcodes to the bridge_program:

```
op::lift_own    — read u32 from flat args, extract host object from
                  source handle table
op::lower_own   — insert host object into target handle table, write
                  u32 to flat args
op::lift_borrow — read u32, lookup (not extract) host object
op::lower_borrow — insert temporary entry, record for cleanup on return
op::cleanup_borrows — remove all temporary borrow entries on return
```

These opcodes are JIT-able (fixed instruction sequences), just like
the existing string/list copy opcodes.

### slow_dispatch in runtime::provide<>

`libraries/psizam/include/psizam/runtime.hpp:421-465` — the
`slow_dispatch` lambda in `register_one_host_method`. Currently handles
void, integral, and float return types. Needs to also handle:

- `psio::own<T>` — return value is a u32 handle, but the host method
  returns a `psio::own<T>` which wraps a u32. The handle is in the
  callee's (host's) handle table. For host→guest calls, the lift
  is trivial (the host created the handle, just return the u32).
  
- `std::expected<own<T>, error>` — result variant. On success, the
  u32 handle. On error, the error enum.

For the host-function path (guest calls host), the handle table is
the host struct's table (e.g. `DbHost::databases`). The host method
already returns the handle index wrapped in `own<T>`. The slow_dispatch
just needs to unwrap it to a u32:

```cpp
if constexpr (psio::detail::is_own_ct<ReturnType>::value) {
   rv = result.release();  // unwrap own<T> → u32, prevent destructor drop
}
```

For `std::expected<own<T>, error>`:
```cpp
if constexpr (is_expected_own<ReturnType>::value) {
   if (result.has_value())
      rv = result->release();
   else
      rv = encode_error(result.error());  // error in high bits or separate slot
}
```

### is_scalar_wasm_type_v

`libraries/psizam/include/psizam/hosted.hpp:52-68` — the
classification function. Currently returns `false` for `own<T>` and
`borrow<T>`, forcing them through the canonical path. This is correct
for cross-instance calls (where handle translation is needed). But for
**host-function calls** (guest → host, same handle table), `own<T>` IS
effectively scalar — the u32 handle is valid in the host's table.

Two options:

1. **Treat own<T>/borrow<T> as scalar for host-function registration
   only** — the fast trampoline passes the u32 through, the host
   method receives `own<T>{handle_u32}` directly. This works because
   the host's handle table IS the table the guest is using.

2. **Keep them non-scalar, fix the canonical path** — teach
   slow_dispatch to unwrap `own<T>` via `.release()` and wrap
   `std::expected<own<T>, error>`.

Option 1 is the immediate fix. Option 2 is needed for cross-instance
resource transfer (where the handle tables differ).

## Concrete steps

### Step 1: Fix slow_dispatch for host-function registration

In `register_one_host_method`, extend the return-type handling to
cover `own<T>`, `borrow<T>`, and `std::expected<own<T>, error>`.
For host-function calls, the handle u32 passes through directly
(same table, no translation needed). The fix is just unwrapping
the C++ type to extract the u32.

### Step 2: Fix lift_canonical_args for own<T>/borrow<T> arguments

The `host_lift_policy` reads flat args. For `own<T>` args, it needs
to read a u32 and construct `own<T>{u32}`. For `borrow<T>`, same
but `borrow<T>{u32}`. This may already work if `canonical_lift_flat`
has specializations for these types.

### Step 3: Cross-instance resource transfer (bridge executor)

Add `op::lift_own`, `op::lower_own`, `op::lift_borrow`,
`op::lower_borrow`, `op::cleanup_borrows` to the bridge_program
opcode set. These operate on InstanceContext handle tables during
cross-instance dispatch. The fiber must be on the correct strand
when touching each table.

### Step 4: Validation

- `runtime::provide<db_host>(mod, host)` compiles and works
- `AppServer::runWorker` registers DbHost through provide<>
- blockchain.wasm runs with psi:db/* imports resolved
- Cross-instance resource transfer (own<database> from blockchain
  to token) works through the bridge

## Files affected

- `libraries/psizam/include/psizam/runtime.hpp` — slow_dispatch
- `libraries/psizam/include/psizam/hosted.hpp` — is_scalar_wasm_type_v
- `libraries/psizam/include/psizam/bridge_executor.hpp` — resource ops
- `libraries/psizam/include/psizam/canonical_abi.hpp` — lift/lower
- `libraries/psiserve/include/psiserve/runtime_host.hpp` — InstanceContext handle tables
- `libraries/psiserve/src/runtime.cpp` — AppServer DbHost registration
