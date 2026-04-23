#define CATCH_CONFIG_MAIN
#include <catch2/catch.hpp>

#include <psiserve/host_api.hpp>
#include <psiserve/log.hpp>
#include <psiserve/process.hpp>
#include <psiserve/scheduler.hpp>

#include <array>
#include <cstring>
#include <string>

// Focused coverage of HostApi::psiLog.  Drives it with a synthetic
// linear memory so the dispatch path that quill takes — truncation,
// null-memory guards, log-count accounting — is exercised without
// spinning up a full WASM instance.
//
// The actual log text goes through quill's async backend; rather than
// introspect quill internals (which change between versions), we
// observe the public side effects:
//   * psiLog never crashes or UB-accesses memory.
//   * Every successful invocation bumps logCount().
//   * Null-memory calls are counted and produce a host-side warning
//     instead of a segfault.
//
// Pairing these assertions with manual log inspection (when debugging
// a real service) is enough to catch regressions in the boundary.

using psiserve::HostApi;
using psiserve::Process;
using psiserve::Scheduler;
using psiserve::WasmPtr;
using psiserve::WasmSize;

// Mirror the level constants from <psi/host.h> without pulling in a
// wasm-only header on the host-side test binary.
constexpr int kDebug = 0;
constexpr int kInfo  = 1;
constexpr int kWarn  = 2;
constexpr int kError = 3;

namespace
{
   struct fixture
   {
      std::array<char, 4096>  memory{};
      Process                 proc;
      Scheduler&              sched;
      HostApi                 host;

      fixture()
         : sched(Scheduler::current())
         , host(proc, sched, memory.data(), nullptr)
      {
         psiserve::log::init();
         memory.fill(0);
      }

      // Place a NUL-free payload at offset 0, return a WasmPtr to it.
      WasmPtr put(std::string_view s)
      {
         std::memcpy(memory.data(), s.data(), s.size());
         return WasmPtr{0};
      }
   };
}

TEST_CASE("psiLog increments log count for each call", "[host_api][log]")
{
   fixture f;

   auto start = f.host.logCount();

   f.host.psiLog(kInfo, f.put("hello"), WasmSize{5});
   f.host.psiLog(kWarn, f.put("warn"),  WasmSize{4});
   f.host.psiLog(kError,f.put("err"),   WasmSize{3});
   f.host.psiLog(kDebug,f.put("dbg"),   WasmSize{3});

   CHECK(f.host.logCount() == start + 4);
}

TEST_CASE("psiLog unknown level defaults to info", "[host_api][log]")
{
   fixture f;
   auto start = f.host.logCount();

   f.host.psiLog(/*level=*/99, f.put("mystery"), WasmSize{7});

   CHECK(f.host.logCount() == start + 1);
}

TEST_CASE("psiLog tolerates zero-length payloads", "[host_api][log]")
{
   fixture f;
   auto start = f.host.logCount();

   f.host.psiLog(kInfo, WasmPtr{0}, WasmSize{0});

   CHECK(f.host.logCount() == start + 1);
}

TEST_CASE("psiLog truncates at log_max_len", "[host_api][log]")
{
   fixture f;

   // Fill the whole memory buffer with a known byte pattern and ask
   // the host to log WAY more than it is allowed to copy.
   std::memset(f.memory.data(), 'A', f.memory.size());

   auto start = f.host.logCount();
   f.host.psiLog(kInfo, WasmPtr{0}, WasmSize{ static_cast<int>(f.memory.size()) });
   CHECK(f.host.logCount() == start + 1);

   // Asking for more than the host is willing to copy must also not
   // walk past the payload we actually set up; this used to be a
   // UB trap before the log_max_len cap was added.
   f.host.psiLog(kInfo, WasmPtr{0}, WasmSize{ HostApi::log_max_len + 1024 });
}

TEST_CASE("psiLog with null linear memory is a diagnostic, not a crash", "[host_api][log]")
{
   psiserve::log::init();

   Process     proc;
   Scheduler&  sched = Scheduler::current();
   HostApi     host(proc, sched, /*wasm_memory=*/nullptr, nullptr);

   auto start = host.logCount();
   host.psiLog(kInfo, WasmPtr{0}, WasmSize{16});

   CHECK(host.logCount() == start + 1);
}
