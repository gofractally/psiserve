#pragma once

// runtime_host.hpp — Host-side implementation of the psi runtime APIs.
//
// Three host interfaces, matching the WIT imports in psi/runtime.hpp:
//
//   IdentityHost   — psi:core/identity    (universal)
//   InstancesHost  — psi:runtime/instances (trusted only)
//   DispatchHost   — psi:runtime/dispatch  (trusted only)
//
// Each is registered via PSIO_HOST_MODULE so psizam::runtime::provide<>
// can walk the interfaces automatically — no manual table.add<>().
//
// Thread safety: these are per-instance host objects (one per WASM
// instance), so no internal locking is needed. The RuntimeHost
// aggregate owns them all and is constructed per-instance.

#include <psi/runtime.hpp>
#include <psio/structural.hpp>
#include <psiserve/module_store.hpp>

#include <psiber/fiber_future.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/strand.hpp>

#include <psizam/handle_table.hpp>
#include <psizam/runtime.hpp>

#include <cstdint>
#include <cstring>
#include <expected>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace psiserve
{

   // ── Instance tracking ──────────────────────────────────────────────

   struct InstanceInfo
   {
      psi::runtime::instance_id  id;
      psi::runtime::name_id     name;
      psizam::instance           inst;
      std::vector<psi::runtime::name_id> bound_interfaces;

      // Each instance gets its own strand for serialized execution.
      // Fibers within the strand take turns; cross-strand calls use
      // fiber migration or promise-based dispatch.
      std::unique_ptr<psiber::strand> exec_strand;
   };

   // ── Future tracking ────────────────────────────────────────────────

   struct FutureInfo
   {
      bool                       resolved = false;
      psi::runtime::bytes        result;
      psi::runtime::error        err = psi::runtime::error::call_failed;
      bool                       has_error = false;

      // When wired to psiber, the promise is fulfilled by the spawned
      // fiber and the future is consumed by await. Null in unit tests
      // where resolution is done manually.
      std::shared_ptr<psiber::fiber_promise<psi::runtime::bytes>> promise;
   };

   // ── IdentityHost ──────────────────────────────────────────────────

   struct IdentityHost
   {
      psi::runtime::instance_id self_id   = 0;
      psi::runtime::instance_id caller_id = 0;

      std::vector<psi::runtime::name_id> bound_interfaces;

      psi::runtime::instance_id caller() { return caller_id; }

      psi::runtime::instance_id self() { return self_id; }

      bool has_import(psi::runtime::name_id iface)
      {
         for (auto n : bound_interfaces)
         {
            if (n == iface)
               return true;
         }
         return false;
      }
   };

   // ── CallerGuard ───────────────────────────────────────────────────
   //
   // RAII guard that sets caller_id before a cross-instance dispatch
   // and clears it on return. Wraps every call/async_call/post.

   struct CallerGuard
   {
      IdentityHost* target_identity;
      psi::runtime::instance_id prev_caller;

      CallerGuard(IdentityHost* target, psi::runtime::instance_id caller)
         : target_identity(target)
         , prev_caller(target ? target->caller_id : 0)
      {
         if (target)
            target->caller_id = caller;
      }

      ~CallerGuard()
      {
         if (target_identity)
            target_identity->caller_id = prev_caller;
      }

      CallerGuard(const CallerGuard&) = delete;
      CallerGuard& operator=(const CallerGuard&) = delete;
   };

   // ── InstancesHost ─────────────────────────────────────────────────

   struct InstancesHost
   {
      psizam::runtime*    rt       = nullptr;
      ModuleStore*        store    = nullptr;
      NameRegistry*       registry = nullptr;

      psi::runtime::instance_id  self_id = 0;
      psi::runtime::name_id      self_name = 0;
      bool                       is_running = true;

      std::unordered_map<psi::runtime::instance_id, InstanceInfo>* instance_map = nullptr;
      psi::runtime::instance_id* next_id = nullptr;

      // Prepared module handles keyed by module hash — avoids re-preparing
      // the same WASM bytes for each instantiation. Shared across the
      // spawn tree.
      std::unordered_map<ModuleHash, psizam::module_handle, ModuleHashHasher>*
         prepared_modules = nullptr;

      std::expected<psi::runtime::instance_id, psi::runtime::error>
      instantiate(psi::runtime::name_id module_name,
                  psi::runtime::thread_hint /*thread*/,
                  psi::runtime::mem_budget /*budget*/,
                  std::uint32_t /*base_ptr*/)
      {
         if (!registry || !store)
            return std::unexpected(psi::runtime::error::access_denied);

         auto hash = registry->resolve(std::to_string(module_name));
         if (!hash)
            return std::unexpected(psi::runtime::error::not_found);

         return instantiate_from_hash(*hash, module_name);
      }

      std::expected<psi::runtime::instance_id, psi::runtime::error>
      instantiate_by_hash(std::vector<std::uint8_t> hash,
                          psi::runtime::thread_hint /*thread*/,
                          psi::runtime::mem_budget /*budget*/,
                          std::uint32_t /*base_ptr*/)
      {
         if (!store)
            return std::unexpected(psi::runtime::error::access_denied);

         if (hash.size() != 32)
            return std::unexpected(psi::runtime::error::invalid_arg);

         ModuleHash mh;
         std::copy_n(hash.begin(), 32, mh.bytes.begin());

         return instantiate_from_hash(mh, 0);
      }

   private:
      std::expected<psi::runtime::instance_id, psi::runtime::error>
      instantiate_from_hash(const ModuleHash& mh, psi::runtime::name_id name)
      {
         auto cached_mod = store->lookup(mh);
         if (!cached_mod)
            return std::unexpected(psi::runtime::error::not_found);

         auto id = (*next_id)++;
         InstanceInfo info;
         info.id   = id;
         info.name = name;
         info.exec_strand = std::make_unique<psiber::strand>();

         // If psizam::runtime is available, prepare + instantiate a
         // real WASM instance from the cached module bytes.
         if (rt && cached_mod->sourceSize() > 0)
         {
            try
            {
               psizam::module_handle mod;

               if (prepared_modules)
               {
                  auto it = prepared_modules->find(mh);
                  if (it != prepared_modules->end())
                  {
                     mod = it->second;
                  }
                  else
                  {
                     auto bytes = cached_mod->bytes();
                     std::vector<uint8_t> wasm(
                        reinterpret_cast<const uint8_t*>(bytes.data()),
                        reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size());
                     psizam::instance_policy policy;
                     policy.initial  = psizam::instance_policy::compile_tier::interpret;
                     policy.metering = psizam::instance_policy::meter_mode::none;
                     mod = rt->prepare(psizam::wasm_bytes{wasm}, policy);
                     prepared_modules->emplace(mh, mod);
                  }
               }
               else
               {
                  auto bytes = cached_mod->bytes();
                  std::vector<uint8_t> wasm(
                     reinterpret_cast<const uint8_t*>(bytes.data()),
                     reinterpret_cast<const uint8_t*>(bytes.data()) + bytes.size());
                  psizam::instance_policy policy;
                  policy.initial  = psizam::instance_policy::compile_tier::interpret;
                  policy.metering = psizam::instance_policy::meter_mode::none;
                  mod = rt->prepare(psizam::wasm_bytes{wasm}, policy);
               }

               info.inst = rt->instantiate(mod);
            }
            catch (...)
            {
               return std::unexpected(psi::runtime::error::call_failed);
            }
         }

         if (instance_map)
            instance_map->emplace(id, std::move(info));

         return id;
      }

   public:

      std::expected<void, psi::runtime::error>
      bind(psi::runtime::instance_id consumer,
           psi::runtime::instance_id provider,
           psi::runtime::name_id iface)
      {
         if (!instance_map)
            return std::unexpected(psi::runtime::error::access_denied);

         auto cit = instance_map->find(consumer);
         if (cit == instance_map->end())
            return std::unexpected(psi::runtime::error::invalid_handle);

         auto pit = instance_map->find(provider);
         if (pit == instance_map->end())
            return std::unexpected(psi::runtime::error::invalid_handle);

         cit->second.bound_interfaces.push_back(iface);
         return {};
      }

      std::expected<void, psi::runtime::error>
      destroy(psi::runtime::instance_id target)
      {
         if (!instance_map)
            return std::unexpected(psi::runtime::error::access_denied);

         auto it = instance_map->find(target);
         if (it == instance_map->end())
            return std::unexpected(psi::runtime::error::not_found);

         instance_map->erase(it);
         return {};
      }

      bool running() { return is_running; }

      psi::runtime::name_id instance_name() { return self_name; }
   };

   // ── DispatchHost ──────────────────────────────────────────────────
   //
   // Unified cross-instance call routing. call() is synchronous,
   // async_call() returns a future handle, post() is fire-and-forget.
   // async_call subsumes psi_spawn.

   struct DispatchHost
   {
      std::unordered_map<psi::runtime::instance_id, InstanceInfo>* instance_map = nullptr;
      psiber::Scheduler* scheduler = nullptr;

      psizam::handle_table<FutureInfo, 256> futures{256};

      using call_fn_t = std::function<
         std::expected<psi::runtime::bytes, psi::runtime::error>(
            psi::runtime::instance_id target,
            psi::runtime::name_id method,
            psi::runtime::bytes args)>;

      using post_fn_t = std::function<
         std::expected<void, psi::runtime::error>(
            psi::runtime::instance_id target,
            psi::runtime::name_id method,
            psi::runtime::bytes args)>;

      using async_call_fn_t = std::function<
         std::expected<psi::runtime::future_handle, psi::runtime::error>(
            psi::runtime::instance_id target,
            psi::runtime::name_id method,
            psi::runtime::bytes args,
            psi::runtime::thread_hint thread,
            psizam::handle_table<FutureInfo, 256>& futures)>;

      call_fn_t       on_call;
      post_fn_t       on_post;
      async_call_fn_t on_async_call;

      std::expected<psi::runtime::bytes, psi::runtime::error>
      call(psi::runtime::instance_id target,
           psi::runtime::name_id method,
           psi::runtime::bytes args)
      {
         if (!instance_map)
            return std::unexpected(psi::runtime::error::access_denied);

         auto it = instance_map->find(target);
         if (it == instance_map->end())
            return std::unexpected(psi::runtime::error::invalid_handle);

         if (on_call)
            return on_call(target, method, std::move(args));

         return std::unexpected(psi::runtime::error::call_failed);
      }

      std::expected<void, psi::runtime::error>
      post(psi::runtime::instance_id target,
           psi::runtime::name_id method,
           psi::runtime::bytes args)
      {
         if (!instance_map)
            return std::unexpected(psi::runtime::error::access_denied);

         auto it = instance_map->find(target);
         if (it == instance_map->end())
            return std::unexpected(psi::runtime::error::invalid_handle);

         if (on_post)
            return on_post(target, method, std::move(args));

         return std::unexpected(psi::runtime::error::call_failed);
      }

      std::expected<psi::runtime::future_handle, psi::runtime::error>
      async_call(psi::runtime::instance_id target,
                 psi::runtime::name_id method,
                 psi::runtime::bytes args,
                 psi::runtime::thread_hint thread)
      {
         if (!instance_map)
            return std::unexpected(psi::runtime::error::access_denied);

         auto it = instance_map->find(target);
         if (it == instance_map->end())
            return std::unexpected(psi::runtime::error::invalid_handle);

         if (on_async_call)
            return on_async_call(target, method, std::move(args), thread, futures);

         // Create future with optional psiber promise
         FutureInfo fi;

         if (on_call)
         {
            auto promise = std::make_shared<psiber::fiber_promise<psi::runtime::bytes>>();
            fi.promise = promise;

            struct AsyncWork
            {
               std::shared_ptr<psiber::fiber_promise<psi::runtime::bytes>> promise;
               call_fn_t                    call_fn;
               psi::runtime::instance_id    target;
               psi::runtime::name_id        method;
               psi::runtime::bytes          args;
            };
            auto work = std::make_shared<AsyncWork>(AsyncWork{
               promise, on_call, target, method, std::move(args)});

            auto run_fn = [work]()
            {
               try
               {
                  auto result = work->call_fn(
                     work->target, work->method, std::move(work->args));
                  if (result)
                     work->promise->set_value(std::move(*result));
                  else
                     work->promise->set_value(psi::runtime::bytes{});
               }
               catch (...)
               {
                  work->promise->set_exception(std::current_exception());
               }
            };

            // Route to the target instance's strand. For fresh
            // instances the strand was created at instantiate time.
            // For this_thread, the target shares the caller's strand
            // (or has its own — same effect, serialized execution).
            if (it->second.exec_strand)
            {
               it->second.exec_strand->post(std::move(run_fn));
            }
            else if (scheduler)
            {
               scheduler->spawnFiber(std::move(run_fn), "async_call");
            }
         }

         auto fh = futures.create(std::move(fi));
         if (fh == psizam::handle_table<FutureInfo, 256>::invalid_handle)
            return std::unexpected(psi::runtime::error::access_denied);

         return fh;
      }

      std::expected<psi::runtime::bytes, psi::runtime::error>
      await(psi::runtime::future_handle future)
      {
         auto* f = futures.get(future);
         if (!f)
            return std::unexpected(psi::runtime::error::invalid_handle);

         // If wired to psiber, block the calling fiber until the
         // promise is fulfilled. fiber_future::get() parks the fiber
         // via the scheduler and wakes it when the result is ready.
         if (f->promise)
         {
            try
            {
               psiber::fiber_future<psi::runtime::bytes> fut(f->promise);
               auto result = fut.get();
               futures.destroy(future);
               return result;
            }
            catch (...)
            {
               futures.destroy(future);
               return std::unexpected(psi::runtime::error::call_failed);
            }
         }

         // Mock path (unit tests): check manual resolution
         if (!f->resolved)
            return std::unexpected(psi::runtime::error::call_failed);

         if (f->has_error)
         {
            auto err = f->err;
            futures.destroy(future);
            return std::unexpected(err);
         }

         auto result = std::move(f->result);
         futures.destroy(future);
         return result;
      }
   };

   // ── RuntimeHost ───────────────────────────────────────────────────

   struct RuntimeHost
   {
      IdentityHost  identity;
      InstancesHost instances;
      DispatchHost  dispatch;

      std::unordered_map<psi::runtime::instance_id, InstanceInfo> instance_map;
      std::unordered_map<ModuleHash, psizam::module_handle, ModuleHashHasher> prepared_modules;
      psi::runtime::instance_id next_id = 1;

      void init(psi::runtime::instance_id self_id,
                psi::runtime::name_id self_name,
                psizam::runtime* rt = nullptr,
                ModuleStore* store = nullptr,
                NameRegistry* registry = nullptr,
                psiber::Scheduler* sched = nullptr)
      {
         identity.self_id   = self_id;
         identity.caller_id = 0;

         instances.self_id   = self_id;
         instances.self_name = self_name;
         instances.rt        = rt;
         instances.store     = store;
         instances.registry  = registry;
         instances.instance_map  = &instance_map;
         instances.next_id       = &next_id;
         instances.prepared_modules = &prepared_modules;

         dispatch.instance_map = &instance_map;
         dispatch.scheduler    = sched;
      }

      void set_caller(psi::runtime::instance_id caller)
      {
         identity.caller_id = caller;
      }

      void clear_caller()
      {
         identity.caller_id = 0;
      }

      // Wire the default dispatch callbacks that route calls through
      // instance_be::call_export_canonical. Call this after init()
      // when psizam instances are available.
      void wire_dispatch()
      {
         dispatch.on_call = [this](
            psi::runtime::instance_id target,
            psi::runtime::name_id method,
            psi::runtime::bytes args)
            -> std::expected<psi::runtime::bytes, psi::runtime::error>
         {
            auto it = instance_map.find(target);
            if (it == instance_map.end())
               return std::unexpected(psi::runtime::error::invalid_handle);

            auto* be = it->second.inst.get_instance_be();
            if (!be)
               return std::unexpected(psi::runtime::error::call_failed);

            // The actual call, executed on the target's strand.
            auto do_call = [&]() -> std::expected<psi::runtime::bytes, psi::runtime::error>
            {
               CallerGuard guard(&identity, identity.self_id);

               auto method_str = std::to_string(method);

               uint64_t slots[16] = {};
               if (!args.empty())
                  slots[0] = static_cast<uint64_t>(args.size());

               auto r = be->call_export_canonical(
                  it->second.inst.host_ptr(), method_str, slots, 1);

               if (r)
               {
                  psi::runtime::bytes result;
                  uint64_t rv = r->to_ui64();
                  result.resize(sizeof(rv));
                  std::memcpy(result.data(), &rv, sizeof(rv));
                  return result;
               }
               return psi::runtime::bytes{};
            };

            // If the target has a strand and we have a scheduler,
            // post the work to the target's strand and wait for the
            // result. This serializes the call with the target's
            // other fibers (the strand guarantee).
            //
            // TODO: fiber migration — the calling fiber could
            // temporarily join the target's strand instead of
            // creating a promise+post. This eliminates the promise
            // overhead for synchronous cross-strand calls.
            if (it->second.exec_strand && dispatch.scheduler)
            {
               psiber::fiber_promise<std::expected<psi::runtime::bytes, psi::runtime::error>> promise;

               it->second.exec_strand->post([&]() {
                  promise.set_value(do_call());
               });

               auto& sched = psiber::Scheduler::current();
               auto* fiber = sched.currentFiber();
               if (fiber && promise.try_register_waiter(fiber))
                  sched.parkCurrentFiber();

               return promise.get();
            }

            // Direct path (no strand, no scheduler — unit tests)
            return do_call();
         };
      }
   };

}  // namespace psiserve

// ── interface_info specializations (WIT module names) ────────────────

namespace psio::detail
{
   template <>
   struct interface_info<psi::runtime::identity>
   {
      static constexpr ::psio::FixedString name = "psi:core/identity";
   };
   template <>
   struct interface_info<psi::runtime::instances>
   {
      static constexpr ::psio::FixedString name = "psi:runtime/instances";
   };
   template <>
   struct interface_info<psi::runtime::dispatch>
   {
      static constexpr ::psio::FixedString name = "psi:runtime/dispatch";
   };
}  // namespace psio::detail

// ── Host module registration ────────────────────────────────────────

PSIO_HOST_MODULE(psiserve::IdentityHost,
   interface(psi::runtime::identity,
             caller, self, has_import))

PSIO_HOST_MODULE(psiserve::InstancesHost,
   interface(psi::runtime::instances,
             instantiate, instantiate_by_hash, bind,
             destroy, running, instance_name))

PSIO_HOST_MODULE(psiserve::DispatchHost,
   interface(psi::runtime::dispatch,
             call, post, async_call, await))
