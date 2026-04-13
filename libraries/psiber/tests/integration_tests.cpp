#include <catch2/catch.hpp>

#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/send_queue.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/thread.hpp>

#include <atomic>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace psiber;

// ── End-to-end: send task → execute → return via promise → wake ─────────────

TEST_CASE("integration: cross-thread task dispatch with promise return", "[integration]")
{
   // Scheduler A: sender (main scheduler)
   auto sched_a = scheduler_access::make(400);

   // Scheduler B: receiver (runs on another thread)
   auto sched_b = scheduler_access::make(401);

   SendQueue sq(4096);

   int result = 0;
   std::atomic<bool> b_ready{false};

   // Scheduler B: keep-alive fiber that runs until all tasks done
   sched_b.spawnFiber([&]() {
      b_ready.store(true, std::memory_order_release);
      // Just stay alive long enough for the task to arrive
      sched_b.sleep(std::chrono::milliseconds{500});
   });

   // Start scheduler B on its own thread
   std::thread thread_b([&]() {
      sched_b.run();
   });

   // Wait for B to be running
   while (!b_ready.load(std::memory_order_acquire))
      ;

   // Scheduler A: dispatch work to B and wait for the result
   sched_a.spawnFiber([&]() {
      fiber_promise<int> promise;

      auto* slot = sq.emplace([&promise]() {
         // This runs on scheduler B's thread
         int computed = 17 * 3;  // 51
         promise.set_value(computed);
      });
      REQUIRE(slot != nullptr);

      sched_b.postTask(slot);
      if (promise.try_register_waiter(sched_a.currentFiber()))
         sched_a.parkCurrentFiber();

      result = promise.get();
   });

   sched_a.run();
   thread_b.join();
   sq.reclaim();

   REQUIRE(result == 51);
}

// ── End-to-end: multiple dispatches in sequence ─────────────────────────────

TEST_CASE("integration: sequential cross-thread dispatches", "[integration]")
{
   auto sched_a = scheduler_access::make(410);
   auto sched_b = scheduler_access::make(411);

   SendQueue sq(4096);

   int sum = 0;
   std::atomic<bool> b_ready{false};

   sched_b.spawnFiber([&]() {
      b_ready.store(true, std::memory_order_release);
      sched_b.sleep(std::chrono::milliseconds{1000});
   });

   std::thread thread_b([&]() {
      sched_b.run();
   });

   while (!b_ready.load(std::memory_order_acquire))
      ;

   sched_a.spawnFiber([&]() {
      for (int i = 1; i <= 5; ++i)
      {
         fiber_promise<int> promise;

         auto* slot = sq.emplace([&promise, i]() {
            promise.set_value(i * 10);
         });
         REQUIRE(slot != nullptr);

         sched_b.postTask(slot);
         if (promise.try_register_waiter(sched_a.currentFiber()))
            sched_a.parkCurrentFiber();

         sum += promise.get();
         sq.reclaim();
      }
   });

   sched_a.run();
   thread_b.join();

   // 10 + 20 + 30 + 40 + 50 = 150
   REQUIRE(sum == 150);
}

// ── End-to-end: multiple fibers dispatching concurrently ────────────────────

TEST_CASE("integration: multiple fibers dispatch to same remote scheduler", "[integration]")
{
   auto sched_a = scheduler_access::make(420);
   auto sched_b = scheduler_access::make(421);

   constexpr int num_fibers = 4;

   // Each fiber gets its own SendQueue (sender-owned)
   std::vector<std::unique_ptr<SendQueue>> queues;
   for (int i = 0; i < num_fibers; ++i)
      queues.push_back(std::make_unique<SendQueue>(4096));

   std::atomic<int> total{0};
   std::atomic<bool> b_ready{false};

   sched_b.spawnFiber([&]() {
      b_ready.store(true, std::memory_order_release);
      sched_b.sleep(std::chrono::milliseconds{1000});
   });

   std::thread thread_b([&]() {
      sched_b.run();
   });

   while (!b_ready.load(std::memory_order_acquire))
      ;

   for (int f = 0; f < num_fibers; ++f)
   {
      sched_a.spawnFiber([&, f]() {
         fiber_promise<int> promise;

         auto* slot = queues[f]->emplace([&promise, f]() {
            promise.set_value((f + 1) * 100);
         });
         REQUIRE(slot != nullptr);

         sched_b.postTask(slot);
         if (promise.try_register_waiter(sched_a.currentFiber()))
            sched_a.parkCurrentFiber();

         total.fetch_add(promise.get(), std::memory_order_relaxed);
      });
   }

   sched_a.run();
   thread_b.join();

   for (auto& q : queues)
      q->reclaim();

   // 100 + 200 + 300 + 400 = 1000
   REQUIRE(total.load() == 1000);
}

// ── Exception propagation through thread::call ──────────────────────────────

TEST_CASE("integration: thread::call propagates exceptions", "[integration][exception]")
{
   psiber::thread worker("worker");

   bool caught = false;
   std::string msg;

   psiber::thread caller([&]() {
      try
      {
         worker.call([]() -> int {
            throw std::runtime_error("remote failure");
         });
      }
      catch (const std::runtime_error& e)
      {
         caught = true;
         msg    = e.what();
      }
   });

   caller.quit();
   worker.quit();

   REQUIRE(caught);
   REQUIRE(msg == "remote failure");
}

TEST_CASE("integration: thread::call propagates void exceptions", "[integration][exception]")
{
   psiber::thread worker("worker");

   bool caught = false;

   psiber::thread caller([&]() {
      try
      {
         worker.call([]() {
            throw std::logic_error("void failure");
         });
      }
      catch (const std::logic_error& e)
      {
         caught = true;
      }
   });

   caller.quit();
   worker.quit();

   REQUIRE(caught);
}

TEST_CASE("integration: thread::call works after exception", "[integration][exception]")
{
   psiber::thread worker("worker");

   int result = 0;
   bool caught = false;

   psiber::thread caller([&]() {
      // First call throws
      try
      {
         worker.call([]() -> int {
            throw std::runtime_error("fail");
         });
      }
      catch (...)
      {
         caught = true;
      }

      // Second call succeeds — channel isn't broken
      result = worker.call([]() { return 42; });
   });

   caller.quit();
   worker.quit();

   REQUIRE(caught);
   REQUIRE(result == 42);
}

TEST_CASE("integration: fiber_promise propagates exception", "[integration][exception]")
{
   auto sched = scheduler_access::make(440);

   bool caught = false;
   std::string msg;

   sched.spawnFiber([&]() {
      fiber_promise<int> promise;

      // Simulate: another fiber fulfills with an exception
      sched.spawnFiber([&]() {
         promise.set_exception(
            std::make_exception_ptr(std::runtime_error("async boom")));
      });

      if (promise.try_register_waiter(sched.currentFiber()))
         sched.parkCurrentFiber();

      try
      {
         promise.get();
      }
      catch (const std::runtime_error& e)
      {
         caught = true;
         msg    = e.what();
      }
   });

   sched.run();

   REQUIRE(caught);
   REQUIRE(msg == "async boom");
}

// ── End-to-end: fiber_mutex protecting shared state across fibers ────────────

TEST_CASE("integration: fiber_mutex protects cross-fiber state", "[integration]")
{
   auto sched = scheduler_access::make(430);

   fiber_mutex mtx;
   int shared_state = 0;

   constexpr int num_fibers    = 10;
   constexpr int ops_per_fiber = 100;

   for (int f = 0; f < num_fibers; ++f)
   {
      sched.spawnFiber([&]() {
         for (int i = 0; i < ops_per_fiber; ++i)
         {
            mtx.lock(sched);
            // Read-modify-write under lock (non-atomic intentionally)
            int val = shared_state;
            shared_state = val + 1;
            mtx.unlock();

            // Yield between iterations to create interleaving
            if (i % 10 == 0)
               sched.yieldCurrentFiber();
         }
      });
   }

   sched.run();
   REQUIRE(shared_state == num_fibers * ops_per_fiber);
}
