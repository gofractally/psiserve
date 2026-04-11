# psiserve Design

psiserve is a high-performance WebAssembly TCP application server. It is built from four components:

```
┌──────────────────────────┐
│  Service (user code)     │  application
│  #include <psi.h>        │
├──────────────────────────┤
│  libpsi                  │  libc — thin WASM-side stubs + ring helpers
├──────────────────────────┤
│  psix                    │  kernel — scheduler, fibers, COW, I/O engine,
│                          │  fd table, page management (simulated POSIX)
├──────────────────────────┤
│  psizam                  │  CPU — WASM engine (interpreter, JIT)
├──────────────────────────┤
│  Linux / macOS           │  hardware — kqueue, io_uring, mmap
└──────────────────────────┘
```

- **psiserve** — the product (server binary, the whole stack)
- **libpsi** — WASM-side library services link against (`#include <psi.h>`)
- **psix** — the kernel: simulated POSIX for WASM (scheduler, fibers, COW forking, I/O engine, fd table, page management)
- **psizam** — the CPU: WASM engine (parser, interpreter, JIT, execution contexts)

The design mirrors Unix conventions wherever possible — a developer who has written a Unix daemon can write a psiserve service without reading docs.

## POSIX Mapping

| Unix | psix | Notes |
|---|---|---|
| Process | WASM service instance | Isolated linear memory, own fd table |
| Address space | WASM linear memory | Guard-page protected |
| Thread | Fiber | Cooperative, explicit yield points |
| fork() | COW fork template instance | mmap, not memcpy |
| exec() | `_start()` | Per-connection entry point |
| `_init()` / constructors | `_init()` | Template setup, runs once |
| stdin/stdout/stderr | fd 0 / fd 1 / fd 2 | Socket, log output, error output |
| read()/write() | `ps_read()`/`ps_write()` | Blocking by default, yields fiber |
| fcntl(O_NONBLOCK) | `ps_set_nonblock()` | Returns -EAGAIN instead of yielding |
| poll()/select() | `ps_poll()` | Wait on multiple fds |
| connect() | `ps_connect()` | Returns new fd |
| close() | `ps_close()` | |
| pipe() | `ps_channel_create()` | Shared-memory ring buffer |
| Syscall | Host function call | No kernel trap, just a function call |
| inetd | The runtime itself | Listens, accepts, forks, binds fd 0 |
| kill(pid, sig) | `service_call()` | Synchronous RPC |
| Kernel | psix | Scheduler, I/O engine, page management |

## Service Lifecycle

A service is a compiled WASM module with two entry points:

- `_init()` — runs once on the template instance. Allocates buffers, binds rings, warms caches.
- `_start()` — runs per connection. fd 0 is the client socket, fd 1/2 are log/error output. Rings are allocated and clean. Just do I/O.

```c
void _init() {
    ps_bind_ring(0, PS_READ,  &rx);
    ps_bind_ring(0, PS_WRITE, &tx);
    load_config();
    warm_cache();
}

void _start() {
    // fd 0 is live, rings are ready, caches are warm (COW shared)
    http_request req = ps_read_request(0);
    db_result r = service_call(DB_SERVICE, QUERY, &req);
    ps_write_response(0, format(r));
}
```

psix manages the lifecycle: listen on port, accept connection, COW fork from template (reset ring cursors, zero-map stack pages, bind fd 0 to new socket), call `_start()`. The service doesn't know about networking — it inherits its environment, just like an inetd child process.

Services can also be long-lived daemons (timers, watchers, background workers) that aren't tied to a single connection.

## API Surface

### I/O — POSIX-style, blocking by default

```c
int  ps_read(int fd, void* buf, int len);        // blocks (yields fiber) until data ready
int  ps_write(int fd, void* buf, int len);        // blocks until written
int  ps_connect(const char* addr, int port);      // blocks until connected, returns fd
void ps_close(int fd);
void ps_set_nonblock(int fd);                     // subsequent calls return -EAGAIN
```

### Multiplexing — for services with multiple fds

```c
typedef struct {
    int fd;
    int events;    // PS_READABLE | PS_WRITABLE | PS_ERROR
    int revents;   // filled by host on return
} ps_event_t;

int ps_poll(ps_event_t* events, int count, int timeout_ms);
```

### Timers

```c
int ps_timer_create(int interval_ms, int flags);  // returns fd
// Timer fires show up as PS_READABLE on the timer fd
```

### Inter-Service Communication

```c
// Synchronous RPC — blocks until target returns
int service_call(int service_id, int method, void* args, int args_len,
                 void* result, int result_len);

// Channels — bidirectional ring buffers between services
int ps_channel_create(int buf_size);               // returns fd
int ps_channel_connect(int service_id, int port);  // returns fd
// Then use ps_read/ps_write/ps_poll on the channel fd
```

### Ring Buffer Registration — for zero-copy I/O

```c
typedef struct {
    uint32_t head;     // writer advances
    uint32_t tail;     // reader advances
    uint32_t buf;      // pointer to data buffer in linear memory
    uint32_t size;     // buffer size (power of 2)
} ps_ring_t;

void ps_bind_ring(int fd, int direction, ps_ring_t* ring);
```

Services allocate ring buffers in their own linear memory and tell psix where they are. psix passes those addresses directly to kernel read()/write() syscalls — data moves kernel <-> WASM memory with no intermediate buffer.

For advanced services, the ring can be read/written directly (pure memory access, no host call) with host calls only needed to yield when the ring is empty/full.

## Implementation

### Fibers

Each service instance has one or more fibers. A fiber is a cooperative execution context — it runs until it makes a blocking host call, then yields to the psix scheduler.

**Interpreter backend:** The WASM operand stack, call stack, PC, and locals are all fields in an `execution_context` struct. Yielding = returning from `execute()`. Resuming = calling `execute()` again with the same struct. No native stack involvement.

**JIT backend:** WASM calls compile to native call instructions, so the native stack holds WASM frames. Each fiber gets its own mmap'd native stack region (~64KB + guard page). Yielding = `swapcontext()` (~6 instructions). Resuming = swap back.

### Spawning Fibers

When a service needs a new fiber (e.g., to handle an incoming RPC while the main handler is suspended), psix:

1. Maps zero-fill anonymous pages over the WASM stack region of linear memory
2. Creates a new execution context pointing to the same heap/globals
3. Calls the target function on the new fiber

The new fiber shares the instance's heap and globals but has its own private stack pages (zero-mapped, not COW — the new fiber starts with a fresh stack). No copying of any kind — just an `mmap`.

When the fiber completes, the original stack pages are restored via `mmap`.

### Cooperative Scheduling

Between yields, a fiber has exclusive access to its instance's memory. No locks, no atomics, no surprises. The developer's mental model: "between host calls, I own everything."

Yield points are explicit and visible — they only occur inside `ps_read`, `ps_write`, `ps_poll`, `service_call`, and similar host functions. The developer always knows when a context switch can happen.

### The Scheduler (per OS thread)

```
loop:
    now = clock()
    fire expired timers -> push fibers to ready_queue

    timeout = next_timer - now (or infinity)
    events = kqueue_wait(timeout)  // or io_uring

    for each event:
        move data between kernel and WASM ring buffers
        push owning fiber to ready_queue

    while ready_queue not empty:
        fiber = ready_queue.pop()
        resume fiber  // runs until next yield
```

### I/O Engine

psix is a syscall proxy. It passes WASM-specified buffer addresses directly to kernel I/O calls:

```cpp
// WASM said its ring buffer is at offset 0x9000 in linear memory
char* buf = wasm_instance->linear_memory + ring->buf;

// Kernel reads/writes directly into WASM memory
read(real_fd, buf + (ring->head & mask), available);
```

**macOS:** kqueue for readiness notification + read()/write() with WASM memory pointers. 2 copies per direction (kernel <-> WASM, both unavoidable).

**Linux:** io_uring for batched async I/O. Registered buffers for pinned WASM memory. IORING_OP_SEND_ZC for zero-copy send. 1 copy for sends, 2 for receives.

The platform difference is abstracted behind an `io_engine` interface. The WASM API is identical on both.

### Inter-Service Communication

> **See [psix IPC Design](psix-ipc-design.md) for the full design.**

All inter-service communication is host-mediated message passing using WIT canonical ABI. There is no shared WASM memory between instances. The host lifts args from the sender's memory, routes the call, and lowers args into the receiver's memory.

**Key design decisions:**

- **One instance per service per thread** — fibers within an instance share heap (direct in-memory communication between connections), but instances on different threads are fully isolated
- **Everything is a typed function call** — RPC, signals, and events all reduce to calling WIT-defined interface methods. `*-api` interfaces are RPC (1:1, sync), `*-signals` interfaces are events (1:N, async by default, sync if return value)
- **Signals/slots model** — signals are WIT resources with `connect`/`disconnect`/`emit`. Static signals scope to a service type, instance signals scope to a specific instance
- **Per-thread outbound ring buffer** — sender bump-allocates messages into local ring, receivers read directly from sender's ring (zero receiver-side allocation). Cross-thread notification via atomic `has_messages` bitmap + `wake_fd`
- **Host owns all routing** — same-thread calls are direct fiber switches (one copy), cross-thread calls go through the ring. The WASM code never knows which path is taken

### COW Instance Forking

COW forking is used for **scaling** — forking a service instance to another OS thread when one thread saturates — not for per-connection isolation. Within a thread, connections are fibers sharing one instance.

1. Template instance is loaded and initialized (run `_init()`, populate caches, etc.)
2. On scaling event, `memcpy` the template's hot pages into a pre-allocated instance slot
3. Stack pages reset via `madvise(MADV_DONTNEED)` (no VMA churn)
4. New instance on target thread begins handling connections independently

> **See [psix IPC Design](psix-ipc-design.md) §2** for memory pool allocation strategy (pre-allocated pools, bitmap + CAS, static VMA tree).

### Interface Definitions (WIT)

Service interfaces are defined using the WebAssembly Component Model's WIT format:

```wit
interface balance-api {
    get-balance: func(account-id: u32) -> u64;
    transfer: func(from: u32, to: u32, amount: u64) -> result<_, string>;
}
```

Tooling generates caller stubs and dispatch functions. The canonical ABI defines the memory layout for arguments and results. For same-thread calls, args are laid out in shared memory and read directly by the callee — no lift/lower through the host.

### Module Composition: Bound, Linked, Connected

Three modes of composing modules, with increasing isolation:

| Mode | Resolution | Memory | Overhead | Isolation | Analogy |
|------|-----------|--------|----------|-----------|---------|
| **Bound** | Content hash | Shared | Zero | None | Static `.a` library |
| **Linked** | Name | Shared | Zero (re-link on upgrade) | None | Shared `.so` library |
| **Connected** | Name | Separate | Copy at boundary | Full sandbox | Unix IPC / COM apartments |

**Bound** — the module links against a specific version (content hash) of a library. The dependency is sealed. Standard WASM static linking via `wasm-ld` on relocatable `.o` files. Shared linear memory, direct `call` instructions, zero runtime overhead.

**Linked** — the module declares a dependency by name. The runtime resolves to the current version and links them. When the library is upgraded, dependents are automatically re-linked. Same static linking mechanics as bound, but with mutable resolution.

**Connected** — separate instances with separate linear memories. Communication through Component Model canonical ABI (host lifts/lowers at boundaries). Full sandbox isolation. This is how services communicate at runtime — described in the [psix IPC design](psix-ipc-design.md).

Both bound and linked modes use WASM's relocatable object format (`.o` files with `linking` and `reloc.*` custom sections). All indices are padded LEB128 for in-place patching. A **link.wasm** module (the linker compiled to WASM) can perform linking inside the sandbox — deterministic, crash-isolated, and cacheable. JIT compilation is per-function and cached by content, so re-linking after a library upgrade does not require re-JITing unchanged functions.

> **See [psix IPC Design](psix-ipc-design.md) §7** for full details on the composition model, relocatable linking mechanics, and per-function JIT caching.

## Detailed Design Documents

- **[psix IPC, Fiber, and Memory Design](psix-ipc-design.md)** — Inter-process communication (signals/slots, ring buffers, cross-thread coordination), fiber allocation (bitmap + CAS pools, static VMA tree), WIT interface conventions (`*-api` for RPC, `*-signals` for events), service discovery, and host-managed message routing. Supersedes the inter-service communication sections below where they conflict.
- **[psizam Architecture Evolution](llvm-backend-design.md)** — WASM engine refactoring: decoupled compile/execute, LLVM backend, sandboxed compilation, code caching.
- **[Master Implementation Plan](master-plan.md)** — Layered build plan from scaffold to work-stealing.

## Design Principles

1. **Simple things are simple.** A basic HTTP service is sequential blocking code. No event loops, no callbacks, no state machines.

2. **POSIX mental model.** Developers already know read/write/poll/connect. Same patterns, same semantics, sandboxed execution.

3. **Copies only at boundaries.** Zero copies within a thread. One copy at thread boundaries. Two copies at the kernel boundary (unavoidable). Nothing else.

4. **Hardware-enforced isolation.** Guard pages for memory bounds. RO mappings for inter-service channels. SIGSEGV on violation. No runtime checks on the hot path.

5. **Explicit yield points.** Context switches only occur inside host function calls. The developer always knows when interleaving can happen. No hidden preemption.

6. **Everything is an fd.** Sockets, timers, channels, service connections — all managed through the same fd-based API, all waited on through `ps_poll()`.

## Performance: HTTP + Database Request Path

### Copy analysis: standard web stack vs psiserve

A typical request through nginx → app server → database:

```
                                     Standard stack        psiserve
                                     ──────────────        ────────
NIC DMA → kernel buf                 copy 1 (unavoidable)  copy 1 (unavoidable)
kernel → proxy (nginx)               copy 2 (read buf)     —
proxy → upstream socket              copy 3 (write buf)    —
kernel → app server                  copy 4 (read buf)     copy 2 (WASM rx ring)
parse HTTP                           copy 5 (string alloc) zero (parse from ring)
app → DB (serialize query)           copy 6 (protocol)     —
kernel round-trip to DB process      copy 7, 8 (IPC)       —
DB result → app (deserialize)        copy 9 (protocol)     copy 3 (DB mem → HTTP mem)
format response                      copy 10 (render buf)  zero (write into tx ring)
app → proxy                          copy 11 (write)       —
proxy → client                       copy 12 (write)       copy 4 (WASM tx ring)
kernel → NIC DMA                     copy 13 (unavoidable) copy 5 (unavoidable)

Total:                               ~13 copies            5 copies
```

The standard stack pays for: reverse proxy hop (2 copies), kernel IPC to a separate DB process (2 copies), serialization at every boundary (protocol buffers, JSON, SQL wire format), and intermediate buffers at each layer.

psiserve eliminates these because:
- No reverse proxy — WASM services handle connections directly
- No DB process boundary — the database is a WASM service in the same address space
- No serialization — service calls pass data through shared memory, not wire protocols
- No intermediate buffers — kernel reads/writes directly into WASM ring buffers

With same-thread shared-page channels between HTTP and DB services, copy 3 is also eliminated (DB writes result into shared ring, HTTP reads directly). Total: 4 copies, all at unavoidable hardware/kernel boundaries.

### Concurrency model

Compared to standard approaches:

| | Thread-per-connection (Apache) | Event loop (Node.js) | Goroutines (Go) | psiserve |
|---|---|---|---|---|
| Connections | ~10K (thread limit) | ~100K (single thread) | ~100K (goroutines) | ~100K (fibers) |
| Instance cost | ~8MB stack | shared state | ~4KB stack | ~64KB stack + COW pages |
| Isolation | none (shared process) | none (shared heap) | none (shared process) | full (WASM sandbox) |
| DB access | serialize over socket | serialize over socket | serialize over socket | direct memory read |
| Context switch | ~1-5μs (kernel) | N/A (callbacks) | ~200ns (runtime) | ~2ns (swapcontext) |

Key differences:
- **Per-instance fibers:** Multiple connections share one instance via cooperative fibers. Direct in-memory communication between connections (no pub/sub needed). Full isolation between service types.
- **Database access:** Native host function calls to the database — always live, shared across all threads. No serialization, no socket, no query parsing.
- **Database writes:** Serialized through cooperative scheduling — one writer at a time, no locks, no contention, no CAS loops.
- **Read scaling:** COW fork service instances to additional OS threads when one thread saturates. Each fork is a consistent snapshot.
