#include <catch2/catch.hpp>

#include <psiber/reactor.hpp>
#include <psiber/strand.hpp>

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace psiber;

// ── Reactor lifecycle tests ──────────────────────────────────────────────

TEST_CASE("reactor integration: clean shutdown with idle workers", "[reactor-integration]")
{
   reactor pool(4);
   std::this_thread::sleep_for(std::chrono::milliseconds{10});
   pool.stop();
   pool.join();
}

TEST_CASE("reactor integration: clean shutdown with active fibers", "[reactor-integration]")
{
   reactor pool(2);

   std::atomic<bool> started{false};

   pool.scheduler(0).spawnFiber([&]() {
      started.store(true, std::memory_order_relaxed);
      Scheduler::current()->sleep(std::chrono::milliseconds{60000});
   });

   while (!started.load(std::memory_order_relaxed))
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

   pool.stop();
   pool.join();
}

TEST_CASE("reactor integration: fibers run on pool workers", "[reactor-integration]")
{
   reactor pool(2);

   constexpr int N = 8;
   std::atomic<int> completed{0};

   for (int i = 0; i < N; ++i)
   {
      pool.scheduler(i % pool.num_threads()).spawnFiber([&]() {
         completed.fetch_add(1, std::memory_order_relaxed);
      });
   }

   std::this_thread::sleep_for(std::chrono::milliseconds{50});
   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
}

// ── Strand unit tests (queue semantics, no scheduler) ────────────────────

TEST_CASE("reactor integration: strand enqueue and release chain", "[reactor-integration]")
{
   // Test the raw strand queue mechanics without a scheduler
   strand s;
   detail::Fiber f1, f2, f3;

   // First enqueue — becomes active (standalone, returns fiber)
   detail::Fiber* local = s.enqueue(&f1);
   REQUIRE(local == &f1);
   REQUIRE(s.active() == &f1);

   // Second and third — wait behind f1
   REQUIRE(s.enqueue(&f2) == nullptr);
   REQUIRE(s.enqueue(&f3) == nullptr);
   REQUIRE(s.active() == &f1);

   // Release f1 → f2 becomes active (standalone, returns fiber)
   detail::Fiber* next = s.release();
   REQUIRE(next == &f2);
   REQUIRE(s.active() == &f2);

   // Release f2 → f3 becomes active
   next = s.release();
   REQUIRE(next == &f3);
   REQUIRE(s.active() == &f3);

   // Release f3 → empty
   next = s.release();
   REQUIRE(next == nullptr);
   REQUIRE(s.active() == nullptr);
}

TEST_CASE("reactor integration: reactor-bound strand enqueue posts to pool", "[reactor-integration]")
{
   reactor pool(2);
   strand s(pool);

   detail::Fiber f;

   // Enqueue on reactor-bound strand — should post to reactor, return nullptr
   detail::Fiber* local = s.enqueue(&f);
   REQUIRE(local == nullptr);  // reactor will handle it
   REQUIRE(s.active() == &f);

   // The strand should be poppable from the reactor
   strand* popped = pool.try_pop_strand();
   REQUIRE(popped == &s);

   pool.stop();
   pool.join();
}

TEST_CASE("reactor integration: reactor-bound strand release re-posts", "[reactor-integration]")
{
   reactor pool(2);
   strand s(pool);

   detail::Fiber f1, f2;

   s.enqueue(&f1);
   s.enqueue(&f2);

   // Pop the strand (f1 is active)
   strand* popped = pool.try_pop_strand();
   REQUIRE(popped == &s);

   // Release f1 — f2 becomes active, strand re-posts
   detail::Fiber* next = s.release();
   REQUIRE(next == &f2);
   REQUIRE(s.active() == &f2);

   // Strand should be poppable again
   popped = pool.try_pop_strand();
   REQUIRE(popped == &s);

   pool.stop();
   pool.join();
}
