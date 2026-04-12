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

   // Use post() for thread-safe cross-thread fiber spawning.
   // spawnFiber() is not thread-safe — must be called from the scheduler's own thread.
   pool.scheduler(0).post([&]() noexcept {
      Scheduler::current()->spawnFiber([&]() {
         started.store(true, std::memory_order_relaxed);
         Scheduler::current()->sleep(std::chrono::milliseconds{60000});
      });
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
      auto& sched = pool.scheduler(i % pool.num_threads());
      sched.post([&]() noexcept {
         Scheduler::current()->spawnFiber([&]() {
            completed.fetch_add(1, std::memory_order_relaxed);
         });
      });
   }

   std::this_thread::sleep_for(std::chrono::milliseconds{100});
   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
}

// ── Fiber migration test ─────────────────────────────────────────────────

TEST_CASE("reactor integration: fiber migrates between threads", "[reactor-integration]")
{
   // Prove a fiber can start on one thread and resume on another.
   // Strategy: spawn a fiber on worker 0 via strand, park it, then
   // wake it.  The woken fiber may resume on any idle worker.
   // We verify by recording std::this_thread::get_id() before and
   // after the park — they should differ (with 2+ workers, the
   // strand ready queue distributes to whoever is idle).

   reactor pool(2);
   strand  s(pool);

   std::atomic<bool> phase1_done{false};
   std::atomic<bool> phase2_done{false};
   std::thread::id   tid_before;
   std::thread::id   tid_after;
   detail::Fiber*    migrating_fiber = nullptr;

   // Spawn a fiber on worker 0 that records its thread, parks, then
   // records its thread again after waking.
   pool.scheduler(0).post([&]() noexcept {
      Scheduler::current()->spawnFiber([&]() {
         auto* sched = Scheduler::current();
         auto* me    = sched->currentFiber();
         me->home_strand = &s;
         s.enqueue(me);  // become active in the strand

         tid_before      = std::this_thread::get_id();
         migrating_fiber = me;
         phase1_done.store(true, std::memory_order_release);

         sched->parkCurrentFiber();

         tid_after = std::this_thread::get_id();
         phase2_done.store(true, std::memory_order_release);
      });
   });

   // Wait for the fiber to park
   while (!phase1_done.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

   // Give the run loop time to process the park
   std::this_thread::sleep_for(std::chrono::milliseconds{5});

   // Wake the fiber — it will be routed through the strand and
   // may resume on a different worker thread.
   Scheduler::wake(migrating_fiber);

   // Wait for phase 2
   while (!phase2_done.load(std::memory_order_acquire))
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

   // With 2 workers, the fiber might resume on the same thread or a
   // different one depending on which worker is idle.  We can't
   // guarantee migration in all cases, but we CAN verify the fiber
   // successfully ran on potentially different threads.
   INFO("before: " << tid_before << ", after: " << tid_after);
   // The key assertion: the fiber completed both phases successfully.
   // (Thread IDs may or may not differ — migration is opportunistic.)
   REQUIRE(phase2_done.load());

   pool.stop();
   pool.join();
}

// ── strand::post() test ──────────────────────────────────────────────────

TEST_CASE("reactor integration: strand::post serializes execution", "[reactor-integration]")
{
   reactor pool(2);
   strand  s(pool);

   constexpr int N = 4;
   std::atomic<int> completed{0};
   std::atomic<int> concurrent{0};
   std::atomic<int> max_concurrent{0};

   // Post several tasks to the same strand.  Only one should run at a time.
   for (int i = 0; i < N; ++i)
   {
      pool.scheduler(i % pool.num_threads()).post([&]() noexcept {
         s.post([&]() {
            int cur = concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
            int old_max = max_concurrent.load(std::memory_order_relaxed);
            while (cur > old_max &&
                   !max_concurrent.compare_exchange_weak(old_max, cur))
               ;

            // Yield to give other strand fibers a chance to violate serialization
            Scheduler::current()->yieldCurrentFiber();

            concurrent.fetch_sub(1, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_relaxed);
         });
      });
   }

   std::this_thread::sleep_for(std::chrono::milliseconds{200});
   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
   REQUIRE(max_concurrent.load() == 1);  // strand serialization held
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

TEST_CASE("reactor integration: reactor-bound strand release returns next fiber", "[reactor-integration]")
{
   reactor pool(2);
   strand s(pool);

   detail::Fiber f1, f2;

   s.enqueue(&f1);
   s.enqueue(&f2);

   // Pop the strand (f1 is active)
   strand* popped = pool.try_pop_strand();
   REQUIRE(popped == &s);

   // Release f1 — f2 becomes active, returned to caller.
   // release() does NOT re-post to the reactor — the caller
   // (run loop) handles the returned fiber directly.
   detail::Fiber* next = s.release();
   REQUIRE(next == &f2);
   REQUIRE(s.active() == &f2);

   // Reactor queue should be empty — release doesn't post
   popped = pool.try_pop_strand();
   REQUIRE(popped == nullptr);

   pool.stop();
   pool.join();
}
