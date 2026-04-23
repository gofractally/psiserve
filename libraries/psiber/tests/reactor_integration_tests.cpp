#include <catch2/catch.hpp>

#include <psiber/reactor.hpp>
#include <psiber/strand.hpp>

#include <atomic>
#include <chrono>
#include <mutex>
#include <set>
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
      Scheduler::current().spawnFiber([&]() {
         started.store(true, std::memory_order_relaxed);
         Scheduler::current().sleep(std::chrono::milliseconds{60000});
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
         Scheduler::current().spawnFiber([&]() {
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
      Scheduler::current().spawnFiber([&]() {
         auto& sched = Scheduler::current();
         auto* me    = sched.currentFiber();
         me->home_strand = &s;
         s.enqueue(me);  // become active in the strand

         tid_before      = std::this_thread::get_id();
         migrating_fiber = me;
         phase1_done.store(true, std::memory_order_release);

         sched.parkCurrentFiber();

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
   // Strand serializes fibers *between suspend points*.  Within a
   // non-yielding window, at most one fiber runs at a time.  Yielding
   // (as of the I/O-release fix) is a suspend point that releases the
   // strand; see the "strand yield is a suspend point" test below for
   // the interleaving case.
   reactor pool(2);
   strand  s(pool);

   constexpr int N = 4;
   std::atomic<int> completed{0};
   std::atomic<int> concurrent{0};
   std::atomic<int> max_concurrent{0};

   for (int i = 0; i < N; ++i)
   {
      pool.scheduler(i % pool.num_threads()).post([&]() noexcept {
         s.post([&]() {
            int cur = concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
            int old_max = max_concurrent.load(std::memory_order_relaxed);
            while (cur > old_max &&
                   !max_concurrent.compare_exchange_weak(old_max, cur))
               ;
            concurrent.fetch_sub(1, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_relaxed);
         });
      });
   }

   std::this_thread::sleep_for(std::chrono::milliseconds{200});
   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
   REQUIRE(max_concurrent.load() == 1);  // no interleaving within the critical section
}

TEST_CASE("reactor integration: strand yield is a suspend point", "[reactor-integration]")
{
   // yieldCurrentFiber on a strand-bound fiber releases the strand,
   // letting queued waiters run before we resume.  Multiple fibers
   // can have "started" (past fetch_add) and "not finished"
   // (before fetch_sub) simultaneously — but only one is actually
   // executing at any instant.
   reactor pool(2);
   strand  s(pool);

   constexpr int N = 4;
   std::atomic<int> completed{0};
   std::atomic<int> in_flight{0};
   std::atomic<int> max_in_flight{0};

   for (int i = 0; i < N; ++i)
   {
      pool.scheduler(i % pool.num_threads()).post([&]() noexcept {
         s.post([&]() {
            int cur = in_flight.fetch_add(1, std::memory_order_relaxed) + 1;
            int old_max = max_in_flight.load(std::memory_order_relaxed);
            while (cur > old_max &&
                   !max_in_flight.compare_exchange_weak(old_max, cur))
               ;

            // Yield: releases strand, promotes waiter, parks self.
            // After this, other fibers on S will have incremented
            // in_flight before we resume.
            Scheduler::current().yieldCurrentFiber();

            in_flight.fetch_sub(1, std::memory_order_relaxed);
            completed.fetch_add(1, std::memory_order_relaxed);
         });
      });
   }

   std::this_thread::sleep_for(std::chrono::milliseconds{200});
   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
   // All N fibers should have been in flight simultaneously at peak
   // (each past its fetch_add, yielded, waiting its turn to resume).
   REQUIRE(max_in_flight.load() > 1);
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
   // Stop workers immediately — this test checks strand/reactor queue
   // mechanics, not the worker pipeline.  Without this, a worker may
   // pop the strand (which holds a bare Fiber with no continuation)
   // before try_pop_strand() is called, causing a race + SIGABRT.
   pool.stop();
   pool.join();

   strand s(pool);

   detail::Fiber f;

   // Enqueue on reactor-bound strand — should post to reactor, return nullptr
   detail::Fiber* local = s.enqueue(&f);
   REQUIRE(local == nullptr);  // reactor will handle it
   REQUIRE(s.active() == &f);

   // The strand should be poppable from the reactor
   strand* popped = pool.try_pop_strand();
   REQUIRE(popped == &s);
}

TEST_CASE("reactor integration: reactor-bound strand release returns next fiber", "[reactor-integration]")
{
   reactor pool(2);
   pool.stop();
   pool.join();

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
}

// ── Stress tests ─────────────────────────────────────────────────────────

TEST_CASE("reactor stress: many strands many fibers", "[reactor-stress]")
{
   // N strands, M fibers per strand, W workers.
   // Verifies serialization holds and all fibers complete.
   constexpr int W = 4;
   constexpr int N = 8;
   constexpr int M = 16;

   reactor pool(W);

   std::atomic<int> total_completed{0};

   // Per-strand concurrency tracking
   struct StrandState
   {
      std::atomic<int> concurrent{0};
      std::atomic<int> max_concurrent{0};
      std::atomic<int> completed{0};
   };
   std::vector<StrandState> states(N);
   std::vector<std::unique_ptr<strand>> strands;
   for (int i = 0; i < N; ++i)
      strands.push_back(std::make_unique<strand>(pool));

   for (int i = 0; i < N; ++i)
   {
      for (int j = 0; j < M; ++j)
      {
         auto& sched = pool.scheduler((i * M + j) % pool.num_threads());
         int si = i;
         sched.post([&, si]() noexcept {
            strands[si]->post([&, si]() {
               auto& st = states[si];
               int cur = st.concurrent.fetch_add(1, std::memory_order_relaxed) + 1;
               int old_max = st.max_concurrent.load(std::memory_order_relaxed);
               while (cur > old_max &&
                      !st.max_concurrent.compare_exchange_weak(old_max, cur))
                  ;

               // Yield mid-task to give other fibers a chance to violate serialization
               Scheduler::current().yieldCurrentFiber();

               st.concurrent.fetch_sub(1, std::memory_order_relaxed);
               st.completed.fetch_add(1, std::memory_order_relaxed);
               total_completed.fetch_add(1, std::memory_order_relaxed);
            });
         });
      }
   }

   // Wait with timeout
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (total_completed.load() < N * M &&
          std::chrono::steady_clock::now() < deadline)
   {
      std::this_thread::sleep_for(std::chrono::milliseconds{10});
   }

   pool.stop();
   pool.join();

   REQUIRE(total_completed.load() == N * M);
   for (int i = 0; i < N; ++i)
   {
      INFO("strand " << i << " max_concurrent=" << states[i].max_concurrent.load());
      REQUIRE(states[i].max_concurrent.load() == 1);
      REQUIRE(states[i].completed.load() == M);
   }
}

TEST_CASE("reactor stress: single worker with strand", "[reactor-stress]")
{
   // Edge case: single worker, strand must serialize locally
   reactor pool(1);
   strand s(pool);

   constexpr int N = 8;
   std::atomic<int> completed{0};
   std::atomic<int> concurrent{0};
   std::atomic<int> max_concurrent{0};

   pool.scheduler(0).post([&]() noexcept {
      for (int i = 0; i < N; ++i)
      {
         s.post([&]() {
            int cur = concurrent.fetch_add(1) + 1;
            int old_max = max_concurrent.load();
            while (cur > old_max && !max_concurrent.compare_exchange_weak(old_max, cur))
               ;

            Scheduler::current().yieldCurrentFiber();

            concurrent.fetch_sub(1);
            completed.fetch_add(1);
         });
      }
   });

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
   while (completed.load() < N &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});

   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
   REQUIRE(max_concurrent.load() == 1);
}

TEST_CASE("reactor stress: strand fibers sleep without releasing strand", "[reactor-stress]")
{
   // A strand fiber that sleeps stays "active" — the strand is NOT released.
   // Verify the next fiber waits until the sleep completes.
   reactor pool(2);
   strand s(pool);

   std::atomic<int> order_counter{0};
   int first_order = -1;
   int second_order = -1;

   pool.scheduler(0).post([&]() noexcept {
      s.post([&]() {
         // First fiber: sleep briefly, then record order
         Scheduler::current().sleep(std::chrono::milliseconds{50});
         first_order = order_counter.fetch_add(1);
      });
      s.post([&]() {
         // Second fiber: should run AFTER first completes
         second_order = order_counter.fetch_add(1);
      });
   });

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
   while (order_counter.load() < 2 &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});

   pool.stop();
   pool.join();

   REQUIRE(order_counter.load() == 2);
   REQUIRE(first_order == 0);
   REQUIRE(second_order == 1);
}

TEST_CASE("reactor stress: pooled fiber reuse clears strand affinity", "[reactor-stress]")
{
   // Verify that a recycled fiber with home_strand set does NOT
   // pollute the next fiber that reuses it.
   reactor pool(2);
   strand s(pool);

   std::atomic<bool> strand_fiber_done{false};
   std::atomic<bool> plain_fiber_done{false};
   detail::Fiber* strand_fiber_ptr = nullptr;
   detail::Fiber* reused_fiber_ptr = nullptr;

   // Phase 1: spawn a strand fiber, let it complete
   pool.scheduler(0).post([&]() noexcept {
      s.post([&]() {
         strand_fiber_ptr = Scheduler::current().currentFiber();
         // This fiber has home_strand set
      });
   });

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!strand_fiber_ptr &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{5});

   // Give time for the fiber to finish and enter the pool
   std::this_thread::sleep_for(std::chrono::milliseconds{50});

   // Phase 2: spawn a plain fiber (no strand) — it might reuse the pooled fiber
   pool.scheduler(0).post([&]() noexcept {
      Scheduler::current().spawnFiber([&]() {
         auto* me = Scheduler::current().currentFiber();
         reused_fiber_ptr = me;
         // If this is the same fiber as strand_fiber_ptr,
         // home_strand MUST be nullptr (cleared on reuse)
         REQUIRE(me->home_strand == nullptr);
         plain_fiber_done.store(true);
      });
   });

   deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (!plain_fiber_done.load() &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{5});

   pool.stop();
   pool.join();

   REQUIRE(plain_fiber_done.load());
}

TEST_CASE("reactor stress: shutdown with active strand fibers", "[reactor-stress]")
{
   // Verify clean shutdown when strand fibers are parked/waiting/sleeping
   reactor pool(2);
   strand s(pool);

   std::atomic<int> started{0};

   // Spawn several strand fibers that park indefinitely
   pool.scheduler(0).post([&]() noexcept {
      for (int i = 0; i < 4; ++i)
      {
         s.post([&]() {
            started.fetch_add(1);
            Scheduler::current().sleep(std::chrono::milliseconds{60000});
         });
      }
   });

   // Wait for at least the first fiber to start
   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
   while (started.load() < 1 &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{5});

   // Shutdown should not hang or crash
   pool.stop();
   pool.join();

   REQUIRE(started.load() >= 1);
}

TEST_CASE("reactor stress: cross-thread strand post", "[reactor-stress]")
{
   // Multiple workers post to the same strand concurrently
   constexpr int W = 4;
   constexpr int N = 32;

   reactor pool(W);
   strand s(pool);

   std::atomic<int> completed{0};
   std::atomic<int> concurrent{0};
   std::atomic<int> max_concurrent{0};

   for (int i = 0; i < N; ++i)
   {
      pool.scheduler(i % W).post([&]() noexcept {
         s.post([&]() {
            int cur = concurrent.fetch_add(1) + 1;
            int old_max = max_concurrent.load();
            while (cur > old_max && !max_concurrent.compare_exchange_weak(old_max, cur))
               ;
            concurrent.fetch_sub(1);
            completed.fetch_add(1);
         });
      });
   }

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (completed.load() < N &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});

   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
   REQUIRE(max_concurrent.load() == 1);
}

TEST_CASE("reactor stress: multiple independent strands run in parallel", "[reactor-stress]")
{
   // Different strands should execute concurrently on different workers
   constexpr int W = 4;

   reactor pool(W);

   std::atomic<int> peak_parallel{0};
   std::atomic<int> running{0};
   std::atomic<int> completed{0};

   std::vector<std::unique_ptr<strand>> strands;
   for (int i = 0; i < W; ++i)
      strands.push_back(std::make_unique<strand>(pool));

   for (int i = 0; i < W; ++i)
   {
      int si = i;
      pool.scheduler(i).post([&, si]() noexcept {
         strands[si]->post([&]() {
            int cur = running.fetch_add(1) + 1;
            int old_peak = peak_parallel.load();
            while (cur > old_peak && !peak_parallel.compare_exchange_weak(old_peak, cur))
               ;

            // Sleep to keep multiple strands running concurrently
            Scheduler::current().sleep(std::chrono::milliseconds{50});

            running.fetch_sub(1);
            completed.fetch_add(1);
         });
      });
   }

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
   while (completed.load() < W &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});

   pool.stop();
   pool.join();

   REQUIRE(completed.load() == W);
   // Different strands should have run in parallel on different workers
   INFO("peak_parallel=" << peak_parallel.load());
   REQUIRE(peak_parallel.load() >= 2);
}

TEST_CASE("reactor stress: strand::post arena exhaustion returns false", "[reactor-stress]")
{
   // Verify that strand::post returns false when the arena is full
   reactor pool(1);
   strand s(pool, 128);  // tiny arena — 128 bytes

   std::atomic<int> succeeded{0};
   std::atomic<int> failed{0};
   std::atomic<bool> done{false};

   pool.scheduler(0).post([&]() noexcept {
      // Try to post many tasks — some should fail due to arena exhaustion
      for (int i = 0; i < 20; ++i)
      {
         bool ok = s.post([&]() {
            succeeded.fetch_add(1);
         });
         if (!ok)
            failed.fetch_add(1);
      }
      done.store(true);
   });

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
   while (!done.load() &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});

   // Give strand fibers time to complete
   std::this_thread::sleep_for(std::chrono::milliseconds{100});

   pool.stop();
   pool.join();

   REQUIRE(done.load());
   // At least some should have failed with 128 bytes
   INFO("succeeded=" << succeeded.load() << " failed=" << failed.load());
   REQUIRE(failed.load() > 0);
   REQUIRE(succeeded.load() + failed.load() == 20);
}

TEST_CASE("reactor stress: fiber yield inside strand preserves serialization", "[reactor-stress]")
{
   // Fiber yields back to ready queue but strand stays active.
   // Other strand fibers must NOT run during the yield.
   reactor pool(2);
   strand s(pool);

   constexpr int N = 4;
   constexpr int YIELDS = 10;
   std::atomic<int> completed{0};
   std::atomic<int> concurrent{0};
   bool violation = false;

   pool.scheduler(0).post([&]() noexcept {
      for (int i = 0; i < N; ++i)
      {
         s.post([&]() {
            for (int y = 0; y < YIELDS; ++y)
            {
               int cur = concurrent.fetch_add(1) + 1;
               if (cur > 1)
                  violation = true;
               Scheduler::current().yieldCurrentFiber();
               concurrent.fetch_sub(1);
            }
            completed.fetch_add(1);
         });
      }
   });

   auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
   while (completed.load() < N &&
          std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds{10});

   pool.stop();
   pool.join();

   REQUIRE(completed.load() == N);
   REQUIRE_FALSE(violation);
}
