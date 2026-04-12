#include <catch2/catch.hpp>

#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_tx_mutex.hpp>
#include <psiber/fiber_shared_mutex.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/spin_lock.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/send_queue.hpp>
#include <psiber/io_engine_kqueue.hpp>

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
         auto io = std::make_unique<KqueueEngine>();
         Scheduler sched(std::move(io), 100 + t);

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
         auto io = std::make_unique<KqueueEngine>();
         Scheduler sched(std::move(io), 110 + t);

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

   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 120);

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
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 130);

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
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 140);

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
