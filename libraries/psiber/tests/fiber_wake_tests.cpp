#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiber/scheduler.hpp>
#include <psiber/io_engine_kqueue.hpp>

#include <atomic>
#include <thread>

using namespace psiber;

TEST_CASE("Same-thread park and wake", "[wake]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 1);

   std::atomic<int> step{0};

   sched.spawnFiber([&]() {
      // Fiber A: parks itself, then resumes after B wakes it
      step.store(1, std::memory_order_relaxed);
      sched.parkCurrentFiber();
      step.store(3, std::memory_order_relaxed);
   });

   sched.spawnFiber([&]() {
      // Fiber B: waits for A to park, then wakes it
      REQUIRE(step.load(std::memory_order_relaxed) == 1);
      step.store(2, std::memory_order_relaxed);

      // Find fiber A and wake it
      // Fiber A is _fibers[0], but we need to get it via the scheduler.
      // Since A parked, its state is Parked. We can wake it.
      // In real usage, A would stash its Fiber* somewhere before parking.
   });

   // For this test, we need a way to get Fiber A's pointer.
   // Let's restructure with a shared pointer.

   // Reset
   step.store(0);
   auto io2 = std::make_unique<KqueueEngine>();
   Scheduler sched2(std::move(io2), 2);

   Fiber* fiber_a = nullptr;

   sched2.spawnFiber([&]() {
      fiber_a = sched2.currentFiber();
      step.store(1, std::memory_order_relaxed);
      sched2.parkCurrentFiber();
      step.store(3, std::memory_order_relaxed);
   });

   sched2.spawnFiber([&]() {
      REQUIRE(step.load(std::memory_order_relaxed) == 1);
      step.store(2, std::memory_order_relaxed);
      REQUIRE(fiber_a != nullptr);
      Scheduler::wake(fiber_a);
   });

   sched2.run();

   REQUIRE(step.load(std::memory_order_relaxed) == 3);
}

TEST_CASE("Cross-thread park and wake", "[wake]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 3);

   std::atomic<int>  step{0};
   Fiber*            target_fiber = nullptr;

   sched.spawnFiber([&]() {
      target_fiber = sched.currentFiber();
      step.store(1, std::memory_order_release);
      sched.parkCurrentFiber();
      step.store(3, std::memory_order_release);
   });

   // Start a separate thread that will wake the fiber
   std::thread waker([&]() {
      // Wait for fiber to park
      while (step.load(std::memory_order_acquire) != 1)
         ;
      step.store(2, std::memory_order_release);
      Scheduler::wake(target_fiber);
   });

   sched.run();
   waker.join();

   REQUIRE(step.load(std::memory_order_relaxed) == 3);
}

TEST_CASE("Multiple cross-thread wakes batch correctly", "[wake]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 4);

   constexpr int    num_fibers = 8;
   std::atomic<int> woken_count{0};
   Fiber*           fiber_ptrs[num_fibers] = {};
   std::atomic<int> parked_count{0};

   for (int i = 0; i < num_fibers; ++i)
   {
      sched.spawnFiber([&, i]() {
         fiber_ptrs[i] = sched.currentFiber();
         parked_count.fetch_add(1, std::memory_order_release);
         sched.parkCurrentFiber();
         woken_count.fetch_add(1, std::memory_order_relaxed);
      });
   }

   // Wake all fibers from another thread
   std::thread waker([&]() {
      // Wait for all fibers to park
      while (parked_count.load(std::memory_order_acquire) != num_fibers)
         ;
      // Wake them all — they'll all CAS-push onto the same wake list
      for (int i = 0; i < num_fibers; ++i)
         Scheduler::wake(fiber_ptrs[i]);
   });

   sched.run();
   waker.join();

   REQUIRE(woken_count.load(std::memory_order_relaxed) == num_fibers);
}

TEST_CASE("Wake preserves FIFO order", "[wake]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 5);

   constexpr int    num_fibers = 4;
   std::vector<int> wake_order;
   Fiber*           fiber_ptrs[num_fibers] = {};
   std::atomic<int> parked_count{0};

   for (int i = 0; i < num_fibers; ++i)
   {
      sched.spawnFiber([&, i]() {
         fiber_ptrs[i] = sched.currentFiber();
         parked_count.fetch_add(1, std::memory_order_release);
         sched.parkCurrentFiber();
         wake_order.push_back(i);
      });
   }

   std::thread waker([&]() {
      while (parked_count.load(std::memory_order_acquire) != num_fibers)
         ;
      // Wake in order 0, 1, 2, 3
      for (int i = 0; i < num_fibers; ++i)
         Scheduler::wake(fiber_ptrs[i]);
   });

   sched.run();
   waker.join();

   // Should have woken in the order they were pushed (FIFO after reversal)
   REQUIRE(wake_order.size() == num_fibers);
   for (int i = 0; i < num_fibers; ++i)
      REQUIRE(wake_order[i] == i);
}
