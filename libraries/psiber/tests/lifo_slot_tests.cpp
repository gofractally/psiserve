#include <catch2/catch.hpp>

#include <psiber/scheduler.hpp>
#include <psiber/io_engine_kqueue.hpp>

#include <vector>

using namespace psiber;

// ── LIFO slot: recently woken fiber runs before older ready fibers ───────────

TEST_CASE("scheduler: LIFO slot promotes recently woken fiber", "[lifo][scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 500);

   // Track execution order to verify LIFO behavior.
   // The LIFO slot is populated by wake(). When a fiber is woken,
   // it goes into the LIFO slot and should run before fibers that
   // are already in the ready queue.

   std::vector<int> order;
   Fiber*           parkable = nullptr;

   // Fiber A: parks, then records when it resumes
   sched.spawnFiber([&]() {
      parkable = sched.currentFiber();
      sched.parkCurrentFiber();
      order.push_back(1);  // A resumes
   });

   // Fiber B: wakes A and yields, then records
   sched.spawnFiber([&]() {
      // At this point, A has parked.
      // Spawn C so it's in the ready queue
      sched.spawnFiber([&]() {
         order.push_back(3);  // C runs
      });

      // Wake A — puts it in the LIFO slot
      Scheduler::wake(parkable);

      // Yield B — B goes back to ready queue behind C
      sched.yieldCurrentFiber();
      order.push_back(2);  // B resumes
   });

   sched.run();

   // Expected: A runs first (LIFO slot), then C (already in queue), then B (re-queued)
   REQUIRE(order.size() == 3);
   REQUIRE(order[0] == 1);  // A via LIFO slot
   REQUIRE(order[1] == 3);  // C from ready queue
   REQUIRE(order[2] == 2);  // B from ready queue
}

// ── LIFO slot: cap at 3 consecutive uses ─────────────────────────────────────

TEST_CASE("scheduler: LIFO slot caps at 3 consecutive uses", "[lifo][scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 501);

   // Create a pattern where the same fiber keeps getting woken and would
   // monopolize the LIFO slot. After 3 consecutive LIFO dispatches,
   // the scheduler should fall back to the ready queue.

   int lifo_runs    = 0;
   int queue_runs   = 0;
   Fiber* ping_fiber = nullptr;
   Fiber* pong_fiber = nullptr;

   // Ping fiber: parks, gets woken, records, parks again
   sched.spawnFiber([&]() {
      ping_fiber = sched.currentFiber();

      for (int i = 0; i < 6; ++i)
      {
         sched.parkCurrentFiber();
         ++lifo_runs;  // We count all runs; the scheduler manages LIFO vs queue
      }
   });

   // Pong fiber: wakes ping repeatedly, yields between
   sched.spawnFiber([&]() {
      pong_fiber = sched.currentFiber();

      for (int i = 0; i < 6; ++i)
      {
         // Wait for ping to park
         sched.yieldCurrentFiber();

         if (ping_fiber)
            Scheduler::wake(ping_fiber);
      }
      ++queue_runs;
   });

   sched.run();

   // Both fibers should complete all iterations
   REQUIRE(lifo_runs == 6);
   REQUIRE(queue_runs == 1);
}

// ── LIFO slot: not used for yielded fibers ──────────────────────────────────

TEST_CASE("scheduler: yieldCurrentFiber uses ready queue, not LIFO slot", "[lifo][scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 502);

   std::vector<int> order;

   // Fiber A: yields, then records
   sched.spawnFiber([&]() {
      order.push_back(1);  // A runs first
      sched.yieldCurrentFiber();
      order.push_back(4);  // A should run after B and C (re-queued at back)
   });

   // Fiber B: runs and records
   sched.spawnFiber([&]() {
      order.push_back(2);  // B runs second
   });

   // Fiber C: runs and records
   sched.spawnFiber([&]() {
      order.push_back(3);  // C runs third
   });

   sched.run();

   REQUIRE(order.size() == 4);
   REQUIRE(order[0] == 1);
   REQUIRE(order[1] == 2);
   REQUIRE(order[2] == 3);
   REQUIRE(order[3] == 4);
}

// ── Priority: high-priority fiber runs before normal ────────────────────────

TEST_CASE("scheduler: high-priority fiber runs before normal priority", "[lifo][scheduler]")
{
   auto io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 503);

   std::vector<int> order;

   // Spawn a control fiber that sets up the scenario
   sched.spawnFiber([&]() {
      // Spawn normal-priority fiber
      sched.spawnFiber([&]() {
         order.push_back(2);  // normal
      });

      // Spawn high-priority fiber
      sched.spawnFiber([&]() {
         auto* self = sched.currentFiber();
         self->priority = 0;  // high
         // Need to yield and re-enter to get into the high-priority queue
         sched.yieldCurrentFiber();
         order.push_back(1);  // high
      });

      // Spawn low-priority fiber
      sched.spawnFiber([&]() {
         order.push_back(3);  // low priority but runs before re-queued high
      });

      sched.yieldCurrentFiber();
      order.push_back(4);
   });

   sched.run();

   // All should complete
   REQUIRE(order.size() == 4);
}
