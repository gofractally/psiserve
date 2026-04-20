#pragma once

#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_shared_mutex.hpp>
#include <psiber/scheduler.hpp>

#include <cassert>

namespace psiserve
{
   /// Stdlib-BasicLockable wrapper around psiber::fiber_mutex.
   ///
   /// psiber's fiber_mutex takes an explicit Scheduler& on lock() so the
   /// wait-queue can wake fibers without a TLS lookup. psitri uses
   /// std::lock_guard / std::unique_lock, which call lock() with no args.
   /// This adapter plugs the gap by routing through Scheduler::current().
   ///
   /// Precondition for lock()/lock_shared(): the calling OS thread is
   /// currently inside a psiber scheduler run loop. Holding the lock
   /// across fiber yields is supported; holding it across thread hops
   /// is not (same as psiber's own contract).
   class fiber_mutex
   {
     public:
      void lock()
      {
         _m.lock(psiber::Scheduler::current());
      }

      void unlock() { _m.unlock(); }

      bool try_lock() { return _m.try_lock(); }

     private:
      psiber::fiber_mutex _m;
   };

   /// Stdlib-SharedLockable wrapper around psiber::fiber_shared_mutex.
   /// Same scheduler-current contract as fiber_mutex above.
   class fiber_shared_mutex
   {
     public:
      void lock()
      {
         _m.lock(psiber::Scheduler::current());
      }

      void unlock() { _m.unlock(); }

      bool try_lock() { return _m.try_lock(); }

      void lock_shared()
      {
         _m.lock_shared(psiber::Scheduler::current());
      }

      void unlock_shared() { _m.unlock_shared(); }

      bool try_lock_shared() { return _m.try_lock_shared(); }

     private:
      psiber::fiber_shared_mutex _m;
   };

   /// Lock policy for psitri::basic_database and psitri::dwal::basic_dwal_database
   /// that yields fibers on contention instead of blocking the OS thread.
   ///
   /// Usage:
   /// @code
   ///   psitri::basic_database<psiserve::fiber_lock_policy> db{...};
   ///   psitri::dwal::basic_dwal_database<psiserve::fiber_lock_policy> dwal{...};
   /// @endcode
   struct fiber_lock_policy
   {
      using mutex_type        = fiber_mutex;
      using shared_mutex_type = fiber_shared_mutex;
   };

}  // namespace psiserve
