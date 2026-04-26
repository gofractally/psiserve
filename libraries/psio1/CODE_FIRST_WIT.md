# Code-First WIT: Deriving Component Model Interfaces from Source Types

## The Problem with WIT-First

The standard Component Model workflow looks like this:

```
write .wit file → wit-bindgen → generated bindings → implement trait → build
```

You write an interface definition language file by hand, run a code generator,
then implement against the generated types. This is familiar if you've used
protobuf, Thrift, or OpenAPI. It's also the source of familiar pain:

- **Generated code you can't touch.** Want to add a trait impl, derive a macro,
  or customize serialization on a generated struct? You either fight the
  generator or maintain patches that get overwritten on the next build.

- **Two representations of every type.** Your domain model is `Item`. The
  generated binding gives you `wit_inventory::Item`. Now you're writing
  `From<wit_inventory::Item> for Item` everywhere. Every type exists twice,
  and you maintain the conversion layer between them.

- **Build dependency chain.** `wit-bindgen` depends on a specific `wit-parser`,
  which must match your `wasm-tools` version, which must match your
  `cargo-component` version. Version mismatches between these tools are a real
  source of build breakage across the ecosystem.

- **Impedance mismatch.** The generator makes choices about error handling,
  ownership, naming, and collection types that may not match your codebase.
  You write adapter code to bridge "how the generator thinks" and "how your
  code works."

- **Regeneration churn.** Change the `.wit` file, re-run the generator, fix
  compile errors in your implementation, hope you didn't break the custom code
  you wedged around the generated types.


## The Code-First Alternative

psio takes a different approach:

```
write your types → reflection derives everything → done
```

Your struct definitions and method signatures ARE the interface definition.
Compile-time reflection extracts the type structure and mechanically derives
the Component Model binary — the same standard encoding that `wit-bindgen` and
`wasm-tools` produce.

### C++ Example

```cpp
struct Item {
   uint64_t    id;
   std::string name;
   uint32_t    price_cents;
   bool        in_stock;
   std::optional<uint32_t> weight_grams;
};
PSIO1_REFLECT(Item, id, name, price_cents, in_stock, weight_grams)

struct InventoryApi {
   std::optional<Item> get_item(uint32_t item_id);
   uint64_t            add_item(Item item);
   void                ping();
};
PSIO1_REFLECT(InventoryApi,
   method(get_item, item_id),
   method(add_item, item),
   method(ping)
)

// Generate the standard Component Model binary
auto binary = psio1::generate_wit_binary<InventoryApi>("my:inventory@1.0.0");
```

### Rust Example

```rust
wit_macro::wit_world! {
    package = "my:inventory@1.0.0";

    pub struct Item {
        pub id: u64,
        pub name: String,
        pub price_cents: u32,
        pub in_stock: bool,
        pub weight_grams: Option<u32>,
    }

    pub trait InventoryApi {
        fn get_item(&self, item_id: u32) -> Option<Item>;
        fn add_item(&self, item: Item) -> u64;
        fn ping(&self);
    }
}
```

Both produce **byte-identical** Component Model binary. The same bytes. Verified
by binary comparison of type sections and export sections, and by round-tripping
through `wasm-tools component wit`.


## How It Works

### The Type Mapping

There is a deterministic, mechanical mapping from language types to WIT types:

| C++ | Rust | WIT |
|-----|------|-----|
| `bool` | `bool` | `bool` |
| `uint8_t` / `int8_t` | `u8` / `i8` | `u8` / `s8` |
| `uint16_t` / `int16_t` | `u16` / `i16` | `u16` / `s16` |
| `uint32_t` / `int32_t` | `u32` / `i32` | `u32` / `s32` |
| `uint64_t` / `int64_t` | `u64` / `i64` | `u64` / `s64` |
| `float` / `double` | `f32` / `f64` | `f32` / `f64` |
| `std::string` | `String` | `string` |
| `std::vector<T>` | `Vec<T>` | `list<T>` |
| `std::optional<T>` | `Option<T>` | `option<T>` |
| Reflected struct | Struct | `record { ... }` |

Naming conventions are converted automatically: `price_cents` (snake_case) and
`PriceCents` (CamelCase) both become `price-cents` (kebab-case) in WIT.

### The Reflection Mechanism

Each language uses its native compile-time reflection facility:

| Language | Mechanism | Integration |
|----------|-----------|-------------|
| C++ | `PSIO1_REFLECT` macro + template metaprogramming | Inline at compile time |
| Rust | Proc macros + `syn` token parsing | Inline at compile time |
| C# | Roslyn source generators | Inline at compile time |
| Java | Annotation processors | Inline at compile time |
| Go | `go generate` + AST parsing | Build step |
| MoonBit | Derive macros | Inline at compile time |
| C | Use a C++ header for reflection, C for everything else | Mixed |

The principle is the same everywhere: if you can access type structure at
compile time, you can mechanically derive the Component Model binary.

### The Binary Encoding

The output is a standard Component Model binary — the same format defined by
the WebAssembly Component Model specification. It encodes:

- Type definitions (records, lists, options, etc.) as instance type items
- Named type exports with equality bounds
- Function type definitions with named parameters
- Interface and world structure

This binary is embedded as a `component-type:name` custom section in the WASM
output. Any standard tool can read it:

```bash
$ wasm-tools component wit my-module.wasm
package my:inventory@1.0.0;

interface inventory-api {
  record item {
    id: u64,
    name: string,
    price-cents: u32,
    in-stock: bool,
    weight-grams: option<u32>,
  }

  get-item: func(item-id: u32) -> option<item>;
  add-item: func(item: item) -> u64;
  ping: func();
}

world inventory-api-world {
  export inventory-api;
}
```


## Full Ecosystem Compatibility

A common concern: "If you don't start from a `.wit` file, you break ecosystem
interop."

This is incorrect. The component-type custom section we emit is the standard
binary encoding. Any consumer can:

```
our WASM module → wasm-tools component wit → .wit file → wit-bindgen → their bindings
```

The consumer doesn't know or care whether the producer started from a `.wit`
file or from C++ types. They see the same component-type section, the same WIT,
the same interface. The contract IS independent of the implementation — it's in
the binary, in a standard format. It just happens to be derived from source
types rather than hand-authored.


## What You Don't Deal With

Problems that exist in WIT-first workflows that don't exist here:

**No generated code to manage.** Your types are your types. Add trait impls,
derive macros, custom serialization — whatever your codebase needs. There's no
generated layer to work around.

**No two-representation problem.** Your domain type IS the interface type.
One struct. Zero conversion boilerplate. No `From<Generated> for Mine`
everywhere.

**No build tool dependency.** The encoder is a header file (C++) or a proc
macro crate (Rust). No external tools to version-match, no CLI to install,
no build step to orchestrate.

**No regeneration step.** Change your struct, the interface updates
automatically. There is no step two.

**No impedance mismatch.** There's no generator making choices that conflict
with your codebase conventions. Your code is the source. The binary is a
projection of it.


## When WIT-First Still Makes Sense

Code-first is not universally better. WIT-first is the right choice when:

- **Multiple teams need to agree on an interface before implementation begins.**
  The `.wit` file serves as a design document and coordination artifact.

- **You need WIT features with no direct language equivalent** — `resource` types
  with ownership semantics, `stream`/`future` for async, explicit versioning
  syntax.

- **You're publishing a WASI proposal.** WASI interfaces are defined as `.wit`
  files by convention, and consumers expect to find them in that form.

Code-first is the right choice when:

- **One team controls both sides** and the implementation IS the specification.
- **You want zero ceremony** — define types, get interop, no intermediate steps.
- **You're building a vertically integrated system** where the interface evolves
  with the code.
- **You want your domain types to be your interface types** without a translation
  layer.


## The Principle

WIT-first makes you work for the toolchain. Code-first makes the toolchain
work for you.

The Component Model binary format is a serialization of type information that
the compiler already has. The "hard part" isn't encoding — it's getting access
to type structure at compile time. Once you have that (via PSIO1_REFLECT,
proc macros, source generators, or annotation processors), everything else is
mechanical. The types define the interface. The binary is derived. The ecosystem
sees standard Component Model. Everyone interoperates.

Most developers assume you must start from an IDL file because that's how
protobuf, Thrift, and gRPC work. The Component Model doesn't require it. The
binary encoding is well-defined and public. Any tool that can produce it is a
first-class citizen. Starting from source types and deriving the binary is not
a workaround — it's a legitimate, fully compatible approach that eliminates an
entire category of integration pain.
