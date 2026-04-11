# psiserve Master Development Plan

## Starting Point

- **psizam** — WASM engine is mature: parser, interpreter, JIT (x86_64 + aarch64), host function binding, allocator with guard pages. 99% spec test pass rate.
- **psiserve library** — empty scaffold, ready to build.
- **Design** — complete: process model, IPC, scheduling, configuration, API surface, composition model, dynamic WASM management, privilege model. See `psiserve-design.md` and `psix-ipc-design.md`.

## Principles

- Each phase produces a **working, benchmarkable system**
- Tests at every phase — no moving forward on broken foundations
- Interpreter backend first, JIT later — interpreter is simpler to debug and context-switch
- macOS first (kqueue), Linux (epoll/io_uring) added in parallel when foundation is solid
- Each phase's deliverable is a sentence: "I can now do X"

---

## Phase 1: Echo Server

**Goal:** One WASM module, one thread, accepts TCP connections, echoes bytes back.

This proves the entire vertical stack works: psizam executes WASM → host functions handle I/O → scheduler parks/resumes processes → real TCP connections work.

### Build

- `libraries/psiserve/` — core library
  - `process.hpp` — Process struct: execution context, state (READY/RUNNING/BLOCKED/DONE), fd table
  - `fd_table.hpp` — per-process fd table, fd types (socket)
  - `io_engine.hpp` — abstract I/O engine interface
  - `io_engine_kqueue.hpp` — kqueue implementation (macOS)
  - `scheduler.hpp` — single-thread scheduler: ready queue, timer check, I/O poll, run loop
  - `host_api.hpp` — host function registration: `psi_accept`, `psi_read`, `psi_write`, `psi_close`
  - `runtime.hpp` — top-level: load WASM, register host functions, run scheduler

- `programs/psiserve-cli/` — minimal CLI
  - `main.cpp` — load one WASM file, bind one port, run

- `services/echo/` — test WASM service
  - `echo.c` — accept loop, read/write echo, compiled with wasi-sdk

- `tests/`
  - Process state machine tests
  - Fd table tests
  - Scheduler unit tests (mock I/O)
  - Integration test: start server, connect with netcat, verify echo

### Deliverable

```bash
psiserve echo.wasm --port 8080
# In another terminal:
echo "hello" | nc localhost 8080
# → "hello"

wrk -t1 -c10 -d5s http://localhost:8080/  # raw TCP echo benchmark
```

**"I can run a WASM service that handles TCP connections."**

---

## Phase 2: Multi-Process Scheduling

**Goal:** Many WASM instances on one thread, cooperatively scheduled.

### Build

- Multiple instances of the same module (configurable count)
- Shared listen socket across instances (host distributes `accept()`)
- Process pool: pre-allocate N instances, bitmap allocation
- Blocking semantics: `psi_accept` / `psi_read` park the process, scheduler runs the next
- `psi_poll` for multi-fd multiplexing
- `psi_set_nonblock`
- Timers: `psi_timer_create`, `psi_timer_set`, `psi_clock`

### Tests

- 100 instances sharing one port, 100 concurrent connections
- Verify all connections get served (no starvation)
- Timer accuracy test
- Poll test: one process watching multiple fds

### Deliverable

```bash
psiserve echo.wasm --port 8080 --instances 100
wrk -t4 -c100 -d10s ...  # 100 concurrent connections
```

**"I can run 100 WASM processes on one thread, cooperatively scheduled."**

---

## Phase 3: Configuration

**Goal:** TOML config, named resources, per-service config files, multi-service.

### Build

- Include toml++ (single header)
- `config.hpp` — load `psiserve.toml` + glob `services/*.toml`
- `psi_getenv` — sandboxed environment (only forwarded vars, `${VAR}` syntax)
- `psi_getfd` — named resource lookup (listen sockets, preopened dirs)
- Load multiple different WASM modules from config
- CLI: `psiserve --config etc/psiserve.toml`

### Directory layout

```
etc/psiserve.toml
etc/services/echo.toml
pkg/psi/echo@0.1.0/echo.wasm
```

### Tests

- Config parsing tests (valid, invalid, missing fields)
- psi_getenv sandboxing (host env not leaked)
- psi_getfd lookup
- Multi-service: echo on port 8080, different service on port 9090

### Deliverable

```bash
psiserve --config etc/psiserve.toml
# Loads echo.toml, starts echo service on configured port
```

**"I can configure and run multiple services from TOML files."**

---

## Phase 4: Multi-Threading

**Goal:** OS thread pool, instances distributed across threads.

### Build

- `thread.hpp` — Thread struct: own scheduler, own I/O engine, own process list
- Thread pool startup (configurable count from TOML)
- Thread placement: `auto` (round-robin), `pin(thread)`, `spread(n)`
- Per-thread kqueue/epoll instance
- Shared listen socket (SO_REUSEPORT or host-distributed accept)

### Tests

- Echo server on 4 threads, verify all connections served
- Pin test: service on thread 0 only
- Spread test: 4 instances across 4 threads

### Deliverable

```bash
# etc/psiserve.toml: threads = 4
# etc/services/echo.toml: instances.min = 4
wrk -t4 -c400 -d10s ...  # multi-threaded benchmark
```

**Benchmark: echo server vs Go net/http, vs nginx (proxy_pass to echo).**

**"I can run services across multiple OS threads."**

---

## Phase 5: Filesystem + HTTP Server

**Goal:** Static file HTTP server, benchmarkable against nginx.

### Build

- `psi_open`, `psi_stat`, `psi_close` on preopened directories
- `psi_readdir` for directory listing
- Preopened directory mounting from service TOML `[filesystem]`
- Path traversal prevention (reject `..`, enforce sandbox)
- `psi_shutdown` (half-close for HTTP/1.1)
- `psi_getpeername`

- `services/httpd/httpd.c` — static file server from `httpd-service.md`

### Tests

- Open/read/stat on mounted directory
- Path traversal rejected
- 404 for missing files
- HTTP/1.1 keep-alive
- Benchmark: wrk against small file, 64KB file

### Deliverable

```bash
# etc/services/httpd.toml: filesystem "/htdocs" = { host = "var/www/html" }
wrk -t4 -c100 -d10s http://localhost:8080/index.html
```

**Benchmark: psiserve httpd vs nginx, vs Go http.FileServer, vs Node.js.**

**"I can serve static files from a sandboxed directory."**

---

## Phase 6: Inter-Service Communication

**Goal:** Services call each other via typed interfaces. Same-thread fast path, cross-thread via message ring.

### Build

- WIT interface loading (parse `.wit` files or use pre-generated dispatch tables)
- Canonical ABI lift/lower for scalar types and simple structs
- Service-to-service RPC: caller blocks, host switches to callee, result returns
- Same-thread: direct process switch (one copy)
- Cross-thread: per-thread outbound ring buffer
  - `message_ring.hpp` — bump allocator, per-receiver linked lists, hash table
  - `has_messages` atomic bitmap + `wake_fd`
  - Consumer progress tracking (`consumed_by[]`)
- `psi_channel_create`, `psi_channel_connect` for fd-based channels

### Tests

- Same-thread RPC: service A calls service B, gets result
- Cross-thread RPC: same, across threads
- Message ring: write/read/reclaim cycle
- Backpressure: ring full → sender blocks
- Benchmark: RPC latency (same-thread vs cross-thread)

### Deliverable

HTTP router service forwards requests to a backend echo/file service via WIT RPC.

**"Services can call each other across threads."**

---

## Phase 7: Dynamic WASM Management

**Goal:** Deploy, start, stop services at runtime via the `psi-wasm` and `psi-admin` interfaces.

### Build

- `psi_wasm_validate` — check module is well-formed
- `psi_wasm_compile` — parse + interpreter setup (or JIT)
- `psi_wasm_instantiate` — create process from compiled module with config
- `psi_admin_deploy` / `psi_admin_start` / `psi_admin_stop` / `psi_admin_scale`
- `psi-threads`: `thread_count`, `current_thread`, `migrate`
- Admin service: HTTP API for deploy/start/stop/status

### Tests

- Deploy a WASM module via admin API
- Start/stop it
- Scale up/down
- Migrate between threads
- Deploy malformed WASM → validation error
- Status endpoint shows running services

### Deliverable

```bash
curl -X POST http://localhost:9090/deploy -F "module=@echo.wasm" -F "name=echo2"
curl -X POST http://localhost:9090/start/echo2
curl http://localhost:9090/status
# { "services": [{ "name": "echo2", "instances": 4, "state": "running" }] }
```

**"I can deploy and manage services at runtime without restarting."**

---

## Phase 8: Signals, Discovery, Events

**Goal:** Qt-style signals/slots, service discovery.

### Build

- `signal.hpp` — Signal resource: connect/disconnect/emit
- Static signals (type-scoped) vs instance signals
- Async signals (void, fire-and-forget) vs sync signals (with return, sender blocks)
- `psi-discovery` interface: resolve, resolve_all, spawn, watch
- `service_ref` as opaque handle

### Tests

- Signal emit → all connected slots called
- Static signal: new instance auto-included
- Async vs sync delivery
- Discovery: resolve by interface name
- Watch: notification when instance starts/stops

### Deliverable

Chat-like demo: service A emits "message-sent" signal, services B and C receive it.

**"Services can discover each other and broadcast events."**

---

## Phase 9: WASM Linking + JIT Caching

**Goal:** Link relocatable `.o` files at runtime. Cache JIT output per-function.

### Build

- `.o` file parser: `linking` and `reloc.*` custom sections
- Symbol resolution across modules
- Index renumbering + relocation patching (padded LEB128)
- Emit linked module
- Per-function JIT caching: content-hash → cached native code
- Native-level linking: build combined function table from cached pieces

### Tests

- Link `app.o` + `lib.o` → `applib.wasm`, verify it runs correctly
- Verify library functions aren't re-JITed on re-link
- Cache hit/miss verification
- Bound (by hash) vs linked (by name) resolution

### Deliverable

```bash
# User uploads app.o, chain links against stdlib
psiserve-ctl link app.o --with psi-stdlib → applib.wasm
# Subsequent links with same stdlib skip JIT for stdlib functions
```

**"I can link WASM modules at runtime and cache compiled code."**

---

## Phase 10: Execution Control

**Goal:** Gas metering, deterministic execution, resource limits.

### Build

- Instruction counting in interpreter (increment counter per instruction)
- JIT: inject counter decrement at loop back-edges and function entries
- `psi_meter_set_limit`, `psi_meter_remaining`, `psi_meter_consumed`
- Objective mode: softfloat (already in psizam), deterministic execution
- Subjective mode: native float, max performance
- Per-service memory limits (enforce `memory.grow` ceiling)

### Tests

- Infinite loop terminated by gas limit
- Gas consumed matches expected instruction count
- Objective mode: same result on x86_64 and aarch64
- Memory limit: `memory.grow` beyond limit returns -1

### Deliverable

**"I can run untrusted WASM with resource limits and deterministic execution."**

---

## Phase 11: Database Integration

**Goal:** psitri exposed as `psi-db` host interface. Transaction support.

### Build

- `psi-db` host functions: open, begin_read, begin_write, get, put, remove, commit, abort
- Cursor/iteration support
- Transaction context propagated through service-to-service calls
- Scoped access: user services only see their own tables

### Tests

- Basic CRUD through psi-db
- Transaction commit/abort/rollback
- Cross-service call within same transaction
- Scoped access: service A can't read service B's tables

### Deliverable

Minimal blockchain prototype: consensus service receives transactions, executes them against psitri via psi-db, routes HTTP requests to user-deployed smart contracts.

**"I can build a blockchain node on psiserve."**

---

## Phase 12: Production Hardening

**Goal:** Performance optimizations, operational polish.

### Build

- Memory pools: pre-allocated contiguous regions, bitmap + CAS allocation, `madvise` recycling
- COW instance forking for scaling
- io_uring backend (Linux)
- Ring buffer registration (`psi_bind_ring`) for zero-copy I/O
- `psi_sendfile` for static file serving
- `readv`/`writev`
- SIGHUP config reload
- Graceful shutdown (drain connections, stop services)
- epoll backend (Linux, for older kernels)

### Tests

- VMA count stable under load (no fragmentation)
- io_uring vs kqueue benchmark
- sendfile benchmark (vs buffered copy)
- Graceful shutdown: all connections drained before exit
- Config reload: new services start, removed services stop

### Deliverable

**"psiserve is production-ready with optimized I/O and clean operations."**

---

## Phase Summary

| Phase | Deliverable | Key Metric |
|-------|------------|------------|
| 1 | Echo server | It works |
| 2 | Multi-process scheduling | 100 concurrent connections, one thread |
| 3 | Configuration | Multi-service from TOML |
| 4 | Multi-threading | Echo benchmark vs Go/nginx |
| 5 | Filesystem + HTTP | httpd benchmark vs nginx |
| 6 | Inter-service communication | RPC latency |
| 7 | Dynamic WASM management | Deploy at runtime |
| 8 | Signals + discovery | Event broadcast |
| 9 | WASM linking + caching | Link time, JIT cache hit rate |
| 10 | Execution control | Gas metering accuracy |
| 11 | Database integration | Blockchain prototype |
| 12 | Production hardening | io_uring throughput, VMA stability |

## Build & Test

```bash
# macOS (Homebrew Clang required)
export CC=$(brew --prefix llvm)/bin/clang
export CXX=$(brew --prefix llvm)/bin/clang++

cmake -B build/Debug -G Ninja -DCMAKE_BUILD_TYPE=Debug -DPSISERVE_ENABLE_TESTS=ON
cmake --build build/Debug
cd build/Debug && ctest --output-on-failure
```

## Test WASM Modules

Compiled from C using wasi-sdk:

- `services/echo/echo.c` — TCP echo, Phase 1
- `services/httpd/httpd.c` — static file HTTP server, Phase 5
- `services/admin/admin.c` — runtime admin API, Phase 7
- `services/chat/chat.c` — signal/slot demo, Phase 8
