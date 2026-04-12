#include <catch2/catch.hpp>

#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_tx_mutex.hpp>
#include <psiber/fiber_shared_mutex.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/scheduler.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace psiber;

TEST_CASE("fiber_mutex basic lock/unlock", "[mutex]")
{
   auto sched = scheduler_access::make(30);

   fiber_mutex mtx;
   int         counter = 0;

   sched.spawnFiber([&]() {
      mtx.lock(sched);
      counter = 1;
      mtx.unlock();
   });

   sched.spawnFiber([&]() {
      mtx.lock(sched);
      counter = 2;
      mtx.unlock();
   });

   sched.run();
   REQUIRE(counter == 2);
}

TEST_CASE("fiber_mutex FIFO ordering under contention", "[mutex]")
{
   auto sched = scheduler_access::make(31);

   fiber_mutex      mtx;
   std::vector<int> order;
   Fiber*           fiber_ptrs[3] = {};

   // Control fiber: acquires the lock first, then spawns workers
   sched.spawnFiber([&]() {
      mtx.lock(sched);

      // While we hold the lock, spawn 3 fibers that will contend
      for (int i = 0; i < 3; ++i)
      {
         sched.spawnFiber([&, i]() {
            fiber_ptrs[i] = sched.currentFiber();
            mtx.lock(sched);
            order.push_back(i);
            mtx.unlock();
         });
      }

      // Yield to let the workers try to acquire (they'll block)
      sched.yieldCurrentFiber();

      // Release — workers should wake in FIFO order
      mtx.unlock();
   });

   sched.run();

   REQUIRE(order == std::vector<int>{0, 1, 2});
}

TEST_CASE("fiber_mutex try_lock", "[mutex]")
{
   auto sched = scheduler_access::make(32);

   fiber_mutex mtx;
   bool        try_result = false;

   sched.spawnFiber([&]() {
      REQUIRE(mtx.try_lock());

      sched.spawnFiber([&]() {
         try_result = mtx.try_lock();
      });

      sched.yieldCurrentFiber();
      mtx.unlock();
   });

   sched.run();
   REQUIRE(try_result == false);
}

TEST_CASE("fiber_shared_mutex concurrent readers", "[mutex]")
{
   auto sched = scheduler_access::make(33);

   fiber_shared_mutex mtx;
   std::atomic<int>   concurrent{0};
   int                max_concurrent = 0;

   for (int i = 0; i < 4; ++i)
   {
      sched.spawnFiber([&]() {
         mtx.lock_shared(sched);
         int c = concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
         if (c > max_concurrent)
            max_concurrent = c;
         sched.yieldCurrentFiber();  // stay locked to allow overlap
         concurrent.fetch_sub(1, std::memory_order_relaxed);
         mtx.unlock_shared();
      });
   }

   sched.run();
   REQUIRE(max_concurrent == 4);
}

TEST_CASE("fiber_shared_mutex writer excludes readers", "[mutex]")
{
   auto sched = scheduler_access::make(34);

   fiber_shared_mutex mtx;
   std::vector<int>   order;

   sched.spawnFiber([&]() {
      mtx.lock(sched);  // exclusive
      order.push_back(1);

      // Spawn a reader that will block
      sched.spawnFiber([&]() {
         mtx.lock_shared(sched);
         order.push_back(2);
         mtx.unlock_shared();
      });

      sched.yieldCurrentFiber();
      mtx.unlock();
   });

   sched.run();
   REQUIRE(order == std::vector<int>{1, 2});
}

TEST_CASE("fiber_promise same-thread", "[promise]")
{
   auto sched = scheduler_access::make(35);

   fiber_promise<int> promise;
   int                result = 0;

   sched.spawnFiber([&]() {
      promise.waiting_fiber = sched.currentFiber();

      // Spawn a fiber that will fulfill the promise
      sched.spawnFiber([&]() {
         promise.set_value(42);
         Scheduler::wake(promise.waiting_fiber);
      });

      sched.parkCurrentFiber();
      result = promise.get();
   });

   sched.run();
   REQUIRE(result == 42);
}

TEST_CASE("fiber_promise<void>", "[promise]")
{
   auto sched = scheduler_access::make(36);

   fiber_promise<void> promise;
   bool                signaled = false;

   sched.spawnFiber([&]() {
      promise.waiting_fiber = sched.currentFiber();

      sched.spawnFiber([&]() {
         promise.set_value();
         Scheduler::wake(promise.waiting_fiber);
      });

      sched.parkCurrentFiber();
      promise.get();
      signaled = true;
   });

   sched.run();
   REQUIRE(signaled);
}

TEST_CASE("fiber_promise cross-thread", "[promise]")
{
   auto sched = scheduler_access::make(37);

   fiber_promise<int> promise;
   int                result = 0;

   sched.spawnFiber([&]() {
      promise.waiting_fiber = sched.currentFiber();

      std::thread fulfiller([&]() {
         promise.set_value(99);
         Scheduler::wake(promise.waiting_fiber);
      });
      fulfiller.detach();

      sched.parkCurrentFiber();
      result = promise.get();
   });

   sched.run();
   REQUIRE(result == 99);
}

TEST_CASE("fiber_tx_mutex wound-wait: older wounds younger", "[wound-wait]")
{
   auto sched = scheduler_access::make(38);

   fiber_tx_mutex mtx;
   bool           younger_wounded = false;

   // Younger transaction (timestamp 200) acquires first
   sched.spawnFiber([&]() {
      auto* self      = sched.currentFiber();
      self->tx_timestamp = 200;
      mtx.lock(sched, 200);

      // Spawn older transaction (timestamp 100) that will contend
      sched.spawnFiber([&]() {
         auto* self      = sched.currentFiber();
         self->tx_timestamp = 100;
         mtx.lock(sched, 100);
         // After younger releases, older gets the lock
         mtx.unlock();
      });

      // Yield to let the older tx attempt to lock (and wound us)
      sched.yieldCurrentFiber();

      // Check if we were wounded
      younger_wounded = sched.currentFiber()->wounded.load(std::memory_order_acquire);

      mtx.unlock();
   });

   sched.run();
   REQUIRE(younger_wounded == true);
}

TEST_CASE("fiber_tx_mutex younger waits for older", "[wound-wait]")
{
   auto sched = scheduler_access::make(39);

   fiber_tx_mutex   mtx;
   std::vector<int> order;

   // Older transaction (timestamp 100) acquires first
   sched.spawnFiber([&]() {
      auto* self      = sched.currentFiber();
      self->tx_timestamp = 100;
      mtx.lock(sched, 100);
      order.push_back(1);

      // Spawn younger transaction (timestamp 200)
      sched.spawnFiber([&]() {
         auto* self      = sched.currentFiber();
         self->tx_timestamp = 200;
         mtx.lock(sched, 200);
         order.push_back(2);
         mtx.unlock();
      });

      sched.yieldCurrentFiber();
      mtx.unlock();
   });

   sched.run();
   // Younger should execute after older releases
   REQUIRE(order == std::vector<int>{1, 2});
}
