#pragma once

#include <psiber/fiber.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <exception>
#include <optional>
#include <typeinfo>

namespace psiber
{

   /// Zero-allocation return value for cross-thread fiber communication.
   ///
   /// Typically lives on the sender's stack (which is stable because
   /// the sender is parked).  The receiver writes the value directly
   /// into the promise and wakes the sender via the intrusive wake list.
   ///
   /// If the fulfilling side throws, the exception is captured and
   /// rethrown on get() — same semantics as std::promise/std::future.
   ///
   /// Debug safety:
   /// - Destructor asserts if an exception was stored but never retrieved
   /// - Destructor warns if the promise was never fulfilled (abandoned)
   ///
   /// Usage:
   ///   fiber_promise<int> promise;
   ///   promise.waiting_fiber = sched.currentFiber();
   ///   // ... dispatch work to another thread ...
   ///   sched.parkCurrentFiber();
   ///   int result = promise.get();  // rethrows if remote side threw
   template <typename T>
   class fiber_promise
   {
     public:
      ~fiber_promise()
      {
         if (_exception && !_retrieved)
            std::fprintf(stderr,
               "psiber: fiber_promise<%s> destroyed with unchecked exception "
               "(fiber %u)\n", typeid(T).name(),
               waiting_fiber ? waiting_fiber->id : 0);
         assert(!(_exception && !_retrieved) &&
                "fiber_promise destroyed with unchecked exception");
      }

      fiber_promise() = default;
      fiber_promise(const fiber_promise&) = delete;
      fiber_promise& operator=(const fiber_promise&) = delete;

      /// Set the value (called by the fulfilling side, possibly another thread).
      void set_value(T value)
      {
         _storage.emplace(static_cast<T&&>(value));
         _ready.store(true, std::memory_order_release);
      }

      /// Capture an exception to be rethrown on get().
      void set_exception(std::exception_ptr eptr)
      {
         _exception = eptr;
         _ready.store(true, std::memory_order_release);
      }

      /// Get the value (called by the waiting side after being woken).
      /// Rethrows if the fulfilling side called set_exception().
      T get()
      {
         _retrieved = true;
         if (_exception)
            std::rethrow_exception(_exception);
         return static_cast<T&&>(*_storage);
      }

      bool is_ready() const { return _ready.load(std::memory_order_acquire); }

      /// The fiber waiting for this promise.  Set before parking.
      Fiber* waiting_fiber = nullptr;

     private:
      std::optional<T>      _storage;
      std::exception_ptr    _exception;
      std::atomic<bool>     _ready{false};
      bool                  _retrieved{false};
   };

   /// Specialization for void — signal-only, no value storage.
   template <>
   class fiber_promise<void>
   {
     public:
      ~fiber_promise()
      {
         if (_exception && !_retrieved)
            std::fprintf(stderr,
               "psiber: fiber_promise<void> destroyed with unchecked exception "
               "(fiber %u)\n",
               waiting_fiber ? waiting_fiber->id : 0);
         assert(!(_exception && !_retrieved) &&
                "fiber_promise destroyed with unchecked exception");
      }

      fiber_promise() = default;
      fiber_promise(const fiber_promise&) = delete;
      fiber_promise& operator=(const fiber_promise&) = delete;

      void set_value()
      {
         _ready.store(true, std::memory_order_release);
      }

      void set_exception(std::exception_ptr eptr)
      {
         _exception = eptr;
         _ready.store(true, std::memory_order_release);
      }

      void get()
      {
         _retrieved = true;
         if (_exception)
            std::rethrow_exception(_exception);
      }

      bool is_ready() const { return _ready.load(std::memory_order_acquire); }

      Fiber* waiting_fiber = nullptr;

     private:
      std::exception_ptr _exception;
      std::atomic<bool>  _ready{false};
      bool               _retrieved{false};
   };

}  // namespace psiber
