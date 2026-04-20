// host.cpp — loads the wasi_echo guest WASM and runs it with real WASI
// host implementations backed by the psiber scheduler.
//
// Build:  cmake --build build/Debug --target wasi_echo_host
// Run:    ./build/Debug/bin/wasi_echo_host [port]
//
// The guest WASM is embedded at build time (bin2hpp), so no separate
// .wasm file is needed at runtime.

#include "shared.hpp"
#include "guest_wasm.hpp"

#include <psizam/runtime.hpp>

#include <wasi/0.2.3/io_host.hpp>
#include <wasi/0.2.3/sockets_host.hpp>

#include <psiber/scheduler.hpp>

#include <cstdint>
#include <iostream>
#include <string>

using namespace wasi_host;
using psiber::Scheduler;

int main(int argc, char** argv)
{
   uint32_t port = 8080;
   if (argc > 1)
      port = static_cast<uint32_t>(std::stoul(argv[1]));

   auto& sched = Scheduler::current();

   WasiIoHost      io;
   WasiSocketsHost sockets(io);

   using namespace psizam;

   runtime rt;

   auto policy = instance_policy{
      .trust_level = instance_policy::runtime_trust::untrusted,
      .floats      = instance_policy::float_mode::soft,
      .memory      = instance_policy::mem_safety::checked,
      .initial     = instance_policy::compile_tier::interpret,
      .metering    = instance_policy::meter_mode::none,
   };

   auto tmpl = rt.prepare(wasm_bytes{guest_wasm_bytes}, policy);

   rt.provide(tmpl, io);
   rt.provide(tmpl, sockets);

   std::cout << "WASI echo service starting on port " << port << "...\n";

   sched.spawnFiber(
       [&]
       {
          auto inst = rt.instantiate(tmpl, policy);
          inst.as<wasi_echo_service>().run(port);
       },
       "wasi-echo");

   sched.run();
   return 0;
}
