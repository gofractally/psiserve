#include <catch2/catch.hpp>

#include <psiber/reactor.hpp>
#include <psiber/strand.hpp>

#include <atomic>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace psiber;

// ── Arena allocator tests ───────────────────────────────────────────────

TEST_CASE("strand arena: basic alloc and free", "[strand]")
{
   strand s(4096);

   void* p1 = s.alloc(64);
   REQUIRE(p1 != nullptr);

   void* p2 = s.alloc(64);
   REQUIRE(p2 != nullptr);
   REQUIRE(p2 != p1);

   s.free(p1);
   s.free(p2);
}

TEST_CASE("strand arena: alloc-after-free reuses memory", "[strand]")
{
   strand s(256);

   void* p1 = s.alloc(64);
   REQUIRE(p1 != nullptr);
   s.free(p1);

   // Next alloc should reuse the freed block
   void* p2 = s.alloc(64);
   REQUIRE(p2 != nullptr);
   // p2 should be the same as p1 (reused from free list)
   REQUIRE(p2 == p1);
}

TEST_CASE("strand arena: exhaustion returns nullptr", "[strand]")
{
   // Tiny arena — just enough for one small allocation
   // sizeof(Block) = 8, total = (8 + 16 + 15) & ~15 = 32
   strand s(64);

   void* p1 = s.alloc(16);
   REQUIRE(p1 != nullptr);

   void* p2 = s.alloc(16);
   REQUIRE(p2 != nullptr);

   // Arena should be full (2 * 32 = 64)
   void* p3 = s.alloc(16);
   REQUIRE(p3 == nullptr);
}

TEST_CASE("strand arena: free then alloc after exhaustion", "[strand]")
{
   strand s(64);

   void* p1 = s.alloc(16);
   void* p2 = s.alloc(16);
   REQUIRE(s.alloc(16) == nullptr);  // full

   s.free(p1);

   // Should succeed now — reuses freed block
   void* p3 = s.alloc(16);
   REQUIRE(p3 != nullptr);
   REQUIRE(p3 == p1);
}

TEST_CASE("strand arena: cross-thread free", "[strand]")
{
   strand s(4096);

   constexpr int  num_allocs  = 32;
   constexpr int  num_threads = 4;

   // Allocate on the main thread
   std::vector<void*> ptrs;
   for (int i = 0; i < num_allocs; ++i)
   {
      void* p = s.alloc(32);
      REQUIRE(p != nullptr);
      ptrs.push_back(p);
   }

   // Free from multiple threads
   std::atomic<int> freed{0};
   std::vector<std::thread> threads;
   int per_thread = num_allocs / num_threads;

   for (int t = 0; t < num_threads; ++t)
   {
      threads.emplace_back([&, t]() {
         for (int i = t * per_thread; i < (t + 1) * per_thread; ++i)
         {
            s.free(ptrs[i]);
            freed.fetch_add(1, std::memory_order_relaxed);
         }
      });
   }

   for (auto& t : threads)
      t.join();

   REQUIRE(freed.load() == num_allocs);

   // All blocks returned — should be able to alloc them all again
   for (int i = 0; i < num_allocs; ++i)
   {
      void* p = s.alloc(32);
      REQUIRE(p != nullptr);
   }
}

TEST_CASE("strand arena: variable sizes", "[strand]")
{
   strand s(4096);

   void* p1 = s.alloc(8);
   REQUIRE(p1 != nullptr);

   void* p2 = s.alloc(128);
   REQUIRE(p2 != nullptr);

   void* p3 = s.alloc(1);
   REQUIRE(p3 != nullptr);

   // Free in different order
   s.free(p2);
   s.free(p1);
   s.free(p3);

   // Reallocate — should reuse
   void* p4 = s.alloc(128);
   REQUIRE(p4 != nullptr);
}

TEST_CASE("strand arena: free nullptr is safe", "[strand]")
{
   strand s(256);
   s.free(nullptr);  // should not crash
}

TEST_CASE("strand arena: alignment", "[strand]")
{
   strand s(4096);

   for (int i = 0; i < 16; ++i)
   {
      void* p = s.alloc(i + 1);
      REQUIRE(p != nullptr);
      // All allocations should be 16-byte aligned
      REQUIRE((reinterpret_cast<uintptr_t>(p) % 16) == 0);
   }
}

// ── Strand scheduling tests ─────────────────────────────────────────────

TEST_CASE("strand scheduling: first enqueue becomes active", "[strand]")
{
   strand s(256);

   detail::Fiber fiber;
   s.enqueue(&fiber);

   REQUIRE(s.active() == &fiber);
}

TEST_CASE("strand scheduling: second enqueue waits", "[strand]")
{
   strand s(256);

   detail::Fiber f1, f2;
   s.enqueue(&f1);
   s.enqueue(&f2);

   // Only f1 is active
   REQUIRE(s.active() == &f1);
}

TEST_CASE("strand scheduling: release promotes next", "[strand]")
{
   strand s(256);

   detail::Fiber f1, f2, f3;
   s.enqueue(&f1);
   s.enqueue(&f2);
   s.enqueue(&f3);

   REQUIRE(s.active() == &f1);

   // Release f1 → f2 becomes active
   detail::Fiber* next = s.release();
   REQUIRE(next == &f2);
   REQUIRE(s.active() == &f2);

   // Release f2 → f3 becomes active
   next = s.release();
   REQUIRE(next == &f3);
   REQUIRE(s.active() == &f3);

   // Release f3 → nothing left
   next = s.release();
   REQUIRE(next == nullptr);
   REQUIRE(s.active() == nullptr);
}

TEST_CASE("strand scheduling: FIFO order", "[strand]")
{
   strand s(256);

   constexpr int N = 8;
   detail::Fiber fibers[N];

   // Enqueue a blocker first, then N-1 waiters
   s.enqueue(&fibers[0]);
   for (int i = 1; i < N; ++i)
      s.enqueue(&fibers[i]);

   // Release should produce FIFO order
   std::vector<int> order;
   order.push_back(0);  // fibers[0] was active

   for (int i = 1; i < N; ++i)
   {
      detail::Fiber* next = s.release();
      REQUIRE(next == &fibers[i]);
      order.push_back(i);
   }

   REQUIRE(s.release() == nullptr);

   std::vector<int> expected;
   for (int i = 0; i < N; ++i)
      expected.push_back(i);
   REQUIRE(order == expected);
}

TEST_CASE("strand scheduling: release then re-enqueue", "[strand]")
{
   strand s(256);

   detail::Fiber f1;
   s.enqueue(&f1);
   REQUIRE(s.active() == &f1);

   s.release();
   REQUIRE(s.active() == nullptr);

   // Re-enqueue — should become active again
   s.enqueue(&f1);
   REQUIRE(s.active() == &f1);
}

// ── Arena coalescing tests ─────────────────────────────────────────────

TEST_CASE("strand arena: coalescing adjacent blocks", "[strand]")
{
   // Use a small arena so we can observe coalescing effects.
   // Each alloc of 16 bytes uses 32 bytes (16 header + 16 payload).
   strand s(256);

   // Allocate 4 adjacent blocks
   void* p1 = s.alloc(16);
   void* p2 = s.alloc(16);
   void* p3 = s.alloc(16);
   void* p4 = s.alloc(16);
   REQUIRE(p1 != nullptr);
   REQUIRE(p2 != nullptr);
   REQUIRE(p3 != nullptr);
   REQUIRE(p4 != nullptr);

   // Free all 4 blocks (they're adjacent in the bump region)
   s.free(p1);
   s.free(p2);
   s.free(p3);
   s.free(p4);

   // Now try to allocate a single large block that only fits if
   // the 4 freed blocks were coalesced.
   // 4 blocks × 32 bytes each = 128 bytes total region freed.
   // A 96-byte payload needs 112 bytes (16 header + 96 payload).
   // This exceeds any single 32-byte block but fits the coalesced region.
   void* big = s.alloc(96);
   REQUIRE(big != nullptr);
}

TEST_CASE("strand arena: non-adjacent blocks are not coalesced", "[strand]")
{
   strand s(512);

   // Allocate 3 blocks, free only blocks 1 and 3 (keep 2)
   void* p1 = s.alloc(16);
   void* p2 = s.alloc(16);
   void* p3 = s.alloc(16);
   REQUIRE(p1 != nullptr);
   REQUIRE(p2 != nullptr);
   REQUIRE(p3 != nullptr);

   s.free(p1);
   s.free(p3);
   // p2 is still allocated — p1 and p3 are NOT adjacent

   // A 48-byte payload needs 64 bytes.
   // p1 = 32 bytes, p3 = 32 bytes, neither is large enough alone.
   // They can't coalesce because p2 sits between them.
   void* big = s.alloc(48);
   // Should come from bump pointer (still has space), not from coalescing
   REQUIRE(big != nullptr);
   // Verify it's NOT from the free list (should be past p3)
   REQUIRE(big > p3);
}

// ── strand::sync (cross-strand migration) tests ─────────────────────────

namespace
{
   // Helper: spin-wait for a flag with a short backoff.
   void wait_until(std::atomic<bool>& flag)
   {
      while (!flag.load(std::memory_order_acquire))
         std::this_thread::sleep_for(std::chrono::milliseconds{1});
   }

   // Helper: post a strand fiber from an external thread.  strand::post
   // requires a scheduler context, so we post work to a reactor worker
   // first, which then spawns the strand fiber.  Pass a sched_index
   // when tests need to ensure two posted fibers land on different
   // workers (e.g. one holds a strand busy-spinning while another runs).
   template <typename F>
   void post_strand_fiber(reactor& pool, strand& s, F&& f, uint32_t sched_index = 0)
   {
      pool.scheduler(sched_index).post(
         [&s, fn = std::forward<F>(f)]() mutable noexcept {
            s.post(std::move(fn));
         });
   }
}

TEST_CASE("strand::sync returns a value across strands", "[strand][sync]")
{
   reactor pool(1);
   strand  a(pool, 1024);
   strand  b(pool, 1024);

   std::atomic<int>  result{0};
   std::atomic<bool> done{false};

   post_strand_fiber(pool, a, [&] {
      int x = b.sync([&] { return 42; });
      result.store(x, std::memory_order_release);
      done.store(true, std::memory_order_release);
   });

   wait_until(done);
   REQUIRE(result.load() == 42);

   pool.stop();
   pool.join();
}

TEST_CASE("strand::sync with void return runs the callable", "[strand][sync]")
{
   reactor pool(1);
   strand  a(pool, 1024);
   strand  b(pool, 1024);

   std::atomic<int>  count{0};
   std::atomic<bool> done{false};

   post_strand_fiber(pool, a, [&] {
      b.sync([&] { count.fetch_add(1, std::memory_order_relaxed); });
      done.store(true, std::memory_order_release);
   });

   wait_until(done);
   REQUIRE(count.load() == 1);

   pool.stop();
   pool.join();
}

TEST_CASE("strand::sync: home_strand tracks current strand during migration",
          "[strand][sync]")
{
   reactor pool(1);
   strand  a(pool, 1024);
   strand  b(pool, 1024);

   std::atomic<bool> done{false};
   const strand*     home_before = nullptr;
   const strand*     home_during = nullptr;
   const strand*     home_after  = nullptr;

   post_strand_fiber(pool, a, [&] {
      auto* me    = Scheduler::current().currentFiber();
      home_before = me->home_strand;
      b.sync([&] {
         auto* m2    = Scheduler::current().currentFiber();
         home_during = m2->home_strand;
      });
      home_after = me->home_strand;
      done.store(true, std::memory_order_release);
   });

   wait_until(done);
   REQUIRE(home_before == &a);
   REQUIRE(home_during == &b);
   REQUIRE(home_after == &a);

   pool.stop();
   pool.join();
}

TEST_CASE("strand::sync propagates exceptions and restores source strand",
          "[strand][sync][exception]")
{
   reactor pool(1);
   strand  a(pool, 1024);
   strand  b(pool, 1024);

   std::atomic<bool> caught{false};
   std::atomic<bool> done{false};
   const strand*     home_after = nullptr;

   post_strand_fiber(pool, a, [&] {
      try
      {
         b.sync([&] { throw std::runtime_error("migration failure"); });
      }
      catch (const std::runtime_error& e)
      {
         if (std::string{e.what()} == "migration failure")
            caught.store(true, std::memory_order_release);
      }
      home_after = Scheduler::current().currentFiber()->home_strand;
      done.store(true, std::memory_order_release);
   });

   wait_until(done);
   REQUIRE(caught.load());
   REQUIRE(home_after == &a);

   pool.stop();
   pool.join();
}

TEST_CASE("strand::sync: contended target strand queues migrant",
          "[strand][sync]")
{
   // Two pool threads so a holder on B can run concurrently with a
   // migrant trying to enter B.
   reactor pool(2);
   strand  a(pool, 1024);
   strand  b(pool, 1024);

   std::atomic<bool> b_busy{false};
   std::atomic<bool> b_release{false};
   std::atomic<int>  phase{0};
   std::atomic<bool> migrant_done{false};

   // Holder on B: hold B continuously via pure busy-wait until we
   // signal release.  Can't use yield/sleep/park here — all release
   // the strand, which would let the migrant in and defeat the point.
   // Post to scheduler 0 so it monopolizes that thread.
   post_strand_fiber(pool, b, [&] {
      b_busy.store(true, std::memory_order_release);
      while (!b_release.load(std::memory_order_acquire))
      {
         // busy-spin — no suspend, no strand release
      }
   }, /*sched_index=*/0);

   wait_until(b_busy);

   // Migrant on A: must run on a different scheduler than the busy-
   // spinning holder, or it'll never get CPU.
   post_strand_fiber(pool, a, [&] {
      phase.store(1, std::memory_order_release);
      b.sync([&] { phase.store(2, std::memory_order_release); });
      phase.store(3, std::memory_order_release);
      migrant_done.store(true, std::memory_order_release);
   }, /*sched_index=*/1);

   // Give the migrant time to park; it should be stuck at phase 1.
   std::this_thread::sleep_for(std::chrono::milliseconds{20});
   REQUIRE(phase.load() == 1);

   // Free B; migrant should now complete.
   b_release.store(true, std::memory_order_release);

   wait_until(migrant_done);
   REQUIRE(phase.load() == 3);

   pool.stop();
   pool.join();
}

TEST_CASE("strand::sync: multiple migrants serialize through target",
          "[strand][sync][concurrent]")
{
   reactor pool(2);
   strand  a(pool, 4096);
   strand  b(pool, 4096);

   constexpr int     N = 16;
   std::atomic<int>  completed{0};
   std::atomic<int>  concurrent_on_b{0};
   std::atomic<int>  max_concurrent_on_b{0};

   for (int i = 0; i < N; ++i)
   {
      post_strand_fiber(pool, a, [&] {
         b.sync([&] {
            int c = concurrent_on_b.fetch_add(1, std::memory_order_acq_rel) + 1;
            int m = max_concurrent_on_b.load(std::memory_order_relaxed);
            while (c > m && !max_concurrent_on_b.compare_exchange_weak(
                              m, c, std::memory_order_relaxed))
            {}
            concurrent_on_b.fetch_sub(1, std::memory_order_acq_rel);
         });
         completed.fetch_add(1, std::memory_order_release);
      });
   }

   while (completed.load(std::memory_order_acquire) < N)
      std::this_thread::sleep_for(std::chrono::milliseconds{1});

   REQUIRE(completed.load() == N);
   // Strand B serializes — only one fiber on B at any time.
   REQUIRE(max_concurrent_on_b.load() == 1);

   pool.stop();
   pool.join();
}
