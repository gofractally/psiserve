#include <psiber/scheduler.hpp>
#include <psiber/thread.hpp>
#include <psiber/tcp_socket.hpp>
#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/spin_lock.hpp>

#include <boost/context/continuation.hpp>
#include <boost/context/fixedsize_stack.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace ctx = boost::context;

using namespace psiber;
using Clock = std::chrono::steady_clock;

// ── Helpers ──────────────────────────────────────────────────────────────────

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

// ══════════════════════════════════════════════════════════════════════════════
// Comparison benchmarks — structurally parallel to boost_fiber_bench.cpp.
// Same tests, same iteration counts, same labels.
// ══════════════════════════════════════════════════════════════════════════════

// ── 1. Raw context switch (control) ─────────────────────────────────────────

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
// Equivalent: spawn a lightweight fiber and run it to completion.
//
// psiber:      sched.spawnFiber(fn); sched.run();
// boost.fiber: boost::fibers::fiber f(fn); f.join();
//
// Both create a userspace fiber on a pre-allocated stack and execute
// a callable.  psiber's public entry point (psiber::thread) creates
// an OS thread per instance — a deliberate design choice for the
// thread-per-task architecture (see notes below).  For apples-to-apples
// fiber lifecycle cost, we use the scheduler's spawnFiber() directly.
//
// ── API design note ──────────────────────────────────────────────────
// psiber uses dedicated OS threads (psiber::thread) rather than
// multiplexing N fibers onto one thread.  Each thread owns its own
// scheduler, kqueue/epoll engine, and fiber pool.  Fibers are spawned
// *within* a thread, not as standalone objects.  This means:
//   - psiber::thread ≈ std::thread + scheduler (heavy, one-time setup)
//   - spawnFiber()   ≈ boost::fibers::fiber   (lightweight, per-task)
// The trade-off: no global fiber scheduler to configure, but fiber
// creation requires an existing thread context.

static void bench_fiber_create_join()
{
   constexpr int N = 10'000;
   auto& sched = Scheduler::current();

   int count = 0;

   sched.spawnFiber([&]() {
      auto start = Clock::now();
      for (int i = 0; i < N; ++i)
      {
         fiber_promise<void> done;
         sched.spawnFiber([&done]() {
            done.set_value();
         });

         // Park if the spawned fiber hasn't completed yet
         if (!done.is_ready())
         {
            if (done.try_register_waiter(sched.currentFiber()))
               sched.parkCurrentFiber();
         }
         done.get();
         ++count;
      }
      auto end = Clock::now();
      report("fiber create+run+join", count, elapsed_us(start, end));
   });

   sched.run();
}

// ── 3. Context switch (yield) ───────────────────────────────────────────────

static void bench_context_switch()
{
   constexpr int N = 100'000;
   auto& sched = Scheduler::current();

   int count = 0;

   sched.spawnFiber([&]() {
      for (int i = 0; i < N; ++i)
      {
         ++count;
         sched.yieldCurrentFiber();
      }
   });

   sched.spawnFiber([&]() {
      for (int i = 0; i < N; ++i)
      {
         ++count;
         sched.yieldCurrentFiber();
      }
   });

   auto start = Clock::now();
   sched.run();
   auto end = Clock::now();

   report("context switch (yield)", count, elapsed_us(start, end));
}

// ── 4. Fiber mutex (uncontended) ────────────────────────────────────────────

static void bench_fiber_mutex()
{
   constexpr int N = 500'000;
   auto& sched = Scheduler::current();

   fiber_mutex mtx;
   int count = 0;

   sched.spawnFiber([&]() {
      for (int i = 0; i < N; ++i)
      {
         mtx.lock(sched);
         ++count;
         mtx.unlock();
      }
   });

   auto start = Clock::now();
   sched.run();
   auto end = Clock::now();

   report("fiber_mutex (uncontended)", count, elapsed_us(start, end));
}

// ── 5. Fiber mutex (contended) ──────────────────────────────────────────────

static void bench_fiber_mutex_contended()
{
   constexpr int N           = 100'000;
   constexpr int num_fibers  = 10;
   auto& sched = Scheduler::current();

   fiber_mutex mtx;
   int count = 0;

   for (int f = 0; f < num_fibers; ++f)
   {
      sched.spawnFiber([&]() {
         for (int i = 0; i < N; ++i)
         {
            mtx.lock(sched);
            ++count;
            mtx.unlock();
         }
      });
   }

   auto start = Clock::now();
   sched.run();
   auto end = Clock::now();

   report("fiber_mutex (10-way contended)", count, elapsed_us(start, end));
}

// ── 6. Async + future (same thread) ────────────────────────────────────────
//
// Equivalent: spawn a producer fiber, return result via future.
//
// psiber:      spawnFiber → fiber_promise/fiber_future
// boost.fiber: boost::fibers::async() → future.get()
//
// Both spawn a lightweight fiber to compute a value and return it
// to a waiting fiber via a future.  psiber's public thread::async()
// dispatches across OS threads (its primary use case — see note
// below), so for a same-thread comparison we use spawnFiber with
// a fiber_promise, which is the same mechanism async() uses internally.
//
// ── API design note ──────────────────────────────────────────────────
// psiber::thread::async() always crosses an OS thread boundary
// because psiber's concurrency model is thread-per-task: the caller
// lives on one OS thread, the async work runs on another.  There is
// no same-thread async() because fibers within one scheduler already
// share state and can communicate via fiber_promise directly.
// Boost.Fiber's async() runs in the same thread's fiber scheduler
// because Boost.Fiber multiplexes all fibers onto one thread by
// default.

static void bench_async_future()
{
   constexpr int N = 10'000;
   auto& sched = Scheduler::current();

   int count = 0;

   sched.spawnFiber([&]() {
      auto start = Clock::now();
      for (int i = 0; i < N; ++i)
      {
         fiber_promise<int> promise;

         sched.spawnFiber([&promise, i]() {
            promise.set_value(i);
         });

         // Park if the spawned fiber hasn't completed yet
         if (!promise.is_ready())
         {
            if (promise.try_register_waiter(sched.currentFiber()))
               sched.parkCurrentFiber();
         }
         int v = promise.get();
         (void)v;
         ++count;
      }
      auto end = Clock::now();
      report("async + future (same thread)", count, elapsed_us(start, end));
   });

   sched.run();
}

// ── 7. Cross-thread round-trip ──────────────────────────────────────────────
//
// psiber:      int r = worker.call([&]{ return compute(); });
// boost.fiber: channel + future round-trip (no single-call API exists)

static void bench_cross_thread_call()
{
   constexpr int N = 10'000;

   psiber::thread worker("worker");
   int round_trips = 0;

   psiber::thread caller([&]() {
      auto start = Clock::now();

      for (int i = 0; i < N; ++i)
      {
         int r = worker.call([i]() { return i + 1; });
         (void)r;
         ++round_trips;
      }

      auto end = Clock::now();
      report("cross-thread call", round_trips, elapsed_us(start, end));
   });

   caller.quit();
   worker.quit();
}

// ══════════════════════════════════════════════════════════════════════════════
// psiber-only benchmarks — no Boost.Fiber equivalent.
// ══════════════════════════════════════════════════════════════════════════════

static void bench_spin_lock()
{
   constexpr int N = 1'000'000;

   spin_lock lk;
   int       count = 0;

   auto start = Clock::now();
   for (int i = 0; i < N; ++i)
   {
      lk.lock();
      ++count;
      lk.unlock();
   }
   auto end = Clock::now();

   report("spin_lock (uncontended)", count, elapsed_us(start, end));
}

static void bench_tcp_echo_throughput()
{
   constexpr int    iterations  = 50'000;
   constexpr size_t msg_size    = 64;
   auto& sched = Scheduler::current();

   fiber_promise<uint16_t> port_promise;
   ssize_t                 total_bytes = 0;

   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      auto conn = listener.accept(sched);
      conn.set_nodelay(true);

      char buf[msg_size];
      for (int i = 0; i < iterations; ++i)
      {
         auto r = conn.read_all(sched, buf, msg_size);
         if (!r)
            break;
         conn.write_all(sched, buf, msg_size);
      }
      conn.close();
      listener.close();
   });

   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      auto sock = tcp_socket::connect(sched, "127.0.0.1", port_promise.get());
      sock.set_nodelay(true);

      char send_buf[msg_size];
      char recv_buf[msg_size];
      std::memset(send_buf, 'A', msg_size);

      for (int i = 0; i < iterations; ++i)
      {
         sock.write_all(sched, send_buf, msg_size);
         auto r = sock.read_all(sched, recv_buf, msg_size);
         if (!r)
            break;
         total_bytes += r.bytes;
      }
      sock.close();
   });

   auto start = Clock::now();
   sched.run();
   auto end = Clock::now();

   double us    = elapsed_us(start, end);
   double mbps  = (total_bytes / (us / 1e6)) / (1024.0 * 1024.0);
   report("tcp echo roundtrip (64B)", iterations, us);
   std::printf("  %-40s %8.1f MB/s\n", "  throughput", mbps);
}

static void bench_tcp_throughput_bulk()
{
   constexpr size_t chunk_size  = 64 * 1024;
   constexpr size_t total_size  = 64 * 1024 * 1024;  // 64 MB
   constexpr int    num_chunks  = total_size / chunk_size;
   auto& sched = Scheduler::current();

   fiber_promise<uint16_t> port_promise;
   ssize_t                 total_received = 0;

   std::vector<char> data(chunk_size, 'B');

   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      auto conn = listener.accept(sched);
      char buf[chunk_size];
      while (true)
      {
         auto r = conn.read(sched, buf, sizeof(buf));
         if (!r)
            break;
         total_received += r.bytes;
      }
      conn.close();
      listener.close();
   });

   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      auto sock = tcp_socket::connect(sched, "127.0.0.1", port_promise.get());

      for (int i = 0; i < num_chunks; ++i)
         sock.write_all(sched, data.data(), chunk_size);

      sock.shutdown_write();
      sched.sleep(std::chrono::milliseconds{10});
      sock.close();
   });

   auto start = Clock::now();
   sched.run();
   auto end = Clock::now();

   double us   = elapsed_us(start, end);
   double mbps = (total_received / (us / 1e6)) / (1024.0 * 1024.0);
   std::printf("  %-40s %8.1f MB/s  (%zd bytes in %.1f ms)\n",
               "tcp bulk throughput (64KB chunks)", mbps,
               total_received, us / 1000.0);
}

static void bench_post_saturation()
{
   std::printf("\n  post() saturation: N producers -> 1 receiver (5ms sleep, empty callable)\n");
   std::printf("  %4s  %10s  %10s  %10s  %6s\n",
               "N", "posts/sec", "per-prod", "theoretical", "effic");

   for (int num_producers : {1, 2, 4, 8, 16, 32})
   {
      psiber::thread receiver("receiver");

      std::atomic<int64_t> received{0};
      std::atomic<bool>    running{true};
      constexpr auto       test_duration = std::chrono::seconds{2};

      std::vector<std::unique_ptr<psiber::thread>> producers;
      for (int p = 0; p < num_producers; ++p)
      {
         producers.push_back(std::make_unique<psiber::thread>(
            [&receiver, &received, &running]() {
               while (running.load(std::memory_order_relaxed))
               {
                  receiver.post([&received]() noexcept {
                     received.fetch_add(1, std::memory_order_relaxed);
                  });
                  Scheduler::current().sleep(std::chrono::milliseconds{5});
               }
            }, "producer"));
      }

      std::this_thread::sleep_for(test_duration);
      running.store(false, std::memory_order_release);

      for (auto& p : producers)
         p->quit();
      receiver.quit();

      int64_t   total       = received.load();
      double    per_sec     = total / 2.0;
      double    per_prod    = per_sec / num_producers;
      double    theoretical = num_producers * 200.0;
      double    efficiency  = (per_sec / theoretical) * 100.0;

      std::printf("  %4d  %10.0f  %10.0f  %10.0f  %5.1f%%\n",
                  num_producers, per_sec, per_prod, theoretical, efficiency);
      std::fflush(stdout);
   }
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
   std::printf("\n=== psiber benchmarks ===\n\n");

   // ── Comparison benchmarks (match boost_fiber_bench.cpp) ──
   std::printf("  ── vs Boost.Fiber ──\n\n");
   run_bench("raw_context_switch",      bench_raw_context_switch);
   run_bench("fiber_create_join",       bench_fiber_create_join);
   run_bench("context_switch",          bench_context_switch);
   run_bench("fiber_mutex",             bench_fiber_mutex);
   run_bench("fiber_mutex_contended",   bench_fiber_mutex_contended);
   run_bench("async_future",            bench_async_future);
   run_bench("cross_thread_call",       bench_cross_thread_call);

   // ── psiber-only benchmarks ──
   std::printf("\n  ── psiber only ──\n\n");
   run_bench("spin_lock",               bench_spin_lock);
   run_bench("tcp_echo_throughput",     bench_tcp_echo_throughput);
   run_bench("tcp_throughput_bulk",     bench_tcp_throughput_bulk);
   run_bench("post_saturation",         bench_post_saturation);

   std::printf("\n");
   return 0;
}
