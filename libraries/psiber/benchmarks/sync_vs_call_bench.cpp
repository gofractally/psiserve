// Benchmark: strand::sync (migration) vs strand::post + fiber_promise (call)
//
// Both patterns: fiber on strand A invokes a trivial callable that returns
// an int on strand B, then returns to A.  Repeat N times.

#include <psiber/fiber_promise.hpp>
#include <psiber/reactor.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/strand.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <type_traits>

using namespace psiber;
using Clock = std::chrono::steady_clock;

static double elapsed_ns(Clock::time_point s, Clock::time_point e)
{
   return std::chrono::duration<double, std::nano>(e - s).count();
}

static void report(const char* name, int ops, double ns_total)
{
   double ns_per_op = ns_total / ops;
   double ops_per_sec = (ops / ns_total) * 1e9;
   std::printf("  %-32s %10d ops  %9.1f ns/op  %12.0f ops/sec\n",
               name, ops, ns_per_op, ops_per_sec);
   std::fflush(stdout);
}

template <typename F>
static auto strand_call(strand& target, F&& fn) -> std::invoke_result_t<F>
{
   using R = std::invoke_result_t<F>;
   auto&  sched  = Scheduler::current();
   Fiber* caller = sched.currentFiber();

   fiber_promise<R>  promise;
   fiber_promise<R>* promise_ptr = &promise;

   target.post([promise_ptr, f = std::forward<F>(fn)]() mutable {
      if constexpr (std::is_void_v<R>)
      {
         f();
         promise_ptr->set_value();
      }
      else
      {
         promise_ptr->set_value(f());
      }
   });

   if (promise.try_register_waiter(caller))
      sched.parkCurrentFiber();

   if constexpr (std::is_void_v<R>)
      promise.get();
   else
      return promise.get();
}

static void run_scenario(int nthreads, int N)
{
   std::printf("\n  pool threads = %d, iterations = %d\n", nthreads, N);

   reactor pool(nthreads);
   strand  a(pool, 8192);
   strand  b(pool, 8192);

   std::atomic<int64_t> sink{0};

   // ── sync ──
   std::atomic<bool> sync_done{false};
   Clock::time_point sync_t0, sync_t1;

   pool.scheduler(0).post([&]() noexcept {
      a.post([&]() {
         for (int i = 0; i < 200; ++i)  // warmup
            sink.fetch_add(b.sync([i] { return i + 1; }),
                           std::memory_order_relaxed);

         sync_t0 = Clock::now();
         for (int i = 0; i < N; ++i)
            sink.fetch_add(b.sync([i] { return i + 1; }),
                           std::memory_order_relaxed);
         sync_t1 = Clock::now();
         sync_done.store(true, std::memory_order_release);
      });
   });
   while (!sync_done.load(std::memory_order_acquire))
      std::this_thread::yield();

   // ── call (post + fiber_promise) ──
   std::atomic<bool> call_done{false};
   Clock::time_point call_t0, call_t1;

   pool.scheduler(0).post([&]() noexcept {
      a.post([&]() {
         for (int i = 0; i < 200; ++i)
            sink.fetch_add(strand_call(b, [i] { return i + 1; }),
                           std::memory_order_relaxed);

         call_t0 = Clock::now();
         for (int i = 0; i < N; ++i)
            sink.fetch_add(strand_call(b, [i] { return i + 1; }),
                           std::memory_order_relaxed);
         call_t1 = Clock::now();
         call_done.store(true, std::memory_order_release);
      });
   });
   while (!call_done.load(std::memory_order_acquire))
      std::this_thread::yield();

   pool.stop();
   pool.join();

   report("strand::sync  (migration)", N, elapsed_ns(sync_t0, sync_t1));
   report("strand::call  (post+promise)", N, elapsed_ns(call_t0, call_t1));

   double sync_ns = elapsed_ns(sync_t0, sync_t1) / N;
   double call_ns = elapsed_ns(call_t0, call_t1) / N;
   std::printf("  speedup: sync is %.2fx faster than call\n", call_ns / sync_ns);

   (void)sink.load();
}

// ── Contended scenario ──
// K background fibers on strand B do busywork (yield loop).  The measured
// fiber on strand A competes with them for B's slot.  Shows honest cost
// when the target is actually busy.
static void run_contended(int nthreads, int N, int background_on_b)
{
   std::printf("\n  pool=%d, iters=%d, background fibers on B = %d\n",
               nthreads, N, background_on_b);

   reactor pool(nthreads);
   strand  a(pool, 8192);
   strand  b(pool, 65536);

   std::atomic<int64_t> sink{0};
   std::atomic<bool>    bg_stop{false};
   std::atomic<int>     bg_started{0};

   auto spawn_bg = [&]() {
      for (int i = 0; i < background_on_b; ++i)
      {
         pool.scheduler(i % nthreads).post([&]() noexcept {
            b.post([&]() {
               bg_started.fetch_add(1, std::memory_order_release);
               // Use sleep (releases strand) so the background fiber
               // competes for B's slot but doesn't starve waiters the
               // way yieldCurrentFiber would (yield keeps _active).
               while (!bg_stop.load(std::memory_order_acquire))
                  Scheduler::current().sleep(std::chrono::milliseconds{1});
            });
         });
      }
      while (bg_started.load(std::memory_order_acquire) < background_on_b)
         std::this_thread::yield();
   };

   spawn_bg();

   std::atomic<bool> sync_done{false};
   Clock::time_point sync_t0, sync_t1;

   pool.scheduler(0).post([&]() noexcept {
      a.post([&]() {
         for (int i = 0; i < 100; ++i)
            sink.fetch_add(b.sync([i] { return i + 1; }),
                           std::memory_order_relaxed);

         sync_t0 = Clock::now();
         for (int i = 0; i < N; ++i)
            sink.fetch_add(b.sync([i] { return i + 1; }),
                           std::memory_order_relaxed);
         sync_t1 = Clock::now();
         sync_done.store(true, std::memory_order_release);
      });
   });
   while (!sync_done.load(std::memory_order_acquire))
      std::this_thread::yield();

   bg_stop.store(true, std::memory_order_release);
   pool.stop();
   pool.join();
   bg_stop.store(false, std::memory_order_release);
   bg_started.store(0, std::memory_order_release);

   // Second pool for the call scenario (fresh state)
   reactor  pool2(nthreads);
   strand   a2(pool2, 8192);
   strand   b2(pool2, 65536);

   for (int i = 0; i < background_on_b; ++i)
   {
      pool2.scheduler(i % nthreads).post([&]() noexcept {
         b2.post([&]() {
            bg_started.fetch_add(1, std::memory_order_release);
            while (!bg_stop.load(std::memory_order_acquire))
               Scheduler::current().sleep(std::chrono::milliseconds{1});
         });
      });
   }
   while (bg_started.load(std::memory_order_acquire) < background_on_b)
      std::this_thread::yield();

   std::atomic<bool> call_done{false};
   Clock::time_point call_t0, call_t1;

   pool2.scheduler(0).post([&]() noexcept {
      a2.post([&]() {
         for (int i = 0; i < 100; ++i)
            sink.fetch_add(strand_call(b2, [i] { return i + 1; }),
                           std::memory_order_relaxed);

         call_t0 = Clock::now();
         for (int i = 0; i < N; ++i)
            sink.fetch_add(strand_call(b2, [i] { return i + 1; }),
                           std::memory_order_relaxed);
         call_t1 = Clock::now();
         call_done.store(true, std::memory_order_release);
      });
   });
   while (!call_done.load(std::memory_order_acquire))
      std::this_thread::yield();

   bg_stop.store(true, std::memory_order_release);
   pool2.stop();
   pool2.join();

   report("strand::sync  (migration)", N, elapsed_ns(sync_t0, sync_t1));
   report("strand::call  (post+promise)", N, elapsed_ns(call_t0, call_t1));

   double sync_ns = elapsed_ns(sync_t0, sync_t1) / N;
   double call_ns = elapsed_ns(call_t0, call_t1) / N;
   std::printf("  speedup: sync is %.2fx faster than call\n", call_ns / sync_ns);

   (void)sink.load();
}

int main()
{
   std::printf("\n=== strand::sync vs strand::call ===\n");
   std::printf("  trivial callable: [i]() { return i + 1; }\n");

   std::printf("\n-- UNCONTENDED (single caller, idle target) --\n");
   run_scenario(/*nthreads=*/1, /*N=*/20'000);
   run_scenario(/*nthreads=*/2, /*N=*/20'000);
   run_scenario(/*nthreads=*/4, /*N=*/20'000);

   std::printf("\n-- CONTENDED (N background fibers on target, sleep-yielding) --\n");
   run_contended(/*nthreads=*/2, /*N=*/5'000, /*bg=*/3);
   run_contended(/*nthreads=*/4, /*N=*/5'000, /*bg=*/7);

   std::printf("\n");
   return 0;
}
