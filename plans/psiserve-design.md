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
| read()/write() | `psi_read()`/`psi_write()` | Blocking by default, yields process |
| fcntl(O_NONBLOCK) | `psi_set_nonblock()` | Returns -EAGAIN instead of yielding |
| poll()/select() | `psi_poll()` | Wait on multiple fds |
| bind() | `psi_bind()` | Bind to port, returns fd |
| listen() | `psi_listen()` | Mark fd as listening |
| accept() | `psi_accept()` | Returns new fd + peer address |
| connect() | `psi_connect()` | Returns new fd |
| shutdown() | `psi_shutdown()` | Half-close (read/write/both) |
| close() | `psi_close()` | |
| getpeername() | `psi_getpeername()` | Peer IP/port for any socket fd |
| pipe() | `psi_channel_create()` | Bidirectional ring buffer |
| clock_gettime() | `psi_clock()` | Monotonic time in nanoseconds |
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
    psi_bind_ring(0, PSI_READ,  &rx);
    psi_bind_ring(0, PSI_WRITE, &tx);
    load_config();
    warm_cache();
}

void _start() {
    // fd 0 is live, rings are ready, caches are warm (COW shared)
    http_request req = psi_read_request(0);
    db_result r = service_call(DB_SERVICE, QUERY, &req);
    psi_write_response(0, format(r));
}
```

psix manages the lifecycle: listen on port, accept connection, COW fork from template (reset ring cursors, zero-map stack pages, bind fd 0 to new socket), call `_start()`. The service doesn't know about networking — it inherits its environment, just like an inetd child process.

Services can also be long-lived daemons (timers, watchers, background workers) that aren't tied to a single connection.

## API Surface

### Architecture: Host Primitives + WASI Standard Interfaces

psiserve is a microkernel. The host provides only raw primitives (`psi:*` interfaces). Higher-level capabilities are standard WASI interfaces (`wasi:*`) implemented by composable system services — not baked into the host.

```
User service                 System service              Host (psix)
────────────                 ──────────────              ───────────
imports wasi:filesystem  →   exports wasi:filesystem
                             imports psi:host-fs      →  OS file I/O
imports wasi:sockets     →   exports wasi:sockets
                             imports psi:host-net     →  OS socket I/O
imports wasi:clocks      →   exports wasi:clocks
                             imports psi:host-clock   →  OS clock
imports wasi:random      →   exports wasi:random
                             imports psi:host-random  →  OS entropy
```

User services import standard WASI interfaces — the same code runs on Wasmtime, Wasmer, or psiserve. The difference is what's behind the interface: on Wasmtime it's direct host calls, on psiserve it's composable services backed by privileged host primitives. The WAC composition controls which implementation backs each interface.

### Privilege Model

Host primitives (`psi:*`) are privileged — only system services can import them. User services only see standard WASI interfaces, mediated by system services. A service's WIT world declaration determines what it can access:

```wit
// System service — privileged, gets raw host access
world fs-service {
    import psi:host-fs/types@0.1.0;           // privileged
    export wasi:filesystem/types@0.2.0;        // provides sandboxed FS
}

// User service — only standard WASI, no host access
world echo-service {
    import wasi:filesystem/types@0.2.0;        // sandboxed via fs-service
    import wasi:sockets/tcp@0.2.0;             // sandboxed via net-service
}
```

### Host Primitives (`psi:*`)

These are the raw host functions — the "syscall" layer. Available to system services and services that the operator explicitly grants access to.

#### I/O — fd-based, blocking by default

```c
int  psi_read(int fd, void* buf, int len);        // blocks (yields process) until data ready
int  psi_write(int fd, void* buf, int len);        // blocks until written
int  psi_readv(int fd, psi_iov_t* iov, int count); // scatter read
int  psi_writev(int fd, psi_iov_t* iov, int count);// gather write
void psi_close(int fd);
void psi_set_nonblock(int fd);                     // subsequent calls return -EAGAIN
```

#### Sockets

```c
int  psi_bind(int port);                           // create + bind, returns listen fd
int  psi_listen(int fd, int backlog);              // mark as listening
int  psi_accept(int fd, psi_addr_t* peer);         // blocks, returns fd + peer address
int  psi_connect(const char* addr, int port);      // blocks until connected, returns fd
int  psi_shutdown(int fd, int how);                // PSI_SHUT_RD, PSI_SHUT_WR, PSI_SHUT_RDWR
int  psi_getpeername(int fd, psi_addr_t* addr);    // peer IP/port for any socket fd

typedef struct {
    uint8_t  ip[16];    // IPv4-mapped-IPv6 or IPv6
    uint16_t port;
    uint8_t  family;    // PSI_AF_INET4, PSI_AF_INET6
} psi_addr_t;
```

#### Multiplexing

```c
typedef struct {
    int fd;
    int events;    // PSI_READABLE | PSI_WRITABLE | PSI_ERROR
    int revents;   // filled by host on return
} psi_event_t;

int psi_poll(psi_event_t* events, int count, int timeout_ms);
```

#### Timers & Clock

```c
int      psi_timer_create(int interval_ms, int flags);  // returns fd
int      psi_timer_set(int fd, int interval_ms);        // modify existing timer
uint64_t psi_clock(void);                                // monotonic nanoseconds
// Timer fires show up as PSI_READABLE on the timer fd
```

#### Host Filesystem (privileged)

```c
int  psi_host_open(const char* path, int flags, int mode);
int  psi_host_stat(const char* path, psi_stat_t* st);
int  psi_host_readdir(int fd, psi_dirent_t* entries, int max);
int  psi_host_mkdir(const char* path, int mode);
int  psi_host_unlink(const char* path);
int  psi_host_rename(const char* old_path, const char* new_path);
// read/write/close use standard psi_read/psi_write/psi_close on returned fd
```

#### Environment

```c
const char* psi_getenv(const char* key);           // sandboxed — only forwarded vars
```

#### Inter-Service Communication

```c
// Synchronous RPC — blocks until target returns
int service_call(int service_id, int method, void* args, int args_len,
                 void* result, int result_len);

// Channels — bidirectional ring buffers between services
int psi_channel_create(int buf_size);               // returns fd
int psi_channel_connect(int service_id, int port);  // returns fd
// Then use psi_read/psi_write/psi_poll on the channel fd
```

#### Ring Buffer Registration — for zero-copy I/O

```c
typedef struct {
    uint32_t head;     // writer advances
    uint32_t tail;     // reader advances
    uint32_t buf;      // pointer to data buffer in linear memory
    uint32_t size;     // buffer size (power of 2)
} psi_ring_t;

void psi_bind_ring(int fd, int direction, psi_ring_t* ring);
```

Services allocate ring buffers in their own linear memory and tell psix where they are. psix passes those addresses directly to kernel read()/write() syscalls — data moves kernel <-> WASM memory with no intermediate buffer.

For advanced services, the ring can be read/written directly (pure memory access, no host call) with host calls only needed to yield when the ring is empty/full.

### Standard Interfaces (`wasi:*`)

These are implemented by composable system services, not the host. User services import them; the WAC composition determines what backs them.

| WASI Interface | Backed by | System service imports |
|---|---|---|
| `wasi:filesystem` | Filesystem service | `psi:host-fs` |
| `wasi:sockets` | Network service | `psi:host-net` (or direct `psi_bind`/`psi_connect`) |
| `wasi:clocks` | Clock service | `psi:host-clock` (or direct `psi_clock`) |
| `wasi:random` | Random service | `psi:host-random` |
| `wasi:cli/environ` | Host directly | `psi_getenv` |

For simple deployments, the operator can grant `psi:*` interfaces directly to user services (skip the system service layer). For multi-tenant or untrusted code, system services mediate all access. The WAC composition controls which model is used — the service code is identical either way.

### Dynamic WASM Management (`psi:wasm`)

Core host interface for validating, compiling, linking, and instantiating WASM modules at runtime. This is what makes psiserve a platform that can host blockchain runtimes, plugin systems, or any application that receives and executes WASM from external sources.

```wit
interface psi-wasm {
    // Validate — check module is well-formed, imports are satisfiable
    validate: func(module: list<u8>) -> result<module-info, validation-error>;

    // Link — combine module with dependencies (bound/linked)
    link: func(module: list<u8>, deps: list<link-dep>) -> result<list<u8>, link-error>;

    // Compile — JIT compile a validated module, cache native code
    compile: func(
        module: list<u8>,
        mode: exec-mode,
    ) -> result<compiled-module, compile-error>;

    // Instantiate — create a running service from compiled module
    instantiate: func(
        compiled: compiled-module,
        config: instance-config,
    ) -> result<service-ref, instance-error>;

    // Teardown
    stop: func(service: service-ref) -> result<_, string>;
    unload: func(service: service-ref) -> result<_, string>;
}

record link-dep {
    name: string,
    module: list<u8>,
}

record module-info {
    imports: list<string>,
    exports: list<string>,
    memory-min: u32,
    memory-max: option<u32>,
}

record instance-config {
    name: string,
    env: list<tuple<string, string>>,
    fds: list<named-fd>,
    exec-mode: exec-mode,
    instruction-limit: option<u64>,
    thread-placement: thread-placement,
}

enum exec-mode { objective, subjective }

// Thread placement — control where instances run
variant thread-placement {
    // Host decides (default — distributes across threads)
    auto,
    // Pin to a specific thread
    pin(thread-id),
    // Pin to same thread as another service (for fast same-thread IPC)
    colocate(service-ref),
    // Spread N instances across N threads (one per thread)
    spread(u32),
    // Restrict to a set of threads
    affinity(list<thread-id>),
}

resource compiled-module {}
```

```wit
// Thread introspection
interface psi-threads {
    // How many OS threads are running
    thread-count: func() -> u32;
    // Which thread is the caller on
    current-thread: func() -> thread-id;
    // Move a service to a different thread
    migrate: func(service: service-ref, target: thread-id) -> result<_, string>;
}
```

Thread placement matters because same-thread communication is a direct process switch (one copy, no atomics), while cross-thread goes through the message ring. A blockchain consensus service uses this to:

- **Pin** the block producer to a dedicated thread (no scheduling contention)
- **Colocate** services that call each other heavily (same-thread fast path)
- **Spread** HTTP instances across all threads (max parallelism)
- **Isolate** untrusted user contracts from system services (separate threads)

```c
// Consensus service deploying a user contract
psi_wasm_instantiate(compiled, &(instance_config_t){
    .name = "tokens",
    .exec_mode = EXEC_OBJECTIVE,
    .instruction_limit = 1000000,
    .thread_placement = PLACEMENT_COLOCATE(consensus_ref),  // same thread as consensus
});

// Spreading HTTP across all cores
psi_wasm_instantiate(compiled, &(instance_config_t){
    .name = "http-router",
    .thread_placement = PLACEMENT_SPREAD(8),  // one instance per thread
});
```

A blockchain consensus service imports `psi-wasm` and `psi-threads` to deploy smart contracts received in transactions and control their placement. A plugin runtime imports them to load extensions. The interface is general-purpose — psiserve doesn't know what the caller is building.

The pipeline is: **validate → link → compile → instantiate (with placement)**. Each step can be called independently and results can be cached. Compilation output is content-addressed and reusable across nodes.

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

Yield points are explicit and visible — they only occur inside `psi_read`, `psi_write`, `psi_poll`, `service_call`, and similar host functions. The developer always knows when a context switch can happen.

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

## Configuration & Deployment

### Design: Capability-Based, Not Policy-Based

Services don't request permissions. They declare what interfaces they need (WIT world = entitlements), the operator decides what concrete resources back those interfaces (composition + config = sandbox). No runtime permission checks — if an interface isn't in your imports, you can't call it.

| Layer | Owner | Format | What it determines |
|-------|-------|--------|--------------------|
| **WIT world** | Service developer | WIT | What the service CAN use (entitlements) |
| **WAC composition** | App deployer | WAC | What backs each capability (sandbox) |
| **TOML config** | Operator | TOML | Operational tuning + resource grants (how many, how fast) |

The service doesn't say "I want port 8080." It says "I need a listening socket." The operator gives it one. The service doesn't say "I want /var/data." It says "I need filesystem access." The operator mounts a directory.

### WIT World — the entitlements

Each service declares its imports and exports using the Component Model's standard world definition:

```wit
package myapp:echo@1.0.0;

world echo-service {
    import psi:io/streams@0.1.0;
    import psi:discovery/api@0.1.0;
    import community:json-parser/api@1.0.0;

    export myapp:echo/api@1.0.0;
    export myapp:echo/signals@1.0.0;
}
```

The world tells psiserve exactly what interfaces must be satisfied before this service can run. It is also the security boundary — a service cannot access interfaces not declared in its world.

### WAC Composition — the sandbox

WAC (WebAssembly Composition) is the Bytecode Alliance standard for describing how components are wired together:

```wac
package psiserve:deployment;

let json = new community:json-parser@1.0.0;
let echo = new myapp:echo@1.0.0 {
    "community:json-parser/api": json,
    ...
};
let auth = new myapp:auth@1.0.0 { ... };

export echo;
export auth;
```

WAC handles both bound (pinned version) and linked (name-resolved) composition. Services exported separately are separate processes with host-mediated IPC (connected mode). `let` bindings are libraries absorbed into the importing service.

### TOML Config — directory-based, per-service

Operational config is split across a directory of TOML files, one per service. Parsed with **toml++** (single header-only file, C++17, MIT license). The exact schema is TBD — what matters is the separation of concerns.

```
etc/
  psiserve.toml               # global: threads, logging
  services/                   # per-service operational config
    echo.toml
    auth.toml
    database.toml
```

Global config handles server-wide concerns:

```toml
# etc/psiserve.toml
[server]
threads = 4
compose = "etc/compose.wac"
pkg_path = "pkg"
```

Per-service config handles resource grants and operational tuning:

```toml
# etc/services/echo.toml
[service]
module = "myapp:echo@1.0.0"

[instances]
min = 2
max = 100

[env]
DATABASE_URL = "${DATABASE_URL}"   # forward from host env
LOG_LEVEL = "debug"                # literal value

[listen]
port = 8080

[filesystem]
"/config" = { host = "pkg/myapp/echo@1.0.0/config", mode = "ro" }
"/tmp" = { type = "memory" }
```

Services receive configuration through `psi_getenv()`. Services see **no host environment by default** — only variables explicitly declared in the service's TOML are visible. `${VAR}` syntax forwards from the host environment (like Docker Compose). Unresolvable references are an error at startup.

### Directory Layout

```
psiserve/
  etc/                          configuration
    psiserve.toml                 global config
    compose.wac                   service composition
    services/                     per-service config
      echo.toml
      auth.toml

  pkg/                          packages (namespace:name@version)
    psi/                          host-provided interfaces
      io@0.1.0/
        wit/
      discovery@0.1.0/
        wit/
    community/                    libraries
      json-parser@1.0.0/
        json-parser.wasm
        wit/
    myapp/                        services
      echo@1.0.0/
        echo.wasm
        wit/
          world.wit

  var/                          runtime state
    cache/                        JIT-compiled native code
    log/
    run/                          PID file, sockets
```

The `pkg/` tree mirrors the standard WIT package naming convention (`namespace:name@version`). Packages are distributed as OCI artifacts (the industry-standard mechanism for WASM distribution) and fetched with `wkg`.

### Startup Sequence

```
1. Load etc/psiserve.toml                (global: threads, paths)
2. Load etc/compose.wac                  (structural: what services, how wired)
3. Glob etc/services/*.toml              (per-service: resources, limits, env)
4. For each service:
     a. Load .wasm from pkg/
     b. Validate world against composition (all imports satisfied?)
     c. Link bound/linked dependencies (wasm-ld)
     d. Pre-bind resources from service TOML:
        - Listening sockets (pre-bound, passed as fds)
        - Filesystem mounts (preopened directories)
        - Environment variables (forwarded/literal)
        - Signal connections (from WAC)
     e. Call _init() on template instance
5. Start OS threads
6. Distribute service instances across threads
7. Call _start() on each instance
     - fd 0 = listening socket (if configured)
     - Filesystem = mounted directories
     - Environment = forwarded variables
     - The service receives what it's given. No runtime permission checks.
```

## Detailed Design Documents

- **[psix IPC, Fiber, and Memory Design](psix-ipc-design.md)** — Inter-process communication (signals/slots, ring buffers, cross-thread coordination), fiber allocation (bitmap + CAS pools, static VMA tree), WIT interface conventions (`*-api` for RPC, `*-signals` for events), service discovery, and host-managed message routing. Supersedes the inter-service communication sections below where they conflict.
- **[psizam Architecture Evolution](llvm-backend-design.md)** — WASM engine refactoring: decoupled compile/execute, LLVM backend, sandboxed compilation, code caching.
- **[Master Implementation Plan](master-plan.md)** — Layered build plan from scaffold to work-stealing.
- **[httpd Service](httpd-service.md)** — Hello world application: static file HTTP server. Benchmark target for comparing psiserve overhead against nginx, Go, Node.js.

## Design Principles

1. **Simple things are simple.** A basic HTTP service is sequential blocking code. No event loops, no callbacks, no state machines.

2. **POSIX mental model.** Developers already know read/write/poll/connect. Same patterns, same semantics, sandboxed execution.

3. **Copies only at boundaries.** Zero copies within a thread. One copy at thread boundaries. Two copies at the kernel boundary (unavoidable). Nothing else.

4. **Hardware-enforced isolation.** Guard pages for memory bounds. RO mappings for inter-service channels. SIGSEGV on violation. No runtime checks on the hot path.

5. **Explicit yield points.** Context switches only occur inside host function calls. The developer always knows when interleaving can happen. No hidden preemption.

6. **Everything is an fd.** Sockets, timers, channels, service connections — all managed through the same fd-based API, all waited on through `psi_poll()`.

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
