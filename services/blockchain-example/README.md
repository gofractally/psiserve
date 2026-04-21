# Example Blockchain Service Design

The purpose of this file is to demonstrate the shape of a multi-threaded blockchain
architecture and verify that the underlying host API of psiserve does what we need it
to do. The core idea is that psiserve is a WASM runtime that allows applications to
build and run in a deterministic manner, in a way that can dynamically update, and
that can run untrusted code, while also providing potentially strong security
guarantees for network services.

The host needs to provide some general services to each WASM it runs, and at the
psiserve level the owner of the machine configures it with "trusted" `.wasm` modules
to begin with.

---

## General Data Flow

1. Psiserve starts, reads a configuration file, and loads `blockchain.wasm` with
   settings according to a local configuration file. The configuration file may
   specify one or more different `.wasm` modules to start at launch. The process of
   starting these initial WASMs should closely mirror the API these same WASMs can
   use to dynamically launch more WASMs — like `dlopen`: by **name**, not by moving
   WASM bytes around. The runtime maintains a **Module Registry** of compiled modules
   keyed by name; `runtime.get_module("blockchain-connection")` is a hash-map lookup
   returning already-compiled native code.

2. This is a server, processing many asynchronous operations; therefore, it has a
   thread pool and the thread pool should match the CPU hardware parallelism level —
   any more will cause problems.

3. The thread pool actively manages a set of **fibers** — application-level,
   cooperatively multi-tasked threads which run until they need to block and then
   wait. The benefit is it turns complex async data paths into apparently sequential
   blocking code. Easier for the developer to reason about.

4. All `.wasm` modules compiled to JIT via the Runtime do so with gas/yield enabled.
   This means that while a WASM is running it will yield to the cooperative scheduler
   at the configured "gas quantum" which is checked on function entry and back-jumps
   (note: may only need to be checked on back-jumps). Developers can then develop
   assuming that sequential statements are atomic unless they cross a function
   boundary or loop back; if they really want to "lock" there is a "disable
   preemption" RAII primitive (`preempt_guard` from `psiserve-scheduler-primitives`).
   From a developer's perspective this is far simpler and much safer than writing
   traditional multi-threaded code. The issue with preemption is that the scheduler
   can cause re-entrant calls back to the WASM, thus the WASM must be prepared for
   it. This is no different than traditional `host → guest → host → guest` call
   patterns — the only difference is the implicit calls to host at the gas quantum.

5. Each `.wasm` instance runs on its own **thread** — not an OS thread, but a
   logical thread managed by psiserve. Two method calls on the same instance
   will never run concurrently; the instance is single-threaded from the
   developer's perspective. `thread ≈ logical process`, `OS thread ≈ hardware CPU`,
   and `psiserve ≈ the kernel / OS`.

```
psiserve (host / OS)
├── Module Registry          ← compiled modules keyed by name
│   ├── "blockchain"         ← from config
│   └── "blockchain-connection" ← from config
│
├── OS Thread Pool (N = hardware cores)
│   └── per-thread Scheduler + I/O engine
│
└── Instances (each on its own logical thread)
    ├── blockchain.wasm          Thread 1
    │   ├── listener fiber       (same thread)
    │   └── block-producer fiber (same thread)
    │
    ├── blockchain-connection.wasm #1   Thread 2
    ├── blockchain-connection.wasm #2   Thread 3
    └── ...
```

Fibers within a thread share the instance's linear memory. Instances on
*different* threads are fully isolated — separate linear memory, separate fd
tables, separate handle tables. The host schedules logical threads across OS
threads transparently; the developer doesn't think about which hardware core
is running their code.

---

## Blockchain Startup

### blockchain.wasm

```cpp
class Blockchain {
    void start() {
        auto my_name = runtime.instance_name();
        _database = open_database("blockchain");  // independent host service

        set< handle<BlockchainConnection> > active_connections;

        // Spawn a listener fiber on the current thread via async_call
        // to self. This is the unified dispatch primitive — same mechanism
        // as cross-instance calls, just targeting self with this_thread.
        // Returns a future allowing the spawner to go on to other things.
        future<result> listener_complete = async_call(self, "listener"_n,
                                                      {}, thread::this_thread);
        // ... listener body runs as a fiber on this thread:
        auto listener = [&]() {
            for (;;) {
                sock = psi_accept(0);   // fd 0 = listen socket from config
                if (sock < 0)
                    return sock;

                // Instantiate by name. The template parameter tells the
                // compiler that "blockchain-connection" exports
                // BlockchainConnection. mem_budget::_1MB gives each
                // connection 1MB of linear memory.
                auto bcc = runtime.instantiate<BlockchainConnection>(
                                "blockchain-connection", thread::fresh,
                                mem_budget::_1MB);
                active_connections.insert(bcc);

                // async_call with thread::fresh — the connection handler
                // runs on its own logical thread. borrow(self) gives it
                // a handle to call back (push_transaction). own(sock)
                // transfers socket ownership — our fd becomes invalid.
                future<result> r = async_call(bcc, "handle_connection"_n,
                    {borrow(self), own(sock)}, thread::fresh);
                r.on_completion([&](result) {
                    active_connections.remove(bcc);
                    runtime.destroy(bcc);  // dlclose — reclaim memory
                });
            }
        };

        // Block production loop — runs on the main fiber of this thread.
        // Produce a new block every 3 seconds until we get whatever kill
        // signal the psiserve host wants to use for a graceful shutdown.
        while (runtime.running()) {
            psi_sleep_until(next_second());
            _database.store(_pending_block.number(), _pending_block);

            for (auto c : active_connections)
                c->broadcast_block(_pending_block);

            start_next_block(_pending_block);
        }
    }

    // ── blockchain_api interface ─────────────────────────────────
    // This is the subset of Blockchain's interface that smart contracts
    // can see. Contracts import it; blockchain.wasm exports it.
    // bind() wires them together selectively by name_id.

    own<database> get_database() {
        // runtime.caller() tells us which instance is calling.
        // We return a DB handle scoped to that caller's account.
        // The subtree is temporary — writes go to a scratch tree_id
        // that only we can commit to the account's db_root key.
        auto caller_account = instance_to_account(runtime.caller());
        return open_subtree(_database, caller_account);
    }

    result call_contract(name_id target, Action action) {
        // Permission-checked cross-contract call.
        // The caller wants to invoke another contract — we mediate.
        auto caller = runtime.caller();
        check_permission(caller, target, action);
        return dispatch_action(target, action);
    }

    result push_transaction(Transaction tx) {
        // blockchain.wasm is privileged to launch new WASM instances.
        // The transaction data tells us which smart contract instances
        // we need to instantiate. Those instances may want to call out
        // to other instances, which requires them to call back to
        // blockchain — blockchain checks permissions and then calls
        // through. The same contract may be referenced multiple times
        // per transaction either directly or indirectly by one contract
        // calling another.
        //
        // The lifetime of a smart contract instantiation is for the
        // transaction. They all live on blockchain.wasm's thread and
        // only have access to the resources blockchain.wasm exposes
        // to them; thus they may not even make any direct host calls
        // unless blockchain gives them a handle to a database resource
        // pre-scoped to their writable region.
        //
        // blockchain.wasm runs in "subjective" mode, and has to
        // carefully control its own operations to stay deterministic.
        // If other WASMs request subjective info like clock() or rand()
        // then blockchain records the data it provides them into the
        // block so replay can be objective.

        // Instantiate each unique contract once per transaction.
        // A transaction may have many actions on the same contract —
        // we reuse the instance across actions within the tx.
        map<name_id, own<SmartContract>> instances;

        for (action : tx.actions) {
            auto account = db.get_account(action.account);
            auto wasm_hash = account.contract_hash();

            if (!instances.contains(action.account)) {
                // First action on this contract in this tx.
                // mem_budget comes from the account's registered policy.
                // Linear memory is allocated from blockchain.wasm's own
                // address space via alloc_child_memory — no mmap.
                auto contract = runtime.instantiate_by_hash<SmartContract>(
                    wasm_hash, thread::this_thread,
                    account.mem_budget());

                // Selectively bind only the blockchain_api interface.
                // The contract can call get_database(), call_contract(),
                // etc. but NOT block_producer or admin methods.
                // name_id: 64-bit, no string allocation, O(1) match.
                runtime.bind(contract, self, "blockchain_api"_n);

                instances[action.account] = std::move(contract);
            }

            // Dispatch: pass raw action bytes to the contract's apply().
            // blockchain.wasm doesn't parse the args — it just passes
            // the bytes through. The contract deserializes them itself.
            runtime.call(instances[action.account], "apply"_n,
                         action.raw_bytes);
        }

        // Destroy all per-tx contract instances — dlclose. Linear
        // memory returned to blockchain.wasm's allocator via
        // free_child_memory.
        for (auto& [account, inst] : instances)
            runtime.destroy(inst);

        return _pending_block.push(tx);
    }

    PendingBlock  _pending_block;
    own<Database> _database;
};
```

### smart_contract.wasm — standardized entry point

Every smart contract exports a single `apply` function. The contract owns its
own dispatch — it interprets the action bytes however it wants.

```cpp
// WIT metadata (in custom section, for tooling / UI / block explorers):
//   interface token {
//     transfer: func(from: string, to: string, amount: u64);
//     approve: func(spender: string, amount: u64);
//   }

// Actual export — what the host calls:
void apply(const uint8_t* data, uint32_t len) {
    // Contract deserializes using PSIO canonical ABI (same format the
    // WIT metadata describes). Or fracpack, protobuf, raw structs —
    // whatever the contract author chose. blockchain.wasm doesn't care.
    auto action = psio::from_frac<Action>(data, len);

    switch (action.method) {
        case "transfer"_n: do_transfer(action.args); break;
        case "approve"_n:  do_approve(action.args);  break;
        default:           abort_message("unknown method");
    }
}

own<database> do_transfer(TransferArgs args) {
    // Contract calls blockchain_api to get its scoped database.
    // blockchain.wasm's get_database() uses caller() to identify us
    // and returns a subtree handle.
    auto db = blockchain_api::get_database();
    auto tx = db.start_write();
    auto balances = tx.open_table("balances"_n);
    // ... transfer logic ...
    tx.commit();
}
```

### Dispatch model: schema vs entry point

Two independent concerns:

1. **Entry point** (`apply`): the host calls `apply(ptr, len)` with raw bytes.
   One `memcpy` into the contract's linear memory, one scalar function call.
   No host-side type introspection, no canonical ABI lift/lower overhead.

2. **Schema** (WIT metadata): the contract's WASM embeds WIT interface
   descriptions in custom sections (generated by `PSIO_REFLECT`). These describe
   the method signatures, parameter types, and serialization format. **The runtime
   never reads them** — they exist for:
   - UI tools generating transaction forms
   - Block explorers decoding action data
   - SDKs generating typed client bindings
   - Inter-contract calls (where the host does need type info for lift/lower
     between two instances' memories via `blockchain_api.call_contract()`)

The contract typically uses PSIO's canonical ABI as its serialization format
(same encoding the WIT describes), so the schema and the wire format agree.
But the contract is free to use any format — the schema is descriptive, not
prescriptive for the `apply` dispatch path.

### blockchain_connection.wasm

Blockchain connections are designed to operate on their own thread so the incoming
data and speculative executions don't bog down the single-threaded block production.

```cpp
class BlockchainConnection {
    bool handle_connection(borrow<Blockchain> bc, own<socket> sock) {
        // We're on our own thread. sock is ours — it appeared in our
        // handle table when the host transferred ownership from the
        // parent instance.

        while (sock.open()) {
            auto msg = read_message(sock);
            if (validate(msg)) {
                // Cross-instance call: host lifts args from our memory,
                // dispatches to blockchain.wasm's thread (serialized
                // with block production), lowers args there, runs
                // push_transaction, lifts result, lowers back here.
                // Our fiber blocks until the call returns.
                auto result = bc.push_transaction(msg.trx);
                sock.send(result);
            } else {
                sock.send(error);
            }
        }
        // own<socket> destructor calls psi_close automatically.
    }
};
```

---

## How Resources Cross Instance Boundaries

### The problem with raw integers (psibase approach)

In psibase, host resources were passed as bare `u32` integers — file handles,
database handles, instance handles were all just numbers. The guest passed an
integer to a host function, the host looked it up in a flat table. This worked,
but had serious limitations:

- **No type safety**: a database handle and a socket handle were both `u32`. Pass
  the wrong one and you get a runtime error, not a compile error.
- **No ownership tracking**: the host couldn't know when a handle was done being
  used. Guests had to explicitly call `close(handle)` or leak.
- **No transfer semantics**: passing a handle to another instance meant passing a
  raw integer, with no way to enforce "the sender gives up access when the
  receiver gets it."
- **Manual serialization**: psibase needed `exportHandles` / `importHandles` to
  serialize handle tables across `call` boundaries — error-prone glue code.

### The typed resource model (psiserve approach)

psiserve uses WIT Component Model **resources** — the host knows the **type** of
every handle, not just its integer value. This is possible because the WASM module
embeds WIT metadata (via custom sections generated by `PSIO_REFLECT`) that tells
the host the full type signature of every import and export.

When blockchain.wasm's `get_database()` returns `own<database>`, the host knows:
- The return type is `own<database>` (not just "some u32")
- `database` is a resource type with specific methods (`start_read`, `start_write`, etc.)
- The `own` wrapper means **ownership transfers** — the caller gives up the handle

The host maintains a **per-instance, per-type handle table**. Each resource type
(`database`, `transaction`, `table`, `cursor`, `socket`, etc.) has its own table.
Under the hood, a handle is still a `u32` index — but the host tracks what type
it refers to, which instance owns it, and whether it's owned or borrowed.

### How `own<T>` works

`own<T>` is an **owning handle** — like `std::unique_ptr`. When an `own<database>`
crosses an instance boundary:

1. **Caller side (lift)**: The host reads the `u32` handle from the caller's
   `database` handle table. It resolves the handle to the underlying host object
   (a `shared_ptr` to a real psitri database subtree in native memory). It
   **removes** the entry from the caller's table — the caller no longer owns it.

2. **Callee side (lower)**: The host **inserts** the same host object into the
   callee's `database` handle table, producing a new `u32` handle local to that
   instance. The callee now owns it.

3. **The callee** sees a fresh `own<database>` with a valid handle in *its* table.
   It can call `start_write()`, `open_table()`, `upsert()` — all of which the host
   dispatches to the real psitri object behind the handle. The caller's old handle
   is now invalid — using it traps.

4. **On drop**: when the `own<database>` goes out of scope (or the instance is
   destroyed), the host calls `[resource-drop]database` which releases the
   underlying host object. If it was a temporary subtree, the writes are discarded
   unless blockchain.wasm already committed them.

### How `borrow<T>` works

`borrow<T>` is a **temporary, non-owning reference** — like `const&`. When
blockchain_connection.wasm calls `bcc.handle_connection(borrow(self), own(sock))`:

1. The host reads the `u32` self-handle from blockchain.wasm's table.
2. It creates a **temporary entry** in the callee's handle table pointing to the
   same host object. This entry is valid only for the duration of the call.
3. The callee can call methods on it — when `bc.push_transaction(...)` is called,
   the host resolves the borrow handle back to blockchain.wasm's Blockchain object
   and dispatches the call to blockchain.wasm's thread.
4. On return from the outer call, the temporary borrow entry is **automatically
   removed** from the callee's table. The callee cannot store it.

### Why this matters

The host can enforce invariants that raw integers cannot:

- **Type safety**: passing `own<database>` where `own<socket>` is expected is a
  compile-time error (C++ side) or a host-side type mismatch (WASM side).
- **Ownership tracking**: the host knows exactly which instance owns which
  resources. When an instance dies, all its owned resources are automatically
  cleaned up.
- **Transfer enforcement**: `own<T>` in a function parameter means the caller
  *must* give up the handle. The host removes it from the caller's table as part
  of the call — there's no way to "keep a copy."
- **No manual serialization**: handle transfer is automatic in the canonical ABI.
  No `exportHandles` / `importHandles` glue.
- **Scoped borrowing**: `borrow<T>` handles are automatically revoked on return.
  A callee cannot squirrel away a reference to use later.

### No `shared<T>` — and why that's fine

The WIT Component Model deliberately has no shared ownership type. `own<T>` is
single-owner (move-only), `borrow<T>` is temporary (call-scoped). No `shared<T>`.
This is intentional: shared references across component boundaries create reference
cycles and make garbage collection impossible — the host can't know when to clean
up if multiple components hold shared references to the same object.

But the **host implementation** absolutely uses `shared_ptr` internally. When
blockchain.wasm calls `open_subtree(db, account)`, the host creates a
`shared_ptr<subtree_impl>`. The `own<database>` that gets passed to a contract is
a handle to that same `shared_ptr`. If blockchain.wasm keeps its own internal
reference (to commit the subtree later), both references point to the same host
object. The single-owner semantics are enforced at the **WASM boundary** (handle
tables), not in host memory.

```
blockchain.wasm calls open_subtree(db, account)
  → host creates shared_ptr<subtree>
  → host inserts into blockchain's handle table → own<database>
  → blockchain calls get_database() from a contract's perspective:
    → host lifts from blockchain's table
      (blockchain keeps a separate internal shared_ptr for commit tracking)
    → host lowers into contract's table → own<database>
  → contract writes to it (goes to scratch tree_id via shared_ptr)
  → contract returns, own<database> dropped, handle removed from table
  → blockchain still has its internal shared_ptr
  → blockchain decides: commit the scratch tree_id, or discard it
```

### Comparison with psibase's handle model

psibase had `exportHandles` / `importHandles` — **guest-side serialization** where
the guest packed its handle table into bytes and unpacked on the other side. Every
cross-service `call()` needed explicit marshalling code. WIT's `own<T>` /
`borrow<T>` is **host-side mediation** — the host transfers handle table entries as
part of the call dispatch. The guest code is just `func(own<database>)` and the
host does the rest. Same underlying concept (handle tables mapping `u32` indices to
host objects), but the WIT approach eliminates the manual glue.

### Implementation

`psio::own<T>` and `psio::borrow<T>` are defined in
`libraries/psio/cpp/include/psio/wit_resource.hpp`. Resource types inherit from
`psio::wit_resource`. The WIT metadata is generated by `PSIO_REFLECT` and embedded
in the WASM module's custom sections. The host reads this metadata at instantiation
time to set up the typed handle tables.

---

## Database Layers

There are three levels of database access, all backed by the same psitri engine
but presenting different views:

### 1. psitri raw API — full database engine

blockchain.wasm (and other trusted services) get the full psitri surface:
`psi::db::store::open("blockchain")` returns an `own<database>` rooted at a
top-level psitri root. Full access to all tables, snapshots, subtree manipulation.

### 2. Detached subtree — scratch workspace with commit/abort

When blockchain.wasm needs to give a contract isolated storage, it creates a
**detached subtree**: a scratch root node backed by psitri but not yet committed
to the main database. The contract can create tables, write keys, run
sub-transactions — all within this scratch space. The writes are real (they go
to psitri's write-ahead log) but the root is **temporary**: only blockchain.wasm
can commit it by writing the resulting tree_id to the account's `db_root` key in
its own database.

This is the `open_subtree(db, account)` call. The returned `own<database>` is
backed by a `shared_ptr<subtree>` in host memory. The contract sees a full
database interface; blockchain.wasm retains the ability to commit or discard.

### 3. Table API — what contracts see

From `libraries/psi-api/include/psi/db.hpp`. The contract's perspective: it has
`own<database>` and calls `start_write()`, `open_table()`, `upsert()`, etc. It
doesn't know it's operating on a detached subtree. The interface is identical to
what blockchain.wasm itself uses — just rooted at a different node.

### Database modes (from psibase's lesson)

psibase exposed several database "types" through the same KV API: objective
(consensus-visible), subjective (node-local), write-only (event logs). The data
went to different places depending on the mode. In psiserve, the equivalent is
that blockchain.wasm creates subtree handles with different backing semantics:

- **Objective subtree**: writes go to the consensus-visible scratch tree. Committed
  by blockchain.wasm into the block's state root. This is the normal contract DB.
- **Subjective subtree**: writes go to a node-local store. Never enters consensus.
  Used for node-specific caching, indexing, analytics.
- **Event log**: append-only writes. Contracts emit events; blockchain.wasm stores
  them in the block's receipt log. Not queryable by the emitting contract.

The contract doesn't choose the mode — blockchain.wasm does, when it creates the
subtree handle. The contract sees the same `database` resource interface regardless.
This keeps the contract simple and the policy in blockchain.wasm's control.

---

## How Smart Contracts Get Resources

Contracts don't receive resources through `init()` or tokens. They **import an
interface** (`blockchain_api`) and call methods on it. The host wires those imports
to blockchain.wasm's exports via `bind(name_id)`. blockchain.wasm uses `caller()`
to identify who's asking and scopes the response.

```
contract.wasm                      blockchain.wasm               host
─────────────                      ───────────────               ────

imports blockchain_api ──────────→ exports blockchain_api
                                   exports block_producer
                                   exports admin_api

bind(contract, blockchain, "blockchain_api"_n)  ← only this one

calls get_database() ────────────→ get_database() {
                                      who = caller()              ← host
                                      acct = lookup(who)
                                      return open_subtree(db, acct)
                                   }
                                   ← own<database> (scoped subtree)
receives own<database> ←──────────
calls db.start_write() ──────────────────────────────────────────→ host
calls table.upsert() ───────────────────────────────────────────→ host
```

- **No init()** — contracts call `get_database()` when they need a DB.
- **No tokens** — `caller()` is unforgeable, provided by the host. Like `getpid()`.
- **No middle man for DB calls** — the returned `own<database>` is a real host
  resource backed by psitri. DB operations go directly to the host.
- **Scoped subtree** — writes land in a scratch tree_id that only blockchain.wasm
  can commit to the account's `db_root` key.
- **Selective binding** — `bind(name_id)` wires exactly one interface. Unbound
  imports trap. `name_id` is 64-bit, no strings cross the WASM boundary.

---

## Traps, Handlers, and Propagation

### Trap handlers are stack-based, not parent-child

Any instance with the `psi:runtime/metering` capability can push a trap handler
onto the call stack. When a trap fires, it unwinds to the **nearest handler on
the current fiber's call stack**, regardless of which instance registered it.
Multiple instances in the call chain can each have handlers — they nest like
try/catch frames.

```
blockchain.wasm (has metering capability)
  set_trap_handler(H1)
  connection.wasm (also trusted, also has metering capability)
    set_trap_handler(H2)
    contract_A
      contract_B
        TRAP → unwinds to H2 (nearest)
        H2 decides: resume, unwind (reaches H1), or consume
```

`set_trap_handler` is a capability — it must be bound via `psi:runtime/metering`.
Untrusted contracts cannot register trap handlers. Gas, time limits, and stack
overflows always unwind to a handler registered by a privileged instance.

### Trap handler API

The handler fires **before** the unwind, giving the registering instance a chance
to respond. The `resumable` flag tells the handler whether resumption is possible.

```cpp
struct trap_info {
    trap_reason  reason;
    bool         resumable;     // host sets this — can execution continue?
    uint64_t     gas_consumed;
    instance_id  instance;      // which instance trapped
};

enum class trap_action { resume, unwind };
```

**Resumable traps** — the handler can fix the problem and return `resume`;
execution continues at the trap site as if nothing happened:

| Trap | Resumable | Handler can... |
|------|-----------|----------------|
| `gas_exhausted` | Yes | bump gas ceiling, execution resumes at probe |
| `time_limit` | Yes | extend deadline, execution resumes |

**Non-resumable traps** — the handler can capture diagnostics but must return
`unwind` (host forces unwind if it returns `resume`):

| Trap | Resumable | Why not |
|------|-----------|---------|
| `import_not_bound` | No | no function to call, no return value to fabricate |
| `stack_overflow` | No | guard page hit, native stack is gone |
| `abort` | No | explicit kill |
| `trapped_future` | No | the async work is dead, no result exists |

```cpp
// Example: blockchain.wasm sets a handler before dispatching contracts
runtime.set_trap_handler([&](trap_info info) -> trap_action {
    if (info.reason == trap_reason::gas_exhausted && info.resumable) {
        auto remaining = tx.max_gas - info.gas_consumed;
        if (remaining > 0) {
            runtime.set_gas_ceiling(info.gas_consumed + remaining);
            return trap_action::resume;  // contract continues
        }
    }
    last_error = info;
    return trap_action::unwind;
});
```

### Unresolved imports

If a contract calls an import that was never bound, it traps with reason
`import_not_bound`. This is non-resumable — there's no function to call and
no return value to fabricate. The trap unwinds to the nearest handler.

Contracts that want to **probe** for optional features use `has_import` on
`psi:core/identity` (available to everyone) to check before calling:

```cpp
if (has_import("event_log"_n)) {
    event_log::emit(receipt);
} else {
    // feature not available, skip
}
```

This is zero-cost — no trap, no handler, just a boolean check. Preferred over
calling and catching.

### Synchronous propagation

On a synchronous call (`runtime.call`), traps propagate up the call stack
through nested handler frames:

```
A.set_handler(H1)
  runtime.call(B, method, args)
    B.set_handler(H2)
      runtime.call(C, method, args)
        C traps
        → H2 fires (nearest handler)
        H2 returns unwind
        → runtime.call(C) returns with trap_info in side channel
      B sees trap, decides to also unwind
      → H1 fires
      H1 returns unwind
      → runtime.call(B) returns with trap_info
    A handles the error
```

If **no handler** is on the stack when a trap fires:
- The host catches it at the fiber root
- The fiber is terminated, the instance is marked as trapped
- If the fiber was inside a `runtime.call()`, the call returns with trap_info

### Async propagation

Across async boundaries (`async_call` / `post`), there's no shared call stack.
The receiver's fiber is independent:

**`async_call` (sender awaits result):**

1. Receiver's fiber runs, trap fires
2. If receiver has a handler → handler fires, can consume or unwind
3. If no handler (or handler returns `unwind`) → fiber terminated, **future
   marked as trapped** with trap_info attached
4. Sender calls `await(future)` → wakes up, host sees future is trapped →
   **fires `trapped_future` trap on sender's fiber** (non-resumable)
5. Sender's nearest handler gets it with full context (reason, instance,
   gas consumed from the original trap)
6. If sender has no handler → same rules, propagates up sender's stack

**`post` (fire-and-forget):**

1. Receiver's fiber runs, trap fires
2. Handler fires if registered; otherwise fiber terminated
3. **No propagation** — nobody is waiting. Host logs the trap (telemetry),
   cleans up the instance, discards uncommitted state.

### Trap propagation summary

| Scenario | Behavior |
|----------|----------|
| Sync, handler on stack | Nearest handler fires, can resume (if resumable) or unwind |
| Sync, no handler | Unwind to `runtime.call()` return with trap_info |
| Sync, no `runtime.call()` | Host terminates fiber, marks instance trapped |
| `async_call`, receiver has handler | Receiver's handler fires |
| `async_call`, receiver no handler | Future marked trapped |
| `await` trapped future | `trapped_future` trap fires on sender (non-resumable) |
| `post`, trap in receiver | Host consumes, logs, cleans up. No propagation. |

### Gas visibility

Gas is **invisible to contracts**. Contracts cannot read `gas_consumed()` or
set ceilings — `psi:runtime/metering` is not bound to them. If a contract needs
cost information, it goes through blockchain.wasm (via `blockchain_api`) which
mediates and records subjective data into the block for deterministic replay.

Resource limits that don't use the trap handler mechanism:

| Limit | Behavior |
|-------|----------|
| Memory (`memory.grow` exceeds max_pages) | Returns -1, no trap |
| DB write quota | Returns `error::quota_exceeded`, no trap |

---

## psiserve Host API — Interface Groups

The host API is organized into named interfaces. Each interface is an
independently bindable capability. A parent can only bind interfaces it
itself possesses, and can further restrict child instances by withholding
interfaces.

These are the **WIT imports** the host provides — the actual ABI boundary.
C++ sugar (`own<T>`, templates, `_n` literals) is built on top by PSIO
reflection; the host doesn't know about guest-side types.

### `psi:core/identity` — everyone gets this

```
caller:     func() -> u64       // instance_id of the cross-instance caller
self:       func() -> u64       // this instance's id
has-import: func(iface: u64) -> u32  // probe: is this interface bound? (name_id)
```

Every instance, including untrusted contracts, gets `psi:core/identity`.
These are the only universally available imports. `has_import` lets contracts
check for optional features before calling — zero-cost, no trap.

### `psi:runtime/instances` — launch and manage instances

```
instantiate:         func(name: u64, thread: u8, mem-budget: u8,
                          base-ptr: u32) -> u32
instantiate-by-hash: func(hash-ptr: u32, hash-len: u32, thread: u8,
                          mem-budget: u8, base-ptr: u32) -> u32
destroy:             func(instance: u32)
bind:                func(consumer: u32, provider: u32, iface: u64)
register-contract-policy: func(config-ptr: u32, config-len: u32,
                                db: u32, key-prefix: u64)
running:             func() -> u32
instance-name:       func() -> u64
```

`mem-budget` is a power-of-2 exponent: the sub-instance's maximum linear
memory is `1 << mem_budget` bytes (e.g. 16 = 64KB, 20 = 1MB, 22 = 4MB).

`base-ptr`: if non-zero, the caller provides the child's linear memory
at this offset within the caller's own linear memory. The caller owns
and manages this memory. If zero, the host allocates via
`alloc-child-memory` / `free-child-memory` (or the system arena if those
exports are absent). See **Sub-Instance Memory Allocation** below.

`destroy` is the `dlclose` equivalent — tears down the instance and
reclaims its linear memory immediately. Callers should destroy instances
when done to avoid memory accumulation on long-lived connections.

Trusted services (blockchain.wasm, httpd.wasm) get this. Smart contracts
do not — they cannot launch other instances directly. Cross-contract calls
go through blockchain.wasm's `blockchain_api.call_contract()`.

### `psi:runtime/dispatch` — cross-instance calls

```
call:       func(target: u32, method: u64, args-ptr: u32, args-len: u32,
                 ret-ptr: u32, ret-len: u32) -> i32
post:       func(target: u32, method: u64, args-ptr: u32, args-len: u32)
async-call: func(target: u32, method: u64, args-ptr: u32, args-len: u32,
                 thread: u8) -> u32
await:      func(future: u32, ret-ptr: u32, ret-len: u32) -> i32
```

- `call` — synchronous, caller blocks until target returns
- `post` — fire-and-forget, no return value
- `async-call` — returns a future handle, does not block. `thread` hint
  controls scheduling: `0 = this_thread`, `1 = fresh`. `await` blocks
  until result ready.

`async_call` subsumes the legacy `psi_spawn` concept: spawning a fiber
within the same instance is `async_call(self, func, args, this_thread)`.
Handing a connection to a new instance on its own thread is
`async_call(bcc, handle_connection, args, fresh)`. One primitive, thread
policy is orthogonal.

### `psi:runtime/metering` — gas and time control

```
gas-consumed:      func() -> u64
set-gas-ceiling:   func(value: u64)
set-time-limit:    func(timeout-ms: u32)
set-trap-handler:  func(func-table-idx: u32)
```

Only the orchestrator (blockchain.wasm) gets this. Contracts cannot see gas
or set ceilings — gas is a host implementation detail invisible to them.

### `psi:runtime/fibers` — timers and clock

```
sleep-until: func(deadline-ns: u64)
clock:       func(clock-id: u32) -> u64
```

`clock` returns nanoseconds directly (i64 return, no pointer indirection).
Clock IDs: 0 = realtime (wall clock), 1 = monotonic (steady clock).
For deterministic contracts, blockchain.wasm can virtualize the clock by
providing a mediated version through `blockchain_api` that returns the
block timestamp instead of wall time.

`sleep-until` suspends the current fiber until the monotonic clock
reaches the given deadline. Used for block production timers.

**Note:** `spawn-fiber` is gone. In-process concurrency uses
`psi:runtime/dispatch::async_call(self, func, args, this_thread)` —
same mechanism as cross-instance calls, no separate primitive. A WASI
compatibility shim can map legacy `psi_spawn` to this if needed.

### `psi:runtime/io` — sockets and files

```
accept:   func(listen-fd: u32) -> i32
read:     func(fd: u32, buf: u32, len: u32) -> i32
write:    func(fd: u32, buf: u32, len: u32) -> i32
close:    func(fd: u32)
connect:  func(host-ptr: u32, host-len: u32, port: u32) -> i32
open:     func(dir-fd: u32, path-ptr: u32, path-len: u32) -> i32
fstat:    func(fd: u32, size-ptr: u32) -> i32
sendfile: func(out-fd: u32, in-fd: u32, count: u64) -> i64
cork:     func(fd: u32)
uncork:   func(fd: u32)
```

Only trusted services with I/O access. Untrusted contracts have no socket
or filesystem access — those imports don't exist in their pzam.

### `psi:db/store` — database access

```
open:          func(name: u64) -> u32           // handle to database resource
open-subtree:  func(db: u32, scope: u64) -> u32 // scoped sub-handle
```

Plus all the `database`, `transaction`, `table`, `cursor` resource methods
defined in `libraries/psi-api/include/psi/db.hpp`. These are resource
methods, not flat imports — the host resolves the `u32` handle and
dispatches to the resource implementation.

### `psi:fs/store` — content-addressed blobs

```
put:        func(data-ptr: u32, data-len: u32, cid-ptr: u32, cid-len: u32) -> i32
get:        func(cid-ptr: u32, cid-len: u32, offset: u64, buf: u32, len: u32) -> i32
stat:       func(cid-ptr: u32, cid-len: u32, size-ptr: u32) -> i32
```

### `psi:debug/console` — development only

```
write-console: func(msg-ptr: u32, msg-len: u32)
abort-message: func(msg-ptr: u32, msg-len: u32)
```

### Capability binding model

A parent instance can only bind interfaces it possesses. When blockchain.wasm
instantiates a contract:

```cpp
auto contract = runtime.instantiate_by_hash(wasm_hash, thread::this_thread);

// Contract gets identity (universal) + blockchain_api (selective)
// It does NOT get: psi:runtime/instances, psi:runtime/dispatch,
//   psi:runtime/metering, psi:runtime/io, psi:db/store, psi:fs/store
runtime.bind(contract, self, "blockchain_api"_n);
```

The contract's pzam was compiled with `trust::untrusted` — its import table
has no entries for the withheld interfaces. Even if it tried to call them,
the imports don't exist in its module.

**Inheritance rule**: a WASM cannot grant what it doesn't possess, but can
restrict further. The capability set is monotonically decreasing down the
spawn tree. blockchain.wasm has everything; contracts get only what
blockchain.wasm explicitly binds.

---

## Sub-Instance Memory Allocation

When a trusted service (blockchain.wasm) spawns sub-instances (contracts,
UI modules) on `thread::this_thread`, the sub-instance's linear memory can
be allocated **from the parent's own address space** rather than via a
separate `mmap`. This avoids kernel VMA churn and address space exhaustion
when spawning many short-lived instances (e.g., one contract per
transaction, one UI module per HTTP request).

### Memory safety tiers

| Tier | Address reservation | Bounds enforcement | Who gets it |
|------|--------------------|--------------------|-------------|
| **Guarded** | 4GB+ per instance (mmap + guard pages) | Hardware page fault, zero per-access cost | Trusted config-launched services only, scarce |
| **Checked** | Power-of-2 slice (64KB–256MB) | Software bounds check per access | Sub-instances, untrusted contracts, all memory64 |
| **wasm16** | 64KB (1 page) | None needed — 16-bit pointer can't escape | Contracts declaring max 1 page |

Guarded instances are scarce because each reserves 4GB+ of virtual
address space. The total is configured at startup (`guarded_pool_size`,
default 64). These are reserved for the small number of long-running
trusted services. Everything else uses checked mode.

### How parent-provided memory works

There are two ways to provide memory for a sub-instance, controlled by
the `base-ptr` parameter on `instantiate`:

#### Caller-provided (`base-ptr != 0`)

The caller allocates memory in its own linear memory (e.g. via its own
`malloc`) and passes the offset as `base-ptr`. The caller owns this
memory and is responsible for freeing it after `destroy`.

```
// Caller manages memory directly
ptr = malloc(1 << 22);   // 4MB in caller's linear memory
contract = instantiate(name, this_thread, 22, ptr);
// ... use contract ...
destroy(contract);
free(ptr);               // caller frees when ready
```

#### Host-allocated (`base-ptr = 0`)

The caller exports `alloc-child-memory` and `free-child-memory`. The
host calls these to allocate/free on behalf of the caller:

```
alloc-child-memory: func(size: u32, align: u32) -> u32
free-child-memory:  func(ptr: u32, size: u32, align: u32)
```

The presence of these exports is the **capability declaration** — it
signals to the host that this module is prepared to host sub-instances.
If absent, `instantiate` with `base-ptr = 0` falls back to host-managed
memory from the system arena.

```
// Host manages allocation via caller's exports
contract = instantiate(name, this_thread, 22, 0);
// host called alloc-child-memory(4MB, 4MB) internally
// ... use contract ...
destroy(contract);
// host called free-child-memory(ptr, 4MB, 4MB) internally
```

#### In both cases

1. Host computes `base = parent.linear_memory() + offset`.
2. Host constructs a `wasm_allocator` for the sub-instance backed by
   `(base, 1 << mem_budget)` — no `mmap`, no `mprotect`, no kernel call.
3. Sub-instance's bounds checks validate against its slice limit.
4. The child's memory is within the parent's address space.

The `mem_budget` parameter is a power-of-2 exponent:

```
mem_budget  bytes        typical use
────────────────────────────────────────
16          64KB         wasm16 contracts (zero bounds-check cost)
20          1MB          lightweight contracts
22          4MB          standard contracts
24          16MB         data-heavy contracts
26          64MB         large services
```

### Why this works

- **No kernel interaction**: the parent's guarded region is already
  mapped. Sub-allocation is pure userspace pointer arithmetic.
- **No address space cost**: 1000 sub-instances × 64KB = 64MB, all
  within the parent's existing 4GB region.
- **Parent controls the budget**: the parent's allocator decides where
  memory goes. A bump allocator wastes nothing. dlmalloc handles
  fragmentation. The host just calls the parent's own exports.
- **Clean teardown**: `destroy` calls `free-child-memory` to return the
  slice. If the parent exits first, its entire region is reclaimed — no
  leak.
- **`dlclose` semantics**: callers must `destroy` sub-instances when
  done. Long-lived connections that accumulate UI instances without
  destroying them will exhaust their allocation budget and get errors
  from subsequent `instantiate` calls.

### Host-side instance overhead

The per-instance host-side context (execution context, operand stack,
call stack, gas state) is ~4–16KB — trivially small compared to even the
minimum 64KB linear memory allocation. The dominant cost is always the
linear memory itself, which the parent controls.

### Relationship to memory64

memory64 (64-bit WASM addresses) **cannot use guard pages** because a
64-bit pointer can escape any virtual reservation. memory64 must use
checked mode — the same software bounds checks used by sub-instances.
The code paths unify: checked mode is parameterized on address width
(u32 for memory32, u64 for memory64), same bounds check, same
sub-allocation model.

See `.issues/psizam-memory64-not-enforced.md` for the full analysis.

### Zero-copy cross-instance calls (shared address space optimization)

When a sub-instance's linear memory is a slice of the parent's memory
(`base-ptr != 0` or host-allocated via `alloc-child-memory`), the host
knows that both memories share the same native address space. This
enables a **pointer-translation** path that avoids the canonical ABI's
normal alloc + memcpy:

**Standard path (separate address spaces):**

```
parent calls child.apply(action_bytes):
  1. Host reads action_bytes from parent's memory              (lift)
  2. Host calls child's cabi_realloc to allocate in child      (alloc)
  3. Host copies bytes into child's memory                     (memcpy)
  4. Host calls child.apply(child_ptr, len)
  5. Child writes result into its memory
  6. Host calls parent's cabi_realloc for return value          (alloc)
  7. Host copies result into parent's memory                   (memcpy)
```

**Shared-address-space path (pointer translation):**

```
parent calls child.apply(action_bytes):
  1. Host translates: child_ptr = parent_ptr - slice_offset    (subtract)
  2. Host calls child.apply(child_ptr, len)                    (no copy)
  3. Child writes result into its memory (= parent's memory)
  4. Host translates: parent_ptr = child_ptr + slice_offset    (add)
  5. Done                                                      (no copy)
```

Zero allocations, zero copies. The host replaces lift/lower with pointer
arithmetic. The data never moves.

**Guest-side type implications:**

This changes the **ownership and lifetime** of return values, which the
guest must know at compile time. Two proxy flavors are generated from
the same `PSIO_INTERFACE` declaration:

- **Borrowed proxy** (host-allocated, separate address spaces): return
  values are views into the child's memory. Valid until `destroy`.
  Caller must copy what it needs before destroying the instance.

- **Owned proxy** (caller-provided memory, shared address space): return
  values are offsets within the caller's own memory. Persist past
  `destroy`. Caller owns the data and the memory it sits in.

The distinction is a template parameter on the proxy type, determined
at `instantiate` time by whether `base-ptr` was zero or not. Same
interface, same host call, different C++ types enforcing correct
lifetime handling.

```cpp
// Borrowed proxy — host-allocated, return values are views
auto contract = instantiate<SmartContract>(hash, this_thread, _4MB, 0);
auto result = contract.apply(args);  // result is a borrowed view
my_copy = copy(result);              // must copy before destroy
destroy(contract);                   // result now invalid

// Owned proxy — caller-provided, return values in caller's memory
auto contract = instantiate_in<SmartContract>(hash, this_thread, _4MB, buf);
auto result = contract.apply(args);  // result is a caller-owned offset
destroy(contract);                   // result still valid in buf
use(result);                         // safe — data is in caller's memory
```

### Use case: deferred output buffering

The shared-address-space model changes how contract outputs (console
logs, event receipts, return data) flow through the system. Instead of
the psibase model where each `writeConsole` was an immediate host call
that copied bytes into a host-side buffer, contracts buffer their own
output in their own linear memory. The parent reads it out on success
via pointer translation — zero copies at every stage.

```
token.apply(action):
   // Contract builds log entries in its own linear memory.
   // The buffer is paid for out of the contract's mem_budget.
   log_buf.append("transfer: bank -> alice 100 tokens")
   log_buf.append("balance updated: bank=999900, alice=100")
   // ... do the actual work ...
   return {status: ok, log_ptr: log_buf.ptr, log_len: log_buf.len}

blockchain (on success):
   // result.log_ptr is in token's memory = blockchain's memory.
   // Translate pointer, pass directly to host — zero copy.
   psi_write_console(slice_offset + result.log_ptr, result.log_len)

blockchain (on revert):
   destroy(contract)
   // Logs discarded with the instance. Never touched the host.
```

This delivers several properties that per-call host buffering cannot:

- **Resource accountability**: the contract pays for log/event buffer
  space out of its own `mem_budget`. A chatty contract that logs 1MB
  eats its own allocation, not the host's. If it exceeds its budget,
  `memory.grow` fails and the contract handles the pressure — the host
  and parent are unaffected.

- **Deferred commit**: blockchain.wasm can inspect contract outputs
  before deciding whether to flush them to the host. Reverted
  transactions discard all output with `destroy` — zero host work.
  Successful transactions forward the output in one shot.

- **No host-side buffer management**: the host doesn't need to decide
  how much log buffer to allocate per contract, when to flush, or how
  to handle overflow. The contract's own allocator makes those
  decisions within its bounded memory.

- **Generalizes to all contract output**: event receipts, return values,
  diagnostic data, serialized state snapshots — anything the contract
  produces follows the same pattern. The contract writes it, the parent
  reads it at a translated offset, the host gets a direct pointer.

This is a meaningful architectural advantage of the shared-address-space
model over standard WASM component isolation, even though it requires
a non-standard calling convention. The performance and resource
accountability benefits justify the deviation for server-side runtimes
where the trust hierarchy (host > trusted service > contract) is
explicit and enforced.

---

## Compilation Pipeline

```
wasm bytes  ──→  pzam (JIT-compiled native code)  ──→  instance
   │                    │                                   │
   │                    │  determined by module_config:      │  determined by:
   │                    │    - JIT tier                      │    - thread hint
   │                    │    - float mode (soft/native)      │    - gas ceiling
   │                    │    - metering caps (bitmap)        │    - max pages
   │                    │    - memory safety mode            │    - bound interfaces
   │                    │    - trust level (baked into pzam) │
   │                    │    - link deps (libc++, malloc)    │
   │                    │                                   │
   cache key:           cache key:                          managed by:
   content hash         (wasm_hash, config_hash)            caller (guest or host)
```

- **wasm cache** — avoids re-parsing. Keyed by content hash.
- **pzam cache** — avoids re-JIT. Keyed by `(wasm_hash, config_hash)`. Same wasm
  with different trust/float/metering configs produces different pzam.
- **instance** — fresh linear memory + globals. Host caches trusted singletons
  by name; guest manages its own pool for per-transaction contracts.

The guest declares what it needs (`instantiate_by_hash`), the host handles the
full pipeline transparently (check pzam cache → check wasm cache → fetch from
registered DB location → JIT → cache → instantiate).

---

## Design Decisions (resolved)

1. **`psi_spawn` is deprecated**: in-process fiber spawning is
   `async_call(self, func, args, this_thread)` — same dispatch primitive
   as cross-instance calls. No separate `psi_spawn` API.

2. **Instance lifecycle**: parent calls `destroy(instance)` explicitly
   (`dlclose` semantics). Connection close destroys all instances the
   connection spawned. Untrusted code that fails to destroy gets bounded
   by its address-space budget.

3. **Clock API**: `psi.clock(clock_id) -> i64` returns nanoseconds
   directly (no pointer indirection, no errno). WASI `clock_time_get`
   not used — `psi.clock` is virtualizable per-instance for determinism.
   A WASI compatibility shim is trivial if needed.

4. **Sub-instance memory**: parent-provided via `alloc-child-memory` /
   `free-child-memory` exports. Power-of-2 budgets. No mmap churn. See
   **Sub-Instance Memory Allocation** section above.

5. **UI instance lifetime**: per-connection, per-request pattern.
   `bc_connection` instantiates UI modules on demand, destroys after
   response. Optional LRU cache (max 2–3) within a connection to
   amortize instantiation cost for repeated queries to the same service.
   Not singleton, not pooled — each connection manages its own.

## Open Questions

1. **Sequential / event DB**: psibase had `putSequential`/`getSequential` for
   append-only event logs. Should psiserve's `psi:db/store` include an event
   emission primitive for blockchain receipts and contract events?

2. **Subtree commit semantics**: uncommitted subtrees auto-discard on instance
   destruction, or does blockchain.wasm need to explicitly abort?

3. **Trap handler reentrancy**: the trap handler runs in blockchain.wasm's
   context. Can it make host calls (e.g., read gas_consumed, inspect the
   trapped instance's state)? Or is it restricted to just returning
   `resume` / `unwind`?

4. **Determinism boundary**: how does the host distinguish objective instances
   (soft floats, no clock, gas-metered) from subjective ones? Is this a
   compile-time flag in `module_config`, or a per-interface binding decision?

5. **`psi:runtime/dispatch` granularity**: should `call`, `post`, and
   `async_call` be separate interfaces, or is the whole dispatch group
   either available or not? Finer granularity means a contract could have
   `call` (sync) but not `post` (fire-and-forget).

6. **Capability bitmap format**: what is the concrete representation of the
   capability set? A `u64` bitmap with one bit per interface? Or a list of
   `name_id`s? The bitmap is faster but limits the number of interfaces to 64.

7. **Checked-mode read strategy**: deferred watermark (zero branches per
   read, one check at loop/call boundaries) vs per-load bounds check
   (one branch per read, immediate trap). Both paths exist in the codebase.
   Benchmark needed to quantify the difference on real workloads. See
   `.issues/psizam-memory64-not-enforced.md`.
