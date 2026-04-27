# psio: stack-cached size precomputation for length-prefix encoding

**Status:** design draft.

## Problem

Several wire formats psio supports (protobuf, fracpack, ssz, pssz, bin,
borsh, bincode, avro, pjson, …) have **length-prefixed length-delimited
fields**: a varint or fixed-width prefix that gives the byte size of
the bytes that follow. To write the prefix, the encoder must know the
size *before* writing the body.

The naïve solution is to walk the value tree twice: once to compute
sizes, once to emit bytes. The current psio protobuf encoder does
exactly this:

```cpp
write(node):
   emit_tag(node)
   emit_varint(size(node))      // ← walks subtree
   for each child:
      write(child)               // ← recursive — child also walks
                                 //   ITS subtree to write its own
                                 //   length prefix
```

The problem is **fractal**: every length-prefixed level inside the tree
re-walks its own subtree. For a depth-D tree with N nodes, the
naïve approach is **O(D × N)** total work for a single encode. For
flat structures (D=2) that's only 2× overhead. For BeaconState-class
schemas (D≈5) it's 5×.

## Goals

1. Encode work scales as **O(N)**, regardless of nesting depth.
2. **Zero heap allocation** in the encode path.
3. **No domain-type pollution** — user types stay clean (no
   `_cached_size_` field, no sidecar interface).
4. **No wire-format compromise** — output is byte-identical to canonical
   length-prefix output.
5. **Bounded subtrees short-circuit** — when a subtree's size is known
   at compile time, the size pass elides for that subtree entirely.

## Non-goals

- Format-design changes. Wire format unchanged.
- Caching across encode calls (each call is independent).
- Concurrency (single-threaded encode is the contract).

## Approach

Two compile-time-derived facts about each reflected type T:

1. `pb_bounded_size_v<T>` — exact byte size when T is bounded
   (consteval; 0 means unbounded).
2. `pb_dynamic_size_count_v<T>` — number of length-prefix slots that
   need a runtime-computed size (consteval; counts only positions
   that are length-delim AND unbounded).

For unbounded types, the encoder allocates a stack array
`std::array<uint32_t, N>` where `N = pb_dynamic_size_count_v<T>`.

The encode is two passes through the value tree, walking in lockstep:

- **Size pass** populates `sizes[idx++]` at each unbounded length-delim
  position.
- **Write pass** consumes `sizes[idx++]` to emit each length prefix,
  then writes the body.

Both passes have identical traversal logic (so the `idx` counter stays
synchronized), and each pass is straight-line code per node. No
recursion-amplified work, no per-level retraversal, no `if (cached)`
branches.

```cpp
template <typename T>
auto encode(const T& value) {
   std::vector<char> out;

   constexpr auto N = pb_dynamic_size_count_v<T>;
   std::array<std::uint32_t, N> sizes{};   // stack — sized at compile time

   if constexpr (N > 0) {
      std::size_t idx = 0;
      pb_size_pass(value, sizes, idx);
   }

   std::size_t idx = 0;
   pb_write_pass(value, sizes, idx, out);
   return out;
}
```

For `N == 0` (every subtree is bounded), the size pass elides and
encode is straight-line: emit_tag + emit_consteval_length + emit
fixed bytes per field. This is **codegen-quality output from
reflection alone**, no per-encode work beyond the actual byte writes.

## Compile-time traits

### `pb_bounded_size_v<T>`

Exact byte cost of T when T's encoding is fully determined at compile
time. Zero indicates "unbounded — use runtime sizing".

```cpp
template <typename T> constexpr std::size_t pb_bounded_size_v = 0;

template <> constexpr std::size_t pb_bounded_size_v<bool>     = 1; // 1 byte
template <> constexpr std::size_t pb_bounded_size_v<float>    = 4;
template <> constexpr std::size_t pb_bounded_size_v<double>   = 8;
// etc. for fixed-wire integer types

template <std::size_t N>
constexpr std::size_t pb_bounded_size_v<bytes_n<N>> =
    varint_size(N) + N;

// Reflected struct: bounded iff every field is bounded.
template <typename T>
   requires Reflected<T>
constexpr std::size_t pb_bounded_size_v<T> = []() consteval {
   using R = reflect<T>;
   std::size_t total = 0;
   bool        all_bounded = true;
   [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((total += field_bounded_size<T, Is>(),
        all_bounded = all_bounded && field_is_bounded<T, Is>()),
       ...);
   }(std::make_index_sequence<R::member_count>{});
   return all_bounded ? total : 0;
}();
```

`varint`-encoded integer types are **not** considered bounded by their
exact size (the byte cost depends on the value). They contribute their
upper bound (5 bytes for `uint32_t` varint, 10 for `uint64_t` varint)
when used in size estimation, or 0 to indicate "unbounded" if exact
sizing is required.

### `pb_dynamic_size_count_v<T>`

Number of length-prefix slots T's encoded form requires:

```cpp
template <typename T> constexpr std::size_t pb_dynamic_size_count_v = 0;

template <typename E, typename A>
constexpr std::size_t pb_dynamic_size_count_v<std::vector<E, A>> =
   1                                                 // packed payload length OR
                                                     //   (per-element header, see below)
   + (pb_dynamic_size_count_v<E> > 0
      ? /* worst-case per-element count */ 0    // can't know N at compile time
      : 0);

template <>
constexpr std::size_t pb_dynamic_size_count_v<std::string> = 1;

template <>
constexpr std::size_t pb_dynamic_size_count_v<std::vector<uint8_t>> = 1;

// Reflected struct: sum over fields. Each field contributes its own
// dynamic count plus 1 if the field itself is length-delim AND
// unbounded.
template <typename T>
   requires Reflected<T>
constexpr std::size_t pb_dynamic_size_count_v<T> = []() consteval {
   using R = reflect<T>;
   std::size_t total = 0;
   [&]<std::size_t... Is>(std::index_sequence<Is...>) {
      ((total += field_dynamic_size_count<T, Is>()), ...);
   }(std::make_index_sequence<R::member_count>{});
   return total;
}();
```

**Caveat for `vector<T>` where T is itself a Reflected message:**
the size cache slots for the element messages aren't a compile-time
constant (depends on N). For these, fall back to either:

(a) Per-element backpatching (uses 0 slots in the cache);
(b) Reserve a dynamic vector at encode time, sized from `v.size()`,
    used only for that level's slot.

Approach (a) keeps the size cache fully compile-time-allocated; the
per-element length-delim is handled by writing tag + reserving 5
varint bytes + writing element body + patching length. This is the
"backpatching" path used as a fallback for repeated unbounded
elements.

## Two-pass walker

Both passes have the same shape — a templated visitor that
either accumulates into `sizes[]` or emits to `sink`:

```cpp
template <typename T>
void pb_size_pass(const T& value,
                  std::array<std::uint32_t, N>& sizes,
                  std::size_t& idx);

template <typename T>
void pb_write_pass(const T& value,
                   const std::array<std::uint32_t, N>& sizes,
                   std::size_t& idx,
                   Sink& sink);
```

For each field, dispatch on type:

| field shape | size pass | write pass |
|---|---|---|
| bounded scalar | no-op (size in tag-cost constant) | emit tag + value |
| bounded struct | no-op | emit tag + emit_varint(consteval_size) + recursive write_pass |
| `string` / `bytes` | `sizes[idx++] = value.size()` | emit tag + emit_varint(sizes[idx++]) + body |
| `vector<bounded T>` (packed) | `sizes[idx++] = N * sizeof(T)` | emit tag + emit_varint(sizes[idx++]) + memcpy body |
| `vector<varint T>` (packed) | walk, sum varint sizes → `sizes[idx++] = sum` | emit tag + emit_varint(sizes[idx++]) + per-element varint write |
| `vector<message T>` (repeated) | per-element size_pass + sum into `sizes[idx++]` for each element header (or use backpatching) | per-element: emit tag + emit_varint(sizes[idx++]) + recursive write_pass |
| `optional<T>` | size_pass on T iff present (idx still advances exactly when written) | emit T iff present |

The lockstep invariant: both passes increment `idx` at exactly the
same positions. If T contains conditional positions (e.g.,
`optional<U>` where U is unbounded), both passes must check the
condition identically.

## Bench impact (predicted)

Test bag:
```cpp
struct Bag {
   std::string                 name;
   std::vector<std::uint8_t>   payload;
   std::vector<std::int32_t>   ids;
   std::vector<Sub>            entries;
   std::optional<std::int32_t> count;
   std::int64_t                seq;
   double                      score;
};
```

`pb_dynamic_size_count_v<Bag> = 1 (name) + 1 (payload) + 1 (ids
packed) + per-element-count-of(entries)` slots.

For the entries, since `Sub` itself has a string field, each Sub has
1 dynamic slot. With 4 entries, total dynamic slots for Bag = 1 + 1 +
1 + 4 × 2 = 11 (4 element-header slots + 4 inner-string slots).
Stack array: 11 × 4 = 44 bytes. Negligible.

| approach | encode | decode |
|---|---:|---:|
| status quo (recursive size walks) | 84 ns | 157 ns |
| backpatching only (no cache) | ~70 ns | 157 ns |
| **stack-cached two-pass (this design)** | **~50 ns** | 157 ns |
| libprotobuf fast path | 64 ns | 104 ns |

Predicted: psio encode beats libprotobuf on the Bag struct after
this change.

For all-bounded types like Validator (no dynamic slots),
encode becomes straight-line emit, faster than libprotobuf because
libprotobuf still pays its cached_size lookup overhead.

## Cross-format applicability

The same optimization applies wherever a wire format requires
length-prefix-of-bytes-that-follow. Survey:

| format | applies? | notes |
|---|---|---|
| **protobuf** | yes | the original case (this issue) |
| **fracpack** (frac32, frac16) | yes | offset table at front of each container; sizes needed before writing |
| **ssz** | yes | SSZ Container fixed-then-variable layout — variable-field offsets at front of fixed area |
| **pssz** | yes | same as ssz with parametric width |
| **bin** | yes | length-prefixed binary; same pattern |
| **borsh** | yes | length-prefixed collections; nested structs may have inner length-delim fields |
| **bincode** | yes | varint-prefixed strings/vectors |
| **avro** | yes | block-encoded arrays/maps with length prefix per block |
| **pjson** (generic forms) | yes | tail-indexed slot tables computed by walking the tree; current value_size walk has the same recursion overhead |
| msgpack | partial | array/map have COUNT prefix not BYTE prefix; bin/str have byte-length prefix only at leaves. Less recursion savings; worth doing but smaller win. |
| json | no | no length prefixes; brackets/quotes delimit on the fly. Encoder is naturally one-pass. |
| capnp | no | builder pattern — pre-allocates slots in arena, fills in place. Sizes determined by schema-compiled layout, not runtime. |
| flatbuf | no | leaves-up build — every field's offset is known when written, no length prefix to backpatch. |

So the optimization helps **9 out of 12 formats** psio supports. The
common machinery — `bounded_size_v`, `dynamic_size_count_v`,
`size_pass`, `write_pass` lockstep walker — should live in a shared
header (e.g., `psio/encode_size_cache.hpp`) and be parameterized by
the per-format size/write functors.

### Phase 1: protobuf

Implement the design in `protobuf.hpp`. Get the bench numbers,
verify correctness against round-trip tests and against
libprotobuf wire byte-equality.

### Phase 2: fracpack + ssz/pssz

These are the next-highest-impact targets. fracpack and ssz both
have offset tables at the front of containers. The size cache
populates the offsets array.

For pssz specifically (where psio's pssz_format is the canonical
schema-driven format for blockchain state), this could close the
gap to capnp/flatbuf encode latency on BeaconState-class shapes.

### Phase 3: bin / borsh / bincode / avro

Apply the same pattern. Each format has slightly different
length-prefix encoding (some use varint, some fixed u32, etc.) but
the cache shape is identical.

### Phase 4: pjson

pjson's `value_size` recursion is a candidate. Each container
tail-indexed encode currently walks children to compute total size,
then emits children, then emits index. Consteval the bounded
contributions, stack-cache the dynamic ones. Wins compound with
adaptive slot widths.

### Phase 5: msgpack (low priority)

msgpack's per-leaf length prefixes are local — no fractal cost.
Apply only if profiling justifies.

## Risks and open questions

- [ ] **`vector<message T>`** size-cache requires either per-element
  backpatching OR a runtime-sized cache for that level. Decision:
  start with backpatching for the per-element case (simple, tested),
  then evaluate hybrid.
- [ ] **Lockstep invariant** — size_pass and write_pass must traverse
  in identical order. Easy to break if one path branches differently.
  Mitigation: both passes share the same template-instantiated
  visitor body, parameterized by an `op` tag (size vs write).
- [ ] **Stack-array size limits** — for very large schemas (1000+
  dynamic positions), `std::array<uint32_t, N>` on the stack might
  hit limits. Mitigate with `[[no_unique_address]]` or a
  `large_size_cache_threshold` heuristic that switches to heap
  allocation. Probably unnecessary for typical schemas.
- [x] **Recursive types** — `pb_dynamic_size_count_v<T>` can't be
  consteval for self-referential types like `struct Tree { vector<Tree>
  children; }` (the count depends on the runtime tree shape, not just
  on the type). Solution: a **count pass** before the size pass.
  See "Recursive types — runtime-sized cache" below.

## Recursive types — runtime-sized cache

For self-referential reflected types (`struct Tree { vector<Tree>
children; }`) the dynamic-size-position count depends on the runtime
shape of the value, not just on the type. The consteval count
returns a sentinel "unbounded — count at runtime" instead of an exact
number, and the encoder takes a slightly different path:

```cpp
template <typename T>
auto encode(const T& value) {
   if constexpr (pb_dynamic_size_count_v<T> != size_count_dynamic) {
      // Compile-time count — fast path (above).
      constexpr auto N = pb_dynamic_size_count_v<T>;
      std::array<std::uint32_t, N> sizes{};
      // ... two passes ...
   } else {
      // Recursive / shape-dependent — three passes.
      std::size_t K = pb_count_pass(value);   // pass 0: count slots

      // Small-buffer optimization: stack for typical sizes, heap
      // for outliers. The SBO threshold matches what psio already
      // uses elsewhere (e.g., pjson_json's slot_rec sbo[32]).
      constexpr std::size_t SBO = 64;
      std::uint32_t            sbo[SBO];
      std::vector<std::uint32_t> heap;
      std::uint32_t*           sizes;
      if (K <= SBO) {
         sizes = sbo;
      } else {
         heap.resize(K);
         sizes = heap.data();
      }

      std::size_t idx = 0;
      pb_size_pass(value, std::span(sizes, K), idx);
      idx = 0;
      pb_write_pass(value, std::span(sizes, K), idx, out);
   }
}
```

`pb_count_pass` is a single shape-only walk that visits each node and
increments a counter for each unbounded length-prefix position. No
size arithmetic, no encode work — just structural traversal. For a
tree of N nodes it's O(N) with very small per-node cost (one branch +
one increment per dynamic position).

**Three passes for recursive types** vs **two for non-recursive** vs
**one for fully bounded**. Each pass is light and straight-line; total
work is still O(total nodes), independent of nesting depth.

**SBO threshold rationale.** `K = 64` covers schemas with up to ~64
length-delim positions (typical Ethereum messages: 5-30; large RPC
payloads: 50-100). Above that, one `std::vector` allocation per
encode call — still O(1) heap allocations, scales fine for huge
schemas. `alloca` was considered but rejected for portability and
because the SBO+heap pattern already exists elsewhere in psio.

**Hybrid for mixed-depth types.** A type whose top-level fields are
bounded but contains a recursive sub-tree can use the bounded-size
fast path for the bounded prefix and the count-pass-based path for
the recursive sub-tree only. The trait `pb_dynamic_size_count_v<T>`
returns `size_count_dynamic` only when the recursion makes the count
unknowable; otherwise it returns the exact compile-time count.

## Verification plan

1. Round-trip tests on existing protobuf test corpus — must pass byte-identically.
2. libprotobuf wire-equality test — psio output must equal libprotobuf
   output for the same logical message.
3. Bench against the existing comparison (Person + Bag), verify the
   predicted gains.
4. Stress test with deeply nested types (Bag containing vector<Bag>
   containing vector<Bag>) to validate that the lockstep invariant
   holds across recursion.
5. Recursive-type test: encode a `Tree { string label; vector<Tree>
   children; }` of arbitrary shape, verify the count-pass returns
   the correct K for the SBO/heap split, verify byte-equal round-
   trip across various shapes (linear chain, balanced tree, fan-out
   leaves, single deeply-nested branch).

## Status

Design complete. Phase 1 (protobuf) implementation can start.

Stack-cached two-pass with consteval bounded short-circuit is the
target design. It's strictly dominant over backpatching-only and over
caching-on-message — same big-O complexity, smaller constant factors,
no domain-type pollution.

## File ownership

- `libraries/psio/cpp/include/psio/encode_size_cache.hpp` — shared
  trait machinery (bounded_size_v, dynamic_size_count_v) and the
  generic size_pass/write_pass walker template.
- `libraries/psio/cpp/include/psio/protobuf.hpp` — first format to
  consume the new machinery (replaces `pb_value_size` + `pb_write_value`
  recursive callers).
- Subsequent format files adopt the pattern over phases 2-5.
