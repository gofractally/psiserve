# WIT Marshaling and Inter-Module Dispatch Design

## Problem

Two WASM modules need to call each other's functions with rich types (records, strings, lists, variants). The Component Model defines WIT for interface descriptions and the Canonical ABI for memory layout, but the standard approach has costs:

1. **Canonical ABI is a calling convention, not a wire format.** It defines how to lay out data in linear memory with C-like alignment and pointer-based variable-length fields. It is not a serialization format.
2. **Separate memories require copy-at-boundary.** Each component has its own linear memory. The host lifts data from the caller's memory and lowers it into the callee's memory.
3. **Shared memory breaks ownership.** If two modules share linear memory (direct linking), both think they own `malloc`/`free`. One module freeing memory allocated by the other's allocator causes corruption.
4. **Standard tooling requires multi-step code generation.** wit-bindgen generates language-specific bindings, which must be maintained in sync with `.wit` files.

## Design Goals

- **Single-macro developer experience** for C++ modules: `PSIO_REFLECT` + `PSIZAM_COMPONENT` generates WIT and embeds it in the WASM binary. No external code generation tools.
- **Auto-detected calling conventions** so the host can wire up modules using different marshaling strategies without manual configuration.
- **Zero-alloc dispatch for simple types** using stack-allocated C struct views into WASM memory.
- **Fracpack fallback for complex types** when zero-alloc views aren't possible.
- **Async dispatch support** since the marshaling layer naturally decouples caller and callee execution.
- **Full ecosystem compatibility** with standard Component Model tooling (wasmtime, wasm-compose, wit-bindgen).

---

## Architecture Overview

```
Module A                        Host                         Module B
────────                        ────                         ────────
call exported func  ──→  read WIT for both sides
                         compare WASM signatures to WIT
                         determine calling convention per-function
                         generate JIT'd adapter (cached)
                              │
                         adapter: lift from A's memory
                                  lower into B's memory
                                  call B's export
                                  lift result from B
                                  lower result into A
                              │
result returned     ←──  resume A
```

The host is always in the middle. It has access to both linear memories, both WIT descriptions, and both sets of WASM export signatures. It makes all marshaling decisions.

---

## WIT Embedding via constexpr

### The Single-Macro Goal

A C++ developer writes:

```cpp
struct my_api {
    std::string greet(std::string name);
    uint64_t    add(uint32_t a, uint32_t b);
};
PSIO_REFLECT(my_api, method(greet, name), method(add, a, b))
PSIZAM_COMPONENT(my_api)
```

`PSIZAM_COMPONENT` does two things:

1. **Generates WIT at compile time** using `constexpr` functions over `psio::reflect<T>` and embeds it in a `component-type` custom section in the WASM binary.
2. **Emits a convention tag** alongside the WIT indicating how this module's exports encode their parameters (canonical, fracpack, or proxy_type).

### constexpr WIT Generation

C++23 enables this:

```cpp
// Two-pass: compute size, then fill buffer
template<typename T>
consteval size_t wit_size();

template<typename T>
consteval auto wit_string() {
    // C++23 allows transient constexpr std::string inside consteval
    std::string s;
    // iterate psio::reflect<T>::data_members, member_functions
    // map C++ types to WIT types: uint32_t→u32, std::string→string, etc.
    // build .wit text
    std::array<char, wit_size<T>()> buf{};
    std::copy(s.begin(), s.end(), buf.begin());
    return buf;
}
```

PSIO_REFLECT's member names and type metadata are compile-time constants (string literals, member pointers). The type mapping is trivially constexpr:

| C++ Type | WIT Type |
|----------|----------|
| `uint32_t` / `int32_t` | `u32` / `s32` |
| `float` / `double` | `f32` / `f64` |
| `bool` | `bool` |
| `std::string` | `string` |
| `std::vector<T>` | `list<T>` |
| `std::optional<T>` | `option<T>` |
| `std::variant<T...>` | `variant { ... }` |
| `std::tuple<T...>` | `tuple<T...>` |
| PSIO_REFLECT struct | `record { field: type, ... }` |

### Embedding in WASM Binary

Clang's WASM backend supports custom sections:

```cpp
constexpr auto wit = wit_string<MyExports>();

__attribute__((section(".custom_section.component-type")))
static const char _wit_section[sizeof(wit)] = /* ... */;
```

The `component-type` custom section is the standard location where the WASM ecosystem expects to find WIT type information. Standard tools (wasm-tools, wasmtime) can read it.

### Convention Tag

Alongside the WIT, a small metadata section identifies psiserve-aware modules:

```cpp
__attribute__((section(".custom_section.psi-abi")))
static const char _abi_tag[] = "psi";
```

- **No tag present** → standard ecosystem module, assume Canonical ABI
- **`psi`** → built with psiserve toolchain, host can apply optimizations (single-alloc placement, bump allocator detection)

The tag is not a calling convention selector — the Canonical ABI is always the calling convention. The tag tells the host that the module was built with our toolchain and may support optimizations like bump allocator batching.

---

## Host-Side Signature Validation

The host validates that each WIT function signature matches the actual WASM export:

| WIT Function | Expected WASM Signature | Notes |
|---|---|---|
| `func(a: u32, b: u32) -> u64` | `(i32, i32) -> (i64)` | Scalars pass directly |
| `func(name: string) -> string` | `(i32, i32, i32) -> void` | (ptr, len, retptr) |
| `func(r: record{a: u32, b: f32}) -> u32` | `(i32, f32) -> (i32)` | Record fields flattened if <= 16 |
| Large record (>16 flat fields) | `(i32) -> (i32)` | Spilled to memory, single pointer |

The host computes the Canonical ABI flattening of each WIT function type and verifies it matches the actual WASM export signature. Mismatches are rejected at deploy time — before any code runs.

---

## Marshaling Strategy

The host knows the complete type tree from WIT/reflection. It uses this to generate optimal marshaling code, choosing between two strategies based on the callee's allocator.

### The Ownership Constraint

The Canonical ABI gives the callee ownership of each allocation. The callee has the right to independently `free()` any string, list, or record that was allocated in its memory via `cabi_realloc`. This is a hard contract — violating it causes allocator corruption or crashes in long-running modules.

This means the host cannot batch multiple allocations into one block unless it knows the callee will never call `free()` on individual sub-allocations.

### Allocator Classification

At deploy time, the host inspects the callee's `cabi_realloc` export bytecode to classify the allocator:

| Allocator Type | Detection | Optimization |
|---|---|---|
| **Bump** | `cabi_realloc` only advances a pointer, `free` is empty or absent | Single-alloc: batch everything into one `cabi_realloc` call |
| **Real malloc** | `cabi_realloc` calls a non-trivial allocator, `free` does real work | Per-field: call `cabi_realloc` for each independently-freeable piece |

### Path 1: Bump Allocator — Single-Alloc Direct Placement

**When:** The callee uses a bump allocator (common for short-lived request handlers, per-fork processes).

The host computes the total size for all allocations, calls `cabi_realloc` once, and lays out everything contiguously:

```
Host walks the source object tree:
    items: 3 records
        [0]: name="alice" (5 bytes), value=42
        [1]: name="bob"   (3 bytes), value=7
        [2]: name="carol" (5 bytes), value=99

Compute total size:
    list array:  3 × 8 bytes (ptr + len per record)     = 24
    record [0]:  4 (value) + 4 (pad) + 8 (name ptr+len) = 16
    record [1]:  same                                    = 16
    record [2]:  same                                    = 16
    string data: 5 + 3 + 5                               = 13
    total:                                                = 85 bytes

One call: cabi_realloc(0, 0, 8, 85) → base_ptr

Write everything contiguously, fixing up pointers:
    [base_ptr + 0]:  list array (3 entries, pointers to records below)
    [base_ptr + 24]: record[0] { value=42, name_ptr=→base+72, name_len=5 }
    [base_ptr + 40]: record[1] { value=7,  name_ptr=→base+77, name_len=3 }
    [base_ptr + 56]: record[2] { value=99, name_ptr=→base+80, name_len=5 }
    [base_ptr + 72]: "alice"
    [base_ptr + 77]: "bob"
    [base_ptr + 80]: "carol"

Call export with: (base_ptr, 3)  — standard Canonical ABI list args
```

Since `free()` is a no-op, the callee can "free" individual pieces without harm. The entire block is reclaimed when the process resets or dies.

This is ideal for psiserve's fork-per-request model where each process instance is short-lived.

### Path 2: Real Allocator — Per-Field Canonical ABI

**When:** The callee uses a real `malloc`/`free` (long-running daemons, stateful services).

The host follows standard Canonical ABI lowering: call `cabi_realloc` separately for each independently-owned allocation. Each string, each list backing array, each spilled record gets its own allocation that the callee can independently free.

For the same example:

```
cabi_realloc for string "alice"  → ptr_a
cabi_realloc for string "bob"    → ptr_b
cabi_realloc for string "carol"  → ptr_c
cabi_realloc for list array (3 records with embedded string ptrs) → list_ptr

Write record[0] with name_ptr=ptr_a into list_ptr+0
Write record[1] with name_ptr=ptr_b into list_ptr+8
Write record[2] with name_ptr=ptr_c into list_ptr+16

Call export with: (list_ptr, 3)
```

More `cabi_realloc` calls, but each allocation is independently freeable. The callee's allocator stays consistent.

### Path 3: Zero-Alloc View (reading FROM WASM memory)

**When:** Lifting results or reading arguments that are already in WASM memory.

The host constructs a C struct on the stack with pointers directly into WASM linear memory:

```cpp
// WIT: record { id: u32, name: string, scores: list<f32> }
struct view {
    uint32_t id;          // copied from WASM memory
    const char* name;     // points into WASM linear memory
    size_t name_len;
    const float* scores;  // points into WASM linear memory
    size_t scores_len;
};
```

Zero allocations. Pointers are valid for the duration of the call.

**Compile-time viewability check:**

```cpp
template<typename T>
constexpr bool is_viewable = /* recursively check:
    - all scalar fields: yes
    - string fields: yes (ptr + len)
    - vector<scalar> fields: yes (ptr + len)
    - nested record where is_viewable<nested>: yes
    - vector<non-scalar>: NO (need to alloc descriptor array)
    - anything else with variable-length-of-variable-length: NO
*/;
```

For non-viewable types being read, the host walks the Canonical ABI layout in-place — no allocation, just pointer arithmetic with validation.

### Sender-Side: Stack Buffer

The sender (caller) has no ownership constraint. The host reads args immediately during the call — the buffer only needs to survive until the host has lifted the data. So the sender can:

1. Build the entire argument tree on the **WASM shadow stack** (part of linear memory — just adjust the stack pointer). Canonical ABI layout, driven by WIT types. Zero allocations.
2. The host lifts from it immediately.
3. On return, the stack pointer resets — zero-cost cleanup.

For values too large for the stack (~64KB), a single `malloc` + `free` after the call. Still one alloc, one free, no fragmentation, no serialization.

The generated caller stub (from `PSIZAM_COMPONENT` or wit-bindgen) does this automatically — the developer just calls the function with native types.

### Complete Cost Summary

| Side | Strategy | Allocs | Copies |
|---|---|---|---|
| **Sender** | Stack buffer (or 1 heap for large) | 0 typical | 0 (host reads in-place) |
| **Host** | Lift from sender, lower into receiver | 0 | 1 (sender → receiver) |
| **Receiver (bump)** | Single `cabi_realloc`, direct placement | 1 | 0 (host writes directly) |
| **Receiver (real malloc)** | Per-field `cabi_realloc` | N | 0 (host writes directly) |

Total: **1 copy** in all cases. The variation is only in allocation count on the receiver side, determined at deploy time by allocator classification.

No serialization format is involved — just memory layout driven by WIT types. The host reads structured data from one linear memory and writes structured data into another.

### Fracpack's Role

Fracpack is **not** used for inter-module dispatch. It is the wire/storage format:

- **Message rings** — cross-thread IPC serializes into fracpack in the ring buffer
- **Disk storage** — `.pzam` metadata, database records
- **Network** — RPC across machines

For in-process dispatch, the host always uses Canonical ABI layout (single-alloc or per-field depending on allocator). This ensures every callee sees standard pointers regardless of who built it.

---

## Adapter Generation

The host generates a JIT-compiled adapter function for each inter-module call. The adapter is specialized to the specific type signature and cached.

### What the Adapter Does

For each (import, export) pair wired together at compose time:

1. **Lift** args from the caller's memory (zero-alloc view or in-place walk)
2. **Lower** args into the callee's memory:
   - Bump allocator: compute total size, single `cabi_realloc`, direct placement with pointer fixups
   - Real allocator: per-field `cabi_realloc` for each independently-freeable piece
3. **Call** the callee's export
4. **Lift** results from the callee (zero-alloc view)
5. **Lower** results into the caller via the same allocator-appropriate strategy
6. **Cleanup** via `cabi_post_return_*` if the callee exports it

For all-scalar functions, the adapter is trivial — just forward the flat args. The allocator classification is a deploy-time decision baked into the generated adapter code.

### JIT Adapter Compilation

At deploy/compose time:

1. Host reads WIT from both modules
2. For each (import, export) pair:
   a. Walk the WIT type tree
   b. Generate adapter code: size computation, single alloc, direct placement, pointer fixup
   c. JIT-compile the adapter with psizam
   d. Cache keyed by type signature hash

The adapter is native code at call time. No interpretation of WIT types on the hot path.

---

## Handler Generation: PZAM_COMPONENT Macro

### Division of Labor

The macro knows: class name, method names, parameter names.
Templates know: parameter types, return types, member function pointers.

Neither alone can generate a typed `extern "C"` export. Together they can.

### The 16-Wide Entry Point

The Canonical ABI specifies MAX_FLAT_PARAMS=16. Functions with <=16 flat values pass args directly; beyond that, args spill to a pointer. Instead of having the macro decide (it can't — it doesn't know the types), every export uses a fixed 16-parameter signature:

```cpp
extern "C" int64_t add(int64_t a0, int64_t a1, int64_t a2, int64_t a3,
                       int64_t a4, int64_t a5, int64_t a6, int64_t a7,
                       int64_t a8, int64_t a9, int64_t a10, int64_t a11,
                       int64_t a12, int64_t a13, int64_t a14, int64_t a15) {
    return ComponentProxy<calculator>::call<&calculator::add>(
        &_impl, a0, a1, a2, a3, a4, a5, a6, a7,
        a8, a9, a10, a11, a12, a13, a14, a15);
}
```

The template `call<&calculator::add>` deduces from the member function pointer that only `a0` and `a1` are used and they're `int32_t`. Unused parameters are dead stack space (128 bytes — nothing). The WIT tells the host exactly how many args to pass.

For functions exceeding 16 flat values, `a0` is a pointer to a memory-spilled struct — `ComponentProxy::call` detects this at compile time from the type tree and constructs `COwned<Args>` from the pointer.

### The Complete Macro

```cpp
#define PZAM_COMPONENT(Class, ...) \
    /* 1. PSIO_REFLECT for the class */ \
    PSIO_REFLECT(Class, __VA_ARGS__) \
    /* 2. Static instance */ \
    static Class _pzam_impl; \
    /* 3. constexpr WIT embedded in custom section */ \
    /* (wit_string<Class>() generates .wit text from reflection) */ \
    __attribute__((section(".custom_section.component-type"))) \
    static const auto _pzam_wit = psizam::wit_section<Class>(); \
    /* 4. cabi_realloc export */ \
    extern "C" __attribute__((export_name("cabi_realloc"))) \
    void* cabi_realloc(void* old, size_t old_size, size_t align, size_t new_size) { \
        return realloc(old, new_size); \
    } \
    /* 5. Per-method extern "C" exports (macro iterates method names) */ \
    _PZAM_EXPORT_METHODS(Class, __VA_ARGS__)
```

`_PZAM_EXPORT_METHODS` uses preprocessor iteration (Boost.PP-style) to stamp out one 16-wide `extern "C"` function per `method(...)` entry. Each body is identical — just `ComponentProxy<Class>::call<&Class::name>(&_pzam_impl, a0..a15)`.

### CView / COwned in the Call Path

`ComponentProxy::call<MemberPtr>` inspects the member function pointer's parameter types at compile time:

- **All scalar** → read flat values from `a0..aN`, call method directly, return flat result. Zero allocations.
- **Has complex types, <=16 flat** → interpret `a0..aN` as flattened Canonical ABI values (pointers + lengths for strings/lists), construct `COwned<Args>`, call method, destroy `COwned` (frees allocations). The flat values carry the pointers.
- **>16 flat values** → `a0` is a pointer to a spilled struct. Construct `COwned<Args>` from pointer, call method, destroy `COwned`.

For return values:
- **Scalar** → return directly as `int64_t`
- **Complex** → allocate result in linear memory, return pointer. Host lifts via `CView`.

### Complete Developer Experience

```cpp
// calculator.cpp
#include <psizam/component.hpp>

struct calculator {
    int32_t add(int32_t a, int32_t b) { return a + b; }
    int32_t mul(int32_t a, int32_t b) { return a * b; }
    std::string greet(std::string name) { return "Hello, " + name; }
};

PZAM_COMPONENT(calculator,
    method(add, a, b),
    method(mul, a, b),
    method(greet, name)
)
```

Compile: `clang++ --target=wasm32-wasi -o calculator.wasm calculator.cpp`

The `.wasm` binary contains:
- Typed exports matching Canonical ABI signatures
- `component-type` custom section with valid WIT
- `cabi_realloc` export
- Standard-compatible — runs on psiserve, wasmtime, or any Component Model host

No external code generators. No build steps beyond the compiler. One file, one macro.

---

## Async Dispatch

The marshaling architecture naturally enables async dispatch because the host is always in the middle and the data is fully copied at boundaries.

### Why Marshaling Enables Async

In synchronous dispatch:
```
A calls func(args) → host lifts → host lowers into B → B executes → result returns to A
```

With async, the host can decouple the two sides:
```
A calls func(args) → host lifts args into host-owned buffer → A continues
                     ... later ...
                     host lowers from buffer into B → B executes → result buffered
                     ... later ...
                     A polls/awaits → host lowers result into A → A reads result
```

The key insight: once the host has lifted the args from A's memory, A's memory is no longer needed. The args exist independently in the host's buffer (or in the message ring). A can continue executing, mutate its memory, even handle other calls. The lifted args are a snapshot.

### Integration with psix Process Model

This fits directly into the psix IPC design (see `psix-ipc-design.md`):

- **Same-thread RPC** — synchronous: lift from caller, lower into callee, switch process context, callee runs, switch back. One copy per direction.
- **Same-thread signal (void, async)** — lift args, enqueue for each slot, caller continues immediately. Slots run when scheduled.
- **Cross-thread RPC** — lift args into sender's message ring, notify receiver thread, sender process blocks. Receiver lifts from ring, lowers into target, executes, sends result back. Sender resumes.
- **Cross-thread signal (async)** — lift args into ring, notify, sender continues. No blocking.

The marshaling layer produces a self-contained buffer (fracpack or Canonical ABI layout) that can be:
- Consumed immediately (same-thread sync)
- Queued in a ring buffer (cross-thread)
- Stored for later delivery (async signals)
- Broadcast to multiple receivers (1:N signals — lifted once, lowered N times)

### Signal/Slot Pattern

Signals are naturally async. When a module emits a signal:

1. Host lifts the signal args from the emitter's memory (one lift)
2. For each connected slot:
   - If same thread: lower into slot's memory, schedule for execution
   - If cross thread: copy into message ring, notify
3. Emitter continues immediately (void signals) or blocks until all slots complete (return-value signals)

The lifted args are shared across all slots — lifted once, lowered N times. Different slots can even use different conventions if they're implemented with different toolchains.

---

## Direct Linking Constraints

### The Ownership Problem

When two modules share linear memory (bound or linked mode), both have their own `malloc`/`free` implementations managing the same heap. If module A allocates memory and passes a pointer to module B, and B calls `free()` on it, B's allocator doesn't recognize the block — corruption or crash.

### When Direct Linking Works

Direct linking (shared memory, no marshaling) is only safe between modules that:

1. **Share a single allocator** — both link the same libc, so `malloc`/`free` are the same implementation
2. **Follow explicit ownership conventions** — clearly documented who allocates and who frees
3. **Are built together** — same toolchain, same ABI, tested as a unit

This is the "bound" and "linked" composition modes. They're equivalent to static linking a `.a` library — the modules become one program. No WIT marshaling is involved. Direct `call` instructions, shared globals, shared linear memory.

### When Marshaling Is Required

Any time the modules don't share an allocator — which is always the case for:
- Modules from different languages (Rust + C++)
- Modules from different authors/teams
- Untrusted modules
- Cross-service calls

In these cases, the host must intermediate: lift from source memory, lower into target memory using `cabi_realloc`, and let each side manage its own allocations.

---

## Memory Lifecycle

### Who Allocates

For calls into a WASM module, the **callee allocates** in its own linear memory. The host calls `cabi_realloc` (exported by the callee) to allocate space, then copies data in.

### Who Frees

- **Arguments**: the callee owns them after the call. If the callee doesn't need them after returning, it frees them. If it stores them, they stay allocated.
- **Return values**: the callee allocates them via its own allocator. The host reads them, lowers into the caller's memory, then calls `cabi_post_return_<func>` (if exported) to let the callee free the return buffer.

### Bump Allocator Optimization

Many WASM modules (especially short-lived request handlers) use a bump allocator for `cabi_realloc` — it just advances a pointer, never frees. This is ideal for the request-per-fork model in psiserve: the entire linear memory is recycled when the process dies.

The host can detect this at deploy time by inspecting the `cabi_realloc` bytecode. If it's a simple bump allocator:
- No `cabi_post_return_*` calls needed
- Multiple allocations can be batched into one (pre-compute total size)
- The host knows `free()` is a no-op

For long-running modules with real allocators, the full allocation/free protocol applies.

---

## Validation

The host validates all data crossing the boundary:

1. **Pointer range**: `ptr + len <= memory.size()` for every `(ptr, len)` pair
2. **Alignment**: pointer is aligned to the type's natural alignment
3. **Recursive validation**: `list<string>` means validating the outer list bounds, then each inner string's `(ptr, len)`
4. **Discriminant range**: variant/enum discriminants must be in range

Validation is structural, not semantic. A "string" could be invalid UTF-8, a record could have nonsensical field values. The host ensures memory safety (no out-of-bounds reads), not data correctness. Worst case, the receiver gets garbage — but the host doesn't crash.

### WIT/Binary Mismatch

If a module's WIT doesn't match its actual WASM exports (developer error or malicious), the host detects this at deploy time:

- Export names in WIT must exist in the WASM export section
- The Canonical ABI lowering of the WIT function type must match the core WASM signature
- Mismatches are rejected before any code runs

This is structural validation only. The host cannot verify that the module's code actually implements the semantics described by its WIT. That's the module developer's responsibility, enforced by testing and attestation (see `.pzam` attestation in Phase 7).

---

## Implementation Plan

### Phase 1: CView/COwned Type System (psio)

- Implement `CView<T>` — non-owning view with `string_view`/`span` fields, frees only descriptor arrays
- Implement `COwned<T>` — owning wrapper that frees all `cabi_realloc`'d allocations on destruction
- Implement `is_viewable<T>` — compile-time check for zero-alloc viewability
- Implement promotion: `T(COwned<T> const&)` deep-copy constructor
- Test: round-trip CView/COwned for records with strings, nested lists, optional fields

### Phase 2: constexpr WIT Generation + PZAM_COMPONENT Macro

- Implement `consteval wit_string<T>()` using `psio::reflect<T>`
- Implement `ComponentProxy<T>::call<MemberPtr>` — type-driven dispatch via member pointer introspection
- Implement `PZAM_COMPONENT` macro: PSIO_REFLECT + constexpr WIT in custom section + `cabi_realloc` + 16-wide `extern "C"` exports
- Test: compile calculator component to WASM, verify WIT readable by `wasm-tools component wit`, verify exports are callable

### Phase 3: Host-Side Marshaling

- Read `component-type` custom section from WASM/`.pzam` binaries
- Validate WIT function types against WASM export signatures
- Implement allocator classification (inspect `cabi_realloc` bytecode)
- Implement lowering: single-alloc direct placement (bump) or per-field `cabi_realloc` (real malloc)
- Implement lifting: zero-alloc view into WASM linear memory with bounds/alignment validation
- Wire into `pzam_dynamic_instance::call()`

### Phase 4: JIT Adapter Generation + Composition

- Generate type-specialized adapter native code at compose time
- Handle all-scalar fast path (direct flat arg forwarding)
- Handle complex types (allocator-appropriate lowering + pointer fixup)
- `pzam compose` CLI: read WIT from both modules, verify imports match exports, generate adapters
- Cache adapters keyed by type signature hash

### Phase 5: Async Dispatch + psix Integration

- Integrate marshaling with psix message ring
- Lift args into ring buffer (self-contained snapshot, survives caller mutation)
- Support signal/slot pattern: lift once, lower N times
- Same-thread optimization: direct process switch, skip ring
- Return-value signals: collect results, merge, return to emitter

---

## File Summary

| File | Layer | Purpose |
|------|-------|---------|
| `psio/wit_types.hpp` | psio | WIT type structs (exists) |
| `psio/wit_parser.hpp` | psio | .wit text parser (exists) |
| `psio/wit_gen.hpp` | psio | C++ reflection → WIT generation (exists) |
| `psio/ctype.hpp` | psio | `CView<T>`, `COwned<T>` — reflection-driven type projections (new) |
| `psizam/canonical_abi.hpp` | psizam | Canonical ABI layout computation (exists) |
| `psizam/component.hpp` | psizam | `PZAM_COMPONENT` macro + `ComponentProxy<T>` (new) |
| `psizam/marshal.hpp` | psizam | Host-side lift/lower, allocator classification, adapter generation (new) |
| `psizam/pzam_dynamic.hpp` | psizam | Dynamic dispatch API (new) |

## Relationship to Other Design Documents

- **[psix IPC Design](psix-ipc-design.md)** — defines the process model, message ring, and cross-thread coordination that this marshaling layer plugs into
- **[psiserve Design](psiserve-design.md)** — defines bound/linked/connected composition modes; this document specifies how the "connected" mode marshaling works
- **[WIT Integration Plan](../plans/purring-jingling-squid.md)** — the implementation plan for WIT type structs, parser, and Canonical ABI (Phases 1-6 of that plan are prerequisites for this design)
