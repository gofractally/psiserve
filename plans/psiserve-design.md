# psiserve Design

psiserve is a microkernel operating system for WebAssembly services, exposed through a POSIX-like API. The design mirrors Unix conventions wherever possible — a developer who has written a Unix daemon can write a psiserve service without reading docs.

## POSIX Mapping

| Unix | psiserve | Notes |
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
| Kernel | Host runtime | Scheduler, I/O engine, page management |

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

The host manages the lifecycle: listen on port, accept connection, COW fork from template (reset ring cursors, zero-map stack pages, bind fd 0 to new socket), call `_start()`. The service doesn't know about networking — it inherits its environment, just like an inetd child process.

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

Services allocate ring buffers in their own linear memory and tell the host where they are. The host passes those addresses directly to kernel read()/write() syscalls — data moves kernel <-> WASM memory with no intermediate buffer.

For advanced services, the ring can be read/written directly (pure memory access, no host call) with host calls only needed to yield when the ring is empty/full.

## Implementation

### Fibers

Each service instance has one or more fibers. A fiber is a cooperative execution context — it runs until it makes a blocking host call, then yields to the scheduler.

**Interpreter backend:** The WASM operand stack, call stack, PC, and locals are all fields in an `execution_context` struct. Yielding = returning from `execute()`. Resuming = calling `execute()` again with the same struct. No native stack involvement.

**JIT backend:** WASM calls compile to native call instructions, so the native stack holds WASM frames. Each fiber gets its own mmap'd native stack region (~64KB + guard page). Yielding = `swapcontext()` (~6 instructions). Resuming = swap back.

### Spawning Fibers

When a service needs a new fiber (e.g., to handle an incoming RPC while the main handler is suspended), the host:

1. Maps zero-fill anonymous pages over the stack region of linear memory
2. Creates a new execution context pointing to the same heap/globals
3. Calls the target function on the new fiber

The new fiber shares the instance's heap and globals but has its own private stack pages. No copying of any kind — just an `mmap` to zero the stack region.

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

The host is a syscall proxy. It passes WASM-specified buffer addresses directly to kernel I/O calls:

```cpp
// WASM said its ring buffer is at offset 0x9000 in linear memory
char* buf = wasm_instance->linear_memory + ring->buf;

// Kernel reads/writes directly into WASM memory
read(real_fd, buf + (ring->head & mask), available);
```

**macOS:** kqueue for readiness notification + read()/write() with WASM memory pointers. 2 copies per direction (kernel <-> WASM, both unavoidable).

**Linux:** io_uring for batched async I/O. Registered buffers for pinned WASM memory. IORING_OP_SEND_ZC for zero-copy send. 1 copy for sends, 2 for receives.

The platform difference is abstracted behind an `io_engine` interface. The WASM API is identical on both.

### Inter-Service Calls

`service_call()` is synchronous from the caller's perspective. The implementation depends on where the target service lives:

**Same OS thread:**
1. Caller writes args into shared heap (or they're already there)
2. Host maps zero pages over target's stack region
3. Host spawns a new fiber on the target instance
4. Target function runs, reads args from shared memory, writes result
5. Target fiber completes, original stack pages restored
6. Caller's fiber resumes, reads result

Zero copies. Two mmap calls. One or two fiber context switches.

**Different OS thread:**
1. Host copies args into target thread's mailbox queue (memcpy)
2. Host wakes target thread (eventfd/pipe)
3. Target thread's scheduler spawns fiber, dispatches call
4. Result copied back to caller's thread
5. Caller's fiber resumes

One copy per direction. This is the minimum for safe cross-thread communication without locks on the data. This is the only mandatory copy in the system.

**The developer never knows which path is taken.** `service_call()` blocks, returns the result. The runtime picks the optimal path.

### Inter-Service Channels

For streaming communication between services, channels use shared-memory ring buffers with asymmetric page permissions:

- Service A's tx ring: RW in A's linear memory, RO in B's linear memory
- Service B's tx ring: RW in B's linear memory, RO in A's linear memory

Same physical pages, different permissions per mapping. Hardware-enforced: writing to a read-only mapping triggers SIGSEGV. No runtime checks needed.

Each side writes two values (its tx head, its rx tail) and reads two values from the other side (their tx head, their rx tail). Standard SPSC ring buffer protocol.

Cross-thread channels require the host to mediate (copy between thread-local memories) rather than direct page sharing.

### COW Instance Forking

New connections are served by COW-forking a template WASM instance:

1. Template instance is loaded and initialized (run `_init()`, populate caches, etc.)
2. On new connection, `mmap` the template's linear memory pages as MAP_PRIVATE
3. The new instance shares all pages until it writes — then COW gives it a private copy
4. Stack pages are mapped as zero-fill (fresh stack for the new connection)

Cost: one mmap call. No copying until the instance actually mutates data. Read-only pages (code caches, config, static data) are shared across all forked instances forever.

### Interface Definitions (WIT)

Service interfaces are defined using the WebAssembly Component Model's WIT format:

```wit
interface balance-api {
    get-balance: func(account-id: u32) -> u64;
    transfer: func(from: u32, to: u32, amount: u64) -> result<_, string>;
}
```

Tooling generates caller stubs and dispatch functions. The canonical ABI defines the memory layout for arguments and results. For same-thread calls, args are laid out in shared memory and read directly by the callee — no lift/lower through the host.

## Design Principles

1. **Simple things are simple.** A basic HTTP service is sequential blocking code. No event loops, no callbacks, no state machines.

2. **POSIX mental model.** Developers already know read/write/poll/connect. Same patterns, same semantics, sandboxed execution.

3. **Copies only at boundaries.** Zero copies within a thread. One copy at thread boundaries. Two copies at the kernel boundary (unavoidable). Nothing else.

4. **Hardware-enforced isolation.** Guard pages for memory bounds. RO mappings for inter-service channels. SIGSEGV on violation. No runtime checks on the hot path.

5. **Explicit yield points.** Context switches only occur inside host function calls. The developer always knows when interleaving can happen. No hidden preemption.

6. **Everything is an fd.** Sockets, timers, channels, service connections — all managed through the same fd-based API, all waited on through `ps_poll()`.
