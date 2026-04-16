#pragma once

#include <psiber/fiber.hpp>

#include <atomic>
#include <cassert>
#include <cstdio>
#include <exception>
#include <optional>

namespace psiber
{

   /// Wake a fiber via the scheduler.  Defined in scheduler.cpp (not
   /// scheduler.hpp) to break a circular include — fiber_promise needs
   /// to wake fibers, but scheduler.hpp includes fiber_promise.hpp.
   void wake_fiber(Fiber* f);

   /// Base class for the CAS-based producer/consumer coordination
   /// protocol shared by fiber_promise<T> and fiber_promise<void>.
   ///
   /// Protocol:
   ///   _waiter is initially nullptr.
   ///   Consumer: CAS(nullptr -> fiber_ptr).  If CAS fails (kFulfilled),
   ///             value is already ready — skip park.
   ///   Producer: CAS(nullptr -> kFulfilled).  If CAS fails (fiber_ptr),
   ///             a waiter is present — call wake.
   ///
   /// This ensures exactly one side "wins" the nullptr slot:
   ///   - Consumer wins -> parks, producer wakes
   ///   - Producer wins -> consumer sees kFulfilled, skips park
   ///
   /// Memory ordering: the producer's acq_rel CAS in fulfill_and_notify()
   /// releases all prior writes (_storage, _exception).  The consumer's
   /// acquire (either CAS failure in try_register_waiter or load in
   /// is_ready) synchronizes with that release, making the value visible.
   class fiber_promise_base
   {
     public:
      fiber_promise_base() = default;

      fiber_promise_base(const fiber_promise_base&)            = delete;
      fiber_promise_base& operator=(const fiber_promise_base&) = delete;

      /// Move construction.  Only valid before concurrent access begins
      /// (before any thread calls set_value/try_register_waiter).
      fiber_promise_base(fiber_promise_base&& other) noexcept
         : _exception(std::move(other._exception))
         , _waiter(other._waiter.load(std::memory_order_relaxed))
         , _retrieved(other._retrieved)
      {
         other._waiter.store(nullptr, std::memory_order_relaxed);
         other._exception = nullptr;
         other._retrieved = true;  // prevent destructor assertion on source
      }

      fiber_promise_base& operator=(fiber_promise_base&& other) noexcept
      {
         if (this != &other)
         {
            _exception = std::move(other._exception);
            _waiter.store(other._waiter.load(std::memory_order_relaxed),
                          std::memory_order_relaxed);
            _retrieved = other._retrieved;
            other._waiter.store(nullptr, std::memory_order_relaxed);
            other._exception = nullptr;
            other._retrieved = true;
         }
         return *this;
      }

      ~fiber_promise_base()
      {
         if (_exception && !_retrieved)
            std::fprintf(stderr,
               "psiber: fiber_promise destroyed with unchecked exception\n");
         assert(!(_exception && !_retrieved) &&
                "fiber_promise destroyed with unchecked exception");
      }

      /// Capture an exception and notify any waiting fiber.
      void set_exception(std::exception_ptr eptr)
      {
         _exception = eptr;
         fulfill_and_notify();
      }

      /// Check whether the producer has fulfilled this promise.
      bool is_ready() const
      {
         return _waiter.load(std::memory_order_acquire) == kFulfilled;
      }

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

     protected:
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

      /// Rethrow stored exception (if any).  Marks retrieved.
      void rethrow_if_exception()
      {
         _retrieved = true;
         if (_exception)
            std::rethrow_exception(_exception);
      }

      static inline Fiber* kFulfilled =
         reinterpret_cast<Fiber*>(static_cast<uintptr_t>(1));

      std::exception_ptr  _exception;
      std::atomic<Fiber*> _waiter{nullptr};
      bool                _retrieved{false};
   };

   /// Zero-allocation return value for cross-thread fiber communication.
   ///
   /// Typically lives on the sender's fiber stack (stable while parked).
   /// The producer writes the value and notifies via the CAS protocol
   /// inherited from fiber_promise_base.
   template <typename T>
   class fiber_promise : public fiber_promise_base
   {
     public:
      fiber_promise() = default;
      fiber_promise(fiber_promise&&) noexcept = default;
      fiber_promise& operator=(fiber_promise&&) noexcept = default;

      /// Set the value and notify any waiting fiber.
      /// Called by the fulfilling side, possibly from another thread.
      void set_value(T value)
      {
         _storage.emplace(static_cast<T&&>(value));
         fulfill_and_notify();
      }

      /// Get the value (called by the waiting side after being woken).
      /// Rethrows if the fulfilling side called set_exception().
      T get()
      {
         rethrow_if_exception();
         return static_cast<T&&>(*_storage);
      }

     private:
      std::optional<T> _storage;
   };

   /// Specialization for void — signal-only, no value storage.
   template <>
   class fiber_promise<void> : public fiber_promise_base
   {
     public:
      fiber_promise() = default;
      fiber_promise(fiber_promise&&) noexcept = default;
      fiber_promise& operator=(fiber_promise&&) noexcept = default;

      void set_value() { fulfill_and_notify(); }
      void get() { rethrow_if_exception(); }
   };

}  // namespace psiber
