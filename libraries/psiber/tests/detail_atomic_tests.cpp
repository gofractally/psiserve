#include <catch2/catch.hpp>

#include <psiber/detail/bitset_pool.hpp>
#include <psiber/detail/bounded_counter.hpp>
#include <psiber/detail/mpsc_stack.hpp>
#include <psiber/detail/sentinel_stack.hpp>

#include <algorithm>
#include <atomic>
#include <numeric>
#include <set>
#include <thread>
#include <vector>

using namespace psiber::detail;

// ─── Test node types ─────────────────────────────────────────────────────────

struct MpscNode
{
   MpscNode* next = nullptr;
   int       value = 0;
};

struct SentinelNode
{
   SentinelNode* next = nullptr;
   int           value = 0;
};

// ═════════════════════════════════════════════════════════════════════════════
//  mpsc_stack
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("mpsc_stack: single-thread push and drain", "[detail][mpsc]")
{
   mpsc_stack<MpscNode, &MpscNode::next> stack;

   // Empty drain returns nullptr
   REQUIRE(stack.drain() == nullptr);
   REQUIRE_FALSE(stack.probably_non_empty());

   // Push three nodes
   MpscNode a{nullptr, 1}, b{nullptr, 2}, c{nullptr, 3};
   stack.push(&a);
   stack.push(&b);
   stack.push(&c);

   REQUIRE(stack.probably_non_empty());

   // Drain returns LIFO order: c → b → a
   MpscNode* head = stack.drain();
   REQUIRE(head == &c);
   REQUIRE(head->next == &b);
   REQUIRE(head->next->next == &a);
   REQUIRE(head->next->next->next == nullptr);

   // Stack is now empty
   REQUIRE_FALSE(stack.probably_non_empty());
   REQUIRE(stack.drain() == nullptr);
}

TEST_CASE("mpsc_stack: reverse restores FIFO order", "[detail][mpsc]")
{
   mpsc_stack<MpscNode, &MpscNode::next> stack;
   using Stack = decltype(stack);

   MpscNode a{nullptr, 1}, b{nullptr, 2}, c{nullptr, 3};
   stack.push(&a);
   stack.push(&b);
   stack.push(&c);

   MpscNode* head = stack.drain();
   MpscNode* fifo = Stack::reverse(head);

   // FIFO: a → b → c
   REQUIRE(fifo == &a);
   REQUIRE(fifo->next == &b);
   REQUIRE(fifo->next->next == &c);
   REQUIRE(fifo->next->next->next == nullptr);
}

TEST_CASE("mpsc_stack: multi-producer push, single drain", "[detail][mpsc][concurrent]")
{
   // N threads each push M nodes.  Single drain must see all N*M nodes.
   // This tests the CAS-push under contention.
   constexpr int N = 8;   // threads
   constexpr int M = 5000; // nodes per thread

   mpsc_stack<MpscNode, &MpscNode::next> stack;
   using Stack = decltype(stack);

   // Each thread owns its own nodes (no sharing)
   std::vector<std::vector<MpscNode>> per_thread(N);
   for (int t = 0; t < N; ++t)
   {
      per_thread[t].resize(M);
      for (int i = 0; i < M; ++i)
         per_thread[t][i].value = t * M + i;
   }

   // Launch producers
   std::vector<std::thread> threads;
   for (int t = 0; t < N; ++t)
   {
      threads.emplace_back([&stack, &per_thread, t]() {
         for (int i = 0; i < M; ++i)
            stack.push(&per_thread[t][i]);
      });
   }
   for (auto& th : threads)
      th.join();

   // Drain and count
   MpscNode* head = stack.drain();
   std::set<int> seen;
   int count = 0;
   while (head)
   {
      seen.insert(head->value);
      MpscNode* next = head->next;
      head->next = nullptr;
      head = next;
      ++count;
   }

   REQUIRE(count == N * M);
   REQUIRE(seen.size() == static_cast<size_t>(N * M));
}

TEST_CASE("mpsc_stack: producer visibility — drain sees producer writes", "[detail][mpsc][concurrent]")
{
   // Verifies that the release-on-push / acquire-on-drain ordering
   // makes non-atomic payload writes visible to the consumer.
   constexpr int N = 4;
   constexpr int M = 2000;

   mpsc_stack<MpscNode, &MpscNode::next> stack;

   std::vector<std::vector<MpscNode>> per_thread(N);
   for (auto& v : per_thread)
      v.resize(M);

   std::vector<std::thread> threads;
   for (int t = 0; t < N; ++t)
   {
      threads.emplace_back([&stack, &per_thread, t]() {
         for (int i = 0; i < M; ++i)
         {
            // Write the value BEFORE push (release must make this visible)
            per_thread[t][i].value = (t + 1) * 1000 + i;
            stack.push(&per_thread[t][i]);
         }
      });
   }
   for (auto& th : threads)
      th.join();

   // Drain (acquire) — all .value writes must be visible
   MpscNode* head = stack.drain();
   int count = 0;
   while (head)
   {
      // If memory ordering is broken, we might see the default 0
      REQUIRE(head->value != 0);
      head = head->next;
      ++count;
   }
   REQUIRE(count == N * M);
}

// ═════════════════════════════════════════════════════════════════════════════
//  bitset_pool
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("bitset_pool: pop all slots then push back", "[detail][bitset]")
{
   bitset_pool<256> pool;

   // Pop all 256 slots
   std::vector<uint32_t> indices;
   for (int i = 0; i < 256; ++i)
   {
      auto idx = pool.try_pop();
      REQUIRE(idx.has_value());
      indices.push_back(*idx);
   }

   // Next pop should fail
   REQUIRE_FALSE(pool.try_pop().has_value());

   // All indices are unique and in range [0, 256)
   std::sort(indices.begin(), indices.end());
   for (int i = 0; i < 256; ++i)
      REQUIRE(indices[i] == static_cast<uint32_t>(i));

   // Push all back
   for (auto idx : indices)
      pool.push(idx);

   // Can pop all again
   int count = 0;
   while (pool.try_pop().has_value())
      ++count;
   REQUIRE(count == 256);
}

TEST_CASE("bitset_pool: non-power-of-2 size", "[detail][bitset]")
{
   // 100 slots — last word has only 36 valid bits
   bitset_pool<100> pool;

   std::vector<uint32_t> indices;
   while (auto idx = pool.try_pop())
      indices.push_back(*idx);

   REQUIRE(indices.size() == 100);

   // All in range [0, 100)
   std::sort(indices.begin(), indices.end());
   REQUIRE(indices.front() == 0);
   REQUIRE(indices.back() == 99);

   // Push back and verify
   for (auto idx : indices)
      pool.push(idx);

   int count = 0;
   while (pool.try_pop().has_value())
      ++count;
   REQUIRE(count == 100);
}

TEST_CASE("bitset_pool: concurrent pop — no double-claim", "[detail][bitset][concurrent]")
{
   // N threads race to pop from a pool of 256 slots.
   // Each slot must be claimed by exactly one thread.
   constexpr int N = 8;

   bitset_pool<256> pool;

   std::vector<std::vector<uint32_t>> per_thread(N);

   std::vector<std::thread> threads;
   for (int t = 0; t < N; ++t)
   {
      threads.emplace_back([&pool, &per_thread, t]() {
         while (auto idx = pool.try_pop())
            per_thread[t].push_back(*idx);
      });
   }
   for (auto& th : threads)
      th.join();

   // Collect all claimed indices
   std::vector<uint32_t> all;
   for (auto& v : per_thread)
      all.insert(all.end(), v.begin(), v.end());

   // Exactly 256, all unique
   std::sort(all.begin(), all.end());
   REQUIRE(all.size() == 256);
   for (int i = 0; i < 256; ++i)
      REQUIRE(all[i] == static_cast<uint32_t>(i));
}

TEST_CASE("bitset_pool: concurrent pop and push — no lost slots", "[detail][bitset][concurrent]")
{
   // Threads pop a slot, do "work", push it back, repeat.
   // After all threads finish, the pool should have all slots free.
   constexpr int N = 8;
   constexpr int rounds = 2000;

   bitset_pool<256> pool;
   std::atomic<int> total_ops{0};

   std::vector<std::thread> threads;
   for (int t = 0; t < N; ++t)
   {
      threads.emplace_back([&pool, &total_ops]() {
         for (int r = 0; r < rounds; ++r)
         {
            auto idx = pool.try_pop();
            if (idx)
            {
               // Simulate brief work
               total_ops.fetch_add(1, std::memory_order_relaxed);
               pool.push(*idx);
            }
         }
      });
   }
   for (auto& th : threads)
      th.join();

   // All slots should be back in the pool
   int count = 0;
   while (pool.try_pop().has_value())
      ++count;
   REQUIRE(count == 256);
   REQUIRE(total_ops.load() > 0);
}

// ═════════════════════════════════════════════════════════════════════════════
//  bounded_counter
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("bounded_counter: basic increment and decrement", "[detail][counter]")
{
   bounded_counter c(3);

   REQUIRE(c.count() == 0);
   REQUIRE(c.max() == 3);

   REQUIRE(c.try_increment());   // 1
   REQUIRE(c.try_increment());   // 2
   REQUIRE(c.try_increment());   // 3
   REQUIRE_FALSE(c.try_increment()); // at limit

   REQUIRE(c.count() == 3);

   c.decrement();  // 2
   REQUIRE(c.count() == 2);
   REQUIRE(c.try_increment());   // 3 again
   REQUIRE_FALSE(c.try_increment());
}

TEST_CASE("bounded_counter: set_max changes limit", "[detail][counter]")
{
   bounded_counter c(2);

   REQUIRE(c.try_increment());
   REQUIRE(c.try_increment());
   REQUIRE_FALSE(c.try_increment());

   c.set_max(4);
   REQUIRE(c.try_increment());   // 3
   REQUIRE(c.try_increment());   // 4
   REQUIRE_FALSE(c.try_increment());
}

TEST_CASE("bounded_counter: max=0 rejects all", "[detail][counter]")
{
   bounded_counter c(0);
   REQUIRE_FALSE(c.try_increment());
   REQUIRE(c.count() == 0);
}

TEST_CASE("bounded_counter: concurrent increment/decrement — no drift", "[detail][counter][concurrent]")
{
   // N threads each do many increment-then-decrement cycles.
   // After all threads finish, count must be exactly 0.
   // This verifies the speculative-increment-plus-rollback pattern
   // doesn't leak or undercount.
   constexpr int N = 8;
   constexpr int rounds = 10000;

   bounded_counter c(N);  // limit = N, so some contention on the limit check
   std::atomic<int> successes{0};
   std::atomic<int> failures{0};

   std::vector<std::thread> threads;
   for (int t = 0; t < N; ++t)
   {
      threads.emplace_back([&c, &successes, &failures]() {
         for (int r = 0; r < rounds; ++r)
         {
            if (c.try_increment())
            {
               successes.fetch_add(1, std::memory_order_relaxed);
               c.decrement();
            }
            else
            {
               failures.fetch_add(1, std::memory_order_relaxed);
            }
         }
      });
   }
   for (auto& th : threads)
      th.join();

   REQUIRE(c.count() == 0);
   REQUIRE(successes.load() + failures.load() == N * rounds);
   // With limit=N and N threads, most should succeed
   REQUIRE(successes.load() > 0);
}

TEST_CASE("bounded_counter: concurrent saturation — never exceeds max", "[detail][counter][concurrent]")
{
   // Threads try to increment without decrementing.
   // Total successful increments must equal max.
   constexpr int N = 8;
   constexpr uint32_t limit = 100;

   bounded_counter c(limit);

   std::vector<std::atomic<int>> per_thread(N);
   for (auto& a : per_thread)
      a.store(0);

   std::vector<std::thread> threads;
   for (int t = 0; t < N; ++t)
   {
      threads.emplace_back([&c, &per_thread, t]() {
         int local = 0;
         while (c.try_increment())
            ++local;
         per_thread[t].store(local);
      });
   }
   for (auto& th : threads)
      th.join();

   int total = 0;
   for (auto& a : per_thread)
      total += a.load();

   REQUIRE(total == static_cast<int>(limit));
   REQUIRE(c.count() == limit);
}

// ═════════════════════════════════════════════════════════════════════════════
//  sentinel_stack
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("sentinel_stack: single-thread push and pop", "[detail][sentinel]")
{
   sentinel_stack<SentinelNode, &SentinelNode::next> stack;

   // Empty pop returns nullptr
   REQUIRE(stack.try_pop() == nullptr);
   REQUIRE_FALSE(stack.probably_non_empty());

   SentinelNode a{nullptr, 1}, b{nullptr, 2}, c{nullptr, 3};
   stack.push(&a);
   stack.push(&b);
   stack.push(&c);

   REQUIRE(stack.probably_non_empty());

   // Pop returns one at a time (LIFO)
   SentinelNode* p1 = stack.try_pop();
   REQUIRE(p1 == &c);
   REQUIRE(p1->value == 3);

   SentinelNode* p2 = stack.try_pop();
   REQUIRE(p2 == &b);
   REQUIRE(p2->value == 2);

   SentinelNode* p3 = stack.try_pop();
   REQUIRE(p3 == &a);
   REQUIRE(p3->value == 1);

   REQUIRE(stack.try_pop() == nullptr);
}

TEST_CASE("sentinel_stack: multi-producer single-consumer — all nodes seen", "[detail][sentinel][concurrent]")
{
   constexpr int N = 8;
   constexpr int M = 2000;

   sentinel_stack<SentinelNode, &SentinelNode::next> stack;

   std::vector<std::vector<SentinelNode>> per_thread(N);
   for (int t = 0; t < N; ++t)
   {
      per_thread[t].resize(M);
      for (int i = 0; i < M; ++i)
         per_thread[t][i].value = t * M + i;
   }

   // Push from N threads
   std::vector<std::thread> threads;
   for (int t = 0; t < N; ++t)
   {
      threads.emplace_back([&stack, &per_thread, t]() {
         for (int i = 0; i < M; ++i)
            stack.push(&per_thread[t][i]);
      });
   }
   for (auto& th : threads)
      th.join();

   // Pop all from one thread
   std::set<int> seen;
   while (SentinelNode* p = stack.try_pop())
      seen.insert(p->value);

   REQUIRE(seen.size() == static_cast<size_t>(N * M));
}

TEST_CASE("sentinel_stack: multi-consumer — no double-pop", "[detail][sentinel][concurrent]")
{
   // Multiple poppers race on the same stack.
   // Each node must be popped by exactly one thread.
   constexpr int num_nodes = 5000;
   constexpr int num_poppers = 4;

   sentinel_stack<SentinelNode, &SentinelNode::next> stack;

   std::vector<SentinelNode> nodes(num_nodes);
   for (int i = 0; i < num_nodes; ++i)
   {
      nodes[i].value = i;
      stack.push(&nodes[i]);
   }

   std::vector<std::vector<int>> per_thread(num_poppers);

   std::vector<std::thread> threads;
   for (int t = 0; t < num_poppers; ++t)
   {
      threads.emplace_back([&stack, &per_thread, t]() {
         while (SentinelNode* p = stack.try_pop())
            per_thread[t].push_back(p->value);
      });
   }
   for (auto& th : threads)
      th.join();

   // All nodes popped, each exactly once
   std::vector<int> all;
   for (auto& v : per_thread)
      all.insert(all.end(), v.begin(), v.end());

   std::sort(all.begin(), all.end());
   REQUIRE(all.size() == static_cast<size_t>(num_nodes));
   for (int i = 0; i < num_nodes; ++i)
      REQUIRE(all[i] == i);
}

TEST_CASE("sentinel_stack: concurrent push and pop — no lost nodes", "[detail][sentinel][concurrent]")
{
   // Producers push while consumers pop concurrently.
   // Total popped must equal total pushed.
   constexpr int num_producers = 4;
   constexpr int num_consumers = 4;
   constexpr int items_per_producer = 3000;

   sentinel_stack<SentinelNode, &SentinelNode::next> stack;

   std::vector<std::vector<SentinelNode>> producer_nodes(num_producers);
   for (int t = 0; t < num_producers; ++t)
   {
      producer_nodes[t].resize(items_per_producer);
      for (int i = 0; i < items_per_producer; ++i)
         producer_nodes[t][i].value = t * items_per_producer + i;
   }

   std::atomic<bool> producers_done{false};
   std::vector<std::vector<int>> consumed(num_consumers);

   // Start consumers first
   std::vector<std::thread> consumers;
   for (int t = 0; t < num_consumers; ++t)
   {
      consumers.emplace_back([&stack, &producers_done, &consumed, t]() {
         while (!producers_done.load(std::memory_order_acquire) ||
                stack.probably_non_empty())
         {
            if (SentinelNode* p = stack.try_pop())
               consumed[t].push_back(p->value);
            else
               std::this_thread::yield();
         }
         // Final drain — pick up stragglers
         while (SentinelNode* p = stack.try_pop())
            consumed[t].push_back(p->value);
      });
   }

   // Start producers
   std::vector<std::thread> producers;
   for (int t = 0; t < num_producers; ++t)
   {
      producers.emplace_back([&stack, &producer_nodes, t]() {
         for (int i = 0; i < items_per_producer; ++i)
            stack.push(&producer_nodes[t][i]);
      });
   }
   for (auto& th : producers)
      th.join();

   producers_done.store(true, std::memory_order_release);

   for (auto& th : consumers)
      th.join();

   // Collect and verify
   std::set<int> all;
   for (auto& v : consumed)
      for (int val : v)
         all.insert(val);

   REQUIRE(all.size() == static_cast<size_t>(num_producers * items_per_producer));
}
