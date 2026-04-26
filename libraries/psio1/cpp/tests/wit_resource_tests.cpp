#include <catch2/catch.hpp>

#include <psio1/wit_gen.hpp>
#include <psio1/wit_resource.hpp>

#include <cstdint>
#include <string>
#include <vector>

// =========================================================================
// Test resource types — models a key-value store interface
// =========================================================================

// All types use stub method implementations — the WIT generator only needs
// the reflected method signatures, not real logic.

// A read-only snapshot of a database root.
struct snapshot : psio1::wit_resource {
   std::vector<uint8_t> get(std::vector<uint8_t>) { return {}; }
   bool                 exists(std::vector<uint8_t>) { return false; }
};
PSIO1_REFLECT(snapshot, method(get, key), method(exists, key))

// A read-write transaction with commit/rollback.
struct transaction : psio1::wit_resource {
   void                insert(std::vector<uint8_t>, std::vector<uint8_t>) {}
   void                erase(std::vector<uint8_t>) {}
   psio1::own<snapshot> get_snapshot() { return psio1::own<snapshot>{0}; }
   void                commit() {}
};
PSIO1_REFLECT(transaction,
   method(insert, key, value),
   method(erase, key),
   method(get_snapshot),
   method(commit))

// A cursor for range iteration over keys.
struct cursor : psio1::wit_resource {
   bool                 seek(std::vector<uint8_t>) { return false; }
   bool                 next() { return false; }
   std::vector<uint8_t> key() { return {}; }
   std::vector<uint8_t> value() { return {}; }
};
PSIO1_REFLECT(cursor,
   method(seek, key), method(next), method(key), method(value))

// The top-level interface that guests import.
// Free functions return own<T> handles to resources.
struct kv_host {
   psio1::own<snapshot>    open_snapshot(uint32_t) { return psio1::own<snapshot>{0}; }
   psio1::own<transaction> begin(uint32_t) { return psio1::own<transaction>{0}; }
   psio1::own<cursor>      open_cursor(psio1::borrow<snapshot>) { return psio1::own<cursor>{0}; }
};
PSIO1_REFLECT(kv_host,
   method(open_snapshot, root_index),
   method(begin, root_index),
   method(open_cursor, snap))

// A plain record (not a resource) for comparison.
struct point {
   int32_t x = 0;
   int32_t y = 0;
};
PSIO1_REFLECT(point, x, y)

// An interface that uses both records and resources.
struct mixed_interface {
   point             get_origin() { return {}; }
   psio1::own<cursor> scan(point) { return psio1::own<cursor>{0}; }
};
PSIO1_REFLECT(mixed_interface,
   method(get_origin),
   method(scan, from))

// A resource with no methods.
struct opaque_handle : psio1::wit_resource {};
PSIO1_REFLECT(opaque_handle)

// A resource with both data members and methods — data should be ignored.
struct stateful_resource : psio1::wit_resource {
   int32_t internal_state = 0;
   void    do_thing() {}
};
PSIO1_REFLECT(stateful_resource, internal_state, method(do_thing))

// A resource with camelCase/snake_case names for kebab-case testing.
struct MyResource : psio1::wit_resource {
   bool do_something(uint32_t) { return false; }
};
PSIO1_REFLECT(MyResource, method(do_something, param_name))

// A guest exports type for import/export world testing.
struct guest_exports {
   std::string hello() { return ""; }
};
PSIO1_REFLECT(guest_exports, method(hello))

// =========================================================================
// Trait tests
// =========================================================================

TEST_CASE("is_wit_resource_v detects resource types", "[wit][resource]") {
   SECTION("resource types") {
      CHECK(psio1::is_wit_resource_v<snapshot>);
      CHECK(psio1::is_wit_resource_v<transaction>);
      CHECK(psio1::is_wit_resource_v<cursor>);
   }

   SECTION("non-resource types") {
      CHECK_FALSE(psio1::is_wit_resource_v<point>);
      CHECK_FALSE(psio1::is_wit_resource_v<kv_host>);
      CHECK_FALSE(psio1::is_wit_resource_v<int>);
      CHECK_FALSE(psio1::is_wit_resource_v<std::string>);
   }
}

// =========================================================================
// WIT generation — type resolution
// =========================================================================

TEST_CASE("Resource type resolves to resource_ kind", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   int32_t idx = ctx.resolve_type<cursor>();

   REQUIRE(idx >= 0);
   REQUIRE(static_cast<size_t>(idx) < ctx.world.types.size());

   auto& td = ctx.world.types[idx];
   CHECK(td.name == "cursor");
   CHECK(static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::resource_);
   CHECK(td.fields.empty());  // resources are opaque — no data fields
   CHECK(td.method_func_idxs.size() == 4);  // seek, next, key, value
}

TEST_CASE("Resource methods are generated correctly", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   ctx.resolve_type<cursor>();

   auto& td = ctx.world.types[0];
   REQUIRE(td.method_func_idxs.size() == 4);

   // Method 0: seek(key: list<u8>) -> bool
   auto& seek = ctx.world.funcs[td.method_func_idxs[0]];
   CHECK(seek.name == "seek");
   REQUIRE(seek.params.size() == 1);
   CHECK(seek.params[0].name == "key");
   REQUIRE(seek.results.size() == 1);

   // Method 1: next() -> bool
   auto& next_fn = ctx.world.funcs[td.method_func_idxs[1]];
   CHECK(next_fn.name == "next");
   CHECK(next_fn.params.empty());
   REQUIRE(next_fn.results.size() == 1);

   // Method 2: key() -> list<u8>
   auto& key_fn = ctx.world.funcs[td.method_func_idxs[2]];
   CHECK(key_fn.name == "key");
   CHECK(key_fn.params.empty());
   REQUIRE(key_fn.results.size() == 1);

   // Method 3: value() -> list<u8>
   auto& val_fn = ctx.world.funcs[td.method_func_idxs[3]];
   CHECK(val_fn.name == "value");
   CHECK(val_fn.params.empty());
   REQUIRE(val_fn.results.size() == 1);
}

TEST_CASE("own<T> resolves to own_ kind wrapping resource", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   int32_t idx = ctx.resolve_type<psio1::own<cursor>>();

   REQUIRE(idx >= 0);
   auto& td = ctx.world.types[idx];
   CHECK(static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::own_);

   // element_type_idx should point to the cursor resource
   auto& res = ctx.world.types[td.element_type_idx];
   CHECK(res.name == "cursor");
   CHECK(static_cast<psio1::wit_type_kind>(res.kind) == psio1::wit_type_kind::resource_);
}

TEST_CASE("borrow<T> resolves to borrow_ kind wrapping resource", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   int32_t idx = ctx.resolve_type<psio1::borrow<snapshot>>();

   REQUIRE(idx >= 0);
   auto& td = ctx.world.types[idx];
   CHECK(static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::borrow_);

   // element_type_idx should point to the snapshot resource
   auto& res = ctx.world.types[td.element_type_idx];
   CHECK(res.name == "snapshot");
   CHECK(static_cast<psio1::wit_type_kind>(res.kind) == psio1::wit_type_kind::resource_);
}

TEST_CASE("Resource type is deduplicated across own/borrow references", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;

   int32_t own_idx    = ctx.resolve_type<psio1::own<cursor>>();
   int32_t borrow_idx = ctx.resolve_type<psio1::borrow<cursor>>();
   int32_t direct_idx = ctx.resolve_type<cursor>();

   // own<cursor> and borrow<cursor> should reference the same cursor resource
   auto& own_td    = ctx.world.types[own_idx];
   auto& borrow_td = ctx.world.types[borrow_idx];

   CHECK(own_td.element_type_idx == direct_idx);
   CHECK(borrow_td.element_type_idx == direct_idx);
}

TEST_CASE("Record type is not affected by resource support", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   int32_t idx = ctx.resolve_type<point>();

   REQUIRE(idx >= 0);
   auto& td = ctx.world.types[idx];
   CHECK(td.name == "point");
   CHECK(static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::record_);
   REQUIRE(td.fields.size() == 2);
   CHECK(td.fields[0].name == "x");
   CHECK(td.fields[1].name == "y");
   CHECK(td.method_func_idxs.empty());
}

// =========================================================================
// WIT text generation
// =========================================================================

TEST_CASE("wit_type_name renders own<T> and borrow<T>", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   ctx.resolve_type<psio1::own<cursor>>();
   ctx.resolve_type<psio1::borrow<snapshot>>();

   // Find the own_ type
   for (size_t i = 0; i < ctx.world.types.size(); i++) {
      auto& td = ctx.world.types[i];
      if (static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::own_) {
         CHECK(psio1::detail::wit_type_name(ctx.world, static_cast<int32_t>(i)) == "own<cursor>");
      }
      if (static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::borrow_) {
         CHECK(psio1::detail::wit_type_name(ctx.world, static_cast<int32_t>(i)) == "borrow<snapshot>");
      }
   }
}

TEST_CASE("Resource generates correct WIT text block", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   ctx.resolve_type<cursor>();

   std::ostringstream os;
   psio1::detail::wit_emit_type(os, ctx.world, ctx.world.types[0], "");
   auto text = os.str();

   CHECK(text.find("resource cursor {") != std::string::npos);
   CHECK(text.find("  seek: func(key: list<u8>) -> bool;") != std::string::npos);
   CHECK(text.find("  next: func() -> bool;") != std::string::npos);
   CHECK(text.find("  key: func() -> list<u8>;") != std::string::npos);
   CHECK(text.find("  value: func() -> list<u8>;") != std::string::npos);
}

TEST_CASE("Full kv_host interface generates valid WIT", "[wit][resource]") {
   auto text = psio1::generate_wit_text<kv_host>("psitri:kv@0.1.0");

   // Package declaration
   CHECK(text.find("package psitri:kv@0.1.0;") != std::string::npos);

   // Resource declarations (emitted as types within the interface)
   CHECK(text.find("resource snapshot {") != std::string::npos);
   CHECK(text.find("resource transaction {") != std::string::npos);
   CHECK(text.find("resource cursor {") != std::string::npos);

   // Resource methods
   CHECK(text.find("get: func(key: list<u8>) -> list<u8>;") != std::string::npos);
   CHECK(text.find("exists: func(key: list<u8>) -> bool;") != std::string::npos);
   CHECK(text.find("insert: func(key: list<u8>, value: list<u8>);") != std::string::npos);
   CHECK(text.find("commit: func();") != std::string::npos);

   // Free functions with own<T> return types
   CHECK(text.find("open-snapshot: func(root-index: u32) -> own<snapshot>;") != std::string::npos);
   CHECK(text.find("begin: func(root-index: u32) -> own<transaction>;") != std::string::npos);

   // borrow<T> in parameters
   CHECK(text.find("open-cursor: func(snap: borrow<snapshot>) -> own<cursor>;") != std::string::npos);
}

TEST_CASE("Transaction method returning own<snapshot>", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   ctx.resolve_type<transaction>();

   auto& td = ctx.world.types[0];  // transaction is first resolved
   REQUIRE(!td.method_func_idxs.empty());

   // get_snapshot method should return own<snapshot>
   auto& get_snap = ctx.world.funcs[td.method_func_idxs[2]];
   CHECK(get_snap.name == "get-snapshot");
   REQUIRE(get_snap.results.size() == 1);

   auto result_name = psio1::detail::wit_type_name(ctx.world, get_snap.results[0].type_idx);
   CHECK(result_name == "own<snapshot>");
}

TEST_CASE("Mixed interface with records and resources", "[wit][resource]") {
   auto text = psio1::generate_wit_text<mixed_interface>("test:mixed@0.1.0");

   // Should have a record
   CHECK(text.find("record point {") != std::string::npos);
   CHECK(text.find("x: s32,") != std::string::npos);
   CHECK(text.find("y: s32,") != std::string::npos);

   // Should have a resource
   CHECK(text.find("resource cursor {") != std::string::npos);

   // Free functions
   CHECK(text.find("get-origin: func() -> point;") != std::string::npos);
   CHECK(text.find("scan: func(from: point) -> own<cursor>;") != std::string::npos);
}

TEST_CASE("Resource with no methods", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   ctx.resolve_type<opaque_handle>();

   auto& td = ctx.world.types[0];
   CHECK(static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::resource_);
   CHECK(td.method_func_idxs.empty());

   std::ostringstream os;
   psio1::detail::wit_emit_type(os, ctx.world, td, "");
   CHECK(os.str().find("resource opaque-handle;") != std::string::npos);
}

TEST_CASE("Resource data members are ignored in WIT output", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   ctx.resolve_type<stateful_resource>();

   auto& td = ctx.world.types[0];
   CHECK(static_cast<psio1::wit_type_kind>(td.kind) == psio1::wit_type_kind::resource_);
   // Data members should NOT appear as fields
   CHECK(td.fields.empty());
   // But methods should be present
   CHECK(td.method_func_idxs.size() == 1);
}

TEST_CASE("kebab-case conversion for resource names and methods", "[wit][resource]") {
   psio1::detail::wit_gen_ctx ctx;
   ctx.resolve_type<MyResource>();

   auto& td = ctx.world.types[0];
   CHECK(td.name == "my-resource");

   auto& func = ctx.world.funcs[td.method_func_idxs[0]];
   CHECK(func.name == "do-something");
   CHECK(func.params[0].name == "param-name");
}

TEST_CASE("Import interface with resources generates correct world block", "[wit][resource]") {
   auto text = psio1::generate_wit_text<guest_exports, kv_host>("test:app@0.1.0");

   // World should import kv_host and export guest_exports
   CHECK(text.find("import kv-host;") != std::string::npos);
   CHECK(text.find("export guest-exports;") != std::string::npos);

   // Resources should appear in the import interface
   CHECK(text.find("resource snapshot {") != std::string::npos);
   CHECK(text.find("resource cursor {") != std::string::npos);
}
