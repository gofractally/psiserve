#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiserve/module_store.hpp>

#include <algorithm>
#include <cstring>

namespace
{
   std::span<const std::byte> as_bytes(const char* s)
   {
      return {reinterpret_cast<const std::byte*>(s), std::strlen(s)};
   }
}  // namespace

// ═══════════════════════════════════════════════════════════════════
// hashBytes
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("hashBytes produces deterministic 32-byte hash", "[module_store]")
{
   auto h1 = psiserve::hashBytes(as_bytes("hello wasm"));
   auto h2 = psiserve::hashBytes(as_bytes("hello wasm"));
   CHECK(h1 == h2);
   CHECK(h1.bytes != psiserve::ModuleHash{}.bytes);
}

TEST_CASE("hashBytes produces different hashes for different inputs", "[module_store]")
{
   auto h1 = psiserve::hashBytes(as_bytes("module_a"));
   auto h2 = psiserve::hashBytes(as_bytes("module_b"));
   CHECK(h1 != h2);
}

TEST_CASE("hashBytes handles empty input", "[module_store]")
{
   auto h = psiserve::hashBytes({});
   CHECK(h.bytes != psiserve::ModuleHash{}.bytes);
}

// ═══════════════════════════════════════════════════════════════════
// ModuleHashHasher
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("ModuleHashHasher is consistent", "[module_store]")
{
   auto h = psiserve::hashBytes(as_bytes("test"));
   psiserve::ModuleHashHasher hasher;
   CHECK(hasher(h) == hasher(h));
}

TEST_CASE("ModuleHashHasher differentiates distinct hashes", "[module_store]")
{
   auto h1 = psiserve::hashBytes(as_bytes("alpha"));
   auto h2 = psiserve::hashBytes(as_bytes("beta"));
   psiserve::ModuleHashHasher hasher;
   CHECK(hasher(h1) != hasher(h2));
}

// ═══════════════════════════════════════════════════════════════════
// NameRegistry
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("NameRegistry resolve returns nullopt for unknown name", "[module_store]")
{
   psiserve::NameRegistry reg;
   CHECK(!reg.resolve("nonexistent").has_value());
}

TEST_CASE("NameRegistry bind and resolve", "[module_store]")
{
   psiserve::NameRegistry reg;
   auto hash = psiserve::hashBytes(as_bytes("mod_bytes"));

   reg.bind("blockchain", hash);
   auto result = reg.resolve("blockchain");
   REQUIRE(result.has_value());
   CHECK(*result == hash);
}

TEST_CASE("NameRegistry rebind replaces previous hash", "[module_store]")
{
   psiserve::NameRegistry reg;
   auto h1 = psiserve::hashBytes(as_bytes("v1"));
   auto h2 = psiserve::hashBytes(as_bytes("v2"));

   reg.bind("service", h1);
   reg.bind("service", h2);
   auto result = reg.resolve("service");
   REQUIRE(result.has_value());
   CHECK(*result == h2);
}

TEST_CASE("NameRegistry unbind removes entry", "[module_store]")
{
   psiserve::NameRegistry reg;
   auto hash = psiserve::hashBytes(as_bytes("data"));

   reg.bind("temp", hash);
   REQUIRE(reg.resolve("temp").has_value());

   reg.unbind("temp");
   CHECK(!reg.resolve("temp").has_value());
}

TEST_CASE("NameRegistry unbind on missing name is safe", "[module_store]")
{
   psiserve::NameRegistry reg;
   reg.unbind("never_existed");
   CHECK(!reg.resolve("never_existed").has_value());
}

TEST_CASE("NameRegistry list returns all bindings", "[module_store]")
{
   psiserve::NameRegistry reg;
   auto h1 = psiserve::hashBytes(as_bytes("a"));
   auto h2 = psiserve::hashBytes(as_bytes("b"));

   reg.bind("alpha", h1);
   reg.bind("beta", h2);

   auto entries = reg.list();
   CHECK(entries.size() == 2);

   std::sort(entries.begin(), entries.end(),
             [](auto& a, auto& b) { return a.first < b.first; });
   CHECK(entries[0].first == "alpha");
   CHECK(entries[0].second == h1);
   CHECK(entries[1].first == "beta");
   CHECK(entries[1].second == h2);
}

TEST_CASE("NameRegistry list is empty initially", "[module_store]")
{
   psiserve::NameRegistry reg;
   CHECK(reg.list().empty());
}

// ═══════════════════════════════════════════════════════════════════
// ModuleStore
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("ModuleStore lookup returns null for unknown hash", "[module_store]")
{
   psiserve::ModuleStore store;
   psiserve::ModuleHash h{};
   CHECK(store.lookup(h) == nullptr);
}

TEST_CASE("ModuleStore getOrCompile caches and returns module", "[module_store]")
{
   psiserve::ModuleStore store;
   auto bytes = as_bytes("(module)");
   auto hash  = psiserve::hashBytes(bytes);

   auto mod = store.getOrCompile(hash, bytes);
   REQUIRE(mod != nullptr);
   CHECK(mod->hash() == hash);
   CHECK(mod->sourceSize() == bytes.size());
   CHECK(store.size() == 1);
}

TEST_CASE("ModuleStore getOrCompile returns cached on second call", "[module_store]")
{
   psiserve::ModuleStore store;
   auto bytes = as_bytes("(module data)");
   auto hash  = psiserve::hashBytes(bytes);

   auto mod1 = store.getOrCompile(hash, bytes);
   auto mod2 = store.getOrCompile(hash, bytes);
   CHECK(mod1.get() == mod2.get());
   CHECK(store.size() == 1);
}

TEST_CASE("ModuleStore stores distinct modules separately", "[module_store]")
{
   psiserve::ModuleStore store;
   auto b1 = as_bytes("module_one");
   auto b2 = as_bytes("module_two");
   auto h1 = psiserve::hashBytes(b1);
   auto h2 = psiserve::hashBytes(b2);

   store.getOrCompile(h1, b1);
   store.getOrCompile(h2, b2);
   CHECK(store.size() == 2);
   CHECK(store.bytes() == b1.size() + b2.size());
}

TEST_CASE("ModuleStore lookup returns cached module", "[module_store]")
{
   psiserve::ModuleStore store;
   auto bytes = as_bytes("cached_mod");
   auto hash  = psiserve::hashBytes(bytes);

   auto mod = store.getOrCompile(hash, bytes);
   auto found = store.lookup(hash);
   REQUIRE(found != nullptr);
   CHECK(found.get() == mod.get());
}

TEST_CASE("ModuleStore bytes tracks total source size", "[module_store]")
{
   psiserve::ModuleStore store;
   CHECK(store.bytes() == 0);

   auto b = as_bytes("12345");
   store.getOrCompile(psiserve::hashBytes(b), b);
   CHECK(store.bytes() == 5);
}

TEST_CASE("ModuleStore upgradeBackend changes backend kind", "[module_store]")
{
   psiserve::ModuleStore store(psiserve::BackendKind::Interpreter);
   auto bytes = as_bytes("upgradeable");
   auto hash  = psiserve::hashBytes(bytes);

   store.getOrCompile(hash, bytes);
   auto mod = store.lookup(hash);
   CHECK(mod->currentBackend()->kind == psiserve::BackendKind::Interpreter);

   bool upgraded = store.upgradeBackend(hash, psiserve::BackendKind::Jit);
   CHECK(upgraded);
   CHECK(mod->currentBackend()->kind == psiserve::BackendKind::Jit);
}

TEST_CASE("ModuleStore upgradeBackend returns false for same kind", "[module_store]")
{
   psiserve::ModuleStore store(psiserve::BackendKind::Jit);
   auto bytes = as_bytes("same_kind");
   auto hash  = psiserve::hashBytes(bytes);

   store.getOrCompile(hash, bytes);
   CHECK(!store.upgradeBackend(hash, psiserve::BackendKind::Jit));
}

TEST_CASE("ModuleStore upgradeBackend returns false for unknown hash", "[module_store]")
{
   psiserve::ModuleStore store;
   psiserve::ModuleHash h{};
   CHECK(!store.upgradeBackend(h, psiserve::BackendKind::Jit));
}

TEST_CASE("ModuleStore evictTo removes unreferenced modules oldest-first", "[module_store]")
{
   psiserve::ModuleStore store;

   auto b1 = as_bytes("aaaa");
   auto b2 = as_bytes("bbbbbbbb");
   auto h1 = psiserve::hashBytes(b1);
   auto h2 = psiserve::hashBytes(b2);

   store.getOrCompile(h1, b1);
   store.getOrCompile(h2, b2);
   CHECK(store.size() == 2);
   CHECK(store.bytes() == 12);

   store.evictTo(8);
   CHECK(store.size() == 1);
   CHECK(store.lookup(h2) != nullptr);
   CHECK(store.lookup(h1) == nullptr);
}

TEST_CASE("ModuleStore evictTo preserves externally-referenced modules", "[module_store]")
{
   psiserve::ModuleStore store;
   auto b = as_bytes("held");
   auto h = psiserve::hashBytes(b);

   auto external_ref = store.getOrCompile(h, b);
   store.evictTo(0);

   // Module is still in store because external_ref holds it
   CHECK(store.size() == 1);
}

TEST_CASE("ModuleStore getOrCompile with explicit backend kind", "[module_store]")
{
   psiserve::ModuleStore store(psiserve::BackendKind::Interpreter);
   auto bytes = as_bytes("explicit_kind");
   auto hash  = psiserve::hashBytes(bytes);

   auto mod = store.getOrCompile(hash, bytes, psiserve::BackendKind::Jit2);
   REQUIRE(mod != nullptr);
   CHECK(mod->currentBackend()->kind == psiserve::BackendKind::Jit2);
}

TEST_CASE("ModuleStore defaultBackend reflects construction param", "[module_store]")
{
   psiserve::ModuleStore s1;
   CHECK(s1.defaultBackend() == psiserve::BackendKind::Jit);

   psiserve::ModuleStore s2(psiserve::BackendKind::Interpreter);
   CHECK(s2.defaultBackend() == psiserve::BackendKind::Interpreter);
}
