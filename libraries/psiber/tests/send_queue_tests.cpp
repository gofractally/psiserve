#include <catch2/catch.hpp>

#include <psiber/send_queue.hpp>
#include <psiber/scheduler.hpp>

#include <atomic>
#include <thread>
#include <vector>

using namespace psiber;

TEST_CASE("SendQueue basic emplace and consume", "[send_queue]")
{
   SendQueue sq(4096);
   int       result = 0;

   auto* slot = sq.emplace([&]() { result = 42; });
   REQUIRE(slot != nullptr);

   // Execute the task
   void* payload = slot + 1;
   slot->run(payload);
   REQUIRE(result == 42);

   // Mark consumed and reclaim
   slot->consumed.store(true, std::memory_order_release);
   sq.reclaim();
   REQUIRE(sq.available() == sq.capacity());
}

TEST_CASE("SendQueue fill and back pressure", "[send_queue]")
{
   // Small ring to force back pressure
   SendQueue sq(512);
   std::vector<TaskSlotHeader*> slots;

   // Fill the ring
   while (true)
   {
      auto* slot = sq.emplace([&]() {});
      if (!slot)
         break;
      slots.push_back(slot);
   }

   REQUIRE(slots.size() > 0);
   REQUIRE(sq.available() < sizeof(TaskSlotHeader) + 16);

   // Consume all and reclaim
   for (auto* s : slots)
      s->consumed.store(true, std::memory_order_release);
   sq.reclaim();

   // Should be able to allocate again
   auto* fresh = sq.emplace([&]() {});
   REQUIRE(fresh != nullptr);
   fresh->consumed.store(true, std::memory_order_release);
}

TEST_CASE("SendQueue variable-size functors", "[send_queue]")
{
   SendQueue sq(8192);

   // Small functor (just a function pointer)
   int small_result = 0;
   auto* s1 = sq.emplace([&]() { small_result = 1; });
   REQUIRE(s1 != nullptr);

   // Larger functor (captures an array)
   std::array<int, 16> captured = {};
   captured[15] = 99;
   auto* s2 = sq.emplace([captured, &small_result]() mutable {
      small_result = captured[15];
   });
   REQUIRE(s2 != nullptr);

   // Execute both
   void* p1 = s1 + 1;
   s1->run(p1);
   REQUIRE(small_result == 1);

   void* p2 = s2 + 1;
   s2->run(p2);
   REQUIRE(small_result == 99);

   // Cleanup
   s1->consumed.store(true, std::memory_order_release);
   s2->consumed.store(true, std::memory_order_release);
   sq.reclaim();
}

TEST_CASE("SendQueue wrap-around", "[send_queue]")
{
   SendQueue sq(1024);
   int       counter = 0;

   // Fill, consume, reclaim, repeat — forces wrap-around
   for (int round = 0; round < 10; ++round)
   {
      std::vector<TaskSlotHeader*> slots;
      while (true)
      {
         auto* slot = sq.emplace([&]() { ++counter; });
         if (!slot)
            break;
         slots.push_back(slot);
      }

      // Execute and consume
      for (auto* s : slots)
      {
         void* p = s + 1;
         s->run(p);
         s->consumed.store(true, std::memory_order_release);
      }
      sq.reclaim();
   }

   REQUIRE(counter > 10);  // should have executed many tasks
}

TEST_CASE("SendQueue cross-thread task dispatch", "[send_queue]")
{
   auto sched = scheduler_access::make(10);

   std::atomic<int> result{0};
   SendQueue        sq(4096);

   sched.spawnFiber([&]() {
      // Fiber waits until result is set by the cross-thread task
      while (result.load(std::memory_order_acquire) == 0)
         sched.sleep(std::chrono::milliseconds{1});
   });

   // Post a task from another thread
   std::thread poster([&]() {
      auto* slot = sq.emplace([&]() {
         result.store(42, std::memory_order_release);
      });
      REQUIRE(slot != nullptr);
      sched.postTask(slot);
   });

   sched.run();
   poster.join();

   REQUIRE(result.load(std::memory_order_relaxed) == 42);

   // Reclaim the slot
   sq.reclaim();
}

TEST_CASE("SendQueue destructor cleans up unconsumed tasks", "[send_queue]")
{
   static std::atomic<int> dtor_count{0};
   dtor_count.store(0);

   struct Tracked
   {
      Tracked()                          = default;
      Tracked(const Tracked&)            = default;
      Tracked& operator=(const Tracked&) = default;
      ~Tracked() { dtor_count.fetch_add(1, std::memory_order_relaxed); }
      void operator()() {}
   };

   {
      SendQueue sq(4096);
      auto* s1 = sq.emplace(Tracked{});
      auto* s2 = sq.emplace(Tracked{});
      REQUIRE(s1 != nullptr);
      REQUIRE(s2 != nullptr);
      // Don't mark consumed — destructor should handle cleanup
   }

   // Tracked objects should have been destroyed
   REQUIRE(dtor_count.load(std::memory_order_relaxed) >= 2);
}
