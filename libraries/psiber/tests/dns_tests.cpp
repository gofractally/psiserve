#include <catch2/catch.hpp>

#include <psiber/dns.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/io_engine_kqueue.hpp>

#include <arpa/inet.h>
#include <cstring>

using namespace psiber;

// ── Resolve localhost ──────────────────────────────────────────────────────

TEST_CASE("dns: resolve localhost", "[dns]")
{
   auto      io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 800);

   bool resolved = false;

   sched.spawnFiber([&]() {
      dns_resolver resolver;
      auto         addrs = resolver.resolve(sched, "localhost", 80);

      REQUIRE(!addrs.empty());

      // At least one should be 127.0.0.1 or ::1
      bool found_loopback = false;
      for (auto& addr : addrs)
      {
         if (addr.ss_family == AF_INET)
         {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(&addr);
            char  buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
            if (std::strcmp(buf, "127.0.0.1") == 0)
               found_loopback = true;
            CHECK(ntohs(sin->sin_port) == 80);
         }
         else if (addr.ss_family == AF_INET6)
         {
            auto* sin6 = reinterpret_cast<struct sockaddr_in6*>(&addr);
            char  buf[INET6_ADDRSTRLEN];
            inet_ntop(AF_INET6, &sin6->sin6_addr, buf, sizeof(buf));
            if (std::strcmp(buf, "::1") == 0)
               found_loopback = true;
            CHECK(ntohs(sin6->sin6_port) == 80);
         }
      }
      REQUIRE(found_loopback);
      resolved = true;
   });

   sched.run();
   REQUIRE(resolved);
}

// ── Resolve a public hostname ──────────────────────────────────────────────

TEST_CASE("dns: resolve dns.google", "[dns][network]")
{
   auto      io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 801);

   bool resolved = false;

   sched.spawnFiber([&]() {
      dns_resolver resolver;
      auto         addrs = resolver.resolve(sched, "dns.google", 443);

      REQUIRE(!addrs.empty());

      // dns.google should resolve to 8.8.8.8 / 8.8.4.4 or their v6 equivalents
      bool found_known = false;
      for (auto& addr : addrs)
      {
         if (addr.ss_family == AF_INET)
         {
            auto* sin = reinterpret_cast<struct sockaddr_in*>(&addr);
            char  buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
            if (std::strcmp(buf, "8.8.8.8") == 0 || std::strcmp(buf, "8.8.4.4") == 0)
               found_known = true;
         }
      }
      REQUIRE(found_known);
      resolved = true;
   });

   sched.run();
   REQUIRE(resolved);
}

// ── Resolution failure ─────────────────────────────────────────────────────

TEST_CASE("dns: non-existent domain throws", "[dns]")
{
   auto      io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 802);

   bool threw = false;

   sched.spawnFiber([&]() {
      dns_resolver resolver;
      try
      {
         resolver.resolve(sched, "this-domain-does-not-exist.invalid", 80);
      }
      catch (const std::runtime_error& e)
      {
         threw = true;
         // Should mention resolution failure
         REQUIRE(std::string(e.what()).find("DNS resolution failed") != std::string::npos);
      }
   });

   sched.run();
   REQUIRE(threw);
}

// ── Multiple resolutions on same resolver ──────────────────────────────────

TEST_CASE("dns: multiple sequential resolutions", "[dns][network]")
{
   auto      io = std::make_unique<KqueueEngine>();
   Scheduler sched(std::move(io), 803);

   int count = 0;

   sched.spawnFiber([&]() {
      dns_resolver resolver;

      auto a1 = resolver.resolve(sched, "localhost", 80);
      REQUIRE(!a1.empty());
      ++count;

      auto a2 = resolver.resolve(sched, "localhost", 443);
      REQUIRE(!a2.empty());
      ++count;
   });

   sched.run();
   REQUIRE(count == 2);
}
