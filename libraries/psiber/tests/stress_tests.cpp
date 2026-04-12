#include <catch2/catch.hpp>

#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_tx_mutex.hpp>
#include <psiber/fiber_shared_mutex.hpp>
#include <psiber/fiber_future.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/spin_lock.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/send_queue.hpp>
#include <psiber/thread.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace psiber;

// ── Multi-thread stress: spin_lock ───────────────────────────────────────────

TEST_CASE("spin_lock stress: many threads, many iterations", "[stress][spin_lock]")
{
   spin_lock  lock;
   int        counter = 0;
   constexpr int iterations  = 100000;
   constexpr int num_threads = 8;

   std::vector<std::thread> threads;
   for (int t = 0; t < num_threads; ++t)
   {
      threads.emplace_back([&]() {
         for (int i = 0; i < iterations; ++i)
         {
            lock.lock();
            ++counter;
            lock.unlock();
         }
      });
   }

   for (auto& t : threads)
      t.join();

   REQUIRE(counter == num_threads * iterations);
}

// ── Multi-thread stress: fiber_mutex ─────────────────────────────────────────

TEST_CASE("fiber_mutex stress: multiple schedulers contending on same mutex", "[stress][mutex]")
{
   // Each thread runs its own Scheduler with fibers that lock/unlock the
   // SAME fiber_mutex.  Cross-thread wakes are exercised when unlock()
   // wakes a waiter on another thread's scheduler.
   fiber_mutex          mtx;
   std::atomic<int>     counter{0};
   constexpr int        fibers_per_thread = 4;
   constexpr int        increments_per_fiber = 500;
   constexpr int        num_threads = 4;

   std::vector<std::thread> threads;
   for (int t = 0; t < num_threads; ++t)
   {
      threads.emplace_back([&, t]() {
         auto sched = scheduler_access::make(100 + t);

         for (int f = 0; f < fibers_per_thread; ++f)
         {
            sched.spawnFiber([&]() {
               for (int i = 0; i < increments_per_fiber; ++i)
               {
                  mtx.lock(sched);
                  int val = counter.load(std::memory_order_relaxed);
                  counter.store(val + 1, std::memory_order_relaxed);
                  mtx.unlock();
               }
            });
         }

         sched.run();
      });
   }

   for (auto& t : threads)
      t.join();

   REQUIRE(counter.load() == num_threads * fibers_per_thread * increments_per_fiber);
}

// ── Multi-thread stress: fiber_shared_mutex ──────────────────────────────────

TEST_CASE("fiber_shared_mutex stress: readers and writers across threads", "[stress][mutex]")
{
   fiber_shared_mutex   mtx;
   std::atomic<int>     shared_counter{0};
   std::atomic<int>     read_count{0};
   constexpr int        num_threads = 4;

   std::vector<std::thread> threads;
   for (int t = 0; t < num_threads; ++t)
   {
      threads.emplace_back([&, t]() {
         auto sched = scheduler_access::make(110 + t);

         // 2 writers
         for (int w = 0; w < 2; ++w)
         {
            sched.spawnFiber([&]() {
               for (int i = 0; i < 100; ++i)
               {
                  mtx.lock(sched);
                  shared_counter.fetch_add(1, std::memory_order_relaxed);
                  mtx.unlock();
               }
            });
         }

         // 4 readers
         for (int r = 0; r < 4; ++r)
         {
            sched.spawnFiber([&]() {
               for (int i = 0; i < 200; ++i)
               {
                  mtx.lock_shared(sched);
                  shared_counter.load(std::memory_order_relaxed);
                  read_count.fetch_add(1, std::memory_order_relaxed);
                  mtx.unlock_shared();
               }
            });
         }

         sched.run();
      });
   }

   for (auto& t : threads)
      t.join();

   REQUIRE(shared_counter.load() == num_threads * 2 * 100);
   REQUIRE(read_count.load() == num_threads * 4 * 200);
}

// ── Multi-thread stress: spin_yield_lock under fiber contention ──────────────

TEST_CASE("spin_yield_lock stress: fibers yield on contention", "[stress][spin_lock]")
{
   spin_yield_lock lock;
   int             counter = 0;
   auto sched = scheduler_access::make(120);

   constexpr int num_fibers  = 8;
   constexpr int iterations  = 1000;

   for (int f = 0; f < num_fibers; ++f)
   {
      sched.spawnFiber([&]() {
         for (int i = 0; i < iterations; ++i)
         {
            lock.lock();
            ++counter;
            lock.unlock();
         }
      });
   }

   sched.run();
   REQUIRE(counter == num_fibers * iterations);
}

// ── Multi-thread stress: SendQueue concurrent senders ────────────────────────

TEST_CASE("SendQueue stress: multiple threads sending to same scheduler", "[stress][send_queue]")
{
   auto sched = scheduler_access::make(130);

   std::atomic<int>  task_count{0};
   constexpr int     num_senders    = 4;
   constexpr int     tasks_per_sender = 100;
   std::atomic<int>  total_expected{num_senders * tasks_per_sender};

   // Keep scheduler alive while tasks are being posted
   sched.spawnFiber([&]() {
      while (task_count.load(std::memory_order_acquire) < total_expected.load())
         sched.sleep(std::chrono::milliseconds{1});
   });

   // Each sender thread has its own SendQueue
   std::vector<std::thread> senders;
   // Keep SendQueues alive until threads finish
   std::vector<std::unique_ptr<SendQueue>> queues;
   for (int s = 0; s < num_senders; ++s)
      queues.push_back(std::make_unique<SendQueue>(8192));

   for (int s = 0; s < num_senders; ++s)
   {
      senders.emplace_back([&, s]() {
         auto& sq = *queues[s];
         for (int i = 0; i < tasks_per_sender; ++i)
         {
            TaskSlotHeader* slot = nullptr;
            while (!slot)
            {
               slot = sq.emplace([&]() {
                  task_count.fetch_add(1, std::memory_order_release);
               });
               if (!slot)
                  sq.reclaim();
            }
            sched.postTask(slot);
         }
      });
   }

   sched.run();

   for (auto& t : senders)
      t.join();
   for (auto& q : queues)
      q->reclaim();

   REQUIRE(task_count.load() == num_senders * tasks_per_sender);
}

// ── Multi-thread stress: many cross-thread wakes ─────────────────────────────

TEST_CASE("Cross-thread wake stress: rapid park/wake cycles", "[stress][wake]")
{
   auto sched = scheduler_access::make(140);

   constexpr int      cycles = 100;
   std::atomic<int>   completed{0};
   Fiber*             fiber_ptr = nullptr;
   std::atomic<int>   parked{0};

   sched.spawnFiber([&]() {
      fiber_ptr = sched.currentFiber();
      for (int i = 0; i < cycles; ++i)
      {
         parked.store(i + 1, std::memory_order_release);
         sched.parkCurrentFiber();
         completed.fetch_add(1, std::memory_order_relaxed);
      }
   });

   std::thread waker([&]() {
      for (int i = 0; i < cycles; ++i)
      {
         // Wait for fiber to park
         while (parked.load(std::memory_order_acquire) != i + 1)
            ;
         Scheduler::wake(fiber_ptr);
      }
   });

   sched.run();
   waker.join();

   REQUIRE(completed.load() == cycles);
}

// ── Multi-thread stress: post() fire-and-forget with N producers ────────────

static void run_post_stress(int num_producers, int posts_per_producer)
{
   auto sched = scheduler_access::make(150);

   std::atomic<int> received{0};
   int total = num_producers * posts_per_producer;

   // Keep scheduler alive until all posts are received
   sched.spawnFiber([&]() {
      while (received.load(std::memory_order_acquire) < total)
         sched.sleep(std::chrono::milliseconds{1});
   });

   // Launch producer threads that fire-and-forget post() into the scheduler
   std::vector<std::thread> producers;
   for (int p = 0; p < num_producers; ++p)
   {
      producers.emplace_back([&]() {
         for (int i = 0; i < posts_per_producer; ++i)
         {
            sched.post([&received]() noexcept {
               received.fetch_add(1, std::memory_order_release);
            });
         }
      });
   }

   sched.run();

   for (auto& t : producers)
      t.join();

   REQUIRE(received.load() == total);
}

TEST_CASE("post() stress: 1 producer thread", "[stress][post]")
{
   run_post_stress(1, 1000);
}

TEST_CASE("post() stress: 2 producer threads", "[stress][post]")
{
   run_post_stress(2, 1000);
}

TEST_CASE("post() stress: 4 producer threads", "[stress][post]")
{
   run_post_stress(4, 1000);
}

TEST_CASE("post() stress: 8 producer threads", "[stress][post]")
{
   run_post_stress(8, 1000);
}

TEST_CASE("post() stress: 8 producers, high volume", "[stress][post]")
{
   run_post_stress(8, 10000);
}

// ── Multi-thread stress: thread::call() with N callers ──────────────────────

static void run_call_stress(int num_callers, int calls_per_caller)
{
   psiber::thread worker("worker");

   std::atomic<int> total{0};
   std::vector<std::unique_ptr<psiber::thread>> callers;

   for (int c = 0; c < num_callers; ++c)
   {
      callers.push_back(std::make_unique<psiber::thread>(
         [&worker, &total, calls_per_caller]()
         {
            for (int i = 0; i < calls_per_caller; ++i)
            {
               int r = worker.call([&]() { return 1; });
               total.fetch_add(r, std::memory_order_relaxed);
            }
         },
         "caller"));
   }

   for (auto& c : callers)
      c->quit();
   worker.quit();

   REQUIRE(total.load() == num_callers * calls_per_caller);
}

TEST_CASE("thread::call() stress: 2 callers", "[stress][call]")
{
   run_call_stress(2, 500);
}

TEST_CASE("thread::call() stress: 4 callers", "[stress][call]")
{
   run_call_stress(4, 500);
}

TEST_CASE("thread::call() stress: 8 callers", "[stress][call]")
{
   run_call_stress(8, 200);
}

// ── Pathological case tests: post() design edge cases ───────────────────────

TEST_CASE("post-to-self: fiber posts to its own scheduler", "[stress][post][pathological]")
{
   // Verifies that a fiber can post() to its own scheduler without deadlock.
   // The drain fiber runs independently, so even if the posting fiber parks,
   // the posted callable still executes.
   auto sched = scheduler_access::make(200);

   std::atomic<int> received{0};
   constexpr int    N = 100;

   sched.spawnFiber([&]() {
      for (int i = 0; i < N; ++i)
      {
         sched.post([&received]() noexcept {
            received.fetch_add(1, std::memory_order_relaxed);
         });
      }
      // Wait for all posts to be processed
      while (received.load(std::memory_order_acquire) < N)
         sched.yieldCurrentFiber();
   });

   sched.run();
   REQUIRE(received.load() == N);
}

TEST_CASE("re-entrant post: drain callable calls post()", "[stress][post][pathological]")
{
   // The drain fiber executes a callable that itself calls post().
   // This exercises the re-entrant path: the callable acquires a WorkItem
   // from the freelist (items are returned one at a time during drain),
   // fills it, and CAS-pushes it onto _work_head.  The drain fiber will
   // pick it up on the next iteration.
   auto sched = scheduler_access::make(201);

   std::atomic<int> depth0{0};
   std::atomic<int> depth1{0};
   std::atomic<int> depth2{0};
   constexpr int    N = 50;

   sched.spawnFiber([&]() {
      for (int i = 0; i < N; ++i)
      {
         sched.post([&]() noexcept {
            depth0.fetch_add(1, std::memory_order_relaxed);
            // Re-entrant: post from within a posted callable
            sched.post([&]() noexcept {
               depth1.fetch_add(1, std::memory_order_relaxed);
               // Double re-entrant
               sched.post([&]() noexcept {
                  depth2.fetch_add(1, std::memory_order_relaxed);
               });
            });
         });
      }

      // Wait for all three depths to complete
      while (depth2.load(std::memory_order_acquire) < N)
         sched.yieldCurrentFiber();
   });

   sched.run();
   REQUIRE(depth0.load() == N);
   REQUIRE(depth1.load() == N);
   REQUIRE(depth2.load() == N);
}

TEST_CASE("post chaining: callable spawns fiber then posts follow-up", "[stress][post][pathological]")
{
   // Tests the pattern: post() → do work → post() follow-up.
   // The drain fiber has full fiber context, so the callable can
   // spawn fibers and yield between posts.
   auto sched = scheduler_access::make(202);

   std::atomic<int> spawned{0};
   std::atomic<int> followed_up{0};
   constexpr int    N = 20;

   sched.spawnFiber([&]() {
      for (int i = 0; i < N; ++i)
      {
         sched.post([&]() noexcept {
            // Spawn a fiber from within the drain context
            sched.spawnFiber([&]() {
               spawned.fetch_add(1, std::memory_order_relaxed);
            });
            // Post a follow-up
            sched.post([&]() noexcept {
               followed_up.fetch_add(1, std::memory_order_relaxed);
            });
         });
      }

      while (followed_up.load(std::memory_order_acquire) < N)
         sched.yieldCurrentFiber();
      // Also wait for spawned fibers
      while (spawned.load(std::memory_order_acquire) < N)
         sched.yieldCurrentFiber();
   });

   sched.run();
   REQUIRE(spawned.load() == N);
   REQUIRE(followed_up.load() == N);
}

TEST_CASE("pool exhaustion: fiber-aware wait under pressure", "[stress][post][pathological]")
{
   // Exhaust the WorkItem pool (256 items) from multiple fibers.
   // Fibers that can't acquire a WorkItem will park and be woken
   // by the drain fiber after items are returned to the freelist.
   auto sched = scheduler_access::make(203);

   std::atomic<int> received{0};
   constexpr int    fibers_count = 8;
   constexpr int    posts_per_fiber = 200;  // 8 * 200 = 1600 > pool size (256)
   constexpr int    total = fibers_count * posts_per_fiber;

   for (int f = 0; f < fibers_count; ++f)
   {
      sched.spawnFiber([&]() {
         for (int i = 0; i < posts_per_fiber; ++i)
         {
            sched.post([&received]() noexcept {
               received.fetch_add(1, std::memory_order_relaxed);
            });
         }
      });
   }

   // Keepalive fiber waits for all posts
   sched.spawnFiber([&]() {
      while (received.load(std::memory_order_acquire) < total)
         sched.sleep(std::chrono::milliseconds{1});
   });

   sched.run();
   REQUIRE(received.load() == total);
}

TEST_CASE("cross-thread pool exhaustion: multiple threads saturate one receiver", "[stress][post][pathological]")
{
   // Multiple OS threads post() to a single scheduler fast enough to
   // exhaust the pool.  Non-fiber callers spin-pause; fiber callers park.
   auto sched = scheduler_access::make(204);

   std::atomic<int> received{0};
   constexpr int    num_threads = 4;
   constexpr int    posts_per_thread = 500;
   constexpr int    total = num_threads * posts_per_thread;

   // Keepalive
   sched.spawnFiber([&]() {
      while (received.load(std::memory_order_acquire) < total)
         sched.sleep(std::chrono::milliseconds{1});
   });

   std::vector<std::thread> threads;
   for (int t = 0; t < num_threads; ++t)
   {
      threads.emplace_back([&]() {
         for (int i = 0; i < posts_per_thread; ++i)
         {
            sched.post([&received]() noexcept {
               received.fetch_add(1, std::memory_order_relaxed);
            });
         }
      });
   }

   sched.run();

   for (auto& t : threads)
      t.join();

   REQUIRE(received.load() == total);
}

TEST_CASE("daemon shutdown: drain fiber exits cleanly on scheduler stop", "[stress][post][pathological]")
{
   // Verify that the drain fiber (a daemon) exits cleanly when all
   // non-daemon fibers complete.  The drain fiber is parked waiting
   // for work; the scheduler must resume it with shutdown_exception.
   // No crash, no hang, no assertion failure.
   for (int trial = 0; trial < 10; ++trial)
   {
      auto sched = scheduler_access::make(210 + trial);

      std::atomic<int> count{0};

      sched.spawnFiber([&]() {
         // Post some work, then exit — drain fiber should still clean up
         sched.post([&count]() noexcept {
            count.fetch_add(1, std::memory_order_relaxed);
         });
         // Yield to let drain run
         sched.yieldCurrentFiber();
      });

      sched.run();
      // Scheduler destructor runs — drain fiber must not crash
      REQUIRE(count.load() >= 1);
   }
}

TEST_CASE("rapid post+quit: post work then immediately quit thread", "[stress][post][pathological]")
{
   // Post work to a thread and immediately quit — exercises the
   // shutdown path where the drain fiber may still have pending items.
   for (int trial = 0; trial < 10; ++trial)
   {
      psiber::thread worker("rapid-quit");

      std::atomic<int> count{0};
      constexpr int    N = 50;

      for (int i = 0; i < N; ++i)
      {
         worker.post([&count]() noexcept {
            count.fetch_add(1, std::memory_order_relaxed);
         });
      }

      worker.quit();
      // All posted work may or may not have executed (quit drains active work)
      // but the key requirement is: no crash, no hang, clean destruction.
      REQUIRE(count.load() <= N);
   }
}

TEST_CASE("thread::spawn(): cross-thread fiber creation", "[stress][spawn][pathological]")
{
   // spawn() heap-allocates the cross-thread dispatch (not the WorkItem pool)
   // and creates a fiber on the destination thread.  Each spawned fiber gets
   // its own stack and can block independently.
   psiber::thread worker("spawn-target");

   std::atomic<int> completed{0};
   constexpr int    N = 100;

   // Spawn N fibers from the main thread (non-fiber context)
   for (int i = 0; i < N; ++i)
   {
      worker.spawn([&completed]() {
         completed.fetch_add(1, std::memory_order_relaxed);
      });
   }

   // Wait for all to finish
   while (completed.load(std::memory_order_acquire) < N)
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

   worker.quit();
   REQUIRE(completed.load() == N);
}

TEST_CASE("thread::spawn(): from fiber context across threads", "[stress][spawn][pathological]")
{
   // A fiber on one thread spawns fibers on another thread.
   // Exercises the heap-allocated postTask path from fiber context.
   psiber::thread target("spawn-target");

   std::atomic<int> completed{0};
   constexpr int    N = 50;

   psiber::thread source([&]() {
      for (int i = 0; i < N; ++i)
      {
         target.spawn([&completed]() {
            completed.fetch_add(1, std::memory_order_relaxed);
         });
      }

      while (completed.load(std::memory_order_acquire) < N)
         Scheduler::current()->sleep(std::chrono::milliseconds{1});
   }, "spawn-source");

   source.quit();
   target.quit();
   REQUIRE(completed.load() == N);
}

TEST_CASE("thread::spawn(): doesn't consume WorkItem pool slots", "[stress][spawn][pathological]")
{
   // Spawn many fibers rapidly — since spawn() uses heap allocation
   // (not the bounded 256-slot WorkItem pool), this should never hit
   // pool exhaustion back-pressure.
   psiber::thread worker("spawn-flood");

   std::atomic<int> completed{0};
   constexpr int    N = 500;  // well above pool size of 256

   for (int i = 0; i < N; ++i)
   {
      worker.spawn([&completed]() {
         completed.fetch_add(1, std::memory_order_relaxed);
      });
   }

   while (completed.load(std::memory_order_acquire) < N)
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

   worker.quit();
   REQUIRE(completed.load() == N);
}

// ── invoke() tests ──────────────────────────────────────────────────────────

TEST_CASE("thread::invoke(): fast getter returns value", "[invoke]")
{
   psiber::thread worker("invoke-target");

   int state = 42;

   psiber::thread caller([&]() {
      int result = worker.invoke([&]() { return state; });
      REQUIRE(result == 42);

      // Mutate and read again
      worker.invoke([&]() { state = 99; });
      int result2 = worker.invoke([&]() { return state; });
      REQUIRE(result2 == 99);
   }, "invoke-caller");

   caller.quit();
   worker.quit();
}

TEST_CASE("thread::invoke(): exception propagates to caller", "[invoke]")
{
   psiber::thread worker("invoke-throw");

   psiber::thread caller([&]() {
      bool caught = false;
      try
      {
         worker.invoke([&]() -> int {
            throw std::runtime_error("invoke error");
         });
      }
      catch (const std::runtime_error& e)
      {
         caught = true;
         REQUIRE(std::string(e.what()) == "invoke error");
      }
      REQUIRE(caught);
   }, "invoke-catch");

   caller.quit();
   worker.quit();
}

TEST_CASE("thread::invoke(): void return type", "[invoke]")
{
   psiber::thread worker("invoke-void");

   int counter = 0;

   psiber::thread caller([&]() {
      worker.invoke([&]() { counter = 7; });
      int val = worker.invoke([&]() { return counter; });
      REQUIRE(val == 7);
   }, "invoke-void-caller");

   caller.quit();
   worker.quit();
}

TEST_CASE("thread::invoke(): many rapid invocations", "[invoke][stress]")
{
   psiber::thread worker("invoke-rapid");

   int counter = 0;
   constexpr int N = 500;

   psiber::thread caller([&]() {
      for (int i = 0; i < N; ++i)
         worker.invoke([&]() { ++counter; });
   }, "invoke-caller");

   caller.quit();
   worker.quit();
   REQUIRE(counter == N);
}

// ── async() tests ───────────────────────────────────────────────────────────

TEST_CASE("thread::async(): returns future with result", "[async]")
{
   psiber::thread worker("async-target");

   psiber::thread caller([&]() {
      auto fut = worker.async([&]() { return 42; });
      // Do other work...
      int result = fut.get();
      REQUIRE(result == 42);
   }, "async-caller");

   caller.quit();
   worker.quit();
}

TEST_CASE("thread::async(): exception stored in future", "[async]")
{
   psiber::thread worker("async-throw");

   psiber::thread caller([&]() {
      auto fut = worker.async([&]() -> int {
         throw std::runtime_error("async error");
      });

      bool caught = false;
      try
      {
         fut.get();
      }
      catch (const std::runtime_error& e)
      {
         caught = true;
         REQUIRE(std::string(e.what()) == "async error");
      }
      REQUIRE(caught);
   }, "async-catch");

   caller.quit();
   worker.quit();
}

TEST_CASE("thread::async(): callable can yield", "[async]")
{
   psiber::thread worker("async-yield");

   psiber::thread caller([&]() {
      auto fut = worker.async([&]() {
         // This runs on its own fiber — yielding is fine
         Scheduler::current()->sleep(std::chrono::milliseconds{5});
         return 123;
      });

      int result = fut.get();
      REQUIRE(result == 123);
   }, "async-yield-caller");

   caller.quit();
   worker.quit();
}

TEST_CASE("thread::async(): void return type", "[async]")
{
   psiber::thread worker("async-void");

   std::atomic<int> done{0};

   psiber::thread caller([&]() {
      auto fut = worker.async([&]() {
         done.store(1, std::memory_order_relaxed);
      });
      fut.get();
      REQUIRE(done.load() == 1);
   }, "async-void-caller");

   caller.quit();
   worker.quit();
}

TEST_CASE("thread::async(): multiple concurrent futures", "[async][stress]")
{
   psiber::thread worker("async-multi");

   psiber::thread caller([&]() {
      constexpr int N = 20;
      std::vector<fiber_future<int>> futures;

      for (int i = 0; i < N; ++i)
      {
         futures.push_back(worker.async([i]() {
            return i * i;
         }));
      }

      for (int i = 0; i < N; ++i)
         REQUIRE(futures[i].get() == i * i);
   }, "async-multi-caller");

   caller.quit();
   worker.quit();
}

// ── drain yield guard tests ─────────────────────────────────────────────────

TEST_CASE("drain yield guard: invoke() callable that yields throws drain_yield_error", "[invoke][guard]")
{
   psiber::thread worker("guard-invoke");

   psiber::thread caller([&]() {
      bool caught = false;
      try
      {
         worker.invoke([&]() {
            // This should throw drain_yield_error
            Scheduler::current()->sleep(std::chrono::milliseconds{1});
         });
      }
      catch (const drain_yield_error&)
      {
         caught = true;
      }
      REQUIRE(caught);
   }, "guard-caller");

   caller.quit();
   worker.quit();
}
