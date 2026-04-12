#pragma once

#include <psiber/fiber_promise.hpp>
#include <psiber/scheduler.hpp>

#include <cassert>
#include <memory>

namespace psiber
{
   /// A fiber-aware future returned by thread::async().
   ///
   /// Wraps a shared promise that is fulfilled by a fiber on another
   /// thread.  Calling get() parks the current fiber until the result
   /// is ready (or returns immediately if already fulfilled).
   ///
   /// Unlike std::future, get() is fiber-aware — it parks instead of
   /// blocking the OS thread.
   template <typename T>
   class fiber_future
   {
     public:
      fiber_future() = default;
      explicit fiber_future(std::shared_ptr<fiber_promise<T>> p) : _promise(std::move(p)) {}

      /// Get the result, parking the current fiber if not ready.
      /// Rethrows if the remote side threw.  Must be called from
      /// a fiber context.
      T get()
      {
         assert(_promise && "fiber_future::get() called on empty future");
         if (!_promise->is_ready())
         {
            Scheduler* sched = Scheduler::current();
            assert(sched && sched->currentFiber());
            _promise->waiting_fiber = sched->currentFiber();
            sched->parkCurrentFiber();
         }
         if constexpr (std::is_void_v<T>)
            _promise->get();
         else
            return _promise->get();
      }

      bool is_ready() const { return _promise && _promise->is_ready(); }

     private:
      std::shared_ptr<fiber_promise<T>> _promise;
   };

}  // namespace psiber
