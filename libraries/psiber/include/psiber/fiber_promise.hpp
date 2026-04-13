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

   /// Wake a fiber via the scheduler.  Defined in scheduler.hpp (which
   /// is included after this header), so we declare it here and rely on
   /// the linker.  This avoids a circular include.
   void wake_fiber(Fiber* f);

   /// Zero-allocation return value for cross-thread fiber communication.
   ///
   /// Uses a CAS-based coordination protocol between consumer (get())
   /// and producer (set_value / set_exception) to avoid the classic race
   /// where a wake is sent to a fiber that hasn't parked yet, or a park
   /// happens after the value is ready but before the wake fires.
   ///
   /// Protocol:
   ///   _waiter is initially nullptr.
   ///   Consumer: CAS(nullptr → fiber_ptr).  If CAS fails (kFulfilled),
   ///             value is already ready — skip park.
   ///   Producer: CAS(nullptr → kFulfilled).  If CAS fails (fiber_ptr),
   ///             a waiter is present — call wake.
   ///
   /// This ensures exactly one side "wins" the nullptr slot:
   ///   - Consumer wins → parks, producer wakes
   ///   - Producer wins → consumer sees kFulfilled, skips park
   template <typename T>
   class fiber_promise
   {
     public:
      ~fiber_promise()
      {
         if (_exception && !_retrieved)
            std::fprintf(stderr,
               "psiber: fiber_promise<%s> destroyed with unchecked exception\n",
               typeid(T).name());
         assert(!(_exception && !_retrieved) &&
                "fiber_promise destroyed with unchecked exception");
      }

      fiber_promise() = default;
      fiber_promise(const fiber_promise&) = delete;
      fiber_promise& operator=(const fiber_promise&) = delete;

      /// Set the value and notify any waiting fiber.
      /// Called by the fulfilling side, possibly from another thread.
      void set_value(T value)
      {
         _storage.emplace(static_cast<T&&>(value));
         _ready.store(true, std::memory_order_release);
         fulfill_and_notify();
      }

      /// Capture an exception and notify any waiting fiber.
      void set_exception(std::exception_ptr eptr)
      {
         _exception = eptr;
         _ready.store(true, std::memory_order_release);
         fulfill_and_notify();
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

      /// Try to register a waiter.  Returns true if the fiber should
      /// park (producer hasn't fulfilled yet).  Returns false if the
      /// value is already ready (producer won the race).
      bool try_register_waiter(Fiber* fiber)
      {
         Fiber* expected = nullptr;
         return _waiter.compare_exchange_strong(
            expected, fiber,
            std::memory_order_acq_rel, std::memory_order_acquire);
      }

     private:
      void fulfill_and_notify()
      {
         // Try to claim the slot with kFulfilled sentinel.
         // If CAS succeeds: no waiter registered yet — done.
         // If CAS fails: a real fiber pointer is there — wake it.
         Fiber* expected = nullptr;
         if (!_waiter.compare_exchange_strong(
               expected, kFulfilled,
               std::memory_order_acq_rel, std::memory_order_acquire))
         {
            // expected now holds the waiter's fiber pointer
            wake_fiber(expected);
         }
      }

      static inline Fiber* kFulfilled =
         reinterpret_cast<Fiber*>(static_cast<uintptr_t>(1));

      std::optional<T>        _storage;
      std::exception_ptr      _exception;
      std::atomic<bool>       _ready{false};
      std::atomic<Fiber*>     _waiter{nullptr};
      bool                    _retrieved{false};
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
               "psiber: fiber_promise<void> destroyed with unchecked exception\n");
         assert(!(_exception && !_retrieved) &&
                "fiber_promise destroyed with unchecked exception");
      }

      fiber_promise() = default;
      fiber_promise(const fiber_promise&) = delete;
      fiber_promise& operator=(const fiber_promise&) = delete;

      void set_value()
      {
         _ready.store(true, std::memory_order_release);
         fulfill_and_notify();
      }

      void set_exception(std::exception_ptr eptr)
      {
         _exception = eptr;
         _ready.store(true, std::memory_order_release);
         fulfill_and_notify();
      }

      void get()
      {
         _retrieved = true;
         if (_exception)
            std::rethrow_exception(_exception);
      }

      bool is_ready() const { return _ready.load(std::memory_order_acquire); }

      bool try_register_waiter(Fiber* fiber)
      {
         Fiber* expected = nullptr;
         return _waiter.compare_exchange_strong(
            expected, fiber,
            std::memory_order_acq_rel, std::memory_order_acquire);
      }

     private:
      void fulfill_and_notify()
      {
         Fiber* expected = nullptr;
         if (!_waiter.compare_exchange_strong(
               expected, kFulfilled,
               std::memory_order_acq_rel, std::memory_order_acquire))
         {
            wake_fiber(expected);
         }
      }

      static inline Fiber* kFulfilled =
         reinterpret_cast<Fiber*>(static_cast<uintptr_t>(1));

      std::exception_ptr    _exception;
      std::atomic<bool>     _ready{false};
      std::atomic<Fiber*>   _waiter{nullptr};
      bool                  _retrieved{false};
   };

}  // namespace psiber
