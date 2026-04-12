#include <catch2/catch.hpp>

#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_tx_mutex.hpp>
#include <psiber/fiber_shared_mutex.hpp>
#include <psiber/scheduler.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace psiber;

// ── Wound-wait: holder detects wound and aborts ──────────────────────────────

TEST_CASE("fiber_tx_mutex: holder checks wounded and aborts", "[wound-wait][deadlock]")
{
   auto sched = scheduler_access::make(200);

   fiber_tx_mutex mtx;
   bool           younger_threw = false;
   bool           older_acquired = false;

   // Younger transaction (ts=200) acquires first
   sched.spawnFiber([&]() {
      auto* self         = sched.currentFiber();
      self->tx_timestamp = 200;
      mtx.lock(sched, 200);

      // Spawn older transaction (ts=100) that will wound us
      sched.spawnFiber([&]() {
         auto* self         = sched.currentFiber();
         self->tx_timestamp = 100;
         mtx.lock(sched, 100);
         older_acquired = true;
         mtx.unlock();
      });

      // Yield to let the older tx attempt lock (it wounds us)
      sched.yieldCurrentFiber();

      // Holder checks wounded — should throw
      try
      {
         fiber_tx_mutex::check_wounded(sched.currentFiber());
         // If we get here, wound wasn't detected
      }
      catch (const wound_exception&)
      {
         younger_threw = true;
         mtx.unlock();
      }
   });

   sched.run();
   REQUIRE(younger_threw == true);
   REQUIRE(older_acquired == true);
}

// ── Wound-wait: three transactions, circular pattern ─────────────────────────

TEST_CASE("fiber_tx_mutex: three transactions, no deadlock", "[wound-wait][deadlock]")
{
   auto sched = scheduler_access::make(201);

   // Three mutexes, three transactions
   fiber_tx_mutex mtx_a, mtx_b, mtx_c;
   std::atomic<int> completed{0};

   // T1 (ts=100, oldest): locks A, then B
   sched.spawnFiber([&]() {
      auto* self         = sched.currentFiber();
      self->tx_timestamp = 100;

      mtx_a.lock(sched, 100);
      sched.yieldCurrentFiber();  // let others start
      mtx_b.lock(sched, 100);

      mtx_b.unlock();
      mtx_a.unlock();
      completed.fetch_add(1, std::memory_order_relaxed);
   });

   // T2 (ts=200): locks B, then C
   sched.spawnFiber([&]() {
      auto* self         = sched.currentFiber();
      self->tx_timestamp = 200;

      bool done = false;
      while (!done)
      {
         try
         {
            mtx_b.lock(sched, 200);
            mtx_c.lock(sched, 200);
            mtx_c.unlock();
            mtx_b.unlock();
            done = true;
         }
         catch (const wound_exception&)
         {
            // Retry after being wounded
            self->wounded.store(false, std::memory_order_relaxed);
         }
      }
      completed.fetch_add(1, std::memory_order_relaxed);
   });

   // T3 (ts=300, youngest): locks C, then A
   sched.spawnFiber([&]() {
      auto* self         = sched.currentFiber();
      self->tx_timestamp = 300;

      bool done = false;
      while (!done)
      {
         try
         {
            mtx_c.lock(sched, 300);
            mtx_a.lock(sched, 300);
            mtx_a.unlock();
            mtx_c.unlock();
            done = true;
         }
         catch (const wound_exception&)
         {
            self->wounded.store(false, std::memory_order_relaxed);
         }
      }
      completed.fetch_add(1, std::memory_order_relaxed);
   });

   sched.run();
   REQUIRE(completed.load() == 3);
}

// ── fiber_shared_mutex: writer doesn't starve ────────────────────────────────

TEST_CASE("fiber_shared_mutex: writer eventually acquires despite readers", "[deadlock][mutex]")
{
   auto sched = scheduler_access::make(202);

   fiber_shared_mutex mtx;
   bool               writer_done = false;
   int                reader_batches = 0;

   // Writer: acquires after initial readers release
   sched.spawnFiber([&]() {
      // Wait for first reader batch to start
      sched.yieldCurrentFiber();

      mtx.lock(sched);
      writer_done = true;
      mtx.unlock();
   });

   // Spawn 4 readers that hold the lock briefly then release
   for (int i = 0; i < 4; ++i)
   {
      sched.spawnFiber([&]() {
         mtx.lock_shared(sched);
         reader_batches++;
         sched.yieldCurrentFiber();
         mtx.unlock_shared();
      });
   }

   sched.run();
   REQUIRE(writer_done == true);
   REQUIRE(reader_batches == 4);
}

// ── fiber_shared_mutex: readers blocked behind writer ────────────────────────

TEST_CASE("fiber_shared_mutex: new readers queue behind pending writer", "[deadlock][mutex]")
{
   auto sched = scheduler_access::make(203);

   fiber_shared_mutex mtx;
   std::vector<int>   order;

   // Reader 1: hold the shared lock
   sched.spawnFiber([&]() {
      mtx.lock_shared(sched);
      order.push_back(1);

      // Spawn writer while we hold shared lock
      sched.spawnFiber([&]() {
         mtx.lock(sched);  // will block — reader holds shared lock
         order.push_back(2);
         mtx.unlock();
      });

      sched.yieldCurrentFiber();  // let writer try to acquire

      // Spawn reader 2 AFTER writer is queued
      sched.spawnFiber([&]() {
         mtx.lock_shared(sched);
         order.push_back(3);
         mtx.unlock_shared();
      });

      sched.yieldCurrentFiber();  // let reader 2 try

      mtx.unlock_shared();  // release — writer should go next, not reader 2
   });

   sched.run();

   // Writer should execute before reader 2 (no reader starvation of writers)
   REQUIRE(order.size() == 3);
   REQUIRE(order[0] == 1);
   REQUIRE(order[1] == 2);
   REQUIRE(order[2] == 3);
}

// ── fiber_mutex: two fibers, two mutexes, opposite order ─────────────────────
// This test verifies that without wound-wait, the system can actually deadlock.
// With fiber_mutex (which has no deadlock prevention), this WOULD deadlock
// if both fibers managed to acquire their first lock simultaneously.
// In cooperative scheduling, this can't happen unless fibers yield between
// lock acquisitions. This test demonstrates the cooperative scheduling property.

TEST_CASE("fiber_mutex: opposite order doesn't deadlock with cooperative scheduling", "[deadlock][mutex]")
{
   auto sched = scheduler_access::make(204);

   fiber_mutex mtx_a, mtx_b;
   int         result = 0;

   // Fiber 1: lock A then B
   sched.spawnFiber([&]() {
      mtx_a.lock(sched);
      mtx_b.lock(sched);
      result += 1;
      mtx_b.unlock();
      mtx_a.unlock();
   });

   // Fiber 2: lock B then A (opposite order)
   sched.spawnFiber([&]() {
      mtx_b.lock(sched);
      mtx_a.lock(sched);
      result += 2;
      mtx_a.unlock();
      mtx_b.unlock();
   });

   sched.run();

   // Both should complete (cooperative scheduling means fiber 1 runs to
   // completion before fiber 2 starts, no interleaving)
   REQUIRE(result == 3);
}

// ── fiber_tx_mutex: wound-wait prevents deadlock with opposite order ─────────

TEST_CASE("fiber_tx_mutex: opposite order resolved by wound-wait", "[wound-wait][deadlock]")
{
   auto sched = scheduler_access::make(205);

   fiber_tx_mutex mtx_a, mtx_b;
   std::atomic<int> completed{0};

   // Older transaction (ts=100): lock A, yield, then lock B
   sched.spawnFiber([&]() {
      auto* self         = sched.currentFiber();
      self->tx_timestamp = 100;

      mtx_a.lock(sched, 100);
      sched.yieldCurrentFiber();  // let younger start
      mtx_b.lock(sched, 100);
      mtx_b.unlock();
      mtx_a.unlock();
      completed.fetch_add(1, std::memory_order_relaxed);
   });

   // Younger transaction (ts=200): lock B, yield, then lock A
   sched.spawnFiber([&]() {
      auto* self         = sched.currentFiber();
      self->tx_timestamp = 200;

      bool done = false;
      while (!done)
      {
         try
         {
            mtx_b.lock(sched, 200);
            sched.yieldCurrentFiber();

            // Check if wounded before trying second lock
            fiber_tx_mutex::check_wounded(sched.currentFiber());

            mtx_a.lock(sched, 200);
            mtx_a.unlock();
            mtx_b.unlock();
            done = true;
         }
         catch (const wound_exception&)
         {
            mtx_b.unlock();  // Release B before retrying
            self->wounded.store(false, std::memory_order_relaxed);
         }
      }
      completed.fetch_add(1, std::memory_order_relaxed);
   });

   sched.run();
   REQUIRE(completed.load() == 2);
}
