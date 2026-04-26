// Phase B step 2: PSIO1_PACKAGE macro.
//
// Verifies that PSIO1_PACKAGE populates the compile-time package registry
// (`psio1::detail::package_info<FixedString>`) and exposes it as the
// TU-local alias `psio_current_package` that downstream macros
// (PSIO1_INTERFACE / PSIO1_WORLD / PSIO1_USE) will consume.

#include <catch2/catch.hpp>

#include <psio1/emit_wit.hpp>
#include <psio1/reflect.hpp>
#include <psio1/schema.hpp>
#include <psio1/structural.hpp>
#include <psio1/wit_resource.hpp>

#include <string_view>
#include <vector>

// Declare the package at global (file) scope — the macro expansion
// re-opens psio1::detail, which is only reachable from the global scope.
PSIO1_PACKAGE(psibase, "0.3.0");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(psibase)

TEST_CASE("PSIO1_PACKAGE: package_info specialization carries name and version",
          "[psio][structural][package]")
{
   using info = psio1::detail::package_info<psio1::FixedString{"psibase"}>;
   REQUIRE(std::string_view{info::name} == "psibase");
   REQUIRE(std::string_view{info::version} == "0.3.0");
}

TEST_CASE("PSIO1_PACKAGE: PSIO1_CURRENT_PACKAGE_ macro resolves to the specialization",
          "[psio][structural][package]")
{
   STATIC_REQUIRE(
       std::is_same_v<PSIO1_CURRENT_PACKAGE_,
                      psio1::detail::package_info<psio1::FixedString{"psibase"}>>);
   REQUIRE(std::string_view{PSIO1_CURRENT_PACKAGE_::name} == "psibase");
   REQUIRE(std::string_view{PSIO1_CURRENT_PACKAGE_::version} == "0.3.0");
}

TEST_CASE("PSIO1_PACKAGE: marker variable has matching FixedString tag",
          "[psio][structural][package]")
{
   STATIC_REQUIRE(std::string_view{psio1::detail::_psio_pkg_psibase.name} == "psibase");
}

// ── PSIO1_INTERFACE(...) ──────────────────────────────────────────────────

namespace structural_test
{
   struct Block
   {
      int height;
   };
   PSIO1_REFLECT(Block, height)

   struct Transaction
   {
      int nonce;
   };
   PSIO1_REFLECT(Transaction, nonce)
}  // namespace structural_test

// The interface tag is a global-scope class whose static members are
// the interface's operations. PSIO1_INTERFACE addresses them directly
// (`&kernel::submit_tx`) — no separate anchor namespace is involved.
struct kernel
{
   static int submit_tx(int x) { return x + 1; }
   static int query_chain()    { return 42; }
};

PSIO1_INTERFACE(kernel,
               types(structural_test::Block, structural_test::Transaction),
               funcs(func(submit_tx, x), func(query_chain)))

TEST_CASE("PSIO1_INTERFACE: interface_info carries name, package, types, funcs",
          "[psio][structural][interface]")
{
   using info = psio1::detail::interface_info<kernel>;

   REQUIRE(std::string_view{info::name} == "kernel");
   STATIC_REQUIRE(std::string_view{info::package::name} == "psibase");
   STATIC_REQUIRE(
       std::is_same_v<info::types,
                      std::tuple<structural_test::Block, structural_test::Transaction>>);
   // Reflection is type-based (decltype), not value-based: the anchor
   // struct doesn't have to define its static members for
   // interface_info to materialize. Signatures are checked via the
   // pointer TYPE; the pointer value is never taken.
   STATIC_REQUIRE(std::tuple_size_v<typename info::func_types> == 2);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<0, info::func_types>,
                                 int (*)(int)>);
   STATIC_REQUIRE(std::is_same_v<std::tuple_element_t<1, info::func_types>,
                                 int (*)()>);
}

TEST_CASE("PSIO1_INTERFACE: interface_of<T> reverse lookup points back at the tag",
          "[psio][structural][interface]")
{
   STATIC_REQUIRE(
       std::is_same_v<psio1::interface_of<structural_test::Block>::type, kernel>);
   STATIC_REQUIRE(
       std::is_same_v<psio1::interface_of<structural_test::Transaction>::type, kernel>);
}

TEST_CASE("PSIO1_INTERFACE: package_of<T> resolves transitively",
          "[psio][structural][interface]")
{
   STATIC_REQUIRE(std::string_view{psio1::package_of<structural_test::Block>::name}
                  == "psibase");
}

// ── PSIO1_USE + PSIO1_WORLD ────────────────────────────────────────────────

PSIO1_USE(wasi, streams, "0.2.0")
PSIO1_USE(wasi, clocks, "0.2.0")

PSIO1_WORLD(node,
           imports(wasi_streams_use_tag, wasi_clocks_use_tag),
           exports(::kernel))

TEST_CASE("PSIO1_USE: use_info carries package, interface, and version",
          "[psio][structural][use]")
{
   using info = psio1::detail::use_info<psio1::detail::wasi_streams_use_tag>;
   STATIC_REQUIRE(std::string_view{info::package} == "wasi");
   STATIC_REQUIRE(std::string_view{info::interface_name} == "streams");
   STATIC_REQUIRE(std::string_view{info::version} == "0.2.0");
}

TEST_CASE("PSIO1_USE: distinct interfaces populate distinct tag types",
          "[psio][structural][use]")
{
   STATIC_REQUIRE_FALSE(std::is_same_v<psio1::detail::wasi_streams_use_tag,
                                       psio1::detail::wasi_clocks_use_tag>);
   using clocks = psio1::detail::use_info<psio1::detail::wasi_clocks_use_tag>;
   STATIC_REQUIRE(std::string_view{clocks::interface_name} == "clocks");
}

TEST_CASE("PSIO1_WORLD: world_info composes package, imports, exports",
          "[psio][structural][world]")
{
   using world = psio1::detail::world_info<psio1::detail::node_world_tag>;
   STATIC_REQUIRE(std::string_view{world::name} == "node");
   STATIC_REQUIRE(std::string_view{world::package::name} == "psibase");
   STATIC_REQUIRE(
       std::is_same_v<world::imports,
                      std::tuple<psio1::detail::wasi_streams_use_tag,
                                 psio1::detail::wasi_clocks_use_tag>>);
   STATIC_REQUIRE(
       std::is_same_v<world::exports,
                      std::tuple<kernel>>);
}

// ── SchemaBuilder::insert_world ──────────────────────────────────────────

TEST_CASE("SchemaBuilder::insert_world: populates envelope from world tag",
          "[psio][structural][insert_world]")
{
   namespace S  = psio1::schema_types;
   auto schema  = S::SchemaBuilder{}
                     .insert_world<psio1::detail::node_world_tag>()
                     .build();

   REQUIRE(schema.package.name == "psibase");
   REQUIRE(schema.package.version == "0.3.0");

   REQUIRE(schema.uses.size() == 2);
   REQUIRE(schema.uses[0].package == "wasi");
   REQUIRE(schema.uses[0].interface_name == "streams");
   REQUIRE(schema.uses[0].version == "0.2.0");
   REQUIRE(schema.uses[1].interface_name == "clocks");

   REQUIRE(schema.interfaces.size() == 1);
   REQUIRE(schema.interfaces[0].name == "kernel");
   REQUIRE(schema.interfaces[0].type_names.size() == 2);
   REQUIRE(schema.get(schema.interfaces[0].type_names[0]) != nullptr);
   REQUIRE(schema.get(schema.interfaces[0].type_names[1]) != nullptr);
}

TEST_CASE("SchemaBuilder::insert_world: envelope survives FracPack round-trip",
          "[psio][structural][insert_world][fracpack]")
{
   namespace S = psio1::schema_types;
   auto schema = S::SchemaBuilder{}
                     .insert_world<psio1::detail::node_world_tag>()
                     .build();

   auto bytes = psio1::to_frac(schema);
   auto rt    = psio1::from_frac<S::Schema>(bytes);

   REQUIRE(rt.package.name == "psibase");
   REQUIRE(rt.uses.size() == 2);
   REQUIRE(rt.interfaces.size() == 1);
   REQUIRE(rt.interfaces[0].name == "kernel");
   REQUIRE(rt.interfaces[0].type_names.size() == 2);
}

// ── AnyType::Resource ───────────────────────────────────────────────────
//
// Types inheriting from psio1::wit_resource are emitted as `Resource`
// variants, carrying reflected methods rather than data members.

namespace structural_test
{
   struct cursor : psio1::wit_resource
   {
      bool                 seek(std::vector<std::uint8_t> key);
      bool                 next();
      std::vector<std::uint8_t> key() const;
   };
   PSIO1_REFLECT(cursor, method(seek, key), method(next), method(key))
}

TEST_CASE("AnyType::Resource: wit_resource emits Resource variant with methods",
          "[psio][structural][resource]")
{
   namespace S = psio1::schema_types;
   auto schema =
       S::SchemaBuilder{}.insert<structural_test::cursor>("cursor").build();

   const auto* any = schema.get("cursor");
   REQUIRE(any != nullptr);
   const auto* res = std::get_if<S::Resource>(&any->value);
   REQUIRE(res != nullptr);
   REQUIRE(res->name == "cursor");
   REQUIRE(res->methods.size() == 3);

   REQUIRE(res->methods[0].name == "seek");
   REQUIRE(res->methods[0].params.size() == 1);
   REQUIRE(res->methods[0].params[0].name == "key");
   REQUIRE(res->methods[0].result.has_value());

   REQUIRE(res->methods[1].name == "next");
   REQUIRE(res->methods[1].params.empty());
   REQUIRE(res->methods[1].result.has_value());

   REQUIRE(res->methods[2].name == "key");
   REQUIRE(res->methods[2].params.empty());
   REQUIRE(res->methods[2].result.has_value());
}

TEST_CASE("AnyType::Resource: survives FracPack round-trip",
          "[psio][structural][resource][fracpack]")
{
   namespace S = psio1::schema_types;
   auto schema =
       S::SchemaBuilder{}.insert<structural_test::cursor>("cursor").build();
   auto bytes = psio1::to_frac(schema);
   auto rt    = psio1::from_frac<S::Schema>(bytes);

   const auto* any = rt.get("cursor");
   REQUIRE(any != nullptr);
   const auto* res = std::get_if<S::Resource>(&any->value);
   REQUIRE(res != nullptr);
   REQUIRE(res->name == "cursor");
   REQUIRE(res->methods.size() == 3);
   REQUIRE(res->methods[0].name == "seek");
   REQUIRE(res->methods[0].params.size() == 1);
}

// ── PSIO1_HOST_MODULE(...) ──────────────────────────────────────────────────────
//
// Binds a host C++ implementation class to an interface/use tag so a
// linker can resolve `provide(impl)` back to the correct slot by type.
// Exercises the tag reverse-lookup, the per-impl method registry, and
// the ODR marker.

namespace structural_test
{
   struct KernelImpl
   {
      int submit_tx(int x) { return x + 100; }
      int query_chain() { return 7; }
   };
}  // namespace structural_test

PSIO1_HOST_MODULE(structural_test::KernelImpl,
          interface(kernel, submit_tx, query_chain))

TEST_CASE("PSIO1_HOST_MODULE: impl_of reverse-maps the impl class to its tag list",
          "[psio][structural][impl]")
{
   STATIC_REQUIRE(
       std::is_same_v<psio1::impl_of<structural_test::KernelImpl>::type,
                      std::tuple<kernel>>);
}

TEST_CASE("PSIO1_HOST_MODULE: iface_impl carries tag, method pointers, and names",
          "[psio][structural][impl]")
{
   using info = psio1::detail::iface_impl<structural_test::KernelImpl, kernel>;

   STATIC_REQUIRE(std::is_same_v<info::tag, kernel>);

   STATIC_REQUIRE(std::tuple_size_v<decltype(info::methods)> == 2);
   REQUIRE(info::names.size() == 2);
   REQUIRE(info::names[0] == "submit_tx");
   REQUIRE(info::names[1] == "query_chain");

   // Invoke through the reflected member pointers to prove they bind
   // to the right members, not just matching names.
   structural_test::KernelImpl k;
   auto submit = std::get<0>(info::methods);
   auto query  = std::get<1>(info::methods);
   REQUIRE((k.*submit)(3) == 103);
   REQUIRE((k.*query)() == 7);
}

TEST_CASE("PSIO1_HOST_MODULE: impl_info aggregates interfaces for an Impl",
          "[psio][structural][impl]")
{
   using info = psio1::detail::impl_info<structural_test::KernelImpl>;
   STATIC_REQUIRE(
       std::is_same_v<info::interfaces,
                      std::tuple<psio1::detail::iface_impl<
                          structural_test::KernelImpl, kernel>>>);
}

// ── emit_wit ────────────────────────────────────────────────────────────
//
// Walks the Schema IR produced by SchemaBuilder and emits WIT text.
// The node world fixture above gives us a realistic payload: a package,
// two `use` lines, one interface with two records and two funcs, and
// a world with imports/exports.

namespace
{
   bool contains(std::string_view hay, std::string_view needle)
   {
      return hay.find(needle) != std::string_view::npos;
   }
}  // namespace

TEST_CASE("emit_wit: envelope produces package, uses, interface, world",
          "[psio][structural][emit_wit]")
{
   namespace S = psio1::schema_types;
   auto schema = S::SchemaBuilder{}
                     .insert_world<psio1::detail::node_world_tag>()
                     .build();
   auto text = S::emit_wit(schema);

   REQUIRE(contains(text, "package psibase@0.3.0;"));
   REQUIRE(contains(text, "use wasi/streams@0.2.0;"));
   REQUIRE(contains(text, "use wasi/clocks@0.2.0;"));
   REQUIRE(contains(text, "interface kernel {"));
   REQUIRE(contains(text, "world node {"));
   REQUIRE(contains(text, "import wasi/streams;"));
   REQUIRE(contains(text, "import wasi/clocks;"));
   REQUIRE(contains(text, "export kernel;"));
}

TEST_CASE("emit_wit: interface types render as records with kebab fields",
          "[psio][structural][emit_wit]")
{
   namespace S = psio1::schema_types;
   auto schema = S::SchemaBuilder{}
                     .insert_world<psio1::detail::node_world_tag>()
                     .build();
   auto text = S::emit_wit(schema);

   REQUIRE(contains(text, "record block {"));
   REQUIRE(contains(text, "height: s32"));
   REQUIRE(contains(text, "record transaction {"));
   REQUIRE(contains(text, "nonce: s32"));
}

TEST_CASE("emit_wit: interface funcs render with kebab names and params",
          "[psio][structural][emit_wit]")
{
   namespace S = psio1::schema_types;
   auto schema = S::SchemaBuilder{}
                     .insert_world<psio1::detail::node_world_tag>()
                     .build();
   auto text = S::emit_wit(schema);

   REQUIRE(contains(text, "submit-tx: func("));
   REQUIRE(contains(text, "query-chain: func()"));
}

TEST_CASE("emit_wit: resource emits methods and kebab-cased signatures",
          "[psio][structural][emit_wit]")
{
   namespace S = psio1::schema_types;
   auto schema =
       S::SchemaBuilder{}.insert<structural_test::cursor>("cursor").build();
   auto text = S::emit_wit(schema);

   REQUIRE(contains(text, "resource cursor {"));
   REQUIRE(contains(text, "seek: func(key: list<u8>) -> bool"));
   REQUIRE(contains(text, "next: func() -> bool"));
   REQUIRE(contains(text, "key: func() -> list<u8>"));
}

