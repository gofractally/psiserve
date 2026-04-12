#pragma once

#include <psiber/fiber.hpp>
#include <psiber/spin_lock.hpp>

#include <atomic>

namespace psiber
{
   class Scheduler;

   /// Fiber-aware FIFO wait queue mutex.
   ///
   /// On contention, the requesting fiber is appended to a wait queue
   /// and parked.  On unlock, the next waiter is woken (either locally
   /// or cross-thread via the intrusive wake mechanism).
   ///
   /// The wait queue is protected by a spin_lock (nanosecond critical section).
   class fiber_mutex
   {
     public:
      /// Lock the mutex.  If contended, parks the current fiber.
      void lock(Scheduler& sched);

      /// Unlock the mutex.  If waiters are queued, wakes the next one.
      void unlock();

      /// Try to acquire without blocking.  Returns true on success.
      bool try_lock();

     private:
      std::atomic<Fiber*> _owner{nullptr};
      Fiber*              _wait_head = nullptr;
      Fiber*              _wait_tail = nullptr;
      spin_lock           _queue_lock;
   };

}  // namespace psiber
