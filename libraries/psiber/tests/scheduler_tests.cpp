#include <catch2/catch.hpp>

#include <psiber/scheduler.hpp>
#include <psiber/spin_lock.hpp>
#include <psiber/io_engine_kqueue.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace psiber;

TEST_CASE("Scheduler::current() is thread-local", "[scheduler]")
{
   REQUIRE(Scheduler::current() == nullptr);

   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 20);

   Scheduler* captured = nullptr;
   sched.spawnFiber([&]() {
      captured = Scheduler::current();
   });
   sched.run();

   REQUIRE(captured == &sched);
   REQUIRE(Scheduler::current() == nullptr);  // cleared after run()
}

TEST_CASE("Basic fiber execution", "[scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 21);

   int result = 0;
   sched.spawnFiber([&]() { result = 42; });
   sched.run();

   REQUIRE(result == 42);
}

TEST_CASE("Multiple fibers execute in order", "[scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 22);

   std::vector<int> order;

   sched.spawnFiber([&]() { order.push_back(1); });
   sched.spawnFiber([&]() { order.push_back(2); });
   sched.spawnFiber([&]() { order.push_back(3); });

   sched.run();

   REQUIRE(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("Fiber sleep and resume", "[scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 23);

   std::vector<int> order;

   sched.spawnFiber([&]() {
      order.push_back(1);
      sched.sleep(std::chrono::milliseconds{10});
      order.push_back(3);
   });

   sched.spawnFiber([&]() {
      order.push_back(2);
   });

   sched.run();

   // Fiber 1 starts, sleeps, fiber 2 runs, then fiber 1 resumes
   REQUIRE(order == std::vector<int>{1, 2, 3});
}

TEST_CASE("Priority queues: high before normal before low", "[scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 24);

   std::vector<int> order;
   Fiber* fiber_ptrs[3] = {};
   std::atomic<int> parked{0};

   // Spawn 3 fibers, they all park immediately
   for (int i = 0; i < 3; ++i)
   {
      sched.spawnFiber([&, i]() {
         fiber_ptrs[i] = sched.currentFiber();
         parked.fetch_add(1, std::memory_order_relaxed);
         sched.parkCurrentFiber();
         order.push_back(i);
      });
   }

   // Spawn a control fiber that sets priorities and wakes them
   sched.spawnFiber([&]() {
      // Wait for all 3 to park
      while (parked.load(std::memory_order_relaxed) != 3)
         sched.sleep(std::chrono::milliseconds{1});

      // Set priorities: fiber 0 = low, fiber 1 = high, fiber 2 = normal
      fiber_ptrs[0]->priority = 2;  // low
      fiber_ptrs[1]->priority = 0;  // high
      fiber_ptrs[2]->priority = 1;  // normal

      // Wake them all
      Scheduler::wake(fiber_ptrs[0]);
      Scheduler::wake(fiber_ptrs[1]);
      Scheduler::wake(fiber_ptrs[2]);
   });

   sched.run();

   // Should execute in priority order: high(1), normal(2), low(0)
   REQUIRE(order == std::vector<int>{1, 2, 0});
}

TEST_CASE("Scheduler interrupt for clean shutdown", "[scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 25);

   std::atomic<bool> fiber_started{false};

   sched.spawnFiber([&]() {
      fiber_started.store(true, std::memory_order_relaxed);
      // This would block forever without interrupt
      sched.sleep(std::chrono::milliseconds{60000});
      // Should NOT reach here if shutdown_exception is thrown
   });

   sched.setShutdownCheck([&]() {
      return fiber_started.load(std::memory_order_relaxed);
   });

   sched.run();
   REQUIRE(fiber_started.load(std::memory_order_relaxed));
}

TEST_CASE("Cross-thread postTask executes on scheduler thread", "[scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 26);

   std::atomic<int>     result{0};
   std::atomic<bool>    task_done{false};
   SendQueue            sq(4096);

   sched.spawnFiber([&]() {
      // Poll until the task has been executed
      while (!task_done.load(std::memory_order_acquire))
         sched.sleep(std::chrono::milliseconds{1});
   });

   std::thread poster([&]() {
      auto* slot = sq.emplace([&]() {
         result.store(99, std::memory_order_relaxed);
         task_done.store(true, std::memory_order_release);
      });
      REQUIRE(slot != nullptr);
      sched.postTask(slot);
   });

   sched.run();
   poster.join();

   REQUIRE(result.load(std::memory_order_relaxed) == 99);
   sq.reclaim();
}

TEST_CASE("spin_lock basic lock/unlock", "[spin_lock]")
{
   psiber::spin_lock lock;

   REQUIRE(lock.try_lock());
   lock.unlock();

   lock.lock();
   REQUIRE(!lock.try_lock());
   lock.unlock();

   REQUIRE(lock.try_lock());
   lock.unlock();
}

TEST_CASE("spin_lock protects shared counter", "[spin_lock]")
{
   psiber::spin_lock  lock;
   int                counter = 0;
   constexpr int      iterations = 10000;
   constexpr int      num_threads = 4;

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
