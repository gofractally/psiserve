#include <psiber/scheduler.hpp>
#include <psiber/thread.hpp>
#include <psiber/tcp_socket.hpp>
#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/spin_lock.hpp>

#include <boost/context/continuation.hpp>
#include <boost/context/protected_fixedsize_stack.hpp>

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

// ── Benchmarks ───────────────────────────────────────────────────────────────

static void bench_raw_context_switch()
{
   constexpr int N = 1'000'000;

   // Two continuations ping-ponging — no scheduler, no kqueue, no queues.
   // This measures pure Boost.Context assembly cost.
   int count = 0;

   ctx::continuation fiber = ctx::callcc(
       std::allocator_arg,
       ctx::protected_fixedsize_stack(64 * 1024),
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

static void bench_fiber_create_destroy()
{
   constexpr int N = 10'000;   auto sched = scheduler_access::make(1000);

   auto start = Clock::now();

   for (int i = 0; i < N; ++i)
      sched.spawnFiber([]() {});

   sched.run();
   auto end = Clock::now();

   report("fiber create+run+destroy", N, elapsed_us(start, end));
}

static void bench_context_switch()
{
   constexpr int N = 100'000;   auto sched = scheduler_access::make(1001);

   int count = 0;

   // Two fibers ping-ponging via yield
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

static void bench_cross_thread_wake()
{
   constexpr int N = 10'000;   auto sched = scheduler_access::make(1002);

   Fiber*           fiber_ptr = nullptr;
   std::atomic<int> parked{0};
   int              wakes = 0;

   sched.spawnFiber([&]() {
      fiber_ptr = sched.currentFiber();
      for (int i = 0; i < N; ++i)
      {
         parked.store(i + 1, std::memory_order_release);
         sched.parkCurrentFiber();
         ++wakes;
      }
   });

   std::thread sender([&]() {
      for (int i = 0; i < N; ++i)
      {
         while (parked.load(std::memory_order_acquire) != i + 1)
            ;
         Scheduler::wake(fiber_ptr);
      }
   });

   auto start = Clock::now();
   sched.run();
   auto end = Clock::now();
   sender.join();

   report("cross-thread wake (park+wake)", wakes, elapsed_us(start, end));
}

static void bench_cross_thread_pingpong()
{
   constexpr int N = 10'000;

   // Two schedulers on separate threads, ping-ponging wake signals.
   // Measures true round-trip cross-thread call latency.
   auto sched_a = scheduler_access::make(2000);
   auto sched_b = scheduler_access::make(2001);

   Fiber*           fiber_a = nullptr;
   Fiber*           fiber_b = nullptr;
   std::atomic<int> ready{0};
   int              round_trips = 0;

   // Fiber on scheduler A: parks, gets woken by B, wakes B back
   sched_a.spawnFiber([&]() {
      fiber_a = sched_a.currentFiber();
      ready.fetch_add(1, std::memory_order_release);

      // Wait for fiber_b to be set
      while (!fiber_b)
         sched_a.yieldCurrentFiber();

      for (int i = 0; i < N; ++i)
      {
         sched_a.parkCurrentFiber();
         // Woken by B — wake B back
         Scheduler::wake(fiber_b);
         ++round_trips;
      }
   });

   // Fiber on scheduler B: initiates the ping, parks, gets woken by A
   sched_b.spawnFiber([&]() {
      fiber_b = sched_b.currentFiber();
      ready.fetch_add(1, std::memory_order_release);

      // Wait for fiber_a to be set
      while (!fiber_a)
         sched_b.yieldCurrentFiber();

      for (int i = 0; i < N; ++i)
      {
         // Initiate: wake A
         Scheduler::wake(fiber_a);
         // Wait for A to wake us back
         sched_b.parkCurrentFiber();
      }
   });

   // Run both schedulers on separate threads
   auto start = Clock::now();
   std::thread thread_b([&]() { sched_b.run(); });
   sched_a.run();
   auto end = Clock::now();
   thread_b.join();

   report("cross-thread ping-pong (round trip)", round_trips, elapsed_us(start, end));
}

static void bench_thread_call()
{
   constexpr int N = 10'000;

   // Clean API: psiber::thread a, b — b.call() dispatches to a and gets result
   psiber::thread worker("worker");
   int            round_trips = 0;

   psiber::thread caller([&]() {
      auto start = Clock::now();

      for (int i = 0; i < N; ++i)
      {
         int r = worker.call([&]() { return i + 1; });
         (void)r;
         ++round_trips;
      }

      auto end = Clock::now();
      report("thread::call() round trip", round_trips, elapsed_us(start, end));
   });

   // Must quit explicitly — keepalive fiber blocks until quit()
   caller.quit();
   worker.quit();
}

static void bench_fiber_mutex()
{
   constexpr int N = 500'000;   auto sched = scheduler_access::make(1003);

   fiber_mutex mtx;
   int         count = 0;

   // Single fiber: uncontended lock/unlock
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

   report("fiber_mutex lock+unlock (uncontended)", count, elapsed_us(start, end));
}

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

   report("spin_lock lock+unlock (uncontended)", count, elapsed_us(start, end));
}

static void bench_tcp_echo_throughput()
{
   constexpr int    iterations  = 50'000;
   constexpr size_t msg_size    = 64;   auto sched = scheduler_access::make(1004);

   fiber_promise<uint16_t> port_promise;
   ssize_t                 total_bytes = 0;

   // Server
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

   // Client
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
   constexpr int    num_chunks  = total_size / chunk_size;   auto sched = scheduler_access::make(1005);

   fiber_promise<uint16_t> port_promise;
   ssize_t                 total_received = 0;

   std::vector<char> data(chunk_size, 'B');

   // Server: just read everything
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

   // Client: write total_size bytes
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

static void bench_fiber_spawn_rate()
{
   constexpr int N = 10'000;   auto sched = scheduler_access::make(1006);

   // Measure just spawn (no run) — minimal captures (SBO-fits)
   auto start = Clock::now();
   for (int i = 0; i < N; ++i)
      sched.spawnFiber([]() {});
   auto end = Clock::now();

   report("fiber spawn (empty)", N, elapsed_us(start, end));

   sched.run();  // drain
}

static void bench_fiber_spawn_captures()
{
   constexpr int N = 10'000;

   // Simulate realistic captures: 48 bytes (shared_ptr + refs + ints)
   // This exceeds libc++ std::function SBO (24 bytes).
   auto dummy = std::make_shared<int>(42);
   int  a = 1, b = 2, c = 3;
   volatile int sink = 0;

   // ── Template path (no std::function, no heap alloc) ──
   {      auto sched = scheduler_access::make(1008);

      auto start = Clock::now();
      for (int i = 0; i < N; ++i)
      {
         sched.spawnFiber([dummy, &sched, &a, &b, &c, i]() {
            (void)dummy; (void)sched; (void)a; (void)b; (void)c; (void)i;
         });
      }
      auto end = Clock::now();
      report("fiber spawn (48B, template)", N, elapsed_us(start, end));
      sched.run();
   }

   // ── std::function path (forces heap alloc for >24B captures) ──
   {      auto sched = scheduler_access::make(1009);

      auto start = Clock::now();
      for (int i = 0; i < N; ++i)
      {
         std::function<void()> fn = [dummy, &sched, &a, &b, &c, i]() {
            (void)dummy; (void)sched; (void)a; (void)b; (void)c; (void)i;
         };
         sched.spawnFiber(std::move(fn));
      }
      auto end = Clock::now();
      report("fiber spawn (48B, std::function)", N, elapsed_us(start, end));
      sched.run();
   }

   sink = a + b + c;
   (void)sink;
}

static void bench_fiber_spawn_pooled()
{
   constexpr int N = 10'000;   auto sched = scheduler_access::make(1010);

   // Measure the full lifecycle with pooling: spawn, run, recycle, repeat.
   // A driver fiber spawns child fibers one at a time, yielding after each
   // so the child runs and recycles before the next spawn.
   // After warmup, all spawns hit the freelist (no mmap).
   sched.spawnFiber([&]() {
      // Warmup: fill the pool with 1 fiber
      sched.spawnFiber([]() {});
      sched.yieldCurrentFiber();

      // Measure: spawn from the pool
      auto start = Clock::now();
      for (int i = 0; i < N; ++i)
      {
         sched.spawnFiber([]() {});
         sched.yieldCurrentFiber();  // let it run and recycle
      }
      auto end = Clock::now();
      report("fiber spawn+run (pooled)", N, elapsed_us(start, end));
   });
   sched.run();
}

static void bench_tcp_connection_rate()
{
   constexpr int N = 1'000;   auto sched = scheduler_access::make(1007);

   fiber_promise<uint16_t> port_promise;
   int                     accepted = 0;

   // Server: accept N connections
   sched.spawnFiber([&]() {
      auto listener = tcp_listener::bind(0);
      port_promise.set_value(listener.port());

      for (int i = 0; i < N; ++i)
      {
         auto conn = listener.accept(sched);
         ++accepted;
         conn.close();
      }
      listener.close();
   });

   // Client: connect N times
   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      uint16_t port = port_promise.get();

      for (int i = 0; i < N; ++i)
      {
         auto sock = tcp_socket::connect(sched, "127.0.0.1", port);
         sock.close();
      }
   });

   auto start = Clock::now();
   sched.run();
   auto end = Clock::now();

   report("tcp connect+accept+close", accepted, elapsed_us(start, end));
}

// ── post() saturation benchmark ─────────────────────────────────────────────

static void bench_post_saturation()
{
   std::printf("\n  post() saturation: N producers → 1 receiver (5ms sleep, empty callable)\n");
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
                  Scheduler::current()->sleep(std::chrono::milliseconds{5});
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

   // Watchdog: kill the whole process if a benchmark hangs
   auto done = std::make_shared<std::atomic<bool>>(false);
   std::thread watchdog([done, name]() {
      for (int i = 0; i < 600; ++i)  // 60 seconds max (saturation bench takes ~14s)
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

   run_bench("raw_context_switch",     bench_raw_context_switch);
   run_bench("fiber_spawn_rate",      bench_fiber_spawn_rate);
   run_bench("fiber_spawn_captures", bench_fiber_spawn_captures);
   run_bench("fiber_spawn_pooled",   bench_fiber_spawn_pooled);
   run_bench("fiber_create_destroy",  bench_fiber_create_destroy);
   run_bench("context_switch",        bench_context_switch);
   run_bench("cross_thread_wake",     bench_cross_thread_wake);
   run_bench("cross_thread_pingpong", bench_cross_thread_pingpong);
   run_bench("thread_call",           bench_thread_call);
   run_bench("spin_lock",             bench_spin_lock);
   run_bench("fiber_mutex",           bench_fiber_mutex);
   run_bench("tcp_connection_rate",   bench_tcp_connection_rate);
   run_bench("tcp_echo_throughput",   bench_tcp_echo_throughput);
   run_bench("tcp_throughput_bulk",   bench_tcp_throughput_bulk);
   run_bench("post_saturation",      bench_post_saturation);

   std::printf("\n");
   return 0;
}
