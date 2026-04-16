/// Boost.Fiber comparison benchmarks.
///
/// Structurally parallel to psiber_bench.cpp — same operations, same
/// iteration counts, same measurement approach.  Only public API is
/// used; code is kept as syntactically close as possible to make the
/// comparison honest.
///
/// Benchmarks omitted (no Boost.Fiber equivalent):
///   - TCP/I/O (Boost.Fiber has no built-in I/O)
///   - post() saturation (no work-pool concept)
///   - spin_lock (not a fiber primitive)

#include <boost/context/continuation.hpp>
#include <boost/context/fixedsize_stack.hpp>
#include <boost/fiber/all.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace ctx = boost::context;

using Clock = std::chrono::steady_clock;

// ── Helpers (identical to psiber_bench.cpp) ─────────────────────────────────

static double elapsed_us(Clock::time_point start, Clock::time_point end)
{
   return std::chrono::duration<double, std::micro>(end - start).count();
}

static void report(const char* name, int ops, double us)
{
   double ops_per_sec = (ops / us) * 1e6;
   double ns_per_op   = (us * 1e3) / ops;
   std::printf("  %-40s %10d ops  %8.1f ns/op  %10.0f ops/sec\n",
               name, ops, ns_per_op, ops_per_sec);
   std::fflush(stdout);
}

// ── 1. Raw context switch (control) ─────────────────────────────────────────
//
// Identical on both sides — both use Boost.Context.

static void bench_raw_context_switch()
{
   constexpr int N = 1'000'000;

   int count = 0;

   ctx::continuation fiber = ctx::callcc(
       std::allocator_arg,
       ctx::fixedsize_stack(64 * 1024),
       [&](ctx::continuation&& main) {
          for (int i = 0; i < N; ++i)
          {
             ++count;
             main = main.resume();
          }
          return std::move(main);
       });

   auto start = Clock::now();
   for (int i = 0; i < N; ++i)
   {
      ++count;
      fiber = fiber.resume();
   }
   auto end = Clock::now();

   report("raw boost.context switch", count, elapsed_us(start, end));
}

// ── 2. Fiber create + run + join ────────────────────────────────────────────
//
// Create an empty fiber, run it, join (wait for completion).
// One at a time, N times.  Measures full lifecycle cost.
//
// boost.fiber: boost::fibers::fiber f(fn); f.join();
// psiber:      sched.spawnFiber(fn); sched.run();
//
// Both are lightweight userspace fiber creation.  See psiber_bench.cpp
// for notes on why psiber's public entry point (psiber::thread) differs.

static void bench_fiber_create_join()
{
   constexpr int N = 10'000;

   auto start = Clock::now();
   for (int i = 0; i < N; ++i)
   {
      boost::fibers::fiber f([]() {});
      f.join();
   }
   auto end = Clock::now();

   report("fiber create+run+join", N, elapsed_us(start, end));
}

// ── 3. Context switch (yield) ───────────────────────────────────────────────
//
// Two fibers ping-ponging via yield.  Measures scheduler overhead
// per context switch.

static void bench_context_switch()
{
   constexpr int N = 100'000;

   int count = 0;

   boost::fibers::fiber f1([&]() {
      for (int i = 0; i < N; ++i)
      {
         ++count;
         boost::this_fiber::yield();
      }
   });

   boost::fibers::fiber f2([&]() {
      for (int i = 0; i < N; ++i)
      {
         ++count;
         boost::this_fiber::yield();
      }
   });

   auto start = Clock::now();
   f1.join();
   f2.join();
   auto end = Clock::now();

   report("context switch (yield)", count, elapsed_us(start, end));
}

// ── 4. Fiber mutex (uncontended) ────────────────────────────────────────────
//
// Single fiber, lock+unlock in a tight loop.

static void bench_fiber_mutex()
{
   constexpr int N = 500'000;

   boost::fibers::mutex mtx;
   int count = 0;

   boost::fibers::fiber f([&]() {
      for (int i = 0; i < N; ++i)
      {
         mtx.lock();
         ++count;
         mtx.unlock();
      }
   });

   auto start = Clock::now();
   f.join();
   auto end = Clock::now();

   report("fiber_mutex (uncontended)", count, elapsed_us(start, end));
}

// ── 5. Fiber mutex (contended) ──────────────────────────────────────────────
//
// 10 fibers contending on one mutex.

static void bench_fiber_mutex_contended()
{
   constexpr int N           = 100'000;
   constexpr int num_fibers  = 10;

   boost::fibers::mutex mtx;
   int count = 0;

   std::vector<boost::fibers::fiber> fibers;
   for (int f = 0; f < num_fibers; ++f)
   {
      fibers.emplace_back([&]() {
         for (int i = 0; i < N; ++i)
         {
            mtx.lock();
            ++count;
            mtx.unlock();
         }
      });
   }

   auto start = Clock::now();
   for (auto& f : fibers)
      f.join();
   auto end = Clock::now();

   report("fiber_mutex (10-way contended)", count, elapsed_us(start, end));
}

// ── 6. Async + future (same thread) ────────────────────────────────────────
//
// Spawn a producer fiber, return result via future.
//
// boost.fiber: auto fut = boost::fibers::async([&]{ return i; }); fut.get();
// psiber:      spawnFiber → fiber_promise → get()
//
// Both spawn a fiber to compute a value and deliver it via future.
// psiber's thread::async() crosses OS threads by design (see
// psiber_bench.cpp for API design notes), so the equivalent same-thread
// operation uses spawnFiber + fiber_promise.

static void bench_async_future()
{
   constexpr int N = 10'000;

   int count = 0;

   boost::fibers::fiber driver([&]() {
      for (int i = 0; i < N; ++i)
      {
         auto fut = boost::fibers::async([i]() { return i; });
         int v = fut.get();
         (void)v;
         ++count;
      }
   });

   auto start = Clock::now();
   driver.join();
   auto end = Clock::now();

   report("async + future (same thread)", count, elapsed_us(start, end));
}

// ── 7. Cross-thread round-trip ──────────────────────────────────────────────
//
// Synchronous call to a worker thread and back.
//
// psiber API:   int r = worker.call([&]{ return compute(); });
//
// Boost.Fiber has no equivalent single-call API.  The idiomatic
// pattern is: push work + promise to a channel, worker fulfills,
// caller waits on the future.  This measures the cost of solving
// the same problem with each library's public API.

static void bench_cross_thread_call()
{
   constexpr int N = 10'000;

   using Work = std::pair<std::function<int()>, boost::fibers::promise<int>*>;
   boost::fibers::buffered_channel<Work> work_ch(64);

   // Worker thread: read work from channel, execute, fulfill promise
   std::thread worker([&]() {
      Work item;
      while (work_ch.pop(item) == boost::fibers::channel_op_status::success)
      {
         item.second->set_value(item.first());
      }
   });

   int round_trips = 0;

   // Caller thread runs a fiber that dispatches work
   boost::fibers::fiber caller([&]() {
      auto start = Clock::now();

      for (int i = 0; i < N; ++i)
      {
         boost::fibers::promise<int> p;
         auto fut = p.get_future();
         work_ch.push(Work{[i]() { return i + 1; }, &p});
         int r = fut.get();
         (void)r;
         ++round_trips;
      }

      auto end = Clock::now();
      report("cross-thread call (channel+future)", round_trips, elapsed_us(start, end));

      work_ch.close();
   });

   caller.join();
   worker.join();
}

// ── Main ─────────────────────────────────────────────────────────────────────

static void run_bench(const char* name, void (*fn)())
{
   std::printf("  [running: %s]\n", name);
   std::fflush(stdout);

   auto done = std::make_shared<std::atomic<bool>>(false);
   std::thread watchdog([done, name]() {
      for (int i = 0; i < 600; ++i)
      {
         std::this_thread::sleep_for(std::chrono::milliseconds{100});
         if (done->load(std::memory_order_relaxed))
            return;
      }
      std::printf("  *** TIMEOUT: %s hung after 60s ***\n", name);
      std::fflush(stdout);
      std::_Exit(1);
   });
   watchdog.detach();

   fn();
   done->store(true, std::memory_order_relaxed);
}

int main()
{
   std::printf("\n=== boost.fiber benchmarks ===\n\n");

   run_bench("raw_context_switch",      bench_raw_context_switch);
   run_bench("fiber_create_join",       bench_fiber_create_join);
   run_bench("context_switch",          bench_context_switch);
   run_bench("fiber_mutex",             bench_fiber_mutex);
   run_bench("fiber_mutex_contended",   bench_fiber_mutex_contended);
   run_bench("async_future",            bench_async_future);
   run_bench("cross_thread_call",       bench_cross_thread_call);

   std::printf("\n");
   return 0;
}
