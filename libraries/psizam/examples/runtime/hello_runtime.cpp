// hello_runtime.cpp — Phase 2: load WASM and call exports via runtime API.
//
// Loads the hello_world guest WASM, registers host functions,
// instantiates, and calls exports through typed proxies. Exercises
// the full runtime lifecycle: prepare → instantiate → call → gas.

#include <psizam/runtime.hpp>

// Interface declarations from the hello_world example
#include <psio1/structural.hpp>
#include <psio1/wit_owned.hpp>
#include <psio1/guest_attrs.hpp>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

// ── Shared interface declarations (same as hello_world/shared.hpp) ──

PSIO1_PACKAGE(hello, "0.1.0");
#undef  PSIO1_CURRENT_PACKAGE_
#define PSIO1_CURRENT_PACKAGE_ PSIO1_PACKAGE_TYPE_(hello)

struct point {
   uint32_t x{};
   uint32_t y{};
};
PSIO1_REFLECT(point, x, y)

struct env {
   static void log_u64(uint64_t n);
   static void log_string(std::string_view msg);
   static uint32_t sum_points_host(point a, point b);
};

struct clock_api {
   static uint64_t now();
};

struct greeter {
   static void               run(uint64_t count);
   static wit::string        concat(std::string_view a, std::string_view b);
   static uint32_t           add(uint32_t a, uint32_t b, uint32_t c);
   static uint32_t           sum_point(point p);
   static point              make_point(uint32_t x, uint32_t y);
   static point              translate(point p, int32_t dx, int32_t dy);
   static uint32_t           sum_list(std::vector<uint32_t> xs);
   static std::optional<uint32_t>
                             find_first(std::vector<int32_t> xs, int32_t needle);
   static wit::vector<uint32_t> range(uint32_t n);
   static wit::vector<point>    make_grid(uint32_t w, uint32_t h);
};

PSIO1_INTERFACE(env,       types(), funcs(func(log_u64, value),
                                         func(log_string, msg),
                                         func(sum_points_host, a, b)))
PSIO1_INTERFACE(clock_api, types(), funcs(func(now)))
PSIO1_INTERFACE(greeter,   types(point),
                          funcs(func(run,         count),
                                func(concat,      a, b),
                                func(add,         a, b, c),
                                func(sum_point,   p),
                                func(make_point,  x, y),
                                func(translate,   p, dx, dy),
                                func(sum_list,    xs),
                                func(find_first,  xs, needle),
                                func(range,       n),
                                func(make_grid,   w, h)))

// ── Host implementation ────────────────────────────────────────────

struct Host {
   int call_count = 0;
   uint64_t fake_clock = 1000;

   void log_u64(uint64_t n) {
      ++call_count;
      std::cout << "  [host] log_u64(" << n << ")\n";
   }

   void log_string(std::string_view msg) {
      std::cout << "  [host] log_string(\"" << msg << "\")\n";
   }

   uint32_t sum_points_host(point a, point b) {
      return a.x + a.y + b.x + b.y;
   }

   uint64_t now() { return fake_clock; }
};

PSIO1_HOST_MODULE(Host,
   interface(env,       log_u64, log_string, sum_points_host),
   interface(clock_api, now))

// ── Helpers ────────────────────────────────────────────────────────

static std::vector<uint8_t> read_file(const char* path) {
   std::ifstream f(path, std::ios::binary);
   if (!f) return {};
   return {std::istreambuf_iterator<char>(f), {}};
}

// ── Main ───────────────────────────────────────────────────────────

int main(int argc, char** argv) {
   if (argc < 2) {
      std::cerr << "Usage: hello_runtime <guest.wasm>\n";
      return 1;
   }

   auto wasm_data = read_file(argv[1]);
   if (wasm_data.empty()) {
      std::cerr << "Cannot read " << argv[1] << "\n";
      return 1;
   }

   using namespace psizam;

   // ── Create runtime ──────────────────────────────────────────────
   runtime rt;

   // ── Policy ──────────────────────────────────────────────────────
   auto policy = instance_policy{
      .trust_level = instance_policy::runtime_trust::untrusted,
      .floats      = instance_policy::float_mode::soft,
      .memory      = instance_policy::mem_safety::checked,
      .initial     = instance_policy::compile_tier::interpret,
      .metering    = instance_policy::meter_mode::gas_trap,
      .gas_budget  = 10'000'000,
   };

   // ── Shared gas ──────────────────────────────────────────────────
   auto gas = std::make_shared<gas_state>();
   gas->deadline = policy.gas_budget;
   policy.shared_gas = gas;

   // ── Register host functions ────────────────────────────────────
   auto tmpl = rt.prepare(wasm_bytes{wasm_data}, policy);

   Host host;
   rt.provide(tmpl, host);

   std::cout << "Module prepared: " << tmpl.wasm_size() << " bytes WASM\n\n";

   // ── Instantiate ─────────────────────────────────────────────────
   auto inst = rt.instantiate(tmpl, policy);
   std::cout << "Instance created\n";
   std::cout << "  gas consumed: " << inst.gas_consumed() << "\n";
   std::cout << "  gas deadline: " << inst.gas_deadline() << "\n";
   std::cout << "  memory size:  " << inst.memory_size() << " bytes\n\n";

   // ── Call exports via typed proxy ────────────────────────────────
   std::cout << "=== add(7, 11, 13) ===\n";
   auto sum = inst.as<greeter>().add(7u, 11u, 13u);
   std::cout << "  result = " << sum << "\n\n";

   std::cout << "=== concat(\"hello, \", \"world\") ===\n";
   auto joined = inst.as<greeter>().concat(
      std::string_view{"hello, "}, std::string_view{"world"});
   std::cout << "  result = \"" << joined.view() << "\"\n\n";

   std::cout << "=== make_point(10, 20) ===\n";
   point p = inst.as<greeter>().make_point(10u, 20u);
   std::cout << "  result = (" << p.x << ", " << p.y << ")\n\n";

   std::cout << "Gas consumed: " << inst.gas_consumed() << "\n";
   std::cout << "Gas deadline: " << inst.gas_deadline() << "\n";

   return 0;
}
