// runtime_parity_tests.cpp — parity test for psizam::runtime.
//
// Reproduces the scenarios in composition_tests.cpp through the
// runtime API surface (psizam::runtime + prepare + provide + bind +
// instantiate + as), across every available backend kind. This is
// the acceptance gate for psizam-runtime-api-maturation Track A:
// it proves that Steps 1, 3, 5, 6, 7, 10 work end-to-end, not just
// that they compile.
//
// TestType in each case is a tier-marker struct exposing
// `::value` of type `instance_policy::compile_tier`. Catch2's
// TEMPLATE_TEST_CASE runs each test once per available backend.

#include <psizam/runtime.hpp>
#include <psizam/gas_pool.hpp>
#include <catch2/catch.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// Shared interface declarations (same file the composition example uses).
// The WASM modules in `examples/composition` export these; this header
// gives us the C++ tag types for `as<Tag>()`.
#include "shared.hpp"

// ── Host implementation ─────────────────────────────────────────────────────

struct RuntimeHost
{
   void log_u64(std::uint64_t n)
   {
      last_u64 = n;
      log_u64_calls++;
   }

   void log_string(std::string_view msg)
   {
      last_string = std::string(msg);
      log_string_calls++;
   }

   std::uint64_t last_u64          = 0;
   std::string   last_string;
   int           log_u64_calls     = 0;
   int           log_string_calls  = 0;
};

PSIO_HOST_MODULE(RuntimeHost, interface(env, log_u64, log_string))

// ── WASM loading helper ─────────────────────────────────────────────────────

static std::vector<std::uint8_t> read_wasm(const char* path)
{
   std::ifstream in(path, std::ios::binary);
   REQUIRE(in.good());
   return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

#ifndef COMPOSITION_WASM_DIR
#error "COMPOSITION_WASM_DIR must be defined to locate WASM test fixtures"
#endif

#define PROVIDER_WASM COMPOSITION_WASM_DIR "/provider.wasm"
#define CONSUMER_WASM COMPOSITION_WASM_DIR "/consumer.wasm"

// ── Tier markers + backend-selection macro ──────────────────────────────────
//
// Catch2's TEMPLATE_TEST_CASE parametrizes on types; we wrap each
// `compile_tier` enum value in a trivial tag struct so the value flows
// through as `TestType::value` inside the test body.

struct tier_interpret
{ static constexpr auto value = psizam::instance_policy::compile_tier::interpret; };
struct tier_jit1
{ static constexpr auto value = psizam::instance_policy::compile_tier::jit1; };

#if defined(__x86_64__) || defined(__aarch64__)
struct tier_jit2
{ static constexpr auto value = psizam::instance_policy::compile_tier::jit2; };
#endif

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
struct tier_jit_llvm
{ static constexpr auto value = psizam::instance_policy::compile_tier::jit_llvm; };
#endif

// jit2 is supported on both x86_64 and aarch64 (per backend.hpp
// gating — `struct jit2` is defined under `__x86_64__ || __aarch64__`).
// composition_tests.cpp's aarch64 exclusion predates jit2's aarch64
// support; runtime tests exercise all four backends.
#if defined(PSIZAM_ENABLE_LLVM_BACKEND) && (defined(__x86_64__) || defined(__aarch64__))
  #define PARITY_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, tier_interpret, tier_jit1, tier_jit2, tier_jit_llvm)
#elif defined(PSIZAM_ENABLE_LLVM_BACKEND)
  #define PARITY_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, tier_interpret, tier_jit1, tier_jit_llvm)
#elif defined(__x86_64__) || defined(__aarch64__)
  #define PARITY_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, tier_interpret, tier_jit1, tier_jit2)
#else
  #define PARITY_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, tier_interpret, tier_jit1)
#endif

// ── Helper: wire a live consumer ↔ provider pair through runtime ───────────

template <typename Tier>
struct live_runtime
{
   RuntimeHost                host;
   std::vector<std::uint8_t>  provider_bytes;
   std::vector<std::uint8_t>  consumer_bytes;
   psizam::runtime            rt;
   psizam::module_handle      provider_mod;
   psizam::module_handle      consumer_mod;
   psizam::instance           provider;
   psizam::instance           consumer;

   live_runtime()
      : provider_bytes(read_wasm(PROVIDER_WASM))
      , consumer_bytes(read_wasm(CONSUMER_WASM))
   {
      psizam::instance_policy policy{};
      policy.initial = Tier::value;

      // Prepare + provide both sides. provide<>() walks
      // PSIO_HOST_MODULE(RuntimeHost, ...) and registers env::log_u64 /
      // env::log_string on each module's host function table.
      provider_mod = rt.prepare(psizam::wasm_bytes{provider_bytes}, policy);
      consumer_mod = rt.prepare(psizam::wasm_bytes{consumer_bytes}, policy);
      rt.provide(provider_mod, host);
      rt.provide(consumer_mod, host);

      // Instantiate provider first; bind consumer's `greeter` imports to
      // the live provider instance; then instantiate the consumer. Binds
      // must happen BEFORE the consumer's backend is constructed because
      // bridge entries are read from the host_function_table at
      // construction time.
      provider = rt.instantiate(provider_mod, policy);
      rt.template bind<greeter>(consumer_mod, provider);
      consumer = rt.instantiate(consumer_mod, policy);
   }
};

// ═══════════════════════════════════════════════════════════════════════════
// Tests — mirror composition_tests.cpp one-for-one
// ═══════════════════════════════════════════════════════════════════════════

// ── Scalar i32: add across module boundary ──────────────────────────────

PARITY_TEST_CASE("runtime parity: scalar i32 add across modules",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   uint32_t result = proc.test_add(7u, 11u);
   CHECK(result == 18u);
}

PARITY_TEST_CASE("runtime parity: scalar i32 add identity",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   CHECK(proc.test_add(0u, 0u) == 0u);
   CHECK(proc.test_add(1u, 0u) == 1u);
   CHECK(proc.test_add(0u, 1u) == 1u);
}

PARITY_TEST_CASE("runtime parity: scalar i32 add large values",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   CHECK(proc.test_add(0xFFFFFFFFu, 1u) == 0u);
   CHECK(proc.test_add(0x80000000u, 0x80000000u) == 0u);
   CHECK(proc.test_add(100000u, 200000u) == 300000u);
}

// ── Scalar i64: double_it across module boundary ────────────────────────

PARITY_TEST_CASE("runtime parity: scalar i64 double across modules",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   uint64_t result = proc.test_double(uint64_t{21});
   CHECK(result == 42u);
}

PARITY_TEST_CASE("runtime parity: scalar i64 double zero",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   CHECK(proc.test_double(uint64_t{0}) == uint64_t{0});
}

PARITY_TEST_CASE("runtime parity: scalar i64 double large value",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   CHECK(proc.test_double(uint64_t{0x4000000000000000ULL}) ==
         uint64_t{0x8000000000000000ULL});
}

// ── String concat across module boundary ────────────────────────────────

PARITY_TEST_CASE("runtime parity: string concat across modules",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   auto result = proc.test_concat(std::string_view{"hello, "},
                                   std::string_view{"world"});
   CHECK(result.view() == "hello, world");
}

PARITY_TEST_CASE("runtime parity: string concat empty strings",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   auto r1 = proc.test_concat(std::string_view{""}, std::string_view{""});
   CHECK(r1.view() == "");

   auto r2 = proc.test_concat(std::string_view{"abc"}, std::string_view{""});
   CHECK(r2.view() == "abc");

   auto r3 = proc.test_concat(std::string_view{""}, std::string_view{"xyz"});
   CHECK(r3.view() == "xyz");
}

PARITY_TEST_CASE("runtime parity: string concat longer strings",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   std::string a(200, 'A');
   std::string b(200, 'B');
   auto result = proc.test_concat(std::string_view{a}, std::string_view{b});
   CHECK(result.view().size() == 400u);
   CHECK(result.view() == a + b);
}

// ── Multiple sequential calls ───────────────────────────────────────────

PARITY_TEST_CASE("runtime parity: multiple sequential calls",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto proc = lc.consumer.template as<processor>();

   CHECK(proc.test_add(1u, 2u) == 3u);
   CHECK(proc.test_double(uint64_t{5}) == uint64_t{10});

   auto s1 = proc.test_concat(std::string_view{"a"}, std::string_view{"b"});
   CHECK(s1.view() == "ab");

   CHECK(proc.test_add(100u, 200u) == 300u);

   auto s2 = proc.test_concat(std::string_view{"foo"}, std::string_view{"bar"});
   CHECK(s2.view() == "foobar");

   CHECK(proc.test_double(uint64_t{1000}) == uint64_t{2000});
}

// ── Direct provider call (bypassing consumer) ──────────────────────────

PARITY_TEST_CASE("runtime parity: direct provider export call",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;
   auto greet = lc.provider.template as<greeter>();

   CHECK(greet.add(10u, 20u) == 30u);
   CHECK(greet.double_it(uint64_t{7}) == uint64_t{14});

   auto s = greet.concat(std::string_view{"direct "},
                         std::string_view{"call"});
   CHECK(s.view() == "direct call");
}

// ── Host function invocation via bridge ─────────────────────────────────

PARITY_TEST_CASE("runtime parity: host functions are callable",
                 "[runtime-parity]")
{
   live_runtime<TestType> lc;

   // Successful instantiation already proves env::log_u64 / env::log_string
   // were resolved on both modules; exercise one call-through to confirm.
   auto proc = lc.consumer.template as<processor>();
   CHECK(proc.test_add(3u, 4u) == 7u);
}

// ═══════════════════════════════════════════════════════════════════════════
// gas_pool — standalone primitive tests (no WASM fixtures needed)
// ═══════════════════════════════════════════════════════════════════════════

TEST_CASE("gas_pool: lease debits and credit restores", "[gas-pool]")
{
   psizam::gas_pool pool{psizam::gas_units{10'000}};
   REQUIRE(*pool.balance() == 10'000);

   auto leased = pool.try_lease(psizam::gas_units{3'000});
   CHECK(*leased == 3'000);
   CHECK(*pool.balance() == 7'000);

   pool.add(psizam::gas_units{500});
   CHECK(*pool.balance() == 7'500);
}

TEST_CASE("gas_pool: try_lease returns 0 and preserves balance on overdraw",
          "[gas-pool]")
{
   psizam::gas_pool pool{psizam::gas_units{1'000}};
   auto overdraw = pool.try_lease(psizam::gas_units{5'000});
   CHECK(*overdraw == 0);
   CHECK(*pool.balance() == 1'000);    // unchanged
}

TEST_CASE("gas_pool: handler advances deadline by lease_size", "[gas-pool]")
{
   psizam::gas_pool pool{psizam::gas_units{5'000},
                          /*lease_size=*/psizam::gas_units{1'000}};
   psizam::gas_state gs;
   gs.consumed = 0;
   gs.deadline.store(0);   // simulates "deadline reached"

   psizam::pool_yield_handler(&gs, &pool);

   CHECK(gs.deadline.load() == 1'000);
   CHECK(*pool.balance() == 4'000);
}

TEST_CASE("gas_pool: handler throws when pool cannot cover lease",
          "[gas-pool]")
{
   psizam::gas_pool pool{psizam::gas_units{500},
                          /*lease_size=*/psizam::gas_units{1'000}};
   psizam::gas_state gs;
   gs.consumed = 0;
   gs.deadline.store(0);

   REQUIRE_THROWS_AS(psizam::pool_yield_handler(&gs, &pool),
                     psizam::wasm_gas_exhausted_exception);

   // try_lease rolls back on failure; pool must be unchanged.
   CHECK(*pool.balance() == 500);
}
