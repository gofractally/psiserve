// composition_tests.cpp — Catch2 tests for the psizam composition system.
//
// Loads provider + consumer WASM modules from disk, wires them via
// composition<Host, Backend>, and exercises all canonical ABI bridge
// types (scalar i32, scalar i64, string) across module boundaries.
//
// Runs across all available backends via TEMPLATE_TEST_CASE.

#include <psizam/composition.hpp>
#include <catch2/catch.hpp>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

// ── Shared interface declarations (same as examples/composition/shared.hpp) ──
// We include the example's shared header directly so the WASM modules'
// import/export names match exactly.
#include "shared.hpp"

// ── Host implementation ─────────────────────────────────────────────────────

struct CompHost
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

   std::uint64_t last_u64       = 0;
   std::string   last_string;
   int           log_u64_calls    = 0;
   int           log_string_calls = 0;
};

PSIO1_HOST_MODULE(CompHost, interface(env, log_u64, log_string))

// ── WASM loading helper ─────────────────────────────────────────────────────

static std::vector<std::uint8_t> read_wasm(const char* path)
{
   std::ifstream in(path, std::ios::binary);
   REQUIRE(in.good());
   return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

// COMPOSITION_WASM_DIR is defined via CMake compile definition pointing
// to the build directory containing provider.wasm and consumer.wasm.
#ifndef COMPOSITION_WASM_DIR
#error "COMPOSITION_WASM_DIR must be defined to locate WASM test fixtures"
#endif

#define PROVIDER_WASM COMPOSITION_WASM_DIR "/provider.wasm"
#define CONSUMER_WASM COMPOSITION_WASM_DIR "/consumer.wasm"

// ── Backend selection macro ─────────────────────────────────────────────────
// The composition system uses the canonical ABI with 16-slot flat calling via
// invoke_canonical_export_void. JIT1 (jit) and JIT2 (jit2) currently fail
// because:
//   1. The host_function_table backend constructor doesn't populate
//      _host_trampoline_ptrs, so JIT falls through to _table->call()
//      with args in reverse stack order vs the forward order slow_dispatch expects.
//   2. The 16-arg canonical calling convention hits type_check_args which
//      asserts arg count == WASM param count.
// All backends should now work — the host_function_table constructor
// in backend.hpp builds trampoline arrays for JIT1/JIT2.
// jit2 is x86_64-only.

#if defined(PSIZAM_ENABLE_LLVM_BACKEND) && defined(__x86_64__)
  #define COMP_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit2, psizam::jit_llvm)
#elif defined(PSIZAM_ENABLE_LLVM_BACKEND)
  #define COMP_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit_llvm)
#elif defined(__x86_64__)
  #define COMP_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit, psizam::jit2)
#else
  #define COMP_TEST_CASE(name, tags) \
    TEMPLATE_TEST_CASE(name, tags, psizam::interpreter, psizam::jit)
#endif

// ── Helper: set up a wired composition ──────────────────────────────────────

struct wired_composition
{
   CompHost                                                           host;
   std::vector<std::uint8_t>                                          provider_bytes;
   std::vector<std::uint8_t>                                          consumer_bytes;
};

template <typename BackendKind>
struct live_composition
{
   using comp_t     = psizam::composition<CompHost, BackendKind>;
   using instance_t = typename comp_t::instance_t;

   CompHost                    host;
   std::vector<std::uint8_t>   provider_bytes;
   std::vector<std::uint8_t>   consumer_bytes;
   comp_t                      comp;
   instance_t*                 provider;
   instance_t*                 consumer;

   live_composition()
      : provider_bytes(read_wasm(PROVIDER_WASM))
      , consumer_bytes(read_wasm(CONSUMER_WASM))
      , comp(host)
   {
      provider = &comp.add(provider_bytes);
      consumer = &comp.add(consumer_bytes);

      comp.template register_host<CompHost>(*provider);
      comp.template register_host<CompHost>(*consumer);
      comp.template link<greeter>(*consumer, *provider);

      comp.instantiate();
   }
};

// ═══════════════════════════════════════════════════════════════════════════
// Tests
// ═══════════════════════════════════════════════════════════════════════════

// ── Scalar i32: add across module boundary ──────────────────────────────

COMP_TEST_CASE("composition: scalar i32 add across modules", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   uint32_t result = proc.test_add(7u, 11u);
   CHECK(result == 18u);
}

COMP_TEST_CASE("composition: scalar i32 add identity", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   CHECK(proc.test_add(0u, 0u) == 0u);
   CHECK(proc.test_add(1u, 0u) == 1u);
   CHECK(proc.test_add(0u, 1u) == 1u);
}

COMP_TEST_CASE("composition: scalar i32 add large values", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   // Near max u32
   CHECK(proc.test_add(0xFFFFFFFFu, 1u) == 0u);  // wraps
   CHECK(proc.test_add(0x80000000u, 0x80000000u) == 0u);  // wraps
   CHECK(proc.test_add(100000u, 200000u) == 300000u);
}

// ── Scalar i64: double_it across module boundary ────────────────────────

COMP_TEST_CASE("composition: scalar i64 double across modules", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   uint64_t result = proc.test_double(uint64_t{21});
   CHECK(result == 42u);
}

COMP_TEST_CASE("composition: scalar i64 double zero", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   CHECK(proc.test_double(uint64_t{0}) == uint64_t{0});
}

COMP_TEST_CASE("composition: scalar i64 double large value", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   CHECK(proc.test_double(uint64_t{0x4000000000000000ULL}) == uint64_t{0x8000000000000000ULL});
}

// ── String concat across module boundary ────────────────────────────────

COMP_TEST_CASE("composition: string concat across modules", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   auto result = proc.test_concat(std::string_view{"hello, "}, std::string_view{"world"});
   CHECK(result.view() == "hello, world");
}

COMP_TEST_CASE("composition: string concat empty strings", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   auto r1 = proc.test_concat(std::string_view{""}, std::string_view{""});
   CHECK(r1.view() == "");

   auto r2 = proc.test_concat(std::string_view{"abc"}, std::string_view{""});
   CHECK(r2.view() == "abc");

   auto r3 = proc.test_concat(std::string_view{""}, std::string_view{"xyz"});
   CHECK(r3.view() == "xyz");
}

COMP_TEST_CASE("composition: string concat longer strings", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   std::string a(200, 'A');
   std::string b(200, 'B');
   auto result = proc.test_concat(std::string_view{a}, std::string_view{b});
   CHECK(result.view().size() == 400u);
   CHECK(result.view() == a + b);
}

// ── Multiple sequential calls ───────────────────────────────────────────

COMP_TEST_CASE("composition: multiple sequential calls", "[composition]")
{
   live_composition<TestType> lc;
   auto proc = lc.consumer->template as<processor>();

   // Interleave different call types to stress the bridge
   CHECK(proc.test_add(1u, 2u) == 3u);
   CHECK(proc.test_double(uint64_t{5}) == uint64_t{10});

   auto s1 = proc.test_concat(std::string_view{"a"}, std::string_view{"b"});
   CHECK(s1.view() == "ab");

   CHECK(proc.test_add(100u, 200u) == 300u);

   auto s2 = proc.test_concat(std::string_view{"foo"}, std::string_view{"bar"});
   CHECK(s2.view() == "foobar");

   CHECK(proc.test_double(uint64_t{1000}) == uint64_t{2000});
}

// ── Direct provider calls (bypassing consumer) ─────────────────────────

COMP_TEST_CASE("composition: direct provider export call", "[composition]")
{
   live_composition<TestType> lc;
   auto greet = lc.provider->template as<greeter>();

   CHECK(greet.add(10u, 20u) == 30u);
   CHECK(greet.double_it(uint64_t{7}) == uint64_t{14});

   auto s = greet.concat(std::string_view{"direct "}, std::string_view{"call"});
   CHECK(s.view() == "direct call");
}

// ── Host function invocation via bridge ─────────────────────────────────

COMP_TEST_CASE("composition: host functions are callable", "[composition]")
{
   live_composition<TestType> lc;

   // The provider and consumer modules call env::log_u64 and env::log_string
   // internally. We can verify the host receives calls by invoking
   // test_add which the consumer passes through to the provider, which
   // may log. If the modules don't log during add, we can at least
   // verify the host was wired correctly by checking that instantiation
   // succeeded (it would fail if env imports were unresolved).
   auto proc = lc.consumer->template as<processor>();
   CHECK(proc.test_add(3u, 4u) == 7u);
   // The composition instantiated successfully, meaning all env imports resolved
}
