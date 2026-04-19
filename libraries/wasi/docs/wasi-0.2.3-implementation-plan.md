# WASI 0.2.3 C++ Bindings Implementation Plan

## Goal

Create C++ bindings (PSIO_INTERFACE + PSIO_HOST_MODULE compatible)
for the WASI 0.2.3 IO, HTTP, and Sockets interfaces. Each binding
follows the pattern established by `wasi/0.2.3/cli.hpp`: struct with
static methods, PSIO_INTERFACE declaration, resource types via
`psio::wit_resource` + `psio::own<T>` / `psio::borrow<T>`.

## Source WIT files

All in `libraries/wasi/wit/0.2.3/`:

### Phase 1: IO (foundation)
- `io/error.wit` — error resource
- `io/poll.wit` — pollable resource
- `io/streams.wit` — input-stream, output-stream resources

### Phase 2: HTTP
- `http/types.wit` — method, scheme, headers, request, response resources
- `http/handler.wit` — incoming-handler, outgoing-handler

### Phase 3: Sockets
- `sockets/network.wit` — network, error-code
- `sockets/tcp.wit` — tcp-socket resource
- `sockets/tcp-create-socket.wit` — create-tcp-socket
- `sockets/udp.wit` — udp-socket resource
- `sockets/udp-create-socket.wit` — create-udp-socket
- `sockets/ip-name-lookup.wit` — resolve-address-stream
- `sockets/instance-network.wit` — instance-network

### Phase 4: Remaining
- `clocks/wall-clock.wit` — wall clock
- `clocks/monotonic-clock.wit` — monotonic clock + pollable
- `filesystem/types.wit` — descriptor, dir-entry-stream
- `filesystem/preopens.wit` — get-directories
- `random/random.wit` — get-random-bytes, get-random-u64
- `random/insecure.wit` — insecure random
- `random/insecure-seed.wit` — insecure seed

## Output files

Each phase creates a header in `libraries/wasi/cpp/include/wasi/0.2.3/`:

```
io.hpp          ← error, pollable, input-stream, output-stream
http.hpp        ← method, scheme, headers, fields, request, response,
                   incoming-handler, outgoing-handler
sockets.hpp     ← network, tcp-socket, udp-socket, ip-name-lookup
clocks.hpp      ← wall-clock, monotonic-clock
filesystem.hpp  ← descriptor, dir-entry-stream, preopens
random.hpp      ← random bytes
```

## Pattern for each interface

1. Read the .wit file
2. For each resource type: create a struct inheriting `psio::wit_resource`,
   add PSIO_REFLECT with empty data members
3. For each interface: create a struct with static methods matching the
   WIT function signatures
4. Map WIT types to C++ types:
   - `u8/u16/u32/u64` → `uint8_t/uint16_t/uint32_t/uint64_t`
   - `s8/s16/s32/s64` → `int8_t/int16_t/int32_t/int64_t`
   - `f32/f64` → `float/double`
   - `bool` → `bool`
   - `string` → `std::string_view` (args), `std::string` (returns)
   - `list<u8>` → `std::vector<uint8_t>` (args), `std::vector<uint8_t>` (returns)
   - `option<T>` → `std::optional<T>`
   - `result<T, E>` → `std::expected<T, E>` or a result struct
   - `tuple<A, B>` → `std::tuple<A, B>`
   - `own<T>` → `psio::own<T>`
   - `borrow<T>` → `psio::borrow<T>`
   - `resource X { ... }` → `struct X : psio::wit_resource {}`
5. Add PSIO_INTERFACE with types() and funcs()
6. Add PSIO_PACKAGE for the wasi:io / wasi:http / wasi:sockets package

## WIT variant/enum mapping

WIT variants map to C++ variants:
```wit
variant stream-error {
   last-operation-failed(error),
   closed
}
```
→
```cpp
struct stream_error {
   std::variant<psio::own<io_error>, std::monostate> value;
   // variant index 0 = last-operation-failed
   // variant index 1 = closed
};
PSIO_REFLECT(stream_error, value)
```

Or simpler with an enum + optional:
```cpp
enum class stream_error_kind { last_operation_failed, closed };
struct stream_error {
   stream_error_kind kind;
   std::optional<psio::own<io_error>> error; // present if last_operation_failed
};
```

## WIT result mapping

WIT `result<T, E>` maps to a struct with ok/err:
```cpp
template <typename T, typename E>
struct wasi_result {
   bool is_ok;
   union {
      T ok_val;
      E err_val;
   };
};
```

Or use `std::variant<T, E>` with index 0 = ok, 1 = err.

## Dependencies between phases

```
Phase 1 (IO):     standalone — error, pollable, streams
Phase 2 (HTTP):   depends on Phase 1 (uses streams, pollable)
Phase 3 (Sockets): depends on Phase 1 (uses streams, pollable)
Phase 4 (Rest):   depends on Phase 1 (clocks use pollable)
```

## Testing

Each phase includes a test in `libraries/wasi/cpp/tests/` that:
1. Verifies the C++ types compile
2. Verifies PSIO_INTERFACE reflection produces valid WIT text
3. Compares generated WIT against the source .wit files (golden diff)

Follow the pattern in `tests/round_trip.cpp`.

## Non-goals

- Host-side IMPLEMENTATION of these interfaces (that's psiserve's job)
- Guest-side runtime linking (that's the runtime's job)
- Async support (that's WASI 0.3)

This plan creates the C++ TYPE DECLARATIONS and PSIO reflections only.
The host implementations are a separate step that wires psiserve's
native I/O, HTTP, and networking into these interfaces.
