#pragma once

#include <psiber/fiber_future.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/send_queue.hpp>

#include <cstring>
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

      // ── Dispatch API ─────────────────────────────────────────────
      //
      //                 sync (blocks)    async (future)    fire-and-forget
      //  own fiber:     call()           async()           spawn()
      //  drain fiber:   invoke()           —               post()
      //
      // Drain fiber callables must NOT yield (sleep/park/I/O).
      // Attempting to yield throws drain_yield_error.

      /// Synchronous cross-thread call (own fiber, may yield).
      ///
      /// Parks the calling fiber, spawns a fiber on this thread to
      /// run `func`, returns the result.  Exceptions propagate.
      /// Must be called from a fiber on another psiber::thread.
      ///
      ///     int result = other_thread.call([&]{ return compute(); });
      ///
      template <typename F>
      auto call(F&& func) -> std::invoke_result_t<F>;

      /// Asynchronous cross-thread call (own fiber, may yield).
      ///
      /// Spawns a fiber on this thread to run `func`.  Returns a
      /// future the caller can .get() whenever ready.  Exceptions
      /// are stored in the future.  The callable may yield.
      /// Must be called from a fiber context.
      ///
      ///     auto fut = other_thread.async([&]{ return slow_query(); });
      ///     // ... do other work ...
      ///     int result = fut.get();
      ///
      template <typename F>
      auto async(F&& func);

      /// Synchronous cross-thread call (drain fiber, non-blocking).
      ///
      /// Parks the calling fiber, runs `func` on this thread's drain
      /// fiber, returns the result.  The callable must NOT yield —
      /// attempting to sleep/park/do I/O throws drain_yield_error.
      /// Use for fast getters and state reads.
      ///
      ///     int n = other_thread.invoke([&]{ return _counter; });
      ///
      template <typename F>
      auto invoke(F&& func) -> std::invoke_result_t<F>;

      /// Fire-and-forget (drain fiber, non-blocking, noexcept).
      ///
      /// Runs serially on the drain fiber.  Must be noexcept — no
      /// error channel.  Must NOT yield.  Zero allocation (pool).
      /// Best for short coordination: counters, state updates,
      /// posting follow-ups.
      ///
      ///     other_thread.post([&]() noexcept { counter++; });
      ///
      template <typename F>
      void post(F&& fn) { _sched->post(std::forward<F>(fn)); }

      /// Fire-and-forget (own fiber, may yield, noexcept).
      ///
      /// Spawns a fiber on this thread.  The callable gets its own
      /// stack and can block on I/O, sleep, lock mutexes.  Must be
      /// noexcept — no error channel.  Use async() if you need
      /// return values or exception propagation.
      ///
      /// Heap-allocates the cross-thread dispatch (not the bounded
      /// WorkItem pool).
      template <typename F>
      void spawn(F&& fn)
      {
         using Decay = std::decay_t<F>;
         constexpr uint32_t payload_size = (sizeof(Decay) + 15) & ~15u;
         constexpr uint32_t total_size   = sizeof(TaskSlotHeader) + payload_size;

         auto* buf     = new char[total_size];
         auto* slot    = new (buf) TaskSlotHeader{};
         new (buf + sizeof(TaskSlotHeader)) Decay(std::forward<F>(fn));

         slot->total_size = total_size;
         slot->next       = nullptr;
         slot->heap_owned = true;
         slot->consumed.store(false, std::memory_order_relaxed);

         slot->run = [](void* p) {
            auto*      entry = static_cast<Decay*>(p);
            Scheduler* s     = Scheduler::current();
            s->spawnFiber(std::move(*entry));
         };
         slot->destroy = [](void* p) {
            static_cast<Decay*>(p)->~Decay();
         };

         _sched->postTask(slot);
      }

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

   // ── Template implementations ─────────────────────────────────────────────

   // ── invoke(): sync, drain fiber, non-blocking ──────────────────────────

   template <typename F>
   auto thread::invoke(F&& func) -> std::invoke_result_t<F>
   {
      using R = std::invoke_result_t<F>;

      Scheduler* caller_sched = Scheduler::current();
      assert(caller_sched && "thread::invoke() must be called from a psiber fiber");
      Fiber* caller_fiber = caller_sched->currentFiber();
      assert(caller_fiber && "thread::invoke() must be called from a psiber fiber");

      // Promise lives on caller's fiber stack (stable while parked).
      fiber_promise<R> promise;
      promise.waiting_fiber = caller_fiber;

      // Capture pointers to stack-locals and post to drain fiber.
      // The post() callable is noexcept — it catches exceptions from
      // func and stores them in the promise.
      F* func_ptr = &func;
      fiber_promise<R>* promise_ptr = &promise;
      Fiber* caller_ptr = caller_fiber;

      _sched->post([func_ptr, promise_ptr, caller_ptr]() noexcept {
         try
         {
            if constexpr (std::is_void_v<R>)
            {
               (*func_ptr)();
               promise_ptr->set_value();
            }
            else
            {
               promise_ptr->set_value((*func_ptr)());
            }
         }
         catch (...)
         {
            promise_ptr->set_exception(std::current_exception());
         }
         Scheduler::wake(caller_ptr);
      });

      caller_sched->parkCurrentFiber();

      if constexpr (std::is_void_v<R>)
         promise.get();
      else
         return promise.get();
   }

   // ── async(): async, own fiber, may yield ───────────────────────────────

   template <typename F>
   auto thread::async(F&& func)
   {
      using R = std::invoke_result_t<F>;

      // Heap-allocated promise shared between caller (future) and spawned fiber.
      auto promise = std::make_shared<fiber_promise<R>>();

      // Spawn a fiber on the target thread to run the callable.
      // The fiber fulfills the promise and wakes the waiting fiber.
      spawn([promise, f = std::forward<F>(func)]() mutable {
         try
         {
            if constexpr (std::is_void_v<R>)
            {
               f();
               promise->set_value();
            }
            else
            {
               promise->set_value(f());
            }
         }
         catch (...)
         {
            promise->set_exception(std::current_exception());
         }
         if (promise->waiting_fiber)
            Scheduler::wake(promise->waiting_fiber);
      });

      return fiber_future<R>(std::move(promise));
   }

   // ── call(): sync, own fiber, may yield ─────────────────────────────────

   template <typename F>
   auto thread::call(F&& func) -> std::invoke_result_t<F>
   {
      using R = std::invoke_result_t<F>;

      Scheduler* caller_sched = Scheduler::current();
      assert(caller_sched && "thread::call() must be called from a psiber fiber");
      Fiber* caller_fiber = caller_sched->currentFiber();
      assert(caller_fiber && "thread::call() must be called from a psiber fiber");

      // Zero-allocation cross-thread dispatch: TaskSlotHeader + lambda payload
      // live on the caller's fiber stack (stable because caller parks).
      // No new/delete, no std::function heap alloc.

      struct CallPayload
      {
         F*                   func;
         fiber_promise<R>*    promise;
         Fiber*               caller;
      };

      // Stack-allocated slot: header + payload, 16-byte aligned
      alignas(16) char buf[sizeof(TaskSlotHeader) + sizeof(CallPayload)];

      auto* slot    = new (buf) TaskSlotHeader{};
      auto* payload = new (buf + sizeof(TaskSlotHeader)) CallPayload{&func, nullptr, caller_fiber};

      fiber_promise<R> promise;
      promise.waiting_fiber = caller_fiber;
      payload->promise      = &promise;

      slot->total_size = sizeof(buf);
      slot->next       = nullptr;
      slot->consumed.store(false, std::memory_order_relaxed);

      slot->run = [](void* p) {
         auto* pl = static_cast<CallPayload*>(p);
         try
         {
            if constexpr (std::is_void_v<R>)
            {
               (*pl->func)();
               pl->promise->set_value();
            }
            else
            {
               pl->promise->set_value((*pl->func)());
            }
         }
         catch (...)
         {
            pl->promise->set_exception(std::current_exception());
         }
         Scheduler::wake(pl->caller);
      };
      slot->destroy = nullptr;  // trivial payload, no destructor needed

      _sched->postTask(slot);

      try
      {
         caller_sched->parkCurrentFiber();
      }
      catch (...)
      {
         // Shutdown interrupted us — spin until the target thread finishes
         // touching our stack-local slot before we unwind.
         while (!slot->consumed.load(std::memory_order_acquire))
         {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
         }
         throw;  // re-throw shutdown_exception
      }

      if constexpr (std::is_void_v<R>)
         promise.get();
      else
         return promise.get();
   }

}  // namespace psiber
