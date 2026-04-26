// host.cpp — the host author's side of the hello contract.
//
// Symmetrical to guest.cpp: both files are impls against shared.hpp,
// written (in real life) by different people. This one provides the
// C++-native implementations of the `env` and `clock_api` interfaces
// declared in shared.hpp, registers them with PSIO1_HOST_MODULE, and drives
// the psizam engine that loads the guest wasm and calls into it.

#include <psizam/hosted.hpp>

#include "guest_wasm.hpp"
#include "shared.hpp"

#include <cstdint>
#include <iostream>

// ── Host-side impl of env + clock_api ────────────────────────────────

struct Host
{
   int           call_count{0};
   std::uint64_t fake_clock{1000};

   void log_u64(std::uint64_t n)
   {
      ++call_count;
      std::cout << "  [host] log_u64(" << n << ")\n";
   }

   void log_string(std::string_view msg)
   {
      std::cout << "  [host] log_string(\"" << msg << "\")\n";
   }

   std::uint32_t sum_points_host(point a, point b)
   {
      std::cout << "  [host] sum_points_host({"
                << a.x << "," << a.y << "}, {"
                << b.x << "," << b.y << "})\n";
      return a.x + a.y + b.x + b.y;
   }

   std::uint64_t now() { return fake_clock; }
};

PSIO1_HOST_MODULE(Host,
          interface(env,       log_u64, log_string, sum_points_host),
          interface(clock_api, now))

// ── Driver ────────────────────────────────────────────────────────────

int main()
{
   Host host;
   psizam::hosted<Host, psizam::interpreter> vm{guest_wasm_bytes, host};
   auto greeter_proxy = vm.as<greeter>();

   std::cout << "run(5):\n";
   greeter_proxy.run(std::uint64_t{5});
   std::cout << "  host saw " << host.call_count << " callbacks\n";

   std::cout << "\nconcat(\"hello, \", \"world\") — zero-copy view:\n";
   auto joined = greeter_proxy.concat(std::string_view{"hello, "},
                                      std::string_view{"world"});
   std::cout << "  \"" << joined.view() << "\"  (no malloc, reads guest memory)\n";

   std::cout << "\nconcat — owning copy:\n";
   wit::string owned_joined = greeter_proxy.concat(
      std::string_view{"hello, "}, std::string_view{"world"});
   owned_joined += "!";
   std::cout << "  \"" << owned_joined.view() << "\"  (malloc'd, mutable)\n";

   std::cout << "\nadd(7, 11, 13):\n";
   std::uint32_t sum = greeter_proxy.add(7u, 11u, 13u);
   std::cout << "  = " << sum << "\n";

   std::cout << "\nsum_point({3, 4}):\n";
   std::uint32_t sp = greeter_proxy.sum_point(point{3, 4});
   std::cout << "  = " << sp << "\n";

   std::cout << "\nmake_point(10, 20):\n";
   point mp = greeter_proxy.make_point(10u, 20u);
   std::cout << "  = (" << mp.x << ", " << mp.y << ")\n";

   std::cout << "\ntranslate({5, 6}, -2, 3):\n";
   point t = greeter_proxy.translate(point{5, 6}, -2, 3);
   std::cout << "  = (" << t.x << ", " << t.y << ")\n";

   std::cout << "\nsum_list([1, 2, 3, 4, 5]):\n";
   std::uint32_t ls = greeter_proxy.sum_list(
      std::vector<std::uint32_t>{1u, 2u, 3u, 4u, 5u});
   std::cout << "  = " << ls << "\n";

   std::cout << "\nfind_first([10, 20, 30, 40], 30):\n";
   auto idx = greeter_proxy.find_first(
      std::vector<std::int32_t>{10, 20, 30, 40}, 30);
   std::cout << "  = " << (idx ? std::to_string(*idx) : std::string{"<none>"}) << "\n";

   std::cout << "\nfind_first([10, 20, 30], 99):\n";
   auto idx2 = greeter_proxy.find_first(
      std::vector<std::int32_t>{10, 20, 30}, 99);
   std::cout << "  = " << (idx2 ? std::to_string(*idx2) : std::string{"<none>"}) << "\n";

   std::cout << "\nrange(6) — zero-copy:\n";
   auto r6 = greeter_proxy.range(6u);
   std::cout << "  [";
   for (std::size_t i = 0; i < r6.size(); ++i)
      std::cout << (i ? ", " : "") << r6[i];
   std::cout << "]  (reads guest memory directly)\n";

   std::cout << "\nmake_grid(3, 2) — zero-copy:\n";
   auto g = greeter_proxy.make_grid(3u, 2u);
   for (std::size_t i = 0; i < g.size(); ++i)
      std::cout << "  [" << i << "] = (" << g[i].x << ", " << g[i].y << ")\n";

   return 0;
}
