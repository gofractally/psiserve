#pragma once

#include <psiber/fiber_promise.hpp>
#include <psiber/scheduler.hpp>

#include <thread>
#include <type_traits>

namespace psiber
{
   /// A fiber-scheduled thread with a clean cross-thread call API.
   ///
   /// Models fc::thread — each thread owns a Scheduler running on a
   /// dedicated OS thread.  Other psiber::threads can call into it
   /// with zero-copy, zero-allocation cross-thread dispatch:
   ///
   ///     psiber::thread worker("compute");
   ///     psiber::thread io("network", [&]() {
   ///        int r = worker.call([&]{ return expensive(); });
   ///     });
   ///
   /// `call()` parks the calling fiber, dispatches the lambda to the
   /// target thread via the MPSC work list, and resumes the caller
   /// when the result is ready.  No heap allocation for the promise
   /// (it lives on the caller's stable fiber stack).
   class thread
   {
     public:
      /// Create a thread.  Optionally run an initial fiber.
      explicit thread(const char* name = nullptr);
      thread(std::function<void()> entry, const char* name = nullptr);
      ~thread();

      thread(const thread&)            = delete;
      thread& operator=(const thread&) = delete;

      /// Synchronous cross-thread call.  Must be called from a fiber
      /// on another psiber::thread.  Parks the calling fiber, runs `func`
      /// on this thread, returns the result.
      ///
      ///     int result = other_thread.call([&]{ return compute(); });
      ///
      template <typename F>
      auto call(F&& func) -> std::invoke_result_t<F>;

      /// Fire-and-forget: post a callable to run on this thread.
      void post(std::function<void()> fn) { _sched->post(std::move(fn)); }

      /// Spawn a fiber on this thread.  Thread-safe.
      void spawn(std::function<void()> fn);

      /// Signal this thread to stop.
      void quit();

      Scheduler& scheduler() { return *_sched; }

     private:
      void start(std::function<void()> entry);

      std::unique_ptr<Scheduler> _sched;
      std::thread                _os_thread;
      std::atomic<Fiber*>        _keepalive{nullptr};
      static inline uint32_t     s_next_index{0};
   };

   // ── Template implementation ──────────────────────────────────────────────

   template <typename F>
   auto thread::call(F&& func) -> std::invoke_result_t<F>
   {
      using R = std::invoke_result_t<F>;

      Scheduler* caller_sched = Scheduler::current();
      assert(caller_sched && "thread::call() must be called from a psiber fiber");
      Fiber* caller_fiber = caller_sched->currentFiber();
      assert(caller_fiber && "thread::call() must be called from a psiber fiber");

      if constexpr (std::is_void_v<R>)
      {
         fiber_promise<void> promise;
         promise.waiting_fiber = caller_fiber;

         _sched->post([&func, &promise, caller_fiber]() {
            func();
            promise.set_value();
            Scheduler::wake(caller_fiber);
         });

         caller_sched->parkCurrentFiber();
      }
      else
      {
         fiber_promise<R> promise;
         promise.waiting_fiber = caller_fiber;

         _sched->post([&func, &promise, caller_fiber]() {
            promise.set_value(func());
            Scheduler::wake(caller_fiber);
         });

         caller_sched->parkCurrentFiber();
         return promise.get();
      }
   }

}  // namespace psiber
