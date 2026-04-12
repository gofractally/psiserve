#include <catch2/catch.hpp>

#include <psiber/strand.hpp>

#include <atomic>
#include <cstring>
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
