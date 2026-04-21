#pragma once

/*
 * psi/runtime.hpp — psi runtime API, code-first (WIT derived by reflection).
 *
 * Three interface groups for the psiserve runtime:
 *
 *   psi:core/identity   — universally available instance identity
 *   psi:runtime/instances — instance lifecycle (trusted services only)
 *   psi:runtime/dispatch  — cross-instance calls (trusted services only)
 *
 * See services/blockchain-example/README.md § "psiserve Host API" for
 * the full design rationale. Untrusted contracts get only identity;
 * everything else is withheld unless explicitly bound.
 */

#include <cstdint>
#include <expected>
#include <vector>

#include <psio/reflect.hpp>

namespace psi::runtime
{

   // ── shared value types ────────────────────────────────────────────

   using instance_id = std::uint64_t;
   using name_id     = std::uint64_t;

   enum class error : std::uint8_t
   {
      not_found        = 0,
      invalid_handle   = 1,
      access_denied    = 2,
      already_exists   = 3,
      import_not_bound = 4,
      call_failed      = 5,
      invalid_arg      = 6,
   };

   enum class thread_hint : std::uint8_t
   {
      this_thread = 0,
      fresh       = 1,
   };

   // ── psi:core/identity ─────────────────────────────────────────────
   //
   // Every instance gets this — untrusted contracts included.
   // caller() returns the instance_id of the cross-instance caller
   // (0 if called by the host directly). self() returns this instance's
   // id. has_import() probes whether an interface is bound.

   struct identity
   {
      instance_id caller();
      instance_id self();
      bool        has_import(name_id iface);
   };
   PSIO_REFLECT(identity,
                method(caller),
                method(self),
                method(has_import, iface))

   // ── mem_budget ──────────────────────────────────────────────────────
   //
   // Power-of-2 exponent for sub-instance linear memory size.
   // The actual size is 1 << value bytes.

   enum class mem_budget : std::uint8_t
   {
      _64KB  = 16,
      _1MB   = 20,
      _4MB   = 22,
      _16MB  = 24,
      _64MB  = 26,
      _256MB = 28,
      _4GB   = 32,
   };

   using future_handle = std::uint32_t;

   // ── psi:runtime/instances ─────────────────────────────────────────
   //
   // Trusted services (blockchain.wasm, httpd.wasm) get this.
   // Untrusted contracts cannot launch instances.
   //
   // base_ptr: if non-zero, caller provides the child's linear memory
   // at this offset within the caller's own linear memory (caller-
   // owned). If zero, host allocates via alloc_child_memory exports
   // or the system arena.

   struct instances
   {
      std::expected<instance_id, error>
      instantiate(name_id module_name, thread_hint thread,
                  mem_budget budget, std::uint32_t base_ptr);

      std::expected<instance_id, error>
      instantiate_by_hash(std::vector<std::uint8_t> hash, thread_hint thread,
                          mem_budget budget, std::uint32_t base_ptr);

      std::expected<void, error>
      bind(instance_id consumer, instance_id provider, name_id iface);

      std::expected<void, error>
      destroy(instance_id target);

      bool running();

      name_id instance_name();
   };
   PSIO_REFLECT(instances,
                method(instantiate, module_name, thread, budget, base_ptr),
                method(instantiate_by_hash, hash, thread, budget, base_ptr),
                method(bind, consumer, provider, iface),
                method(destroy, target),
                method(running),
                method(instance_name))

   // ── psi:runtime/dispatch ──────────────────────────────────────────
   //
   // Unified cross-instance and intra-instance call primitives.
   //
   //   call       — synchronous, caller blocks until target returns
   //   post       — fire-and-forget, no return value
   //   async_call — returns a future handle, does not block.
   //                thread_hint controls scheduling.
   //                Subsumes psi_spawn: async_call(self, func, args,
   //                this_thread) is equivalent to spawning a fiber.
   //   await      — blocks the calling fiber until a future resolves

   using bytes = std::vector<std::uint8_t>;

   struct dispatch
   {
      std::expected<bytes, error>
      call(instance_id target, name_id method, bytes args);

      std::expected<void, error>
      post(instance_id target, name_id method, bytes args);

      std::expected<future_handle, error>
      async_call(instance_id target, name_id method, bytes args,
                 thread_hint thread);

      std::expected<bytes, error>
      await(future_handle future);
   };
   PSIO_REFLECT(dispatch,
                method(call, target, method, args),
                method(post, target, method, args),
                method(async_call, target, method, args, thread),
                method(await, future))

}  // namespace psi::runtime
