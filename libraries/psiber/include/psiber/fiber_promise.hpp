#pragma once

#include <psiber/fiber.hpp>

#include <atomic>
#include <optional>

namespace psiber
{
   class Scheduler;

   /// Zero-allocation return value for cross-thread fiber communication.
   ///
   /// Typically lives on the sender's stack (which is stable because
   /// the sender is parked).  The receiver writes the value directly
   /// into the promise and wakes the sender via the intrusive wake list.
   ///
   /// Usage:
   ///   fiber_promise<int> promise;
   ///   promise.waiting_fiber = sched.currentFiber();
   ///   // ... dispatch work to another thread ...
   ///   sched.parkCurrentFiber();
   ///   int result = promise.get();
   template <typename T>
   class fiber_promise
   {
     public:
      /// Set the value (called by the fulfilling side, possibly another thread).
      void set_value(T value)
      {
         _storage.emplace(static_cast<T&&>(value));
         _ready.store(true, std::memory_order_release);
      }

      /// Get the value (called by the waiting side after being woken).
      T get()
      {
         return static_cast<T&&>(*_storage);
      }

      bool is_ready() const { return _ready.load(std::memory_order_acquire); }

      /// The fiber waiting for this promise.  Set before parking.
      Fiber* waiting_fiber = nullptr;

     private:
      std::optional<T>  _storage;
      std::atomic<bool> _ready{false};
   };

   /// Specialization for void — signal-only, no value storage.
   template <>
   class fiber_promise<void>
   {
     public:
      void set_value()
      {
         _ready.store(true, std::memory_order_release);
      }

      void get() {}

      bool is_ready() const { return _ready.load(std::memory_order_acquire); }

      Fiber* waiting_fiber = nullptr;

     private:
      std::atomic<bool> _ready{false};
   };

}  // namespace psiber
