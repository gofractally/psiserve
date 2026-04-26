# psio3: mutable and mutable_view design

Status: design sketch. Conversation captured 2026-04-25.

## Motivation

psio3 needs a mutable representation that can start from encoded bytes without
fully deserializing into the native C++ type. The common case should preserve
references to existing encoded or native storage, allocate only along modified
paths, and rebuild the canonical encoded representation when mutation is done.

The public API should not expose "COW" in the name. Copy-on-write is an
implementation strategy for `mutable<T, F>` and its field proxies.

## Vocabulary

| Type | Meaning |
|---|---|
| `T` / `NativeT` | User's reflected rich C++ type, e.g. `struct T { int x; std::string s; std::vector<T> v; };` |
| `buffer<T, F, Store>` | Canonical encoded bytes, typed by value type and format |
| `view<T, F>` | Read-only borrowed lens over canonical encoded bytes |
| `mutable<T, F, Storage>` | Owning mutable document/value; owns mutation state and possibly encoded backing |
| `mutable_view<T, F>` | Non-owning mutable lens into a `mutable<T, F>` or mutable borrowed buffer |
| `mut::string` | Mutable string field proxy; borrowed until first mutation, then owns bytes |
| `mut::vector<E>` | Mutable vector field proxy; borrowed until structural mutation, then owns element state |

`mutable<T, F>` is the owner. `mutable_view<T, F>` is a handle into an owner or
into caller-provided mutable bytes. Subfields return views/proxies, not new
owners.

## Starting Storage

Users can begin with either:

1. A non-owned mutable byte buffer.
   - psio3 may mutate bytes in place when the wire shape is fixed-size.
   - psio3 must not reallocate or free the buffer.
   - This maps to a borrowed `std::span<char>`.

2. An owned byte buffer.
   - "Owned" means psio3 has permission to reallocate and free it.
   - The concrete storage can be `std::vector<char>`, `std::string`,
     `std::pmr::vector<char>`, `std::pmr::string`, or any compatible binary
     buffer.

The shared binary-buffer concept should be intentionally small:

```cpp
template <typename B>
concept resizable_binary_buffer =
   requires(B& b, std::size_t n) {
      typename B::value_type;
      { b.data() };
      { b.size() } -> std::convertible_to<std::size_t>;
      b.resize(n);
   } &&
   (std::same_as<std::remove_cv_t<typename B::value_type>, char> ||
    std::same_as<std::remove_cv_t<typename B::value_type>, std::byte> ||
    std::same_as<std::remove_cv_t<typename B::value_type>, unsigned char>);
```

`reserve()` is useful but should probably be optional.

## Record Proxy Shape

For a native reflected type:

```cpp
struct T {
   int x;
   std::string s;
   std::vector<T> v;
};
```

the mutable proxy behaves conceptually like:

```cpp
template <typename F>
struct mutable<T, F> {
   detail::static_part<T, F> _static;

   mut::string              s;
   mut::vector<mutable<T,F>> v;
};
```

`_static` contains the fixed-size part of the record. Depending on the source,
it may refer to:

- a native `T*` when the mutable value was created from a native object
- a mutable wire fixed-region when created from writable encoded bytes
- a borrowed const wire fixed-region plus a lazily allocated mutable copy

The dynamic fields each own their own borrowed-or-owned state.

## Field State Model

Every field proxy is a small state machine:

```cpp
field_slot<X, F> =
     borrowed_native<X*>
   | borrowed_wire<view<X, F>>
   | owned_native<X>
   | owned_mutable<mutable<X, F>>;
```

Reads do not detach. Mutations call `ensure_owned()` or `detach()` and switch to
an owned representation only for that field or path.

Examples:

```cpp
m.x() = 7;              // mutates or copies only the fixed part
m.s().push_back('a');   // detaches only the string
m.v()[3].x() = 9;       // detaches only the vector path required
```

For wire-fixed fields, assignment can write into the mutable fixed region.
For dynamic fields, assignment or structural mutation promotes the field proxy.

## Fixed-Size Mutable Views

The first primitive needed is a mutable view over wire-fixed data:

```cpp
template <typename T, typename F>
   requires format_layout<F>::template is_fixed<T>
class fixed_mut_view {
   std::span<char> bytes_;

public:
   T get() const;
   void set(const T&);

   template <std::size_t I>
   auto field();
};
```

This must operate on wire layout, not C++ object layout. It should never cast
`char*` to `T*` for reflected records, because psio3 wire layout may omit C++
padding or use different fixed-region framing.

Possible public naming:

- Keep `fixed_mut_view` internal as `detail::fixed_mut_view`.
- Expose only `mutable_view<T, F>`, with a fixed-type specialization.

Preference: expose only `mutable_view<T, F>` initially.

## String Proxy Options

`mut::string` should support these borrowed states:

- borrowed `std::string*`
- borrowed `std::string_view`
- borrowed encoded string payload bytes

On mutation, it detaches into an owned binary/string buffer allocated from the
owning `mutable<T, F>`'s resource.

Open choice:

1. Store as `std::pmr::string`.
   - Simple, familiar, contiguous.
   - Best if a resource is available.

2. Store as `std::pmr::vector<char>`.
   - More honest for arbitrary binary bytes.
   - Can still expose string operations for UTF-8/text shapes.

3. Store as generic `BinaryBuffer`.
   - Maximum user control.
   - More template surface and more constraints.

## Vector Proxy Options

`mut::vector<E>` should support borrowed native and borrowed encoded vectors.
There are several implementation levels.

### Option A: detach whole vector on first mutable element access

On `v()[3].x() = 9`, decode/wrap all elements into owned mutable entries.

Pros:
- Easiest to implement.
- Simple iterator/reference rules.

Cons:
- Copies or wraps too much for large vectors.

### Option B: sparse per-element overlay

Keep the vector borrowed. Maintain a sparse map from element index to owned
`mutable<E, F>` or realized `E`.

Pros:
- Mutating one element in a large vector allocates one child.
- Good match for the minimum-copy goal.

Cons:
- Element access has an overlay lookup.
- Insert/erase semantics need additional structure.

### Option C: piece-table vector

Represent the vector as borrowed ranges plus inserted owned ranges:

```cpp
piece =
     borrowed_range { source, begin, count }
   | owned_range    { vector<mutable<E,F>> };
```

Pros:
- Efficient insert/erase without realizing the whole original vector.
- Very close to text editor piece-table designs.

Cons:
- More complicated indexing, iterators, and canonicalization.

Likely path: implement Option A first, then upgrade to Option B or C once the
API and tests are settled.

## Native vs Encoded Sources

The same proxy API should work whether the source is native or encoded.

From native:

```cpp
T obj;
auto m = psio::make_mutable<F>(obj);
```

- fixed fields can borrow native member addresses
- `mut::string` can borrow `&obj.s`
- `mut::vector` can borrow `&obj.v`

From encoded bytes:

```cpp
auto m = psio::make_mutable<T>(F{}, bytes);
```

- fixed fields borrow or own the wire fixed region
- dynamic fields borrow encoded payload spans
- no native `T` is constructed until explicitly requested

This avoids deserializing a full native `T` just to modify one field.

## Format Layout Traits

The mutable layer should be format-generic through a small layout trait:

```cpp
template <typename F>
struct format_layout {
   template <typename T>
   static constexpr bool is_fixed;

   template <typename T>
   static constexpr std::size_t fixed_size;

   template <typename T>
   static constexpr std::size_t record_fixed_size;

   template <typename T, std::size_t I>
   static constexpr std::size_t field_fixed_offset;

   template <typename T, std::size_t I>
   static constexpr bool field_is_inline;

   template <typename T, std::size_t I>
   static std::span<const char> field_payload(std::span<const char> record);

   template <typename T, std::size_t I>
   static void patch_slot(std::span<char> record,
                          std::size_t payload_offset,
                          std::size_t payload_size);
};
```

SSZ/pSSZ, frac, and future formats differ in offset and slot rules. The mutable
overlay should depend on `format_layout<F>`, not hard-code format details.

## Allocator Options

Allocator handling is the big design fork.

### Option 1: explicit memory_resource in mutable owner

`mutable<T, F>` stores a `std::pmr::memory_resource*`. All owned field state
uses that resource.

Pros:
- Simple and idiomatic.
- Works with `std::pmr::string` and `std::pmr::vector`.

Cons:
- Proxies or views may need to carry or recover the resource.

### Option 2: allocator-aware native types

If native `T` uses `std::pmr` or allocator-template fields, psio3 can realize
native values directly into the mutable arena.

Pros:
- Native realization is clean.
- Dynamic fields all allocate from one domain.

Cons:
- Viral requirement on user types.
- Plain `std::string` / `std::vector` still need fallback behavior.

### Option 3: allocator-templated rich types

User writes something like `T<Alloc>` instead of `T`.

Pros:
- Full control over allocator propagation.

Cons:
- Heavy API burden.
- Makes reflection and user ergonomics worse.

### Option 4: recover allocator from this pointer

Allocate all mutable proxy/native state from arena pages or slabs. Each page has
a header pointing at the owning arena. Code can recover the arena from `this`:

```cpp
mutation_arena& arena_from_this(const void* p) {
   auto* page = align_down_to_page_header(p);
   return *page->arena;
}
```

Pros:
- Field proxies do not need allocator members.
- Embedded subobjects can find the arena if they live in psio3 arena pages.

Cons:
- Only works for objects allocated by psio3.
- Stack objects and ordinary `new` allocations cannot discover a psio3 arena.
- Requires careful page/slab discipline.

### Option 5: side table / interval map

Maintain a registry from allocation address ranges to arenas.

Pros:
- More flexible than page masking.

Cons:
- More overhead and complexity.
- Needs synchronization if shared across threads.

Likely path: start with Option 1. Keep the internal layout compatible with
Option 4 so proxy nodes can later recover their arena without storing a pointer.

## Canonicalization

`canonicalize(mutable<T, F>)` walks the proxy tree and emits canonical bytes.

For each record:

1. Compute or reserve the fixed region.
2. Emit fixed fields from:
   - modified fixed-region bytes
   - realized native values
   - borrowed original wire bytes
3. For each dynamic field:
   - if unchanged, append/copy its original payload bytes when valid for the
     target canonical form
   - if changed, recursively canonicalize the child or encode the realized value
4. Patch offsets/slots according to `format_layout<F>`.

Important invariant: canonicalization is the only place where parent offset
slots need to become final. Mutable field proxies should not expose raw offset
slots as user data.

## Public Surface Sketch

```cpp
std::vector<char> bytes = psio::encode(psio::frac32{}, acct);

psio::mutable<Account, psio::frac32> m{
   psio::owned_buffer{std::move(bytes)}};

m.name().assign("alice");
m.balance().amount().set(100);

auto canonical = psio::canonicalize(m);
```

Borrowed mutable buffer:

```cpp
std::span<char> bytes = ...;
auto m = psio::make_mutable<Account>(psio::frac32{}, bytes);

m.fixed_field().set(7);   // in-place if wire-fixed
m.name().assign("bob");   // allocates side storage; original span cannot grow

auto canonical = psio::canonicalize(m);
```

Subfields return views/proxies:

```cpp
psio::mutable_view<std::string, psio::frac32> name = m.name();
psio::mutable_view<Balance, psio::frac32>     bal  = m.balance();
```

## Related Patterns

This design combines known patterns:

- Cap'n Proto style owner plus `Reader`/`Builder` handles.
- Rust `Cow` style borrowed-or-owned detach on mutation.
- Qt implicit sharing style public value behavior with hidden detach.
- Piece-table style unchanged original ranges plus appended owned edits.
- Arrow/Bytes style distinction between immutable bytes and mutable owned bytes.

psio3's specific twist is that the detach state is per reflected field, and
canonicalization is format-aware.

## Initial Implementation Order

1. Implement wire-fixed `mutable_view<T, F>` for primitives, fixed arrays, and
   all-fixed reflected records.
2. Add `format_layout<F>` for the first target format, probably `pssz` or
   `frac32`.
3. Implement `mutable<T, F>` owner with explicit `std::pmr::memory_resource*`.
4. Add `mut::string` with borrowed-or-owned detach.
5. Add simple `mut::vector<E>` Option A.
6. Add canonicalization back to `buffer<T, F>`.
7. Upgrade vectors to sparse overlay or piece-table representation if needed.

## Open Questions

1. Should `mutable<T, F>` be format-specific, or should a format-erased mutable
   tree exist for dynamic schema tooling?
2. Should `std::string` be treated as text or arbitrary binary at the proxy
   storage level? Likely binary storage plus text operations when annotated.
3. How much iterator/reference stability must `mut::vector<E>` provide?
4. Should direct in-place mutation of an owned canonical buffer preserve
   canonical bytes immediately for fixed-only edits, or should all edits go
   through canonicalization for consistency?
5. When source bytes are non-owned mutable and a dynamic field grows, should
   canonicalization return a new owned buffer or require the caller to supply a
   destination?
