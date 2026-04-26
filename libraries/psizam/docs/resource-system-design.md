# WIT Resource System Design

Status: design (2026-04-19). Not yet implemented.

## Overview

Resources are host-owned objects referenced by the guest through opaque
u32 handles. The host maintains a handle table; the guest holds integer
indices. When the guest drops a resource, the host destructs the
underlying object. The guest cannot forge, duplicate, or access the
object except through the host-provided methods.

## Handle Table

The handle table is the core data structure. It maps u32 handles to
host-owned objects. It must be:

- **O(1) lookup** — every method call goes through it
- **O(1) create/destroy** — no allocation on create, no compaction on destroy
- **Bounded** — the guest cannot allocate unbounded handles
- **No host heap allocation per handle** — the table is pre-allocated

### Design: fixed-capacity slab with free list

```cpp
template <typename T, uint32_t Capacity = 256>
struct handle_table {
   struct slot {
      union {
         T        value;         // live object
         uint32_t next_free;     // free list pointer (when empty)
      };
      uint32_t generation = 0;   // detect use-after-free
      bool     occupied   = false;
   };

   slot     slots[Capacity];
   uint32_t free_head = 0;      // head of free list
   uint32_t live_count = 0;
   uint32_t max_live = Capacity; // policy cap (can be < Capacity)

   // Pack handle = (index << 16) | (generation & 0xFFFF)
   // - index: position in slots array
   // - generation: incremented on each reuse, detects stale handles

   uint32_t create(T&& value) {
      if (live_count >= max_live)
         trap("resource limit exceeded");
      uint32_t idx = free_head;
      free_head = slots[idx].next_free;
      slots[idx].value = std::move(value);
      slots[idx].occupied = true;
      ++live_count;
      return (idx << 16) | (slots[idx].generation & 0xFFFF);
   }

   T& get(uint32_t handle) {
      uint32_t idx = handle >> 16;
      uint32_t gen = handle & 0xFFFF;
      assert(idx < Capacity);
      assert(slots[idx].occupied);
      assert((slots[idx].generation & 0xFFFF) == gen);
      return slots[idx].value;
   }

   void destroy(uint32_t handle) {
      uint32_t idx = handle >> 16;
      slots[idx].value.~T();
      slots[idx].occupied = false;
      slots[idx].generation++;    // invalidate stale handles
      slots[idx].next_free = free_head;
      free_head = idx;
      --live_count;
   }
};
```

### Properties

- **No host heap allocation**: the slab is a fixed array, allocated
  once (in the host's stack or as a member of the host struct). No
  `new`, no `std::vector` resize, no `std::unordered_map` insert.

- **Bounded**: `max_live` caps how many resources the guest can hold
  simultaneously. Exceeding it traps. The node operator sets this
  limit per-policy (e.g., 16 modules, 4 instances, 64 export handles).

- **Use-after-free detection**: the generation counter detects stale
  handles. If the guest destroys a resource and tries to use the old
  handle, the generation won't match → trap.

- **No forgery**: handles are (index, generation) pairs. Guessing a
  valid pair requires knowing the current generation of a live slot.
  Not cryptographically secure but sufficient for sandboxed WASM.

## Resource Limits (Abuse Prevention)

Each resource type has a separate cap. The caps are part of the
instance policy:

```cpp
struct resource_limits {
   uint32_t max_modules    = 16;
   uint32_t max_instances  = 4;
   uint32_t max_exports    = 64;   // resolved export handles
   uint32_t max_cursors    = 32;   // database cursors
   uint32_t max_subtrees   = 8;    // psitri subtree references
};
```

The blockchain process sets these limits when launching a smart
contract. The limits prevent a malicious contract from:

- Allocating thousands of module handles (memory exhaustion)
- Opening thousands of database cursors (file descriptor exhaustion)
- Creating instances without destroying them (linear memory exhaustion)

The handle table itself is stack-allocated (or embedded in the host
struct) at a fixed capacity. No heap allocation even when the guest
creates resources at the maximum allowed rate.

## C++ Surface: PSIO1_RESOURCE

```cpp
// Declaration (shared header):
struct wasm_runtime {
   resource module {
      static module create(std::string_view wasm_bytes);
      instance instantiate();
      void drop();
   };

   resource instance {
      uint32_t resolve_export(name_id func_name);
      uint64_t call_by_index(uint32_t func_index,
                             uint64_t arg0, uint64_t arg1);
      void drop();
   };

   static module get_module(name_id name);
};
```

Mapped to C++ with PSIO macros:

```cpp
// Host side — the resource is a C++ class, the handle table
// is managed by the macro-generated glue.

class wasm_module_resource {
   psizam::module_handle mod;
public:
   explicit wasm_module_resource(std::string_view wasm_bytes);
   wasm_instance_resource instantiate();
   // destructor called by handle_table::destroy
   ~wasm_module_resource();
};

class wasm_instance_resource {
   psizam::instance inst;
   uint32_t cabi_realloc_idx;  // pre-resolved
public:
   explicit wasm_instance_resource(psizam::instance i);
   uint32_t resolve_export(psio1::name_id func_name);
   uint64_t call_by_index(uint32_t func_index,
                          uint64_t arg0, uint64_t arg1);
   ~wasm_instance_resource();
};

PSIO1_RESOURCE(wasm_module_resource,
   constructor(wasm_bytes),
   method(instantiate),
   drop)

PSIO1_RESOURCE(wasm_instance_resource,
   method(resolve_export, func_name),
   method(call_by_index, func_index, arg0, arg1),
   drop)
```

### What PSIO1_RESOURCE generates

On the **host side** (inside PSIO1_HOST_MODULE):

```cpp
// For each resource:
//   1. A handle_table<Resource, Capacity> member on the Host
//   2. Host functions: [resource]_create, [resource]_method, [resource]_drop
//   3. Each host function validates the handle, extracts the object,
//      calls the method, returns the result

// Generated:
handle_table<wasm_module_resource, 16> module_handles;

uint32_t wasm_module_resource_create(std::string_view wasm_bytes) {
   return module_handles.create(
      wasm_module_resource{wasm_bytes});
}

uint32_t wasm_module_resource_instantiate(uint32_t handle) {
   auto& mod = module_handles.get(handle);
   auto inst = mod.instantiate();
   return instance_handles.create(std::move(inst));
}

void wasm_module_resource_drop(uint32_t handle) {
   module_handles.destroy(handle);
}
```

On the **guest side** (via PSIO1_IMPORT):

```cpp
// A thin RAII wrapper around the u32 handle.
// drop() is called by the destructor.

class module {
   uint32_t handle_;
public:
   explicit module(std::string_view wasm_bytes)
      : handle_(wasm_runtime::module_create(wasm_bytes)) {}
   ~module() { wasm_runtime::module_drop(handle_); }
   module(module&& o) : handle_(o.handle_) { o.handle_ = UINT32_MAX; }
   module(const module&) = delete;

   instance instantiate() {
      return instance{wasm_runtime::module_instantiate(handle_)};
   }
};
```

## Where Host Allocations Happen

The design minimizes host-side allocations:

| Operation | Allocation? | Where |
|---|---|---|
| Create resource handle | No | slab slot (pre-allocated) |
| Destroy resource handle | No | return to free list |
| Look up handle | No | array index |
| load_module (parse WASM) | Yes | module parsing + JIT compilation |
| instantiate (create backend) | Yes | wasm_allocator + backend |
| resolve_export | No | export table scan (one-time) |
| call_by_index | No | pure execution |

The only allocations are for the WASM engine's internal state
(module parsing, JIT code, linear memory). These are bounded by the
instance policy (max_pages, compile_timeout) and allocated from the
memory pool (pre-allocated at startup).

The handle table itself: **zero allocations**. It's a fixed array
embedded in the host struct. Creating a handle = write to a slot.
Destroying = mark the slot free. Looking up = array index + generation
check.

## Canonical ABI Mapping

In the Component Model, resources are passed as `i32` handles. The
canonical ABI defines:

- `resource.new` → host creates, returns i32 handle
- `resource.rep` → host extracts the representation
- `resource.drop` → guest releases the handle

Our mapping:

| WIT | WASM import | Host function |
|---|---|---|
| `constructor(args)` | `[resource]_create(args) → i32` | `handle_table.create(Resource{args})` |
| `method(args)` | `[resource]_method(i32 self, args) → ret` | `handle_table.get(self).method(args)` |
| `drop` | `[resource]_drop(i32 self)` | `handle_table.destroy(self)` |

The `self` parameter is always the first i32 arg. Methods are
dispatched as `interface.resource_method` in the WASM import table.

## Scoped Resource Cleanup

When a smart contract's execution ends (transaction complete, timeout,
trap), ALL its resources must be cleaned up. The handle table supports
bulk cleanup:

```cpp
void destroy_all() {
   for (uint32_t i = 0; i < Capacity; ++i) {
      if (slots[i].occupied) {
         slots[i].value.~T();
         slots[i].occupied = false;
         slots[i].generation++;
      }
   }
   live_count = 0;
   // rebuild free list
}
```

This runs in O(Capacity), not O(live_count) — but Capacity is small
(16-256). No per-handle tracking of "which transaction owns this"
needed. The entire handle table is scoped to one execution context.

## Export Handle Optimization

`resolve_export` returns a u32 index into the module's export table.
This is NOT a resource handle — it's a plain integer with no
generation counter and no handle table. It's valid for the lifetime
of the instance. No cleanup needed.

The caller caches these indices and uses them for all subsequent
calls. The resolve happens once (string → index), all calls use
the index (array lookup).

## Database Cursor Resources

The same handle_table pattern extends to psitri cursors:

```cpp
PSIO1_RESOURCE(db_cursor_resource,
   constructor(table_name, index_id, lower_bound, upper_bound),
   method(next),        // → optional<row>
   method(prev),        // → optional<row>
   method(get),         // → optional<row> at current position
   drop)
```

The cursor holds a psitri cursor (which pins an MVCC snapshot).
The handle table caps the number of open cursors. Drop releases
the snapshot pin.

## Summary

| Concern | Solution |
|---|---|
| Handle lookup speed | Fixed array, O(1) index |
| Use-after-free | Generation counter |
| Handle forgery | (index, generation) pair |
| Resource exhaustion | Per-type capacity cap |
| Host allocation | Zero (slab pre-allocated) |
| Cleanup on exit | bulk destroy_all() |
| WASM ABI | i32 handle, first param to methods |
| Type safety | Separate handle_table per resource type |
