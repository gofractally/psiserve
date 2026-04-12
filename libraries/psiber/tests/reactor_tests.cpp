#include <catch2/catch.hpp>

#include <psiber/reactor.hpp>
#include <psiber/strand.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace psiber;

// ── Ready-strand queue tests ────────────────────────────────────────────

TEST_CASE("reactor: post and pop single strand", "[reactor]")
{
   reactor pool(2);
   // Stop workers so they don't steal strands during raw queue tests
   pool.stop();
   pool.join();

   strand s;
   detail::Fiber f;
   s.enqueue(&f);

   pool.post_strand(&s);

   strand* popped = pool.try_pop_strand();
   REQUIRE(popped == &s);

   // Queue should now be empty
   REQUIRE(pool.try_pop_strand() == nullptr);
}

TEST_CASE("reactor: post multiple, pop all", "[reactor]")
{
   reactor pool(2);
   // Stop workers so they don't steal strands during raw queue tests
   pool.stop();
   pool.join();

   constexpr int N = 8;
   strand strands[N];
   detail::Fiber fibers[N];

   for (int i = 0; i < N; ++i)
   {
      strands[i].enqueue(&fibers[i]);
      pool.post_strand(&strands[i]);
   }

   // Pop all — should get all N (order is LIFO since CAS stack)
   std::vector<strand*> popped;
   while (auto* s = pool.try_pop_strand())
      popped.push_back(s);

   REQUIRE(popped.size() == N);

   // Verify each was popped exactly once
   for (int i = 0; i < N; ++i)
   {
      bool found = false;
      for (auto* p : popped)
      {
         if (p == &strands[i])
         {
            found = true;
            break;
         }
      }
      REQUIRE(found);
   }
}

TEST_CASE("reactor: pop from empty returns nullptr", "[reactor]")
{
   reactor pool(1);

   REQUIRE(pool.try_pop_strand() == nullptr);
   REQUIRE(pool.try_pop_strand() == nullptr);

   pool.stop();
   pool.join();
}

TEST_CASE("reactor: concurrent post and pop", "[reactor]")
{
   reactor pool(2);
   // Stop workers so they don't interfere with raw queue tests
   pool.stop();
   pool.join();

   constexpr int num_strands   = 64;
   constexpr int num_producers = 4;
   constexpr int per_producer  = num_strands / num_producers;

   strand strands[num_strands];
   detail::Fiber fibers[num_strands];
   for (int i = 0; i < num_strands; ++i)
      strands[i].enqueue(&fibers[i]);

   std::atomic<int> posted{0};
   std::atomic<int> consumed{0};

   // Producers: post strands
   std::vector<std::thread> producers;
   for (int p = 0; p < num_producers; ++p)
   {
      producers.emplace_back([&, p]() {
         for (int i = p * per_producer; i < (p + 1) * per_producer; ++i)
         {
            pool.post_strand(&strands[i]);
            posted.fetch_add(1, std::memory_order_relaxed);
         }
      });
   }

   // Consumer: pop strands
   std::thread consumer([&]() {
      while (consumed.load(std::memory_order_relaxed) < num_strands)
      {
         if (pool.try_pop_strand())
            consumed.fetch_add(1, std::memory_order_relaxed);
         else
         {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
         }
      }
   });

   for (auto& t : producers)
      t.join();
   consumer.join();

   REQUIRE(posted.load() == num_strands);
   REQUIRE(consumed.load() == num_strands);
}

TEST_CASE("reactor: stop and join", "[reactor]")
{
   reactor pool(2);
   pool.stop();
   pool.join();
   // Should not hang
}

TEST_CASE("reactor: destructor stops and joins", "[reactor]")
{
   { reactor pool(2); }
   // Should not hang or crash
}

TEST_CASE("reactor: num_threads accessor", "[reactor]")
{
   reactor pool(3);
   REQUIRE(pool.num_threads() == 3);
   pool.stop();
   pool.join();
}

TEST_CASE("reactor: strand enqueue posts to reactor", "[reactor]")
{
   reactor pool(2);
   // Stop workers so they don't steal the strand before we can pop it
   pool.stop();
   pool.join();

   strand s(pool);

   detail::Fiber f;
   f.home_strand = &s;

   // Enqueue should set _active and post to reactor
   s.enqueue(&f);
   REQUIRE(s.active() == &f);

   // The strand should be in the reactor's ready queue
   strand* popped = pool.try_pop_strand();
   REQUIRE(popped == &s);
}
