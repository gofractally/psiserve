# psix IPC, Process, and Memory Design

## Overview

This document captures the design decisions for psix's process model, inter-process communication, memory allocation, and cross-thread coordination. The guiding principles are:

1. **Each WASM instance is a single-threaded process** — no in-process concurrency, no fibers within an instance
2. **Host schedules processes like an OS** — cooperative multitasking, many processes per OS thread
3. **No shared WASM memory** — instances communicate via host-mediated message passing
4. **Host owns all routing intelligence** — WASM code is dumb, host is the smart middleware
5. **WIT Component Model** — standard interfaces, canonical ABI at boundaries
6. **Zero-copy where possible** — messages live in sender's ring, read directly by host for delivery
7. **Static VMA tree** — all memory regions allocated at startup, recycled via `madvise`
8. **Lock-free coordination** — atomic bitmaps and monotonic offsets, no mutexes

---

## 1. Process Model

### Each WASM Instance is a Process

A WASM instance is a **single-threaded process**. It runs sequential code. When it calls a blocking host function (`psi_read`, `psi_accept`, `psi_poll`), the **entire process** blocks. The host parks it and runs another process.

There are no fibers, coroutines, or concurrency within a process. The WASM code is purely sequential — just like a Unix process.

```
OS Thread ("CPU core"):
    Instance A (process) — blocked on psi_read, parked
    Instance B (process) — blocked on psi_accept, parked
    Instance C (process) — RUNNING
    Instance D (process) — blocked on psi_poll, parked
    Instance E (process) — ready, in queue
```

The host scheduler switches between processes exactly like an OS switches between processes on a single core. The process doesn't know it was parked — it called `psi_read`, and `psi_read` returned when data was ready. Between those two moments, dozens of other processes ran.

### Two Programming Models

**Blocking (simple, one connection at a time):**
```c
void _start() {
    int fd = psi_accept(psi_listen(psi_bind(8080), 128));
    while (psi_read(fd, buf, len) > 0)  // blocks entire process
        psi_write(fd, response, rlen);
}
// Host launches 100 instances, distributes connections
```

**Non-blocking + poll (advanced, many connections):**
```c
void _start() {
    int listen_fd = psi_bind(8080);
    psi_listen(listen_fd, 128);

    while (1) {
        int n = psi_poll(events, count, -1);  // blocks entire process until I/O ready
        for (int i = 0; i < n; i++)
            if (events[i].fd == listen_fd)
                add_connection(psi_accept(listen_fd));
            else
                handle_io(events[i].fd);
    }
}
// Host launches fewer instances, each handles many connections
```

The developer picks the tradeoff. The host doesn't care — it just parks processes when they block and resumes them when I/O is ready.

### Scaling via Process Forking

The host scales a service by launching more instances. Multiple instances of the same service can share a port — the host distributes incoming connections.

```
Port 8080 (shared):
    Thread 1: [HTTP instance A] ← handles connections 1, 2, 3
    Thread 2: [HTTP instance B] ← handles connections 4, 5, 6
    Thread 3: [HTTP instance C] ← handles connections 7, 8, 9

    Host distributes accept() across instances.
```

Forking creates a copy of the instance via `memcpy` into a pre-allocated memory slot (not `mmap` COW — see §2). The fork gets its own heap, globals, and fd table. Cross-fork communication goes through message passing.

### Why Not In-Process Fibers

The execution state for a WASM "fiber" lives **outside** WASM's reach:
- **Interpreter:** operand stack, call stack, PC are in a host-side `execution_context` struct
- **JIT:** native call frames are on a host-side mmap'd native stack

WASM code cannot create or switch between these — only the host can. Making fibers a host API creates a security risk (unbounded fiber spawning). The clean solution: no in-process fibers. Each process is single-threaded. The host manages concurrency at the process level, where it already has full control.

Future: the WASM Stack Switching proposal (`cont.new`/`resume`/`suspend`) would enable WASM-internal fibers. When it's standardized and implemented in psizam, processes could opt into internal concurrency. Until then, processes are single-threaded.

### Why Not Shared WASM Memory Between Instances

WASM linear memory has no thread safety. Message passing is the correct abstraction:

- Developers design as if behind a load balancer with multiple real processes on different machines
- The host optimizes transparently (same-thread = direct copy, cross-thread = ring + wake)
- Scales identically whether instances are on the same machine or different machines

---

## 2. Memory Allocation Strategy

### Problem: VMA Fragmentation

Linux tracks each mapping as a VMA in a per-process red-black tree. Repeated `mmap`/`munmap`/`mprotect` causes:

- VMA tree fragmentation and bloat
- `vm.max_map_count` exhaustion (default 65536)
- Slower fault handling as VMA tree grows

This rules out per-connection `mmap` for COW forking, per-process stack mapping, and frequent `mprotect` for dirty tracking.

### Solution: Pre-allocate, Recycle via `madvise`

All memory pools are allocated as single contiguous `mmap` regions at startup. The VMA tree is static after initialization.

#### Process Native Stack Pool

Each process needs a native stack for JIT execution. These are allocated from a single pre-mapped region:

```
Startup:
    region = mmap(MAX_PROCESSES × 68KB, PROT_NONE)    // one VMA, zero physical RAM
    bitmap[MAX_PROCESSES / 64] words                    // allocation tracking

Alloc (first use):
    claim slot via CAS on bitmap word
    mprotect(slot + 4KB, 64KB, PROT_READ|PROT_WRITE)  // 4KB guard stays PROT_NONE

Alloc (reuse):
    claim slot via CAS on bitmap word
    madvise(MADV_DONTNEED) to reset pages             // keeps mapping, releases physical pages

Free:
    madvise(MADV_DONTNEED)                             // release physical pages
    atomic clear bit in bitmap                          // return to pool
```

At 68KB per process (64KB stack + 4KB guard page):
- 100K processes = ~6.8GB virtual address space (trivial on 64-bit)
- Physical RAM = only touched pages (parked processes may use only a few KB)

#### Bitmap Allocation (CAS, Lock-Free)

Slots are claimed via atomic CAS on 64-bit bitmap words:

```c
// Alloc: scan for word with any 0 bit
uint32_t word_idx = thread_id * stride;  // spread threads across words
for (int i = 0; i < num_words; i++) {
    uint64_t word = bitmap[word_idx].load(relaxed);
    while (word != ~0ULL) {
        uint32_t bit = __builtin_ctzll(~word);
        uint64_t new_word = word | (1ULL << bit);
        if (bitmap[word_idx].compare_exchange_weak(word, new_word, relaxed))
            return word_idx * 64 + bit;
    }
    word_idx = (word_idx + 1) % num_words;
}
return POOL_EXHAUSTED;

// Free: atomic clear
bitmap[word_idx].fetch_and(~(1ULL << bit), relaxed);
```

Each thread starts scanning from a different region (`thread_id * stride`) to reduce cache-line bouncing. Any thread can claim any slot — not partitioned, just hinted.

100K slots = ~1563 words = ~12.5KB bitmap — fits in L1 cache.

#### WASM Instance Memory Pool

Pre-mapped pool of instance memory regions, similar to the native stack pool. Each slot is sized to the service's `memory.initial` with guard pages. Recycled via `madvise(MADV_DONTNEED)`.

#### Spare Pool Policy

Instead of eagerly allocating or unmapping:

- Maintain a minimum number of pre-mapped spare slots
- On demand: if `stacks[slot] == nullptr`, `mmap` on first use (stays mapped forever)
- When idle: a reaper (runs when a thread has nothing to do) reclaims excess idle stacks via `munmap` if free count exceeds `min_spares + hysteresis`
- Ramp-up: first use triggers `mmap`, subsequent reuse is free
- Ramp-down: reaper slowly releases physical pages, only `munmap`s when load has been low for extended period

---

## 3. Process Scheduling

### Processes as the Unit of Scheduling

Each WASM instance (process) has exactly one execution context — one native stack (JIT) or one `execution_context` struct (interpreter). The scheduler switches between processes, not fibers within a process.

#### JIT Backend

Each process gets its own mmap'd native stack from the pool. JIT-compiled WASM calls become native `call` instructions on this stack. Yield = `swapcontext()` (~6 instructions). Resume = swap back.

#### Interpreter Backend

Each process has its own `execution_context` struct (operand stack, call stack, PC, locals — all host-side). Yield = return from `execute()`. Resume = call `execute()` with the same struct.

### Process States

```
READY ──→ RUNNING ──→ BLOCKED (on I/O, RPC, signal)
  ↑                       │
  └───────────────────────┘  (I/O ready, message arrived)
  
DONE ──→ recycled to pool
```

### Per-Thread Scheduler Loop

```
loop:
    // 1. Fire expired timers → push processes to ready queue
    now = clock()
    fire_timers(now)

    // 2. Check cross-thread messages
    bits = atomic_load(&my_has_messages)
    for each set bit:
        follow sender's per-receiver linked list
        deliver messages → push receiving processes to ready queue

    // 3. Wait for I/O events
    timeout = next_timer_or_default
    events = kqueue_wait(timeout)  // wake_fd is registered here
    process_io_events → push processes to ready queue

    // 4. Run ready processes
    while ready_queue not empty:
        proc = ready_queue.pop()
        resume proc  // runs until next blocking host call
```

### Idle Behavior

When a thread has nothing to do (no ready processes, no timers, no messages):
- Run reaper: reclaim excess idle native stacks beyond `min_spares`
- Block in `kqueue_wait` with long timeout — `wake_fd` wakes it when messages arrive

---

## 4. Communication Model

### Everything is a Typed Function Call

All inter-service communication reduces to calling functions on WIT-defined interfaces. The distinction between RPC, signals, and events is determined by conventions and delivery semantics, not by the communication mechanism.

From the WASM perspective: you call imported functions and export functions that get called. The host handles routing, delivery, and lifecycle.

### Host-Managed Communication

The host owns all message lifecycle:

```
Sender WASM                    Host                         Receiver WASM
───────────                    ────                         ─────────────
call imported function →  lift args from sender memory
                          route to target
                          lower args into receiver memory
                          call receiver's exported function →  handle(args...)
                                                           ←  return result
                          lift result from receiver
                          lower result into sender memory
result returned        ←  resume sender process
```

Why host-managed wins:
- **Cross-language by default** — any language that declares WASM imports participates
- **WIT canonical ABI** — standard lift/lower, no custom memory protocol
- **Blocking calls** — caller process blocks naturally, resumes when result ready
- **Transparent optimization** — host can optimize same-thread calls internally
- **No WASM-side library required** — just host function imports

### WIT Canonical ABI as Transport

For simple types (scalars, small structs), canonical ABI is essentially free — it's just the C struct layout in memory or direct WASM function parameters.

For complex messages with strings/lists, canonical ABI requires `cabi_realloc` + per-field allocation. For same-language performance-critical paths, fracpack-encoded bytes can be passed as `list<u8>` through WIT — one allocation, one copy, zero-copy views on the receiving side.

```wit
interface high-perf-api {
    // Simple — WIT is free
    get-balance: func(account-id: u32) -> u64;

    // Complex — fracpack as opaque bytes when both sides are C++
    batch-transfer: func(request: list<u8>) -> list<u8>;
}
```

The host doesn't interpret the payload format — it's just bytes. Serialization is an application-level choice.

---

## 5. Interface Conventions

### Three Interface Types

WIT interface naming and function signatures determine routing semantics:

| Interface Pattern | Return Value | Semantics |
|-------------------|-------------|-----------|
| `*-api` | any | RPC — 1:1, synchronous, routed to specific instance |
| `*-signals`, void functions | none | Async signal — 1:N broadcast, sender continues |
| `*-signals`, with return | has return | Sync signal — 1:N, sender blocks until all slots complete |

```wit
// RPC methods — request/response
interface chat-api {
    get-history: func(room: u32, limit: u32) -> list<message>;
    send-message: func(room: u32, text: string);
}

// Signals — events emitted to connected slots
interface chat-signals {
    // Async — void, fire-and-forget broadcast
    message-sent: func(room: u32, user: u32, text: string);
    typing: func(room: u32, user: u32);

    // Sync — has return, sender blocks until all slots respond
    request-permission: func(room: u32, user: u32) -> bool;
}
```

### Signals and Slots

Inspired by Qt's signals/slots model:

- **Signals** are declared in `*-signals` interfaces — events that get emitted
- **Slots** are any exported method on any interface — any method can receive a signal
- The host connects signals to slots at runtime

A service **imports** a signals interface to emit. A service **exports** any compatible method to receive. The host wires them.

### Instance Handles via WIT Resources

WIT resources serve as opaque instance handles — the "this" pointer for cross-instance calls:

```wit
resource chat-room {
    send: func(user: u32, text: string);
    get-history: func(limit: u32) -> list<message>;
}

interface chat-api {
    open-room: func(room-id: u32) -> chat-room;
    list-rooms: func() -> list<u32>;
}
```

```c
// WASM code — handle is the first parameter, like a C++ this pointer
chat_room room = chat_api_open_room(42);     // host returns handle
chat_room_send(room, user_id, "hello");      // handle routes to specific instance
chat_room_drop(room);                        // release handle
```

The host resolves handles to specific running instances. WASM never sees instance IDs, thread affinity, or routing details.

---

## 6. Signal Resources

Signals are WIT resources with full lifecycle control:

```wit
resource signal {
    emit: func(args: list<u8>);
    connect: func(handler: service-ref);
    disconnect: func(handler: service-ref);
    connect-all: func();  // auto-connect all instances exporting this interface
}

interface psi-signals {
    create: func(interface-name: string, method: string) -> signal;
    create-static: func(interface-name: string, method: string) -> signal;
}
```

### Static vs Instance Signals

| | Static Signal | Instance Signal |
|---|---|---|
| Scope | All instances of a service type | One specific instance |
| New instance comes online | Auto-included | Not affected |
| Create | `psi_signal_create_static(...)` | `psi_signal_create(...)` |
| Analogy | Subscribe to a topic | Follow a specific user |

```c
// Static: receive message-sent from ANY chat service instance
signal global_msgs = psi_signal_create_static("chat-signals", "message-sent");
psi_signal_connect(global_msgs, my_logger);

// Instance: receive events from THIS specific room only
signal room42_msgs = psi_signal_create("chat-signals", "message-sent");
psi_signal_connect(room42_msgs, room42_handler);
```

Static signals are live — when a new instance of the service type starts, it is automatically included as an emitter.

---

## 7. Service Discovery

Host-provided interface available to every service:

```wit
interface psi-discovery {
    // Find by interface — host picks instance (load balanced)
    resolve: func(interface-name: string) -> option<service-ref>;

    // Find all instances implementing an interface
    resolve-all: func(interface-name: string) -> list<service-ref>;

    // Spawn a new instance of a service type
    spawn: func(service-type: string) -> service-ref;

    // Get identity of current caller (inside an RPC handler)
    caller: func() -> service-ref;

    // Watch for instances appearing/disappearing
    watch: func(interface-name: string) -> event-stream;
}

resource service-ref {
    implements: func(interface-name: string) -> bool;
}
```

### Composition Model: Bound, Linked, Connected

Three composition modes with increasing isolation, sharing the same WIT type system:

```
                    shared memory          isolated memory
                 ┌───────────────────┐  ┌──────────────────┐
  by hash        │  BOUND            │  │  (unusual)        │
  (immutable)    │  frozen dep       │  │                   │
                 ├───────────────────┤  ├──────────────────┤
  by name        │  LINKED           │  │  CONNECTED        │
  (mutable)      │  auto-upgradable  │  │  service IPC      │
                 └───────────────────┘  └──────────────────┘
```

#### Bound — pinned to a content hash

The module is linked against a specific version of a library, identified by content hash. The dependency is sealed — upgrading the library does not affect bound consumers. The linked result is stored and cached.

```
User deploys app.o with:  #pragma psi_bind("json-parser", "sha256:a1b2c3...")
Chain produces:           applib.wasm (linked, immutable, cached)
```

This is standard WASM static linking via `wasm-ld`. The library becomes part of the module — shared linear memory, direct `call` instructions, zero overhead. Like linking a `.a` into a binary.

#### Linked — resolved by name, auto-upgradable

The module declares a dependency on a library by name. The chain resolves the name to the current version and links them. When the library is upgraded, all dependent modules are re-linked automatically.

```
User deploys app.o with:  #pragma psi_link("json-parser")
Chain resolves to:        json-parser v2.3 (sha256:d4e5f6...)
Chain produces:           applib.wasm (re-linked on library upgrade)
```

Same static linking mechanics as bound — shared memory, direct calls, zero overhead at runtime. The only difference is that the dependency is mutable. The chain stores the relocatable `.o` and re-links when upstream changes.

#### Connected — separate processes, host-mediated IPC

Separate instances with separate linear memories. Communication goes through the Component Model canonical ABI — host lifts args from sender, lowers into receiver. This is the inter-service communication described in §4-§9 of this document.

```c
service_ref db = psi_resolve("database-service");
service_call(db, QUERY, ...);
```

Like IPC between Unix processes, or COM with apartments. Full sandbox isolation. The host generates **synthetic components** at instantiation to satisfy each service's imports.

#### WASM Relocatable Linking

Both bound and linked modes use WASM's standard relocatable object format. A `.o` file is a normal WASM module with additional custom sections:

- **`linking`** — symbol table (defined/undefined symbols, segment info)
- **`reloc.*`** — relocation entries (patch sites for function indices, memory addresses)
- All indices encoded as **padded LEB128** (5-byte max-width varints) for in-place patching

Linking is a straightforward binary transformation:

1. Parse symbol tables from both modules
2. Resolve references (app's undefined `json_parse` → lib's defined `json_parse`)
3. Concatenate code sections, data segments
4. Renumber all indices (function N in lib becomes N + app_func_count)
5. Patch relocation sites with final values
6. Emit one module

This is mechanically simple (~1000 lines of C for common cases) and fully deterministic — critical for consensus on a blockchain. A **link.wasm** module (the linker itself compiled to WASM) can perform this inside the sandbox, giving crash isolation and resource bounding.

#### Per-Function JIT Caching (No Re-JIT on Re-Link)

JIT compilation is per-function and cached by content. When a library is upgraded and dependents are re-linked:

```
lib.o   →  JIT compile once  →  lib.native   (cached, reused across all consumers)
app.o   →  JIT compile once  →  app.native   (cached per deploy)
                                     ↓
                              native link (build combined function table)
                                     ↓
                              ready to execute
```

Re-linking does **not** require re-JITing the library — its compiled native code is cached and reused. Only new or changed functions are JIT-compiled. The native link step (building a combined function pointer table) is microseconds.

The WASM-level link produces a canonical module for consensus (every node verifies the same bytecode). The native-level JIT is a local per-node optimization — different nodes can use different backends and still agree on results.

#### Summary

| Mode | Resolution | Memory | Overhead | Isolation | Use case |
|------|-----------|--------|----------|-----------|----------|
| Bound | Content hash | Shared | Zero | None | Frozen library dependency |
| Linked | Name | Shared | Zero (re-link on upgrade) | None | Auto-upgradable library |
| Connected | Name | Separate | Copy at boundary | Full sandbox | Inter-service IPC |

The WASM code doesn't know which mode is used — it just calls imported functions. The chain/host handles resolution, linking, and routing.

---

## 8. Cross-Thread Message Ring

### Architecture

Each OS thread has one outbound ring buffer. Messages are bump-allocated into the sender's ring. Receivers read directly from the sender's ring — no receiver-side allocation.

```
Thread 1's ring (sender side):
    [msg for T2][msg for T3][msg for T2][msg for T2][ free ]
     ^─ linked list for T2: msg[0] → msg[2] → msg[3]
     ^─ linked list for T3: msg[1]

Thread 2 (receiver side):
    has no queue — reads directly from Thread 1's ring
```

### Ring Layout

```
Ring buffer: contiguous mmap'd region
Address space: logical 64-bit monotonic (wraps physically via mask)

Message entry:
    [next_ptr: uint64][receiver_id: uint32][payload_len: uint32][payload...]

    next_ptr: next message in per-receiver linked list (64-bit logical address)
```

64-bit logical addresses never wrap in practice (~584 years at 1GB/s). No ABA problems — addresses are monotonically increasing.

### Per-Receiver Linked Lists

Messages for the same receiver are threaded via a linked list within the ring. This avoids linear scanning — the receiver follows its list directly to its messages.

Since the total number of potential receivers is much larger than the number any given sender actually communicates with, per-receiver list heads are stored in a **hash table** rather than a flat array:

```c
struct sender_ring {
    char*    ring_base;
    uint64_t write_head;           // bump allocator, only written by owning thread
    uint64_t ring_size;

    // Per-receiver linked list tails — sparse hash table
    // Key: receiver thread/instance ID
    // Value: { list_head (64-bit addr), list_tail (64-bit addr) }
    // Open-addressing, linear probe — no allocation on insert
    // Single-threaded access (cooperative scheduling) — no atomics needed
    hash_table<receiver_id, list_entry> receiver_lists;
};
```

The hash table is single-threaded (cooperative scheduling means only one process runs at a time per thread) — simple open-addressing with linear probing, no locks, no atomics.

### Write Path (Sender)

Always O(1). No scanning, no checking consumer progress:

```c
// Process on Thread 1 — no contention (cooperative, only one runs at a time)
uint64_t offset = ring->write_head;
ring->write_head += aligned_msg_size;

msg_header* msg = ring_ptr(ring, offset);
msg->receiver_id = target;
msg->payload_len = len;
memcpy(msg->payload, data, len);

// Append to per-receiver linked list
list_entry* entry = ring->receiver_lists.get_or_create(target);
if (entry->tail)
    ring_ptr(ring, entry->tail)->next_ptr = offset;
else
    entry->head = offset;
entry->tail = offset;
msg->next_ptr = 0;  // end of list

// Notify receiver
atomic_fetch_or(&target_thread->has_messages, 1ULL << my_thread_id, release);
write(target_thread->wake_fd, &one_byte, 1);  // wake from kqueue/epoll
```

### Read Path (Receiver)

Host on receiver's thread follows linked list heads:

```c
// Receiver's scheduler — check for cross-thread messages
uint64_t bits = atomic_load(&my_has_messages, acquire);
while (bits) {
    uint32_t sender_tid = __builtin_ctzll(bits);
    sender_ring* sring = &thread_rings[sender_tid];

    // Follow linked list for my thread
    list_entry* entry = sring->receiver_lists.get(my_thread_id);
    while (entry && entry->head) {
        msg_header* msg = ring_ptr(sring, entry->head);

        // Deliver: lift from sender's ring, lower into receiver's WASM memory
        deliver_to_wasm(msg->payload, msg->payload_len, receiver_instance);

        // Advance list head
        entry->head = msg->next_ptr;
        if (entry->head == 0)
            entry->tail = 0;  // list empty
    }

    if (list_empty)
        atomic_fetch_and(&my_has_messages, ~(1ULL << sender_tid), relaxed);

    bits &= ~(1ULL << sender_tid);
}
```

### Reclaim

Consumers publish the last logical address they've consumed. The sender reclaims `min(all consumers)`:

```c
// Per sender ring — one atomic uint64 per consumer
struct sender_ring {
    // ...
    atomic<uint64_t> consumed_by[MAX_THREADS];  // each consumer updates its own slot
};

// Consumer: done processing through this address
sender_ring->consumed_by[my_thread_id].store(last_processed_addr, release);

// Sender: reclaim lazily — host checks when ring space is low
uint64_t min_consumed = UINT64_MAX;
for (int i = 0; i < active_threads; i++)
    min_consumed = min(min_consumed, consumed_by[i].load(relaxed));
reclaim_head = min_consumed;
```

### Backpressure

The ring is the sender's outbound buffer. If a receiver is slow:

1. Messages accumulate in the sender's ring
2. Ring fills up
3. Host blocks the sender's process or returns an error
4. Sender bears the memory cost of its own outbound traffic

A fast producer cannot flood a slow consumer — the consumer has no memory allocated for incoming messages.

### Cross-Thread Notification Summary

| Mechanism | Type | Purpose |
|-----------|------|---------|
| `has_messages` bitmap | `atomic<uint64_t>` per thread | Bit T = "thread T has messages for me" |
| `wake_fd` | eventfd (Linux) / pipe (macOS) | Wake thread from kqueue/epoll |
| `consumed_by[]` | `atomic<uint64_t>` per thread-pair | Consumer progress for ring reclaim |
| Per-receiver linked list | Non-atomic hash table in sender's ring | O(1) message lookup by receiver |

Total atomic surface per thread: one uint64 bitmap read + one uint64 consumed offset write per active sender. All other operations are single-threaded.

---

## 9. Host Delivery

The host is the message broker. It lifts, routes, and lowers all messages.

### Delivery Modes

When a message arrives, the host inspects the target interface and function:

| Pattern | Action |
|---------|--------|
| `*-api` import, with return | RPC: lower args into target, call exported function, lift result, lower into caller, resume caller process |
| `*-api` import, void | RPC: same but no result to return |
| `*-signals` import, void | Async broadcast: lower args into each connected slot's instance, call each slot, don't block sender |
| `*-signals` import, with return | Sync broadcast: call each slot, collect results, merge, return to sender |

### Same-Thread Optimization

When sender and receiver are on the same thread, the host can skip the ring entirely:

1. Lift args from sender's WASM memory
2. Lower directly into receiver's WASM memory (one copy)
3. Switch to receiver process (swapcontext)
4. Receiver handles call, returns result
5. Switch back to sender, lower result into sender's memory
6. Sender resumes

No ring, no notification, no atomic operations. The host detects same-thread targets and takes the fast path transparently.

### Synthetic Components

At instantiation, the host generates a synthetic component for each service's imports. From the WASM's perspective, it's linking against real components:

```
Service imports chat-signals → Host generates:
    ┌─ synthetic component ────────────────────────┐
    │  exports message-sent: func(...)             │
    │  implementation:                              │
    │    lift args from caller's memory             │
    │    for each connected slot:                   │
    │      if same thread: direct process switch    │
    │      if cross thread: write to ring + notify  │
    └──────────────────────────────────────────────┘
```

Standard Component Model linking — nothing custom from the WASM's perspective. All psix routing magic lives inside these generated shims.

Host functions (psix syscalls like `psi-io`, `psi-discovery`) are also components — just implemented in native code rather than WASM. The boundary is the same: WIT interface, canonical ABI. Any host function could be swapped to a WASM implementation (or vice versa) without the caller knowing.

---

## 10. Design Summary

### Data Flow

```
WASM process calls imported function
    → Host lifts args (canonical ABI)
    → Host determines routing:
        Same thread, RPC:     direct process switch, one copy
        Same thread, signal:  call each slot sequentially, one copy each
        Cross thread, RPC:    write to ring → notify → target delivers → result returns
        Cross thread, signal: write to ring × N → notify targets
    → Host lowers result into caller's memory (if sync)
    → Caller process resumes
```

### Memory Architecture

```
Process virtual address space (static after startup):

[native stack pool]                one mmap, guard page stride, bitmap allocation
[WASM instance memory pool]        one mmap per service type, guard page bounded
[thread 1 message ring]            one mmap, 64-bit logical addresses
[thread 2 message ring]            ...
[thread N message ring]            ...
[scheduler/host data]              malloc'd, per-thread

Total mmap calls: ~N+2 at startup. Zero after.
```

### Atomic Coordination (Complete)

| Data | Type | Written by | Read by |
|------|------|-----------|---------|
| Stack pool bitmap | `atomic<uint64_t>[]` | Any thread (CAS) | Any thread |
| `has_messages` | `atomic<uint64_t>` per thread | Sender threads (fetch_or) | Owning thread |
| `consumed_by[]` | `atomic<uint64_t>` per pair | Consumer thread (store) | Host on sender thread (load) |
| `wake_fd` | eventfd/pipe per thread | Any thread (write byte) | Owning thread (kqueue/epoll) |
| Ring `write_head` | Non-atomic uint64 | Owning thread only | — |
| Receiver hash table | Non-atomic | Owning thread only | Receiver threads (read after release fence) |

---

## 11. Class Diagram

```
┌──────────────────────────────────────────────────────────────────┐
│                           Runtime                                │
│──────────────────────────────────────────────────────────────────│
│  registry: ServiceRegistry                                       │
│  stack_pool: NativeStackPool                                     │
│  threads: Thread[]                                               │
│──────────────────────────────────────────────────────────────────│
│  start(num_threads)                                              │
│  load_service(wasm_bytes, wit) -> ServiceType&                   │
│  shutdown()                                                      │
└──────┬──────────────────────┬────────────────────────────────────┘
       │                      │
       ▼                      ▼
┌──────────────────┐  ┌───────────────────────────────────────────┐
│  NativeStackPool │  │          ServiceRegistry                  │
│──────────────────│  │───────────────────────────────────────────│
│  region: void*   │  │  types: map<string, ServiceType>          │
│  bitmap: atomic  │  │  instances: map<handle, Process*>         │
│    <u64>[]       │  │  static_signals: map<string, Signal>      │
│  max_slots: u32  │  │───────────────────────────────────────────│
│──────────────────│  │  register_type(ServiceType)               │
│  alloc() -> id   │  │  resolve(interface) -> ServiceRef         │
│  free(id)        │  │  resolve_all(interface) -> ServiceRef[]   │
│  stack_ptr(id)   │  │  spawn(type, thread) -> ServiceRef        │
└──────────────────┘  └───────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                        ServiceType                               │
│──────────────────────────────────────────────────────────────────│
│  name: string                                                    │
│  compiled_module: psizam::compiled_module                        │
│  host_table: psizam::host_function_table                         │
│  exported_interfaces: vector<string>                             │
│  imported_interfaces: vector<string>                             │
│  signal_interfaces: vector<string>                               │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                          Thread                                  │
│──────────────────────────────────────────────────────────────────│
│  thread_id: u32                                                  │
│  scheduler: Scheduler                                            │
│  ring: MessageRing                                               │
│  processes: vector<Process*>                                     │
│  has_messages: atomic<u64>          // bit per sender thread     │
│  wake_fd: int                       // eventfd or pipe           │
│──────────────────────────────────────────────────────────────────│
│  run()                              // main scheduler loop       │
│  spawn_process(ServiceType&) -> Process*                         │
│  wake()                                                          │
└──────┬──────────────────────┬────────────────────────────────────┘
       │                      │
       ▼                      ▼
┌──────────────────┐  ┌───────────────────────────────────────────┐
│    Scheduler     │  │           MessageRing                     │
│──────────────────│  │───────────────────────────────────────────│
│  ready_queue:    │  │  base: char*                              │
│    Process*[]    │  │  write_head: u64         // non-atomic    │
│  timer_wheel     │  │  capacity: u64                            │
│  io: IoEngine    │  │  receiver_map: HashMap<receiver_id,      │
│──────────────────│  │                         ListEntry>        │
│  run_loop()      │  │  consumed_by: atomic<u64>[]               │
│  push_ready(     │  │───────────────────────────────────────────│
│    Process*)     │  │  write(receiver_id, data, len) -> offset  │
│  drain_messages()│  │  reclaim() -> u64                         │
│  fire_timers()   │  └───────────────────────────────────────────┘
│  wait_for_events()│
└──────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                         Process                                  │
│──────────────────────────────────────────────────────────────────│
│  process_id: u32                                                 │
│  service_type: ServiceType*                                      │
│  state: enum { READY, RUNNING, BLOCKED, DONE }                   │
│  blocked_on: BlockReason          // I/O fd, RPC, poll set       │
│                                                                  │
│  // Execution (one or the other, not both)                       │
│  exec_ctx: psizam::execution_context   // interpreter backend    │
│  native_stack_slot: u32                // JIT backend             │
│  context: ucontext_t                   // for swapcontext (JIT)  │
│                                                                  │
│  // WASM state                                                   │
│  linear_memory: void*                  // WASM linear memory     │
│  fd_table: FdTable                     // per-process fd table   │
│                                                                  │
│  // Signals owned by this process                                │
│  signals: vector<Signal*>                                        │
│──────────────────────────────────────────────────────────────────│
│  resume()                                                        │
│  park(BlockReason)                                               │
│  is_ready() -> bool                                              │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                          Signal                                  │
│──────────────────────────────────────────────────────────────────│
│  interface_name: string                                          │
│  method_name: string                                             │
│  is_static: bool                    // type-scoped vs instance   │
│  slots: vector<SlotConnection>                                   │
│  has_return: bool                   // sync if true              │
│──────────────────────────────────────────────────────────────────│
│  emit(args, len)                                                 │
│  connect(ServiceRef)                                             │
│  disconnect(ServiceRef)                                          │
│  connect_all()                                                   │
└──────────────────────────────────────────────────────────────────┘

┌───────────────────┐  ┌───────────────────────────────────────────┐
│  SlotConnection   │  │           ServiceRef                      │
│───────────────────│  │───────────────────────────────────────────│
│  target: ServiceRef│ │  handle: u32               // opaque      │
│  method: string   │  │───────────────────────────────────────────│
│  thread_id: u32   │  │  // Resolved by host to:                  │
│  process: Process*│  │  thread: Thread*                          │
└───────────────────┘  │  process: Process*                        │
                       └───────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                    IoEngine (abstract)                            │
│──────────────────────────────────────────────────────────────────│
│  wait(timeout_ms) -> Event[]                                     │
│  register_fd(fd, events)                                         │
│  unregister_fd(fd)                                               │
│──────────────────────────────────────────────────────────────────│
│       ┌──────────────┐         ┌───────────────┐                 │
│       │ KqueueEngine │         │ UringEngine   │                 │
│       │ (macOS)      │         │ (Linux)       │                 │
│       └──────────────┘         └───────────────┘                 │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                         FdTable                                  │
│──────────────────────────────────────────────────────────────────│
│  entries: FdEntry[MAX_FDS]                                       │
│──────────────────────────────────────────────────────────────────│
│  alloc(FdEntry) -> fd                                            │
│  get(fd) -> FdEntry&                                             │
│  close(fd)                                                       │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│                     FdEntry (variant)                             │
│──────────────────────────────────────────────────────────────────│
│  SocketFd   { real_fd: int, rx_ring: RingRef, tx_ring: RingRef } │
│  TimerFd    { interval_ms: int, next_fire: timespec }            │
│  SignalFd   { signal: Signal* }                                  │
│  ChannelFd  { peer: ServiceRef, ring: RingRef }                  │
└──────────────────────────────────────────────────────────────────┘

┌──────────────────────────────────────────────────────────────────┐
│              MessageRing::HashMap<K, ListEntry>                  │
│──────────────────────────────────────────────────────────────────│
│  // Open-addressing, linear probe                                │
│  // Single-threaded (cooperative) — no atomics                   │
│  // Only stores active receivers, sparse                         │
│──────────────────────────────────────────────────────────────────│
│  struct ListEntry {                                              │
│      receiver_id: u32                                            │
│      head: u64          // logical ring offset of first msg      │
│      tail: u64          // logical ring offset of last msg       │
│  }                                                               │
│──────────────────────────────────────────────────────────────────│
│  get_or_create(receiver_id) -> ListEntry&                        │
│  get(receiver_id) -> ListEntry*                                  │
│  remove(receiver_id)                                             │
└──────────────────────────────────────────────────────────────────┘
```

### Ownership Hierarchy

```
Runtime
 ├── NativeStackPool (global, one)
 ├── ServiceRegistry (global, one)
 │    ├── ServiceType[] (one per loaded WASM module)
 │    └── StaticSignal[] (one per *-signals interface)
 └── Thread[] (one per OS thread)
      ├── Scheduler
      │    ├── IoEngine (kqueue or io_uring)
      │    ├── ready_queue: Process*[]
      │    └── timer_wheel
      ├── MessageRing (outbound, one per thread)
      │    └── HashMap<receiver_id, ListEntry>
      └── Process[] (many per thread — the unit of scheduling)
           ├── execution_context or native_stack (from pool)
           ├── linear_memory (from instance memory pool)
           ├── FdTable (per-process)
           └── Signal[] (instance-scoped signals)
```

Key relationships:
- `Runtime` owns everything, creates threads at startup
- `Thread` runs many `Process`es cooperatively — like a CPU core running many single-threaded programs
- `Process` is the unit of scheduling — one execution context, one WASM stack, one native stack
- Each `Process` is a single-threaded WASM instance — no internal concurrency
- `Scheduler` picks which ready process to run next
- `MessageRing` is the thread's outbound buffer — written by whichever process is currently running on that thread, read cross-thread via linked list heads
