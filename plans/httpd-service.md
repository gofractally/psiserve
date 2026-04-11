# httpd — Static File HTTP Server

psiserve's hello world application. A minimal high-performance static file server used to benchmark psiserve against other HTTP servers.

## Service Definition

### WIT World

```wit
package psi:httpd@0.1.0;

world httpd-service {
    import psi:io/streams@0.1.0;         // psi_read, psi_write, psi_poll, etc.
    import wasi:filesystem/types@0.2.0;   // file access (backed by host-fs or fs service)
    import wasi:clocks/monotonic@0.2.0;   // psi_clock for logging
}
```

No exports — this is a standalone server, not a library or API provider.

### Configuration

```toml
# etc/services/httpd.toml
[service]
module = "psi:httpd@0.1.0"

[instances]
min = 4
max = 64

[listen]
port = 8080

[filesystem]
"/htdocs" = { host = "/var/www/html", mode = "ro" }

[env]
SERVER_NAME = "psiserve/0.1"
```

### Directory Layout

```
psiserve/
  etc/
    psiserve.toml
    compose.wac
    services/
      httpd.toml
  pkg/
    psi/
      httpd@0.1.0/
        httpd.wasm
        wit/
          world.wit
  var/
    www/
      html/                    # htdocs root
        index.html
        style.css
```

## Implementation

### Simple Blocking Model (v1)

Each instance handles one connection at a time. The host scales by running many instances sharing the same listen socket.

```c
// services/httpd/httpd.c
#include <psi.h>
#include <string.h>
#include <stdio.h>

static char buf[8192];
static const char* server_name;

// Parse "GET /path HTTP/1.x\r\n" → path
static int parse_path(const char* req, int len, char* path, int path_max) {
    if (len < 14) return -1;                     // minimum: "GET / HTTP/1.0\r\n"
    if (memcmp(req, "GET ", 4) != 0) return -1;  // only GET for now

    const char* start = req + 4;
    const char* end = memchr(start, ' ', len - 4);
    if (!end) return -1;

    int plen = end - start;
    if (plen >= path_max) return -1;
    memcpy(path, start, plen);
    path[plen] = '\0';

    // Default to index.html
    if (plen == 1 && path[0] == '/')
        strcpy(path, "/index.html");

    return 0;
}

static const char* content_type(const char* path) {
    const char* dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0) return "image/jpeg";
    if (strcmp(dot, ".gif")  == 0) return "image/gif";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".txt")  == 0) return "text/plain";
    return "application/octet-stream";
}

static void send_error(int fd, int status, const char* reason) {
    int hlen = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\n"
        "Server: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, reason, server_name);
    psi_write(fd, buf, hlen);
}

static void serve_file(int client, const char* url_path) {
    // Build filesystem path: /htdocs + url_path
    char filepath[512];
    snprintf(filepath, sizeof(filepath), "/htdocs%s", url_path);

    // Reject path traversal
    if (strstr(filepath, "..")) {
        send_error(client, 403, "Forbidden");
        return;
    }

    // Stat file for content-length
    psi_stat_t st;
    if (psi_stat(filepath, &st) < 0 || st.type != PSI_FILE_REG) {
        send_error(client, 404, "Not Found");
        return;
    }

    // Open file
    int file_fd = psi_open(filepath, PSI_O_RDONLY);
    if (file_fd < 0) {
        send_error(client, 500, "Internal Server Error");
        return;
    }

    // Send headers
    int hlen = snprintf(buf, sizeof(buf),
        "HTTP/1.1 200 OK\r\n"
        "Server: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %llu\r\n"
        "Connection: close\r\n"
        "\r\n",
        server_name, content_type(url_path), (unsigned long long)st.size);
    psi_write(client, buf, hlen);

    // Stream file body
    int n;
    while ((n = psi_read(file_fd, buf, sizeof(buf))) > 0)
        psi_write(client, buf, n);

    psi_close(file_fd);
}

void _init() {
    server_name = psi_getenv("SERVER_NAME");
    if (!server_name) server_name = "psiserve";
}

void _start() {
    // fd 0 = pre-bound listening socket (shared across instances)
    while (1) {
        psi_addr_t peer;
        int client = psi_accept(0, &peer);
        if (client < 0) continue;

        int n = psi_read(client, buf, sizeof(buf));
        if (n > 0) {
            char path[256];
            if (parse_path(buf, n, path, sizeof(path)) == 0)
                serve_file(client, path);
            else
                send_error(client, 400, "Bad Request");
        }

        psi_close(client);
    }
}
```

### Poll-Based Model (v2)

For higher concurrency — one instance handles many connections via `psi_poll`:

```c
void _start() {
    psi_set_nonblock(0);  // non-blocking accept

    psi_event_t events[64];
    int nfds = 1;
    events[0].fd = 0;
    events[0].events = PSI_READABLE;

    while (1) {
        int n = psi_poll(events, nfds, -1);
        for (int i = 0; i < n; i++) {
            if (events[i].fd == 0 && (events[i].revents & PSI_READABLE)) {
                // Accept new connection
                psi_addr_t peer;
                int client = psi_accept(0, &peer);
                if (client >= 0) {
                    psi_set_nonblock(client);
                    // add to poll set...
                }
            } else if (events[i].revents & PSI_READABLE) {
                // Handle client I/O...
            }
        }
    }
}
```

The v1 blocking model is sufficient for initial benchmarking — the host runs many instances to saturate cores.

## Build

```bash
# Compile with wasi-sdk
/opt/wasi-sdk/bin/clang \
    --target=wasm32-wasi \
    -O2 \
    -o pkg/psi/httpd@0.1.0/httpd.wasm \
    services/httpd/httpd.c
```

## Benchmark Plan

### Test Setup

```bash
# Create test content
echo "<h1>Hello psiserve</h1>" > var/www/html/index.html
dd if=/dev/urandom bs=1K count=1 | base64 > var/www/html/1k.txt
dd if=/dev/urandom bs=1K count=64 | base64 > var/www/html/64k.txt

# Start psiserve
psiserve --config etc/psiserve.toml

# Benchmark with wrk
wrk -t4 -c100 -d10s http://localhost:8080/index.html     # small file
wrk -t4 -c100 -d10s http://localhost:8080/1k.txt          # 1KB
wrk -t4 -c100 -d10s http://localhost:8080/64k.txt         # 64KB
```

### Comparison Targets

| Server | Config | Notes |
|--------|--------|-------|
| **nginx** | `worker_processes 4; sendfile on;` | Gold standard. sendfile gives zero-copy advantage. |
| **Go net/http** | `http.FileServer` | Goroutine-per-connection, good baseline. |
| **Node.js** | `http.createServer` + `fs.createReadStream` | Event loop, single-threaded. |
| **Python** | `http.server` | Baseline floor. |

### What We're Measuring

| Metric | What it tells us |
|--------|------------------|
| **Requests/sec** (small file) | Overhead per request: accept, parse, dispatch, respond |
| **Requests/sec** (64KB file) | Throughput: how fast can we move bytes |
| **p99 latency** | Tail latency: scheduling jitter, GC pauses (others), WASM overhead |
| **Memory per connection** | Instance cost: WASM linear memory + native stack + fd table |
| **CPU profile** | Where time is spent: WASM interp/JIT, host calls, kernel I/O |

### Copy Analysis for This Path

```
                        nginx (sendfile)    psiserve (v1)
                        ────────────────    ─────────────
NIC → kernel            copy 1              copy 1
kernel → userspace      copy 2 (request)    copy 2 (request into WASM ring)
parse HTTP              in nginx buffer     in WASM memory (zero-copy from ring)
read file               sendfile (0 copy)   copy 3 (kernel → WASM via host-fs)
write response header   copy 3              copy 4 (WASM → kernel)
write file body         sendfile splice     copy 5 (WASM → kernel)
kernel → NIC            copy 4              copy 6

Total:                  4 copies            6 copies (but no process boundaries)
```

nginx wins on raw file serving due to `sendfile()`. To match it, psiserve could add `psi_sendfile(socket_fd, file_fd, offset, len)` — a host function that calls `sendfile()` directly, bypassing WASM memory entirely.

```c
// Future optimization: zero-copy file send
psi_sendfile(client, file_fd, 0, st.size);  // kernel-to-kernel, skips WASM
```

With `psi_sendfile`: 4 copies, same as nginx. psiserve's advantage shows up in dynamic content (no process boundaries, no serialization), not static file serving.

### Success Criteria

For v1 (interpreter backend, blocking model):
- Within 5x of nginx on small files (dominated by WASM interpretation overhead)
- Within 2x of Go net/http

For v2 (JIT backend, poll model):
- Within 2x of nginx on small files
- Within 1.5x of Go net/http
- Better tail latency than Go (no GC pauses)

For v3 (JIT + sendfile + ring buffers):
- Parity with nginx on static files
- Better than nginx on dynamic content (no reverse proxy hop)
