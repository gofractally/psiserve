#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiserve/runtime_host.hpp>

#include <cstdint>
#include <string>

namespace
{
   using namespace psi::runtime;
   using psiserve::IdentityHost;
   using psiserve::InstancesHost;
   using psiserve::DispatchHost;
   using psiserve::RuntimeHost;
   using psiserve::ModuleStore;
   using psiserve::NameRegistry;
   using psiserve::InstanceInfo;
   using psiserve::CallerGuard;

   struct runtime_fixture
   {
      ModuleStore   store;
      NameRegistry  registry;
      RuntimeHost   host;

      runtime_fixture()
      {
         host.init(/*self_id=*/1, /*self_name=*/100,
                   /*rt=*/nullptr, &store, &registry);
      }

      instance_id register_and_instantiate(const char* name_str, name_id nid)
      {
         auto bytes = std::span<const std::byte>(
             reinterpret_cast<const std::byte*>(name_str), strlen(name_str));
         auto hash = psiserve::hashBytes(bytes);
         store.getOrCompile(hash, bytes);
         registry.bind(std::to_string(nid), hash);
         auto r = host.instances.instantiate(nid, thread_hint::this_thread,
                                             mem_budget::_4MB, 0);
         return *r;
      }
   };
}  // namespace

// ═══════════════════════════════════════════════════════════════════
// IdentityHost
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("IdentityHost self returns configured id", "[runtime_host]")
{
   IdentityHost id;
   id.self_id = 42;
   CHECK(id.self() == 42);
}

TEST_CASE("IdentityHost caller returns 0 when no cross-instance call", "[runtime_host]")
{
   IdentityHost id;
   id.self_id = 1;
   CHECK(id.caller() == 0);
}

TEST_CASE("IdentityHost caller returns set caller_id", "[runtime_host]")
{
   IdentityHost id;
   id.caller_id = 99;
   CHECK(id.caller() == 99);
}

TEST_CASE("IdentityHost has_import returns false when nothing bound", "[runtime_host]")
{
   IdentityHost id;
   CHECK(!id.has_import(12345));
}

TEST_CASE("IdentityHost has_import returns true for bound interface", "[runtime_host]")
{
   IdentityHost id;
   id.bound_interfaces.push_back(100);
   id.bound_interfaces.push_back(200);

   CHECK(id.has_import(100));
   CHECK(id.has_import(200));
   CHECK(!id.has_import(300));
}

// ═══════════════════════════════════════════════════════════════════
// CallerGuard
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("CallerGuard sets and restores caller_id", "[runtime_host]")
{
   IdentityHost id;
   id.caller_id = 0;

   {
      CallerGuard guard(&id, 42);
      CHECK(id.caller() == 42);
   }
   CHECK(id.caller() == 0);
}

TEST_CASE("CallerGuard restores previous caller on nested calls", "[runtime_host]")
{
   IdentityHost id;
   id.caller_id = 10;

   {
      CallerGuard outer(&id, 20);
      CHECK(id.caller() == 20);
      {
         CallerGuard inner(&id, 30);
         CHECK(id.caller() == 30);
      }
      CHECK(id.caller() == 20);
   }
   CHECK(id.caller() == 10);
}

// ═══════════════════════════════════════════════════════════════════
// RuntimeHost set_caller / clear_caller
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("RuntimeHost set_caller / clear_caller cycle", "[runtime_host]")
{
   runtime_fixture f;

   CHECK(f.host.identity.caller() == 0);

   f.host.set_caller(55);
   CHECK(f.host.identity.caller() == 55);

   f.host.clear_caller();
   CHECK(f.host.identity.caller() == 0);
}

// ═══════════════════════════════════════════════════════════════════
// InstancesHost — instantiate by name
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("InstancesHost instantiate fails for unregistered name", "[runtime_host]")
{
   runtime_fixture f;

   auto r = f.host.instances.instantiate(999, thread_hint::this_thread,
                                         mem_budget::_4MB, 0);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::not_found);
}

TEST_CASE("InstancesHost instantiate succeeds for registered module", "[runtime_host]")
{
   runtime_fixture f;
   auto id = f.register_and_instantiate("wasm", 42);
   CHECK(id >= 1);
}

TEST_CASE("InstancesHost instantiate assigns unique IDs", "[runtime_host]")
{
   runtime_fixture f;

   auto bytes = std::span<const std::byte>(
       reinterpret_cast<const std::byte*>("mod"), 3);
   auto hash = psiserve::hashBytes(bytes);
   f.store.getOrCompile(hash, bytes);
   f.registry.bind("10", hash);

   auto r1 = f.host.instances.instantiate(10, thread_hint::this_thread,
                                          mem_budget::_4MB, 0);
   auto r2 = f.host.instances.instantiate(10, thread_hint::this_thread,
                                          mem_budget::_4MB, 0);
   REQUIRE(r1.has_value());
   REQUIRE(r2.has_value());
   CHECK(*r1 != *r2);
}

// ═══════════════════════════════════════════════════════════════════
// InstancesHost — instantiate by hash
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("InstancesHost instantiate_by_hash fails for wrong size", "[runtime_host]")
{
   runtime_fixture f;

   std::vector<uint8_t> bad_hash{1, 2, 3};
   auto r = f.host.instances.instantiate_by_hash(bad_hash, thread_hint::this_thread,
                                                 mem_budget::_4MB, 0);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::invalid_arg);
}

TEST_CASE("InstancesHost instantiate_by_hash fails for unknown hash", "[runtime_host]")
{
   runtime_fixture f;

   std::vector<uint8_t> unknown(32, 0xFF);
   auto r = f.host.instances.instantiate_by_hash(unknown, thread_hint::this_thread,
                                                 mem_budget::_4MB, 0);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::not_found);
}

TEST_CASE("InstancesHost instantiate_by_hash succeeds for cached module", "[runtime_host]")
{
   runtime_fixture f;

   auto bytes = std::span<const std::byte>(
       reinterpret_cast<const std::byte*>("contract"), 8);
   auto hash = psiserve::hashBytes(bytes);
   f.store.getOrCompile(hash, bytes);

   std::vector<uint8_t> hash_vec(hash.bytes.begin(), hash.bytes.end());
   auto r = f.host.instances.instantiate_by_hash(hash_vec, thread_hint::this_thread,
                                                 mem_budget::_4MB, 0);
   REQUIRE(r.has_value());
   CHECK(*r >= 1);
}

// ═══════════════════════════════════════════════════════════════════
// InstancesHost — bind
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("InstancesHost bind fails for nonexistent consumer", "[runtime_host]")
{
   runtime_fixture f;

   auto r = f.host.instances.bind(999, 1, 100);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::invalid_handle);
}

TEST_CASE("InstancesHost bind fails for nonexistent provider", "[runtime_host]")
{
   runtime_fixture f;
   auto consumer = f.register_and_instantiate("c", 5);

   auto r = f.host.instances.bind(consumer, 999, 100);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::invalid_handle);
}

TEST_CASE("InstancesHost bind succeeds and records interface", "[runtime_host]")
{
   runtime_fixture f;
   auto consumer = f.register_and_instantiate("m1", 7);
   auto provider = f.register_and_instantiate("m2", 8);

   name_id blockchain_api = 42;
   auto r = f.host.instances.bind(consumer, provider, blockchain_api);
   REQUIRE(r.has_value());

   auto& info = f.host.instance_map[consumer];
   CHECK(!info.bound_interfaces.empty());
   CHECK(info.bound_interfaces.back() == blockchain_api);
}

// ═══════════════════════════════════════════════════════════════════
// InstancesHost — destroy
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("InstancesHost destroy removes instance", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("d", 3);
   CHECK(f.host.instance_map.count(inst) == 1);

   auto r = f.host.instances.destroy(inst);
   REQUIRE(r.has_value());
   CHECK(f.host.instance_map.count(inst) == 0);
}

TEST_CASE("InstancesHost destroy fails for unknown instance", "[runtime_host]")
{
   runtime_fixture f;

   auto r = f.host.instances.destroy(999);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::not_found);
}

// ═══════════════════════════════════════════════════════════════════
// InstancesHost — running / instance_name
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("InstancesHost running returns configured value", "[runtime_host]")
{
   runtime_fixture f;

   CHECK(f.host.instances.running() == true);
   f.host.instances.is_running = false;
   CHECK(f.host.instances.running() == false);
}

TEST_CASE("InstancesHost instance_name returns configured name", "[runtime_host]")
{
   runtime_fixture f;

   CHECK(f.host.instances.instance_name() == 100);
}

// ═══════════════════════════════════════════════════════════════════
// DispatchHost — call
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("DispatchHost call fails for unknown target", "[runtime_host]")
{
   runtime_fixture f;

   auto r = f.host.dispatch.call(999, 1, {});
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::invalid_handle);
}

TEST_CASE("DispatchHost call fails when no callback set", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("x", 1);

   auto r = f.host.dispatch.call(inst, 42, {});
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::call_failed);
}

TEST_CASE("DispatchHost call invokes callback and returns result", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("y", 2);

   f.host.dispatch.on_call = [](instance_id, name_id method,
                                psi::runtime::bytes args)
      -> std::expected<psi::runtime::bytes, error>
   {
      psi::runtime::bytes result;
      result.push_back(static_cast<uint8_t>(method & 0xFF));
      result.insert(result.end(), args.begin(), args.end());
      return result;
   };

   psi::runtime::bytes input{0xAA, 0xBB};
   auto r = f.host.dispatch.call(inst, 7, input);
   REQUIRE(r.has_value());
   CHECK(r->size() == 3);
   CHECK((*r)[0] == 7);
   CHECK((*r)[1] == 0xAA);
   CHECK((*r)[2] == 0xBB);
}

// ═══════════════════════════════════════════════════════════════════
// DispatchHost — post
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("DispatchHost post fails for unknown target", "[runtime_host]")
{
   runtime_fixture f;

   auto r = f.host.dispatch.post(999, 1, {});
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::invalid_handle);
}

TEST_CASE("DispatchHost post succeeds with callback", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("z", 4);

   bool posted = false;
   f.host.dispatch.on_post = [&posted](instance_id, name_id, psi::runtime::bytes)
      -> std::expected<void, error>
   {
      posted = true;
      return {};
   };

   auto r = f.host.dispatch.post(inst, 1, {0x01});
   REQUIRE(r.has_value());
   CHECK(posted);
}

// ═══════════════════════════════════════════════════════════════════
// DispatchHost — async_call + await
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("DispatchHost async_call fails for unknown target", "[runtime_host]")
{
   runtime_fixture f;

   auto r = f.host.dispatch.async_call(999, 1, {}, thread_hint::this_thread);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::invalid_handle);
}

TEST_CASE("DispatchHost async_call returns future handle", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("async", 10);

   auto r = f.host.dispatch.async_call(inst, 1, {}, thread_hint::this_thread);
   REQUIRE(r.has_value());
   CHECK(*r != psizam::handle_table<psiserve::FutureInfo, 256>::invalid_handle);
}

TEST_CASE("DispatchHost await on unresolved future returns error", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("await", 11);

   auto fh = f.host.dispatch.async_call(inst, 1, {}, thread_hint::this_thread);
   REQUIRE(fh.has_value());

   auto r = f.host.dispatch.await(*fh);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::call_failed);
}

TEST_CASE("DispatchHost await on resolved future returns result", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("resolved", 12);

   auto fh = f.host.dispatch.async_call(inst, 1, {}, thread_hint::this_thread);
   REQUIRE(fh.has_value());

   auto* fi = f.host.dispatch.futures.get(*fh);
   REQUIRE(fi != nullptr);
   fi->resolved = true;
   fi->result = {0xDE, 0xAD};

   auto r = f.host.dispatch.await(*fh);
   REQUIRE(r.has_value());
   CHECK(r->size() == 2);
   CHECK((*r)[0] == 0xDE);
   CHECK((*r)[1] == 0xAD);

   CHECK(f.host.dispatch.futures.get(*fh) == nullptr);
}

TEST_CASE("DispatchHost await on invalid handle returns error", "[runtime_host]")
{
   runtime_fixture f;

   auto r = f.host.dispatch.await(0xFFFFFFFF);
   REQUIRE(!r.has_value());
   CHECK(r.error() == error::invalid_handle);
}

TEST_CASE("DispatchHost async_call with callback routes correctly", "[runtime_host]")
{
   runtime_fixture f;
   auto inst = f.register_and_instantiate("cb", 13);

   bool called = false;
   f.host.dispatch.on_async_call = [&called](
      instance_id, name_id, psi::runtime::bytes,
      thread_hint thread,
      psizam::handle_table<psiserve::FutureInfo, 256>& futures)
      -> std::expected<future_handle, error>
   {
      called = true;
      auto fh = futures.create(psiserve::FutureInfo{.resolved = true, .result = {0x42}});
      return fh;
   };

   auto fh = f.host.dispatch.async_call(inst, 1, {}, thread_hint::fresh);
   REQUIRE(fh.has_value());
   CHECK(called);

   auto r = f.host.dispatch.await(*fh);
   REQUIRE(r.has_value());
   CHECK((*r)[0] == 0x42);
}

// ═══════════════════════════════════════════════════════════════════
// RuntimeHost — full lifecycle
// ═══════════════════════════════════════════════════════════════════

TEST_CASE("RuntimeHost end-to-end: create, bind, dispatch, destroy", "[runtime_host]")
{
   runtime_fixture f;

   auto bc_inst = f.register_and_instantiate("blockchain", 50);
   auto sc_inst = f.register_and_instantiate("smart_contract", 51);

   name_id blockchain_api = 1000;
   auto bind_r = f.host.instances.bind(sc_inst, bc_inst, blockchain_api);
   REQUIRE(bind_r.has_value());

   f.host.set_caller(sc_inst);
   CHECK(f.host.identity.caller() == sc_inst);

   bool called = false;
   f.host.dispatch.on_call = [&called](instance_id target, name_id method,
                                       psi::runtime::bytes args)
      -> std::expected<psi::runtime::bytes, error>
   {
      called = true;
      return psi::runtime::bytes{0x00};
   };

   auto call_r = f.host.dispatch.call(sc_inst, 42, {});
   REQUIRE(call_r.has_value());
   CHECK(called);

   f.host.clear_caller();
   CHECK(f.host.identity.caller() == 0);

   f.host.instances.destroy(sc_inst);
   f.host.instances.destroy(bc_inst);
   CHECK(f.host.instance_map.empty());
}

TEST_CASE("RuntimeHost CallerGuard with dispatch", "[runtime_host]")
{
   runtime_fixture f;

   CHECK(f.host.identity.caller() == 0);

   {
      CallerGuard guard(&f.host.identity, 10);
      CHECK(f.host.identity.caller() == 10);
   }
   CHECK(f.host.identity.caller() == 0);
}
