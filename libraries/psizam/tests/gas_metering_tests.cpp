// Gas metering — BACKEND_TEST_CASE suite covering trap, user-handler,
// external-interrupt (deadline-store), unlimited, and cross-backend
// determinism. Exercises the paths that the bench never touches.
//
// The WASM module is implementation_limits_wasm's recursive `call`
// function: takes an i32 depth, recurses depth+1 times before
// returning. Each call pays gas_charge once (function entry), so
// depth=N → N+1 charges.
//
// Phase-4 heavy-opcode coverage uses gas_heavy_wasm's `loop_div`
// function: a straight-line loop whose body contains an i64.div_s.
// Each iteration charges 1 (back-edge baseline) + 9 (div_s heavy
// extra) = 10 gas units. See `gas: heavy-op ...` tests below.
//
// gas_state redesign:
//   * `consumed` counts up monotonically (plain uint64_t, owner-only).
//   * `deadline` is an atomic uint64_t; any thread may store it.
//   * Handler fires when `consumed >= deadline`.
//   * The handler signature is (gas_state*, void*); to extend the
//     deadline (yield-style), store `consumed + slice` into it.
//   * External interrupt = deadline.store(0, relaxed) — the next
//     charge site observes `consumed >= 0` and fires the handler.

#include "implementation_limits.hpp"
#include "gas_heavy.wasm.hpp"
#include <atomic>
#include <chrono>
#include <thread>

static void gas_host_call() {}

BACKEND_TEST_CASE("gas: unlimited budget never traps", "[gas]") {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, TestType>;
   rhf_t::add<&gas_host_call>("env", "host.call");

   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   // Default state: deadline=UINT64_MAX, handler=null → never traps.
   CHECK_NOTHROW(bkend.call("env", "call", uint32_t{100}));

   // Explicitly setting an unlimited budget still never traps.
   bkend.get_context().set_gas_budget(psizam::gas_units{psizam::gas_budget_unlimited});
   CHECK_NOTHROW(bkend.call("env", "call", uint32_t{100}));
}

BACKEND_TEST_CASE("gas: trap throws wasm_gas_exhausted_exception", "[gas]") {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, TestType>;
   rhf_t::add<&gas_host_call>("env", "host.call");

   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   bkend.get_context().set_gas_handler(nullptr);                    // default → throw
   bkend.get_context().set_gas_budget(psizam::gas_units{5});        // small budget
   // `call` recurses depth+1 times; depth=50 = 51 charges, well over 5.
   CHECK_THROWS_AS(bkend.call("env", "call", uint32_t{50}),
                   psizam::wasm_gas_exhausted_exception);
}

BACKEND_TEST_CASE("gas: user handler invoked on exhaustion (yield-style)", "[gas]") {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, TestType>;
   rhf_t::add<&gas_host_call>("env", "host.call");

   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   // yield-style handler: advances the deadline by a slice, counts
   // invocations, and returns so execution resumes. Tests the
   // non-throwing handler path cleanly across every backend (JIT
   // exception propagation has its own machinery in other tests).
   static thread_local int yield_count;
   yield_count = 0;
   bkend.get_context().set_gas_handler(+[](psizam::gas_state* gs, void*) {
      yield_count++;
      // Advance deadline by a generous slice so execution resumes.
      gs->deadline.store(gs->consumed + 1000, std::memory_order_relaxed);
   });
   bkend.get_context().set_gas_budget(psizam::gas_units{3});
   // depth=20 recurses 21 times → exhausts budget 3 → handler fires
   // multiple times (each time it advances the deadline by 1000,
   // letting execution resume).
   CHECK_NOTHROW(bkend.call("env", "call", uint32_t{20}));
   CHECK(yield_count > 0);
}

// Helper for the determinism test: instantiate a backend, set a fixed
// budget, call `call(depth)`, and return `consumed` at the first
// handler invocation. A yield-style handler advances the deadline so
// the trap-observation point is clean and execution finishes. Every
// backend with the same budget + same call sees the same `consumed`.
template <typename Impl>
uint64_t gas_consumed_at_first_trap(psizam::wasm_code& code,
                                    uint64_t budget, uint32_t depth) {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, Impl>;
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   static thread_local uint64_t seen_consumed;
   static thread_local bool     seen;
   seen_consumed = 0;
   seen = false;
   bkend.get_context().set_gas_handler(+[](psizam::gas_state* gs, void*) {
      if (!seen) {
         seen_consumed = gs->consumed;
         seen = true;
      }
      gs->deadline.store(psizam::gas_budget_unlimited, std::memory_order_relaxed);
   });
   bkend.get_context().set_gas_budget(psizam::gas_units{budget});
   (void)bkend.call("env", "call", depth);
   return seen_consumed;
}

TEST_CASE("gas: cross-backend determinism", "[gas][determinism]") {
   psizam::registered_host_functions<psizam::standalone_function_t>
      ::add<&gas_host_call>("env", "host.call");
   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);

   // Budget small enough to exhaust mid-recursion but large enough
   // to fire the handler at a well-defined point.
   constexpr uint64_t budget = 5;
   constexpr uint32_t depth  = 50;

   uint64_t interp = gas_consumed_at_first_trap<psizam::interpreter>(code, budget, depth);
   INFO("interpreter first-trap consumed=" << interp);
   REQUIRE(interp >= budget);
#if defined(__x86_64__) || defined(__aarch64__)
   uint64_t j1 = gas_consumed_at_first_trap<psizam::jit>(code, budget, depth);
   uint64_t j2 = gas_consumed_at_first_trap<psizam::jit2>(code, budget, depth);
   INFO("jit first-trap consumed=" << j1);
   INFO("jit2 first-trap consumed=" << j2);
   CHECK(j1 == interp);
   CHECK(j2 == interp);
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
   uint64_t jl = gas_consumed_at_first_trap<psizam::jit_llvm>(code, budget, depth);
   INFO("jit_llvm first-trap consumed=" << jl);
   CHECK(jl == interp);
#endif
}

BACKEND_TEST_CASE("gas: external interrupt via deadline store(0)", "[gas][interrupt]") {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, TestType>;
   rhf_t::add<&gas_host_call>("env", "host.call");

   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   bkend.get_context().set_gas_budget(psizam::gas_units{psizam::gas_budget_unlimited});
   bkend.get_context().set_gas_handler(nullptr);

   // Fire the interrupt immediately: deadline=0 means any consumed bump
   // trips the handler. The next function-entry charge site throws.
   // No race with a live thread — the deadline store happens-before
   // the call. (Live-thread variant is `gas: cross-thread interrupt`.)
   bkend.get_context().gas().deadline.store(0, std::memory_order_relaxed);
   CHECK_THROWS_AS(bkend.call("env", "call", uint32_t{10}),
                   psizam::wasm_gas_exhausted_exception);
}

// Cross-thread interrupt: a watcher thread shortens the deadline while
// the backend is mid-execution. Every charge site re-reads the atomic
// deadline (relaxed load) so the interrupt is observed within one
// charge-site latency. Acceptance criterion from
// .issues/psizam-gas-state-redesign.md:
//   "watcher thread stores a smaller deadline mid-execution; running
//    backend traps within one loop iteration on modes that declare
//    interrupt=true."
BACKEND_TEST_CASE("gas: cross-thread interrupt via watcher thread",
                  "[gas][interrupt][cross-thread]") {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, TestType>;
   rhf_t::add<&gas_host_call>("env", "host.call");

   psizam::wasm_code code(gas_heavy_wasm, gas_heavy_wasm + gas_heavy_wasm_len);
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   bkend.get_context().set_gas_budget(psizam::gas_units{psizam::gas_budget_unlimited});
   bkend.get_context().set_gas_handler(nullptr);

   // A "running" flag so the watcher thread doesn't fire before the
   // backend has actually started executing — otherwise the interrupt
   // collapses into the set-before-call case.
   std::atomic<bool> running{false};
   auto* gs = &bkend.get_context().gas();
   std::thread watcher([&]() {
      while (!running.load(std::memory_order_acquire)) {
         std::this_thread::yield();
      }
      // Give the WASM loop a moment to start making progress, then
      // interrupt. The store is relaxed — the next charge site will
      // observe it regardless of memory ordering.
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
      gs->deadline.store(0, std::memory_order_relaxed);
   });

   running.store(true, std::memory_order_release);

   // loop_div runs (a / b) in a tight iters-iteration loop. With iters
   // large and budget unlimited this would run for a long time if the
   // watcher never fired. Anything under a second is evidence the
   // interrupt was observed within a handful of loop iterations.
   constexpr uint32_t huge_iters = 100'000'000;
   auto t0 = std::chrono::steady_clock::now();
   CHECK_THROWS_AS(bkend.call("env", "loop_div",
                              int64_t{1'000'000}, int64_t{3}, huge_iters),
                   psizam::wasm_gas_exhausted_exception);
   auto elapsed = std::chrono::steady_clock::now() - t0;
   watcher.join();

   // Latency bound: interrupt observed well before the loop would
   // naturally complete. 1 second is generous — actual latency should
   // be microseconds. Keeps the test non-flaky on slow CI.
   CHECK(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() < 1);
}

// ── Phase-4 heavy-opcode accounting ──────────────────────────────────
//
// These tests exercise the per-scope accumulator in
// gas_injector::gas_injection_state. Two paths land heavy-op extras
// at different charge points:
//
//   prologue_gas_extra  — heavy ops outside any loop fold into the
//                         function-entry prepay. Verified below via
//                         implementation_limits's call.indirect
//                         recursion (call_indirect weight=5 ⇒ +4
//                         extra per entry).
//
//   loop_gas_extra      — heavy ops inside a loop fold into the
//                         per-back-edge loop-header charge. Verified
//                         below via gas_heavy_wasm's loop_div (one
//                         i64.div_s per iteration ⇒ +9 extra per
//                         iteration on top of the 1-per-back-edge
//                         baseline).

// Variant of gas_consumed_at_first_trap that targets an arbitrary
// export. Keeps the yield-style handler semantics so the trap point
// is visible as a consumed snapshot.
template <typename Impl>
uint64_t gas_consumed_at_first_trap_export(psizam::wasm_code& code,
                                           const char* export_name,
                                           uint64_t budget,
                                           uint32_t depth) {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, Impl>;
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   static thread_local uint64_t seen_consumed;
   static thread_local bool     seen;
   seen_consumed = 0;
   seen = false;
   bkend.get_context().set_gas_handler(+[](psizam::gas_state* gs, void*) {
      if (!seen) {
         seen_consumed = gs->consumed;
         seen = true;
      }
      gs->deadline.store(psizam::gas_budget_unlimited, std::memory_order_relaxed);
   });
   bkend.get_context().set_gas_budget(psizam::gas_units{budget});
   (void)bkend.call("env", export_name, depth);
   return seen_consumed;
}

// Prologue-extras cross-backend determinism.
//
// call.indirect's body contains one call_indirect outside any loop,
// so the parser folds +4 (call_indirect weight 5 − regular 1) into
// prologue_gas_extra. Every function-entry charge bumps consumed by
// (body_bytes + 4). With a small budget the first such charge trips
// the handler, and every backend sees the same `consumed` value —
// that equality is what this test asserts.
//
// (Why the interpreter matches despite its top-level execute() not
// going through context.call(): the trap fires on the first
// internal WASM `call_indirect` opcode, at which point the
// interpreter's context.call() path charges the same body_bytes + 4
// as each JIT's prologue — see docs/gas-metering-design.md.)
TEST_CASE("gas: heavy-op prologue-extra cross-backend determinism",
          "[gas][determinism][heavy]") {
   psizam::registered_host_functions<psizam::standalone_function_t>
      ::add<&gas_host_call>("env", "host.call");
   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);

   constexpr uint64_t budget = 5;
   constexpr uint32_t depth  = 50;

   uint64_t interp = gas_consumed_at_first_trap_export<psizam::interpreter>(
      code, "call.indirect", budget, depth);
   INFO("interpreter first-trap consumed=" << interp);
   REQUIRE(interp >= budget);
#if defined(__x86_64__) || defined(__aarch64__)
   uint64_t j1 = gas_consumed_at_first_trap_export<psizam::jit>(
      code, "call.indirect", budget, depth);
   uint64_t j2 = gas_consumed_at_first_trap_export<psizam::jit2>(
      code, "call.indirect", budget, depth);
   INFO("jit first-trap consumed=" << j1);
   INFO("jit2 first-trap consumed=" << j2);
   CHECK(j1 == interp);
   CHECK(j2 == interp);
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
   uint64_t jl = gas_consumed_at_first_trap_export<psizam::jit_llvm>(
      code, "call.indirect", budget, depth);
   INFO("jit_llvm first-trap consumed=" << jl);
   CHECK(jl == interp);
#endif
}

// Per-backend weighted-charge sanity check.
//
// Running N iterations of gas_heavy_wasm's loop_div consumes
// iters × (1 + div_rem_extra) = iters × 10 at the loop header,
// plus a single body-bytes prepay at function entry. If the
// heavy-op extra isn't being folded into the loop-header charge,
// consumed gas would be iters × 1 + prepay — an order of magnitude
// less. We assert consumed ≥ iters × 10 to confirm the extra fires,
// and cap it at 3× to catch regressions that over-charge (e.g.
// folding in the full weight *and* the extra).
BACKEND_TEST_CASE("gas: heavy-op loop-extra per-backend",
                  "[gas][heavy]") {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, TestType>;

   psizam::wasm_code code(gas_heavy_wasm,
                          gas_heavy_wasm + gas_heavy_wasm_len);
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   constexpr uint32_t iters             = 1000;
   constexpr int64_t  per_iter_extra    = psizam::gas_costs::div_rem
                                        - psizam::gas_costs::regular;  // 9
   constexpr int64_t  per_iter_weighted = 1 + per_iter_extra;          // 10

   bkend.get_context().set_gas_handler(nullptr);  // trap if we overrun
   bkend.get_context().set_gas_budget(psizam::gas_units{psizam::gas_budget_unlimited});
   (void)bkend.call("env", "loop_div",
                    int64_t{1'000'000}, int64_t{3}, iters);

   uint64_t consumed = bkend.get_context().gas().consumed;
   INFO("consumed=" << consumed << " iters=" << iters
        << " per_iter_weighted=" << per_iter_weighted);

   CHECK(consumed >= static_cast<uint64_t>(iters * per_iter_weighted));
   CHECK(consumed <= static_cast<uint64_t>(3 * iters * per_iter_weighted));
}
