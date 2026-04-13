#pragma once

#include <atomic>
#include <cstdint>

namespace psiber::detail
{
   /// Atomic counter with a configurable upper bound.
   ///
   /// ## How it works
   ///
   /// Tracks a count of outstanding items (e.g., heap-allocated work
   /// items) against a configurable maximum.  The pattern is:
   ///
   ///   1. **try_increment():** `fetch_add(1)` speculatively increments
   ///      the counter, then checks the *previous* value against the
   ///      limit.  If `prev >= max`, the increment is rolled back with
   ///      `fetch_sub(1)` and the call returns false.
   ///
   ///   2. **decrement():** `fetch_sub(1)` unconditionally decrements.
   ///      The caller must guarantee a matching try_increment() succeeded.
   ///
   /// ## Why speculative increment + rollback?
   ///
   /// The alternative — load, compare, then CAS — requires a CAS loop
   /// and is slower under contention.  fetch_add is a single atomic RMW
   /// that never spuriously fails.  The rollback on limit-exceeded adds
   /// one extra fetch_sub on the overflow path (cold), saving a CAS
   /// retry loop on the hot path.
   ///
   /// ## Why `relaxed` ordering is sufficient
   ///
   /// The counter doesn't protect any shared data — it's purely a
   /// capacity gate.  The actual data synchronization happens elsewhere
   /// (the MPSC work list uses release/acquire).  Using `relaxed` here
   /// avoids unnecessary memory barriers on the hot allocation path.
   ///
   /// ## Invariant
   ///
   /// The counter value equals the number of live items.  Every
   /// successful try_increment() has exactly one matching decrement().
   /// The counter never goes below zero (caller guarantee) and never
   /// exceeds `max` for more than the brief window between fetch_add
   /// and fetch_sub in the rollback path (other threads may see a
   /// transiently over-limit value, but they'll also fail their own
   /// try_increment — no over-allocation occurs).
   ///
   /// Usage:
   /// ```
   ///   bounded_counter heap_count(256);
   ///   if (heap_count.try_increment()) {
   ///       auto* p = new Item;
   ///       // ... use p ...
   ///       delete p;
   ///       heap_count.decrement();
   ///   }
   /// ```
   class bounded_counter
   {
     public:
      explicit bounded_counter(uint32_t max = 256) noexcept
         : _max(max) {}

      /// Try to increment the counter.  Returns true if the count
      /// was below the limit (item allocated), false if at capacity.
      bool try_increment() noexcept
      {
         uint32_t prev = _count.fetch_add(1, std::memory_order_relaxed);
         if (prev < _max)
            return true;
         _count.fetch_sub(1, std::memory_order_relaxed);
         return false;
      }

      /// Decrement the counter.  Caller must guarantee a prior
      /// successful try_increment().
      void decrement() noexcept
      {
         _count.fetch_sub(1, std::memory_order_relaxed);
      }

      /// Current count (snapshot — may be stale).
      uint32_t count() const noexcept
      {
         return _count.load(std::memory_order_relaxed);
      }

      /// Change the upper bound.  Not thread-safe against concurrent
      /// try_increment() — call during setup or quiescent periods.
      void set_max(uint32_t max) noexcept { _max = max; }

      /// Current upper bound.
      uint32_t max() const noexcept { return _max; }

     private:
      std::atomic<uint32_t> _count{0};
      uint32_t              _max;
   };

}  // namespace psiber::detail
