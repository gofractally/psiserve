#include <catch2/catch.hpp>

#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_promise.hpp>
#include <psiber/send_queue.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/io_engine_kqueue.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace psiber;

// ── Multiple park/wake cycles on same fiber ─────────────────────────────────

TEST_CASE("scheduler: multiple park/wake cycles on same fiber", "[edge][wake]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 300);

   constexpr int cycles = 50;
   int           count  = 0;
   Fiber*        fiber_ptr = nullptr;

   sched.spawnFiber([&]() {
      fiber_ptr = sched.currentFiber();
      for (int i = 0; i < cycles; ++i)
      {
         sched.parkCurrentFiber();
         ++count;
      }
   });

   // Waker fiber: wake the parked fiber each cycle
   sched.spawnFiber([&]() {
      for (int i = 0; i < cycles; ++i)
      {
         // Yield to let the other fiber park
         sched.yieldCurrentFiber();
         if (fiber_ptr)
            Scheduler::wake(fiber_ptr);
      }
   });

   sched.run();
   REQUIRE(count == cycles);
}

// ── fiber_promise: fulfilled before parking ─────────────────────────────────

TEST_CASE("fiber_promise: value set before get (no parking needed)", "[edge][promise]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 301);

   int result = 0;

   sched.spawnFiber([&]() {
      fiber_promise<int> promise;
      // Set value immediately (before any park)
      promise.set_value(42);
      REQUIRE(promise.is_ready());
      result = promise.get();
   });

   sched.run();
   REQUIRE(result == 42);
}

// ── SendQueue: emplace with large functor ───────────────────────────────────

TEST_CASE("SendQueue: large capture fills ring correctly", "[edge][send_queue]")
{
   SendQueue sq(4096);

   // Create a functor with a ~200-byte capture
   std::array<char, 200> payload;
   payload.fill('X');

   int call_count = 0;
   auto* slot = sq.emplace([payload, &call_count]() {
      // Verify the capture survived placement-new
      for (auto c : payload)
         REQUIRE(c == 'X');
      ++call_count;
   });

   REQUIRE(slot != nullptr);

   // Execute it manually
   void* p = slot + 1;
   slot->run(p);
   slot->destroy(p);
   slot->consumed.store(true, std::memory_order_relaxed);
   sq.reclaim();

   REQUIRE(call_count == 1);
}

// ── SendQueue: back-to-back reclaim cycles ──────────────────────────────────

TEST_CASE("SendQueue: multiple reclaim cycles reuse ring space", "[edge][send_queue]")
{
   SendQueue sq(512);

   constexpr int rounds = 20;
   int           total  = 0;

   for (int r = 0; r < rounds; ++r)
   {
      auto* slot = sq.emplace([&total]() { ++total; });
      REQUIRE(slot != nullptr);

      // Simulate receiver executing the task
      void* p = slot + 1;
      slot->run(p);
      slot->destroy(p);
      slot->consumed.store(true, std::memory_order_relaxed);
      sq.reclaim();
   }

   REQUIRE(total == rounds);
}

// ── scheduler: interrupt wakes parked fibers ────────────────────────────────

TEST_CASE("scheduler: interrupt wakes all blocked/parked fibers", "[edge][scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 302);

   bool fiber1_resumed = false;
   bool fiber2_resumed = false;

   // Fiber that parks indefinitely
   sched.spawnFiber([&]() {
      try
      {
         sched.parkCurrentFiber();
         fiber1_resumed = true;
      }
      catch (...)
      {
         fiber1_resumed = true;  // shutdown_exception
      }
   });

   // Fiber that sleeps a long time
   sched.spawnFiber([&]() {
      try
      {
         sched.sleep(std::chrono::milliseconds{60000});
         fiber2_resumed = true;
      }
      catch (...)
      {
         fiber2_resumed = true;
      }
   });

   // Fiber that triggers interrupt after a short delay
   sched.spawnFiber([&]() {
      sched.yieldCurrentFiber();
      sched.interrupt();
   });

   sched.run();
   REQUIRE(fiber1_resumed == true);
   REQUIRE(fiber2_resumed == true);
}

// ── fiber_mutex: try_lock returns false when held ───────────────────────────

TEST_CASE("fiber_mutex: try_lock fails when another fiber holds lock", "[edge][mutex]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 303);

   fiber_mutex mtx;
   bool        try_result = true;

   sched.spawnFiber([&]() {
      mtx.lock(sched);

      // Spawn a second fiber that tries to lock
      sched.spawnFiber([&]() {
         try_result = mtx.try_lock();
      });

      sched.yieldCurrentFiber();
      mtx.unlock();
   });

   sched.run();
   REQUIRE(try_result == false);
}

// ── fiber_promise: cross-thread fulfillment with large value ────────────────

TEST_CASE("fiber_promise: cross-thread with string value", "[edge][promise]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 304);

   std::string result;

   sched.spawnFiber([&]() {
      fiber_promise<std::string> promise;
      promise.waiting_fiber = sched.currentFiber();

      std::thread fulfiller([&]() {
         promise.set_value("hello from another thread");
         Scheduler::wake(promise.waiting_fiber);
      });

      sched.parkCurrentFiber();
      result = promise.get();
      fulfiller.join();
   });

   sched.run();
   REQUIRE(result == "hello from another thread");
}

// ── scheduler: spawn inside fiber ───────────────────────────────────────────

TEST_CASE("scheduler: fiber spawning child fibers", "[edge][scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 305);

   std::vector<int> order;

   sched.spawnFiber([&]() {
      order.push_back(1);

      // Spawn two children from inside a running fiber
      sched.spawnFiber([&]() { order.push_back(2); });
      sched.spawnFiber([&]() { order.push_back(3); });

      // Yield to let children run
      sched.yieldCurrentFiber();
      order.push_back(4);
   });

   sched.run();

   REQUIRE(order.size() == 4);
   REQUIRE(order[0] == 1);
   // Children should run before parent resumes (parent is at back of ready queue)
   REQUIRE(order[1] == 2);
   REQUIRE(order[2] == 3);
   REQUIRE(order[3] == 4);
}

// ── scheduler: many fibers complete without leaking ─────────────────────────

TEST_CASE("scheduler: 100 fibers complete cleanly", "[edge][scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 306);

   std::atomic<int> completed{0};

   for (int i = 0; i < 100; ++i)
   {
      sched.spawnFiber([&]() {
         completed.fetch_add(1, std::memory_order_relaxed);
      });
   }

   sched.run();
   REQUIRE(completed.load() == 100);
}
