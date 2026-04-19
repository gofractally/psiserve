# Runtime API Design

Status: design (2026-04-18). Not yet implemented.

## Overview

The runtime is the top-level object that manages WASM module loading,
compilation, linking, caching, and instance lifecycle. It sits between
the chain state (psitri tables holding .wasm/.pzam/.a bytes) and the
execution layer (backends, linear memory, fibers).

## Instance Policy

Every instantiation carries a policy that controls safety, performance,
and resource characteristics. The policy is set by the node operator —
not the contract author.

```cpp
struct instance_policy {
   // ── Safety ──────────────────────────────────────────────────────
   enum class float_mode  { soft, native };
   enum class mem_safety  { guarded, checked, unchecked };

   float_mode  floats = float_mode::soft;    // deterministic by default
   mem_safety  memory = mem_safety::checked;  // bounded by default

   // ── Compilation ─────────────────────────────────────────────────
   enum class compile_tier  { interpret, jit_fast, jit_optimized };
   enum class compile_trust { native, sandboxed };

   compile_tier   initial    = compile_tier::jit_fast;
   compile_tier   optimized  = compile_tier::jit_optimized;
   compile_trust  trust      = compile_trust::native;
   uint32_t       compile_timeout_ms = 5000;

   // ── Resources ───────────────────────────────────────────────────
   uint32_t  max_pages   = 256;         // 16MB linear memory cap
   uint32_t  max_stack   = 64 * 1024;   // 64KB call stack

   // ── Metering / Interrupts ───────────────────────────────────────
   enum class meter_mode {
      none,       // no gas, no checks (trusted replay)
      gas_trap,   // gas exhaustion = trap
      gas_yield,  // gas exhaustion = yield fiber, restock
      timeout,    // gas exhaustion = check wall clock, restock or trap
   };

   meter_mode  metering   = meter_mode::gas_trap;
   int64_t     gas_budget = 1'000'000;
   int64_t     gas_slice  = 10'000;     // restock per yield/timeout check
   uint32_t    timeout_ms = 5000;       // for meter_mode::timeout
};
```

## Runtime API

```cpp
class runtime {
public:
   explicit runtime(runtime_config config = {});

   // ── Library registration ────────────────────────────────────────
   // Register chain-resident libraries. The runtime caches compiled
   // native code (.pzam) keyed by content hash. Libraries are .wasm
   // or .a archives.

   void register_library(std::string_view name,
                          std::span<const uint8_t> bytes);

   // ── Host interface registration ─────────────────────────────────
   // Register native C++ implementations for host-provided interfaces.
   // These satisfy imports that no WASM module provides.

   template <typename... Interfaces, typename Host>
   void provide(Host& host);

   // ── Module preparation ──────────────────────────────────────────
   // Prepare a module for execution: parse, compile (or load cached
   // .pzam), resolve imports against registered libraries and host
   // interfaces. Returns a template handle.
   //
   // The template is the "golden copy" — compiled code + resolved
   // link tables + initial memory snapshot. Never executed directly.

   module_handle prepare(std::span<const uint8_t> wasm_bytes,
                         const instance_policy& policy);

   // Check what imports are unresolved before preparing.
   std::vector<unresolved_import> check(
      std::span<const uint8_t> wasm_bytes);

   // ── Instantiation ───────────────────────────────────────────────
   // Create a live instance from a prepared template. The instance
   // gets fresh linear memory (from the arena pool or guard-page pool
   // depending on policy.memory).
   //
   // For server/fiber mode: one instance per fiber.
   // For blockchain: one instance per transaction.

   instance instantiate(const module_handle& tmpl);

   // ── Cache management ────────────────────────────────────────────

   struct cache_stats {
      size_t   modules_cached;
      size_t   pzam_bytes;
      size_t   compile_time_total_ms;
   };

   cache_stats stats() const;
   void evict(std::span<const uint8_t> content_hash);
   void clear_cache();
};
```

## Module Handle

```cpp
class module_handle {
public:
   // Introspection
   std::vector<interface_desc> exports() const;
   std::vector<interface_desc> imports() const;
   std::vector<std::string>    wit_sections() const;

   // Compilation metrics (for oracle reporting)
   uint32_t compile_time_ms() const;
   uint32_t native_code_size() const;
   uint32_t wasm_size() const;
};
```

## Instance

```cpp
class instance {
public:
   // ── Typed call (shared header available) ────────────────────────
   template <typename Tag>
   auto as();
   // Returns a typed proxy: instance.as<greeter>().concat("a", "b")

   // ── Dynamic call (no shared header, WIT-driven) ─────────────────
   native_value call(std::string_view interface_name,
                     std::string_view function_name,
                     native_value* args);

   // ── State ───────────────────────────────────────────────────────
   int64_t gas_remaining() const;
   void    set_gas(int64_t budget);
   void    interrupt();              // set gas to -1

   // ── Memory ──────────────────────────────────────────────────────
   char*       linear_memory();
   std::size_t memory_size() const;

   // ── Lifecycle ───────────────────────────────────────────────────
   // Instance is move-only. Dropped at scope exit.
   // Memory is returned to the pool (madvise DONTNEED).
   ~instance();
   instance(instance&&) noexcept;
   instance& operator=(instance&&) noexcept;
};
```

## Typical Usage

### Block producer
```cpp
runtime rt;
rt.register_library("libc++", chain.load(libc_hash));
rt.provide<chain_api, crypto_api>(host);

auto tmpl = rt.prepare(chain.load(contract_hash), {
   .floats   = float_mode::soft,
   .memory   = mem_safety::guarded,
   .initial  = compile_tier::jit_fast,
   .optimized = compile_tier::jit_optimized,
   .metering = meter_mode::timeout,
   .timeout_ms = 5000,
});

// Per transaction:
auto inst = rt.instantiate(tmpl);
inst.as<contract>().apply(tx.action, tx.data);
// inst dropped — memory returned to guard-page pool
```

### Replay node
```cpp
auto tmpl = rt.prepare(chain.load(contract_hash), {
   .floats   = float_mode::soft,
   .memory   = mem_safety::unchecked,
   .initial  = compile_tier::jit_optimized,
   .metering = meter_mode::none,
});

for (auto& tx : block.transactions) {
   auto inst = rt.instantiate(tmpl);
   inst.as<contract>().apply(tx.action, tx.data);
}
```

### HTTP server (psiserve)
```cpp
runtime rt;
rt.provide<http_api, db_api>(host);

auto tmpl = rt.prepare(app_wasm, {
   .floats   = float_mode::native,
   .memory   = mem_safety::checked,
   .initial  = compile_tier::jit_fast,
   .optimized = compile_tier::jit_optimized,
   .metering = meter_mode::gas_yield,
   .gas_slice = 10'000,
});

// Per HTTP request (on a fiber):
void handle_request(http_request& req) {
   auto inst = rt.instantiate(tmpl);
   auto response = inst.as<app>().handle(req.method, req.path, req.body);
   req.respond(response);
   // inst dropped — memory returned to arena pool
}
```

## Memory Pool Integration

The runtime owns the memory pool. The pool type depends on the policy:

| mem_safety | Pool type | Slot size | Max concurrent |
|---|---|---|---|
| guarded | Guard-page pool | 8GB virtual per slot | cores × call_depth |
| checked | Arena pool | max_pages × 64KB per slot | thousands (fiber count) |
| unchecked | Arena pool | max_pages × 64KB per slot | cores × call_depth |

```cpp
struct runtime_config {
   size_t guarded_pool_size = 64;     // for mem_safety::guarded
   size_t arena_size_gb     = 256;    // for mem_safety::checked/unchecked
};
```

Guard-page pool: allocated at startup, fixed VMA count, never modified.
Arena pool: one mmap at startup, slotted by pointer arithmetic, zero VMA
churn. Reset via madvise(MADV_DONTNEED).

## .pzam Cache

The cache key is:
```
hash(wasm_content_hash, floats, compile_tier, mem_safety)
```

Different policies that affect native code generation produce different
cache entries. Policies that only affect runtime behavior (gas_budget,
timeout_ms, max_pages) share the same cached .pzam.

The .pzam is stored in the psitri table alongside the .wasm. Loading
it is a view (zero-copy). If the .pzam doesn't exist for a given
policy combination, the runtime compiles on first use and stores the
result.

## Linking Model

Import resolution order:
1. Registered libraries (static link — shared memory, direct calls)
2. Other modules in the composition (isolated link — bridge)
3. Host-provided interfaces (host bridge)
4. Unresolved → error

Libraries are linked statically (shared memory) because they're
trusted code (libc++, system libraries). Application modules are
linked in isolation (separate memories, canonical ABI bridge) because
they're from different authors/trust domains.

The link tables (resolved function indices, bridge programs) are part
of the module_handle and cached alongside the .pzam.
