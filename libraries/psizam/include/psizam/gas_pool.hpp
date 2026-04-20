#pragma once

// psizam/gas_pool.hpp — multi-module gas tracking.
//
// A `gas_pool` is a runtime-level ledger of total gas available across a
// call chain. Multiple instances — consumer, provider, further-transitive
// callees — attach to the same pool via `instance_policy::pool`. Each
// instance still carries its own plain `gas_state` (consumed + atomic
// deadline) for the zero-indirection hot path; the pool is only touched
// at lease boundaries (handler entry and instance teardown), not at
// per-charge sites.
//
// Flow:
//   create_gas_pool(budget)
//     pool.remaining = budget
//   instantiate(module, policy with pool)
//     lease `lease_size` from pool → instance._gas.deadline
//     attach pool_yield_handler, user_data=&pool
//   ... execution ...
//     consumed >= deadline → handler fires
//       debit another `lease_size` from pool.remaining
//       deadline += lease_size  (handler advances it)
//       or if pool exhausted → trap
//   instance destructor
//     credit (_gas.deadline - _gas.consumed) back to pool.remaining
//
// Multi-module sharing: when consumer calls provider through a bind
// bridge, both instances are attached to the same pool. Provider's
// per-charge hot path debits its local deadline; when that runs out,
// provider's handler leases from the shared pool. Consumer's subsequent
// execution leases from the same shared pool. Net effect: the pool's
// `remaining` counter reflects total gas consumed across the entire
// call chain, regardless of which instance was executing when.
//
// The pool's atomic counter makes cross-thread lease/credit safe.

#include <psizam/exceptions.hpp>
#include <psizam/gas.hpp>

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>

namespace psizam {

struct gas_pool {
   // Remaining gas in the pool. Debited on lease, credited on
   // instance teardown (unused lease balance).
   std::atomic<uint64_t> remaining{0};

   // How much gas to lease to an instance on each handler invocation.
   // Larger = fewer atomic ops, coarser fairness across concurrent
   // instances. Smaller = tighter sharing, more atomics.
   gas_units lease_size;

   explicit gas_pool(gas_units budget, gas_units lease = gas_units{10'000}) noexcept
      : remaining(*budget), lease_size(lease) {}

   gas_pool(const gas_pool&)            = delete;
   gas_pool& operator=(const gas_pool&) = delete;

   // Read current balance. Relaxed — callers use this for reporting or
   // coarse decision-making, never for correctness invariants.
   gas_units balance() const noexcept {
      return gas_units{remaining.load(std::memory_order_relaxed)};
   }

   // Refill the pool from the host side (e.g. on transaction start for
   // a re-usable pool).
   void add(gas_units delta) noexcept {
      remaining.fetch_add(*delta, std::memory_order_relaxed);
   }

   // Try to lease exactly `amount` from the pool. Returns the amount
   // actually leased: `amount` on success, 0 on exhaustion (pool is
   // left unchanged on failure).
   gas_units try_lease(gas_units amount) noexcept {
      const uint64_t want = *amount;
      uint64_t prev = remaining.fetch_sub(want, std::memory_order_relaxed);
      if (prev >= want) return amount;
      remaining.fetch_add(want, std::memory_order_relaxed);
      return gas_units{0};
   }
};

// Gas handler that leases additional gas from a pool each time an
// instance's deadline is hit. `user_data` must point at a live
// `gas_pool`; callers wire this via `instance_be::set_gas_handler(
//   &pool_yield_handler, pool_ptr)` at instantiate time.
//
// On lease success: advances `gas->deadline` by `pool->lease_size` and
// returns. The instance's next charge continues against the extended
// deadline.
//
// On lease failure (pool exhausted): throws the standard
// `wasm_gas_exhausted_exception` so the JIT trap path unwinds the
// running backend cleanly.
inline void pool_yield_handler(gas_state* gas, void* user_data)
{
   auto* pool  = static_cast<gas_pool*>(user_data);
   auto  lease = pool->try_lease(pool->lease_size);
   if (*lease == 0) {
      // Pool exhausted. The instance's consumed already passed deadline
      // — no further slack available, trap.
      throw wasm_gas_exhausted_exception{"gas_pool: exhausted"};
   }
   // Success — extend the deadline so the caller can continue executing.
   gas->deadline.fetch_add(*lease, std::memory_order_relaxed);
}

} // namespace psizam
