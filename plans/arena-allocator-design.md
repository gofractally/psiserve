# psizam Arena Allocator Design: Bounded Memory for Parse and Execution

## Problem

psizam currently uses `std::vector<T>` for all parsed module data and runtime state. Each vector independently calls `malloc`, and each has its own configurable limit (`max_type_section_elements`, `max_table_elements`, `max_call_depth`, etc.). This creates three problems:

1. **Allocation amplification** — `parse_section_impl` reads a count from untrusted WASM input and calls `vector::resize(count)`, allocating `count * sizeof(T)` bytes before parsing a single element. A 5-byte LEB128 encoding `count = 268M` triggers `malloc(20GB)` for the type section alone. The allocation succeeds (or OOMs the host) before the parser discovers the count is fabricated.

2. **Limit proliferation** — 15+ individual limits, all defaulting to `0xFFFFFFFF`. Each new WASM proposal (exception handling, SIMD, GC, multi-memory) adds new sections and structures requiring new limits. The limits are coupled to struct sizes — adding a field to `func_type` silently increases the amplification ratio.

3. **Runtime fragmentation** — execution state (operand stack, call stack, tables, globals, EH stacks) is scattered across independent heap allocations, each with its own growth policy. `table.grow` does `new[]` + copy + `delete[]`. There is no aggregate bound on total runtime memory.

## Design

Replace scattered `std::vector` allocations with two bounded arenas — one for parsed module data, one for runtime execution state. Linear memory stays separate (guard-page sandboxed).

### Three memory budgets

| Budget | Contents | Backing | Lifetime |
|--------|----------|---------|----------|
| `max_module_memory` | All parsed module data | `malloc` arena | module lifetime |
| `max_instance_memory` | Runtime stacks, tables, globals | `malloc` arena | instance lifetime |
| `max_linear_memory` | WASM linear memory (pages) | `mmap` + guard pages | instance lifetime |

Three numbers replace fifteen limits. The arena capacity IS the policy.

### Module arena

Holds everything currently in the `module` struct:

```
┌─────────────────────────────────────────────────────────┐
│  types  │  imports  │  functions  │  tables  │ memories │
│─────────┼───────────┼─────────────┼──────────┼──────────│
│  globals │  exports  │  elements  │  code    │  data    │
│──────────┼───────────┼────────────┼──────────┼──────────│
│  tags   │  names    │  inner data (param_types,         │
│         │           │  field_str, elem entries, locals,  │
│         │           │  data bytes, branch_hints, ...)    │
└─────────────────────────────────────────────────────────┘
  ← bump pointer advances →                    capacity ─┤
```

**Allocation**: `malloc(max_module_memory)` once. All section data bump-allocated from it.

**Data types**: Replace `std::vector<T>` in `module` with `span<T>` (pointer + length into the arena). Inner vectors (param_types, field_str, etc.) become spans too. The `module` struct becomes:

```cpp
// Before (heap-allocated, each vector does its own malloc)
struct func_type {
   std::vector<value_type> param_types;   // 24 bytes, separate heap alloc
   std::vector<value_type> return_types;  // 24 bytes, separate heap alloc
   uint64_t sig_hash;
   uint8_t  return_count;
   value_type return_type;
   value_type form;
};  // ~80 bytes + 2 heap allocations per instance

// After (arena-backed, no per-element heap alloc)
struct func_type {
   arena_span<value_type> param_types;    // 8 bytes (ptr + u32 len)
   arena_span<value_type> return_types;   // 8 bytes
   uint64_t sig_hash;
   uint8_t  return_count;
   value_type return_type;
   value_type form;
};  // ~32 bytes, zero heap allocations
```

**Parse flow**:

```
1. Pre-scan section headers → collect all counts
2. Compute total arena need:
     sum(section_count[i] * sizeof(section_elem[i]))
   + sum(inner data sizes from bytecode)
3. Assert total <= max_module_memory
4. malloc(max_module_memory)
5. Bump-allocate each section array, parse elements in-place
6. Inner data (param_types, etc.) bump-allocated as each element is parsed
```

Step 2 can't be exact for inner data (param_types sizes aren't known until parsed), but section headers give the outer counts. The arena capacity handles the rest — if inner data pushes past the budget, the bump allocator traps.

**Simplification**: Even without a pre-scan, the current single-pass parse works. Replace `elems.resize(count)` with `elems = arena.alloc<T>(count)`. The arena's capacity check replaces all per-section limit checks. If `count * sizeof(T)` would exceed remaining capacity, the arena traps immediately — no fabricated-count allocation bomb.

### Instance arena

Holds all runtime execution state for one WASM instance:

```
┌──────────────────────────────────────────────────────────────┐
│ globals │ call stack │ dropped flags │ table ptrs │ table    │
│         │ (fixed)    │ (elem+data)   │ + sizes    │ sizes    │
│─────────┼────────────┼───────────────┼────────────┼──────────│
│  operand stack                       │  table 0 entries      │
│  (max_call_depth * max_stack_size)   │  (limits.maximum)     │
└──────────────────────────────────────────────────────────────┘
  ← all sizes known after compile →         grows → │ capacity │
```

**All sizes are known after compilation, before execution**:

| Structure | Size source | When known |
|-----------|-------------|------------|
| Globals | `module.globals.size() * max_global_size` | parse |
| Call stack frames | `max_call_depth * sizeof(activation_frame)` | config |
| Operand stack | `max_call_depth * max(function.stack_size)` | compile |
| Table entries | `sum(table[i].limits.maximum * sizeof(table_entry))` | parse |
| Table metadata | `num_tables * (sizeof(table_entry*) + sizeof(uint32_t))` | parse |
| Dropped flags | `(num_elements + num_data_segments)` bits | parse |

**Compute**:

```cpp
size_t instance_size =
    globals_count * sizeof(global_value) +
    max_call_depth * sizeof(activation_frame) +
    max_call_depth * max_function_stack_slots * sizeof(operand_stack_elem) +
    num_tables * (sizeof(table_entry*) + sizeof(uint32_t)) +
    sum(table_max[i] * sizeof(table_entry)) +
    ceil(num_elements + num_data_segments, 8);

assert(instance_size <= max_instance_memory);
char* arena = (char*)malloc(instance_size);
```

**`table.grow`**: Tables are pre-allocated to `limits.maximum`. The `table_sizes[i]` counter tracks the logical size. `table.grow` just advances the counter and initializes new entries — no realloc, no copy, no free. If `limits.maximum` is not declared, the table's allocation extends to the arena's remaining capacity.

**Zero runtime allocation**: No `malloc`, `new`, or `realloc` during WASM execution. Every memory access is a pointer into one of three regions (module arena, instance arena, linear memory). Deterministic, cache-friendly, and impossible to OOM mid-execution.

### Linear memory (unchanged)

Linear memory continues to use `mmap` + guard pages. It has fundamentally different requirements:

- Needs `PROT_NONE` guard pages for sandboxing (no per-access bounds checks)
- Needs page-granular `mmap`/`mprotect` for `memory.grow`
- Needs COW support for instance forking (psiserve's fork model)
- Already has its own budget via `max_pages`

### JIT code (unchanged)

JIT-generated native code continues to use the existing `growable_allocator` with `mmap` + `mprotect(PROT_EXEC)`. It needs executable permissions that a malloc arena can't provide.

## Limits that survive

A few limits remain because they bound **compute or runtime expansion**, not parse-time allocation:

| Limit | Protects against |
|-------|-----------------|
| `max_call_depth` | runtime recursion depth (also sizes call stack in instance arena) |
| `max_code_bytes` | JIT compile time per function |
| `max_nested_structures` | parser control stack depth |
| `max_br_table_elements` | per-instruction parse compute |
| `max_memory_offset` | guard page coverage (static load/store offsets) |

These are compute/safety bounds, not memory bounds. The arena budgets handle all memory bounding.

## Limits that are removed

These become redundant — the arena capacity subsumes them:

| Removed | Was bounding |
|---------|-------------|
| `max_section_elements` | type/import/function/global/export/element/data section counts |
| `max_type_section_elements` | (same, per-section) |
| `max_import_section_elements` | |
| `max_function_section_elements` | |
| `max_global_section_elements` | |
| `max_export_section_elements` | |
| `max_element_section_elements` | |
| `max_data_section_elements` | |
| `max_element_segment_elements` | elem entries per segment |
| `max_data_segment_bytes` | data bytes per segment |
| `max_linear_memory_init` | cumulative active data segment init |
| `max_symbol_bytes` | import/export name lengths |
| `max_local_sets` | local variable groups per function |
| `max_table_elements` | table entries (instance arena handles this) |
| `max_func_local_bytes` | per-function stack (instance arena handles this) |
| `max_mutable_global_bytes` | mutable globals (instance arena handles this) |

## Migration path

### Phase 1: Module arena (parse safety)

1. Introduce `arena_span<T>` — `{T* data; uint32_t size;}`, non-owning
2. Add `module_arena` — bump allocator backed by `malloc(max_module_memory)`
3. Change `parse_section_impl` to allocate from module_arena instead of `vector::resize`
4. Convert `module` struct fields from `std::vector<T>` to `arena_span<T>` one section at a time
5. Convert inner vectors (param_types, field_str, etc.) to `arena_span`
6. Remove per-section limit options as they become redundant
7. Keep `growable_allocator` for JIT code output (needs mmap + PROT_EXEC)

Each step is independently testable — convert one section, run spec tests, repeat.

### Phase 2: Instance arena (runtime safety)

1. Introduce `instance_arena` — bump allocator sized after compilation
2. Move globals, call stack, operand stack into instance arena
3. Move table entries into instance arena (pre-allocate to limits.maximum)
4. Move EH stacks, dropped flags into instance arena
5. Remove per-feature runtime limits as they become redundant
6. `table.grow` becomes a counter increment, no realloc

### Phase 3: COW integration (psiserve)

The instance arena layout enables efficient COW forking for psiserve:

- Fork = `mmap(MAP_PRIVATE)` over the instance arena + linear memory
- Copy-on-write at page granularity
- Template instance arena is read-only after `_init()`
- Per-connection instances share pages until mutation

## Compatibility

The `options` struct keeps all existing fields during migration, marked deprecated. Code using the old per-section limits continues to work — the arena just enforces a tighter aggregate bound underneath. New code uses `max_module_memory` and `max_instance_memory`.

## Summary

| Before | After |
|--------|-------|
| 15+ configurable limits | 3 memory budgets |
| `malloc` per vector per section | 1 `malloc` per module |
| `new[]`/`delete[]` on table.grow | counter increment |
| amplification: 5 bytes input → 20GB alloc | bounded: arena capacity |
| limits coupled to struct sizes | struct-size-independent |
| OOM possible mid-execution | impossible — pre-allocated |
| scattered heap allocations | contiguous, cache-friendly |
