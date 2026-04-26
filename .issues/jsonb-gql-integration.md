# GraphQL over JSONB — design

**Status:** design draft, 2026-04-26. Companion to
`self-describing-format.md`. Depends on the JSONB format reaching
v1; the GraphQL integration sits on top of that wire layout and the
`jsonb_view<T>` zero-copy reader.

## Motivation

The JSONB format provides O(1) field lookup (schema-aware via offset
arithmetic, schemaless via SIMD-prefilter hash) and zero-copy reads.
Tree-shaped queries — GraphQL is the canonical example — collapse
to a series of compile-time-known offset reads against a JSONB
buffer.

Traditional GraphQL servers pay per-request:

1. Parse query string
2. Validate against schema
3. Build resolver tree
4. Walk data, executing resolvers
5. Emit response JSON

Costs of #1–#3 dominate small queries. With a compile-time-known
query, all of #1–#3 fold into the binary at build time. What's left
at request time is data extraction (#4) and response emission (#5),
both of which collapse to specialized walkers running directly
against the JSONB buffer.

This issue specifies the API and code-generation strategy for
compile-time GraphQL over JSONB. It does not change the JSONB wire
format.

## Goals

1. **Compile-time query parsing and planning.** A GraphQL query
   given as a string literal at compile time becomes a constant
   value carrying the parsed plan, the validated field paths, and
   the result type.
2. **Compile-time schema validation.** Querying a field that
   doesn't exist in the C++ struct is a compile error. Type
   mismatches (string field used as a number) are compile errors.
3. **Specialized walker per query.** Each compiled query produces
   straight-line C++ code — no resolver dispatch, no expression
   tree interpretation. Field accesses are direct
   `jsonb_view<T>::get<&T::field>()` calls.
4. **Three output forms.** A query can produce: a typed C++ struct
   (in-process consumption), JSON text streamed to a writer
   (network response), or a JSONB sub-buffer (caching layer).
5. **Runtime fallback.** Queries from a UI or other dynamic source
   that arrive as runtime strings still work — just without the
   compile-time specialization.
6. **Subscription / live-query friendliness.** A compiled query is
   a value; running it repeatedly against changing data is what
   the design optimizes for.

## Non-goals

- A complete GraphQL execution engine. We're providing the
  *resolver-equivalent* layer over JSONB. Auth, rate limiting,
  query depth limiting, persisted queries, federation, etc. are
  layers above and outside this issue.
- N+1 query elimination through batching. Each query operates on
  a single JSONB document or `jsonb_view`. Cross-document joins
  belong to the storage / query-planner layer.
- GraphQL mutations. The format permits in-place modifications for
  same-or-smaller overwrites, but the GraphQL mutation surface is
  out of scope here.
- Custom directives beyond the standard GraphQL set. Directives
  like `@skip`, `@include`, and `@deprecated` are in scope; novel
  directives need their own design.

## API surface

### Compile-time entry point

```cpp
template <typename Schema, /* string-literal */ const char* QueryStr>
struct gql_query {
    using result_t = /* derived from query selection */;

    // Three execution forms:
    result_t          execute(jsonb_view<Schema>          v) const;
    void              execute_to_json(jsonb_view<Schema>  v,
                                       json_writer&       w) const;
    psjsonb_buffer    execute_to_jsonb(jsonb_view<Schema> v) const;
};

// Sugar:
template <typename Schema, /* string literal */ Q>
inline constexpr gql_query<Schema, Q> compile = {};
```

Usage:

```cpp
constexpr auto user_summary = psio3::gql::compile<User, R"(
    {
        name
        email
        addresses { city country }
    }
)">;

auto result = user_summary.execute(jsonb_view<User>(buffer));
```

The query string is a non-type template parameter via C++20's
string-literal NTTPs. The `gql_query` instantiation does the parse
+ plan in `consteval` context.

### Runtime entry point

```cpp
template <typename Schema>
auto parse_runtime(std::string_view query_str)
    -> std::expected<runtime_gql_query<Schema>, gql_parse_error>;

class runtime_gql_query<Schema> {
public:
    // Result type is a `dynamic_value` (psio3 already has this) —
    // can't be a fixed C++ struct because the query was parsed
    // at runtime.
    dynamic_value execute(jsonb_view<Schema>  v) const;
    void          execute_to_json(jsonb_view<Schema> v,
                                   json_writer&      w) const;
    psjsonb_buffer execute_to_jsonb(jsonb_view<Schema> v) const;
};
```

Runtime queries fall back to:

- Schema validation at parse time (still strict; unknown fields error)
- Walker built as a small bytecode tree that the executor interprets
- Schemaless lookup paths in the runtime walker (hash + verify)

## Result-type derivation

The compile-time query's `result_t` is derived from the selection
shape:

- A leaf field of scalar type T → `T` (or `std::string_view` for
  borrowed strings, depending on the lifetime mode).
- A leaf field of byte type → `std::span<const std::byte>`.
- A leaf field of object type without sub-selection → compile error
  (GraphQL requires selecting fields of object types).
- A field with sub-selection → a struct with the selected sub-fields.
- A list field with element-type sub-selection → `std::vector<...>`.
- A field with a fragment spread (`... on T { ... }`) → `std::variant`.

Concrete example for the user_summary query above:

```cpp
struct UserSummaryAddress {
    std::string_view city;
    std::string_view country;
};

struct UserSummary {
    std::string_view                  name;
    std::string_view                  email;
    std::vector<UserSummaryAddress>   addresses;
};

// Both decltype(user_summary)::result_t and
// decltype(user_summary.execute(view)) are UserSummary.
```

The structs are anonymous-but-named (synthesized by template
metaprogramming with stable, predictable names) so they can be
referred to by name when needed but aren't visible across
translation units.

### Lifetime modes

Two flavors of `result_t` for compile-time queries:

- **Borrow mode (default):** strings are `std::string_view`,
  bytes are `std::span<const std::byte>`. Result borrows from the
  source buffer; valid for the buffer's lifetime. Zero-allocation.
- **Owned mode:** strings are `std::string`, bytes are
  `std::vector<std::byte>`. Result owns its data; valid
  independently of the source. Allocates as it walks.

Choose at compile time:

```cpp
constexpr auto q = psio3::gql::compile<User, "...">;          // borrow
constexpr auto q = psio3::gql::compile_owned<User, "...">;    // owned
```

For typical request/response workloads where the response is
serialized immediately, borrow mode is correct. For queries whose
result is stored or passed across async boundaries, owned mode is
correct.

## What the compiler produces

For the user_summary query, after compile-time planning,
`user_summary.execute(view)` reduces to roughly:

```cpp
UserSummary execute(jsonb_view<User> view) {
    UserSummary out;
    out.name  = view.name();                   // 3 buffer reads
    out.email = view.email();                  // 3 reads
    auto arr  = view.addresses();              // sub-view (jsonb_array_view<Address>)
    out.addresses.reserve(arr.size());
    for (auto addr : arr) {                    // addr is jsonb_view<Address>
        out.addresses.push_back({
            .city    = addr.city(),
            .country = addr.country(),
        });
    }
    return out;
}
```

No resolver dispatch. No expression interpreter. Each accessor
method (`view.name()`, `addr.city()`, etc.) is generated by
`PSIO3_REFLECT` and inlines to the schema-aware fast path: mode
byte read + offset read + value decode. The walker can be inspected
as object code; nothing mysterious.

For `execute_to_json`, the walker streams directly to a writer:

```cpp
void execute_to_json(jsonb_view<User> view, json_writer& w) {
    w.start_object();
    w.key("name");      w.string(view.name());
    w.key("email");     w.string(view.email());
    w.key("addresses"); w.start_array();
    for (auto addr : view.addresses()) {
        w.start_object();
        w.key("city");    w.string(addr.city());
        w.key("country"); w.string(addr.country());
        w.end_object();
    }
    w.end_array();
    w.end_object();
}
```

Constant key strings come from `.rodata` (memcpy at emit time).
Value strings are slices into the input buffer. Writer can be
buffer-backed, socket-backed, file-backed — the walker doesn't
care.

For `execute_to_jsonb`, the walker emits a JSONB sub-buffer with
just the selected fields. Same shape as the encoder: compute total
size from the static fixed contribution + runtime variable
contribution, allocate once, single-pass write.

## Filter arguments

GraphQL field arguments map to compile-time predicates:

```cpp
constexpr auto user_us = psio3::gql::compile<User, R"(
    {
        name
        addresses(country: "US") {
            city
        }
    }
)">;
```

The `country: "US"` argument is parsed at compile time. The walker:

```cpp
for (auto addr : view.addresses()) {
    if (addr.country() == "US") {
        // include this address in the result
    }
}
```

The literal `"US"` lives in `.rodata`. The comparison is a string
compare against a constant — fast, no allocation.

For numeric / range arguments:

```cpp
{
    transactions(amount_gt: 1000) {
        id
        amount
    }
}
```

→ `if (txn.amount() > 1000)` — direct compile-time constant
comparison.

Arguments that aren't compile-time literals (parameterized queries
with runtime variables) take the runtime path; the planner emits
walker code that reads from a parameter slot supplied at execute
time.

```cpp
auto q = psio3::gql::compile<User, R"(
    query($country: String!) {
        addresses(country: $country) { city }
    }
)">;

auto result = q.execute(view, gql::vars{.country = user_input});
```

## Variants and fragment spreads

GraphQL fragment spreads (`... on T { ... }`) map to C++
`std::variant` discriminated unions when the schema uses one:

```cpp
struct AdminUser {
    std::string  name;
    std::vector<std::string> permissions;
};
struct RegularUser {
    std::string  name;
    psio3::date  signup_date;
};
using User = std::variant<AdminUser, RegularUser>;

constexpr auto q = psio3::gql::compile<User, R"(
    {
        ... on AdminUser   { name permissions }
        ... on RegularUser { name signup_date }
    }
)">;
```

The walker reads the variant tag (one buffer read), dispatches
through a `std::visit`-equivalent that's compile-time-resolved.
Since `view.variant_tag()` and `view.as<T>()` would be library
helpers on a struct view (and could collide with reflected field
names), they're free functions in `psio3::`:

```cpp
auto execute(jsonb_view<User> view) -> std::variant<AdminResult, RegularResult> {
    auto tag = psio3::variant_tag(view);  // 1 read
    switch (tag) {
        case 0: {
            auto admin = psio3::as<AdminUser>(view);
            return AdminResult{
                .name        = admin.name(),
                .permissions = /* walk admin.permissions() */,
            };
        }
        case 1: {
            auto regular = psio3::as<RegularUser>(view);
            return RegularResult{
                .name        = regular.name(),
                .signup_date = regular.signup_date(),
            };
        }
    }
}
```

## Subscriptions / live queries

A compiled query is a stateless value. Re-running it against
updated data is the same `execute(new_view)` call. A storage layer
that emits change notifications (psitri does, in some shapes) can
re-execute on each change:

```cpp
// At setup:
constexpr auto live_q = psio3::gql::compile<Account, R"(
    { balance last_login }
)">;

// On every row change:
auto bytes = txn.get(account_id);             // span<const byte>, mmap-backed
jsonb_view<Account> view{bytes};
auto snapshot = live_q.execute(view);         // sub-microsecond
emit_to_subscriber(snapshot);
```

The walker is fast enough that re-running per-change has cost
bounded by the *selection* size, not the document size. This is
where compile-time GraphQL really pays off — high-frequency
subscription workloads where the same query runs thousands of times
against changing data.

## Implementation strategy

### Compile-time GraphQL parser

Implement a `consteval` GraphQL grammar parser. Input is a string
literal NTTP; output is a `gql_ast` constexpr value.

- Tokenizer: walk the string at compile time, emit
  `constexpr std::array<token>`.
- Parser: recursive descent, produces a `gql_ast` tree.
- Validator: walks the AST cross-checked against the C++ schema
  reflection (recursively for sub-selections), emitting `static_assert`
  failures for unknown fields, type mismatches, etc.
- Plan: AST + validated paths → a structure of
  `(field_index_in_T, sub_plan)` tuples. Field index resolves to
  the corresponding `PSIO3_REFLECT`-generated accessor method on
  `jsonb_view<T>` at walker emission time.

This is a substantial piece of code (~few thousand lines of
`consteval`). Existing precedents: ctre (compile-time regex), the
GFL/lefticus ranges-style work, various constexpr JSON parsers.

### Walker codegen

The plan structure is iterated at template instantiation time to
unroll the walker. Each `(field_index, sub_plan)` pair becomes a
call to the corresponding accessor method on the current view —
e.g. for a plan referencing `User::name`, the walker emits
`view.name()`. Each call is followed by either:
- a leaf decode (the field's value as returned), or
- a recursive walker if `sub_plan` is non-leaf (the accessor
  returns a sub-view of the appropriate type).

Heavy use of fold expressions and `if constexpr`. The walker for a
typical query is well under 1000 instructions of native code after
optimization.

### Runtime parser

A separate (non-`consteval`) parser produces the same AST shape.
The runtime walker uses the schemaless `jsonb_object_view` lookup
path and a `dynamic_value`-backed result. Slower per field
(hash + verify vs three reads) but full feature parity.

### Schema introspection

GraphQL traditionally provides an `__schema` introspection query.
Our schema lives in `PSIO3_REFLECT` and `as<Tag>` annotations. A
reflection-driven `__schema` resolver can produce the standard
GraphQL introspection result from psio3 reflection — useful for
GraphiQL and similar tools. Belongs in a follow-up issue.

## Performance expectations

For a typical small query (5 leaf fields, 1 nested array of 3
elements, each with 2 leaf fields = 11 leaf accesses):

- **Compile-time query (borrow mode):** ~100 buffer reads (3 reads
  per field × 11 fields + iteration overhead). Sub-microsecond on
  warm cache.
- **Compile-time query (owned mode):** above + N small
  allocations for the strings (typically pooled in the
  per-request arena). Few microseconds.
- **JSON text emission:** above + N memcpy operations into the
  writer. Few microseconds for small responses; bandwidth-bound
  for large.
- **Runtime query:** ~3× compile-time-query (hash lookup +
  dynamic dispatch). Single-digit microseconds.

Compare to traditional GraphQL servers (Apollo, gqlgen, etc.):
typical small queries take 100s of microseconds even with
caching, dominated by parse + plan + reflection lookup overhead.
The compile-time path should beat them by 100×–1000× on the
specialized query case.

## Verification plan (when implementation begins)

- **Parser correctness.** Run the GraphQL spec's test queries
  through the `consteval` parser; assert they parse to the
  expected AST.
- **Schema validation.** Library of valid + invalid query/schema
  pairs; valid ones compile, invalid ones produce specific
  `static_assert` errors.
- **Walker correctness.** For each (query, schema, JSONB buffer)
  triple, compare the walker's output against a hand-written
  resolver's output. Assert byte-identical.
- **Round-trip with reflection-driven encode.** Pssz value →
  JSONB → query result → reconstructed value (where the query
  selects all fields) should equal the original.
- **Bench against alternatives.** Compare execute time against:
  - simdjson DOM walk + `select(...)` shape
  - rapidjson DOM walk + `select(...)`
  - Apollo-server-style traditional resolver
- **Live-query stress.** Run a compiled query 1M times against
  randomly-mutated documents; assert no allocation in borrow
  mode, no leaks in owned mode (under ASan + leak-sanitizer).

## Open questions

- **Argument typing for runtime variables.** Compile-time `vars{}`
  parameter pack works for compile-time-known argument types,
  but a `query($id: ID!)` call has type info in the GraphQL
  schema, not the C++ schema. Need a mapping convention from
  GraphQL types to C++ types (`ID` → `std::string`? `psio3::uuid`
  via `as<hex_tag>`?).
- **Aliases.** GraphQL allows `aliasName: realField` selections.
  The result struct needs to carry the alias name; member-name
  collision rules need to be defined.
- **Custom scalars.** GraphQL custom scalars need to map to psio3
  adapter types via the `as<Tag>` mechanism. Spec the convention
  and ensure round-trip works.
- **Directives.** `@skip(if: $cond)` and `@include(if: $cond)` are
  conditional. Compile-time variant requires the condition be a
  constexpr / template parameter. Runtime variant evaluates per
  request.
- **Streaming list responses.** Very large arrays might want
  cursor-based / streaming output rather than buffering the
  whole list. Out of scope for v1; revisit if needed.
- **Subscriptions infrastructure.** The walker side is trivial
  (re-execute on change). The pub/sub plumbing belongs to a
  separate psitri-side issue.

## Critical files (when implementation begins)

- `libraries/psio3/cpp/include/psio3/gql.hpp` — the public API
  (`compile`, `parse_runtime`, query types).
- `libraries/psio3/cpp/include/psio3/gql/parser.hpp` — `consteval`
  GraphQL parser.
- `libraries/psio3/cpp/include/psio3/gql/plan.hpp` — AST → walker
  plan conversion.
- `libraries/psio3/cpp/include/psio3/gql/walker.hpp` — walker
  codegen via templates / fold expressions.
- `libraries/psio3/cpp/include/psio3/gql/runtime.hpp` — runtime
  parser + interpreter.
- `libraries/psio3/cpp/include/psio3/jsonb.hpp` — `jsonb_view<T>`
  and `jsonb_object_view` are the primitives this builds on
  (defined by `self-describing-format.md`'s implementation).

## Status

Design draft. The JSONB format itself must reach v1 before
implementation can begin, since this layer assumes the
`jsonb_view<T>::get<&T::field>()` API is available. Both designs
proceed in parallel until the format implementation lands.
