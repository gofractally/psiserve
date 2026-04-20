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
      //  fiber pool:    call()           async()           spawn()
      //  work pool:     invoke()           —               post()
      //
      // All callables run on fibers with full fiber context — they
      // can yield, sleep, do I/O, and use fiber-aware locks.

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

      /// Synchronous cross-thread call (work pool, may yield).
      ///
      /// Parks the calling fiber, runs `func` on this thread via the
      /// WorkItem pool (zero allocation), returns the result.  The
      /// callable gets its own fiber and can yield.  Use for fast
      /// getters, state reads, or short operations.
      ///
      ///     int n = other_thread.invoke([&]{ return _counter; });
      ///
      template <typename F>
      auto invoke(F&& func, post_overflow policy = post_overflow::block,
                  std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
         -> std::invoke_result_t<F>;

      /// Fire-and-forget (work pool, may yield).
      ///
      /// Each callable gets its own fiber.  Zero allocation from the
      /// WorkItem pool.  Best for short coordination: counters, state
      /// updates, posting follow-ups.  Can also block on I/O or locks.
      ///
      ///     other_thread.post([&]() { counter++; });
      ///
      template <typename F>
      void post(F&& fn, post_overflow policy = post_overflow::block,
                std::chrono::milliseconds timeout = std::chrono::milliseconds{1000})
      {
         _sched->post(std::forward<F>(fn), policy, timeout);
      }

      /// Non-throwing post.  Returns true if enqueued, false if full.
      template <typename F>
      bool try_post(F&& fn,
                    try_post_overflow overflow = try_post_overflow::allow_heap) noexcept
      {
         return _sched->try_post(std::forward<F>(fn), overflow);
      }

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
            auto* entry = static_cast<Decay*>(p);
            Scheduler::current().spawnFiber(std::move(*entry));
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

   // ── invoke(): sync, work pool, may yield ────────────────────────────────

   template <typename F>
   auto thread::invoke(F&& func, post_overflow policy,
                       std::chrono::milliseconds timeout) -> std::invoke_result_t<F>
   {
      using R = std::invoke_result_t<F>;

      auto& caller_sched = Scheduler::current();
      Fiber* caller_fiber = caller_sched.currentFiber();
      assert(caller_fiber && "thread::invoke() must be called from a psiber fiber");

      fiber_promise<R> promise;

      F* func_ptr = &func;
      fiber_promise<R>* promise_ptr = &promise;

      _sched->post([func_ptr, promise_ptr]() {
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
      }, policy, timeout);

      if (promise.try_register_waiter(caller_fiber))
         caller_sched.parkCurrentFiber();

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
      // The promise handles wake coordination internally via CAS protocol.
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
      });

      return fiber_future<R>(std::move(promise));
   }

   // ── call(): sync, own fiber, may yield ─────────────────────────────────

   template <typename F>
   auto thread::call(F&& func) -> std::invoke_result_t<F>
   {
      using R = std::invoke_result_t<F>;

      auto& caller_sched = Scheduler::current();
      Fiber* caller_fiber = caller_sched.currentFiber();
      assert(caller_fiber && "thread::call() must be called from a psiber fiber");

      // Zero-allocation cross-thread dispatch: TaskSlotHeader + lambda payload
      // live on the caller's fiber stack (stable because caller parks).
      // No new/delete, no std::function heap alloc.

      struct CallPayload
      {
         F*                   func;
         fiber_promise<R>*    promise;
      };

      // Stack-allocated slot: header + payload, 16-byte aligned
      alignas(16) char buf[sizeof(TaskSlotHeader) + sizeof(CallPayload)];

      auto* slot    = new (buf) TaskSlotHeader{};
      auto* payload = new (buf + sizeof(TaskSlotHeader)) CallPayload{&func, nullptr};

      fiber_promise<R> promise;
      payload->promise = &promise;

      slot->total_size = sizeof(buf);
      slot->next       = nullptr;

      // Pre-mark consumed: drainTaskList() caches this flag before
      // calling run(), and skips the post-run consumed store for
      // slots that are already consumed.  This eliminates a race:
      // run() wakes the caller via promise, and if drainTaskList()
      // tried to write consumed AFTER run(), it would be touching
      // this stack-local slot after the caller had already returned.
      // The promise provides all synchronization for call() — the
      // consumed flag is only needed for SendQueue ring reclamation.
      slot->consumed.store(true, std::memory_order_relaxed);

      // The promise handles wake coordination via CAS protocol.
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
      };
      slot->destroy = nullptr;  // trivial payload, no destructor needed

      _sched->postTask(slot);

      // Register waiter + park.  CAS ensures we only park if the
      // producer hasn't fulfilled yet.
      if (promise.try_register_waiter(caller_fiber))
         caller_sched.parkCurrentFiber();

      if constexpr (std::is_void_v<R>)
         promise.get();
      else
         return promise.get();
   }

}  // namespace psiber
