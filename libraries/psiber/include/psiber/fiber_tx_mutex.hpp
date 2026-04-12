#pragma once

#include <psiber/fiber.hpp>
#include <psiber/spin_lock.hpp>

#include <atomic>
#include <cstdint>

namespace psiber
{
   class Scheduler;

   /// Exception thrown when a transaction is wounded by an older transaction.
   struct wound_exception {};

   /// Fiber-aware wound-wait transaction mutex.
   ///
   /// Uses wound-wait deadlock prevention:
   /// - Each transaction has a timestamp (lower = older = higher priority).
   /// - On contention: older requester wounds (aborts) younger holder.
   /// - Younger requester waits for older holder.
   /// - No deadlock possible: circular wait requires a cycle of
   ///   "younger waits for older", which is impossible.
   class fiber_tx_mutex
   {
     public:
      /// Lock with transaction ordering.  May wound the current holder
      /// if the requester has a lower (older) timestamp.
      void lock(Scheduler& sched, uint64_t tx_timestamp);

      /// Unlock the mutex, wake next waiter.
      void unlock();

      /// Try to acquire without blocking.
      bool try_lock(uint64_t tx_timestamp);

      /// Timestamp of the current holder (0 if unlocked).
      uint64_t holder_timestamp() const { return _owner_timestamp; }

      /// Check if the current holder's fiber has been wounded.
      /// Call this at yield points or before commit.  If true,
      /// the transaction should abort (throw wound_exception) and
      /// release the lock.
      static void check_wounded(Fiber* fiber)
      {
         if (fiber->wounded.load(std::memory_order_acquire))
            throw wound_exception{};
      }

     private:
      Fiber*    _owner           = nullptr;
      uint64_t  _owner_timestamp = 0;
      Fiber*    _wait_head       = nullptr;
      Fiber*    _wait_tail       = nullptr;
      spin_lock _queue_lock;
   };

}  // namespace psiber
