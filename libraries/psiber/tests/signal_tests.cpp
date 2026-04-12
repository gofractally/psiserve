#include <catch2/catch.hpp>

#include <psiber/scheduler.hpp>

#include <csignal>
#include <thread>

using namespace psiber;

// ── Signal: fiber wakes on SIGUSR1 ──────────────────────────────────────────

TEST_CASE("signal: fiber wakes on SIGUSR1", "[signal]")
{
   auto sched = scheduler_access::make(700);

   sched.registerSignal(SIGUSR1);

   bool signal_received = false;

   sched.spawnFiber([&]() {
      // Spawn a fiber that sends SIGUSR1 after a delay
      sched.spawnFiber([&]() {
         sched.sleep(std::chrono::milliseconds{50});
         ::kill(::getpid(), SIGUSR1);
      });

      sched.waitForSignal(SIGUSR1);
      signal_received = true;
   });

   sched.run();
   REQUIRE(signal_received);
}

// ── Signal: cross-thread signal delivery ────────────────────────────────────

TEST_CASE("signal: cross-thread signal delivery", "[signal]")
{
   auto sched = scheduler_access::make(701);

   sched.registerSignal(SIGUSR2);

   bool signal_received = false;

   sched.spawnFiber([&]() {
      sched.waitForSignal(SIGUSR2);
      signal_received = true;
   });

   // Send signal from another thread
   std::thread sender([&]() {
      // Small delay to let the fiber park
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
      ::kill(::getpid(), SIGUSR2);
   });

   sched.run();
   sender.join();
   REQUIRE(signal_received);
}

// ── Signal: multiple signals in sequence ────────────────────────────────────

TEST_CASE("signal: multiple SIGUSR1 in sequence", "[signal]")
{
   auto sched = scheduler_access::make(702);

   sched.registerSignal(SIGUSR1);

   int signal_count = 0;

   sched.spawnFiber([&]() {
      for (int i = 0; i < 3; ++i)
      {
         // Spawn sender for each iteration
         sched.spawnFiber([&]() {
            sched.sleep(std::chrono::milliseconds{20});
            ::kill(::getpid(), SIGUSR1);
         });

         sched.waitForSignal(SIGUSR1);
         ++signal_count;
      }
   });

   sched.run();
   REQUIRE(signal_count == 3);
}
