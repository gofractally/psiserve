// Gas metering — BACKEND_TEST_CASE suite covering trap, user-handler,
// external-interrupt (atomic mode), unlimited, and cross-backend
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

#include "implementation_limits.hpp"
#include "gas_heavy.wasm.hpp"
#include <atomic>
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

   // Default state: strategy=off, unlimited budget → never traps.
   CHECK_NOTHROW(bkend.call("env", "call", uint32_t{100}));

   // Enabling strategy with unlimited budget still never traps.
   bkend.get_context().set_gas_strategy(psizam::gas_insertion_strategy::prepay_max);
   bkend.get_context().set_gas_budget(psizam::gas_budget_unlimited);
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

   bkend.get_context().set_gas_strategy(psizam::gas_insertion_strategy::prepay_max);
   bkend.get_context().set_gas_handler(nullptr);           // default → throw
   bkend.get_context().set_gas_budget(5);                   // small budget
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

   // yield-style handler: restocks the counter + counts invocations,
   // then returns so execution resumes. Tests the non-throwing handler
   // path cleanly across every backend (JIT exception propagation has
   // its own machinery exercised in other tests).
   static thread_local int yield_count;
   yield_count = 0;
   using ctx_t = typename TestType::context;
   bkend.get_context().set_gas_strategy(psizam::gas_insertion_strategy::prepay_max);
   bkend.get_context().set_gas_handler(+[](void* ctx) {
      yield_count++;
      static_cast<ctx_t*>(ctx)->restock_gas(1000);
   });
   bkend.get_context().set_gas_budget(3);
   // depth=20 recurses 21 times → exhausts budget 3 → handler fires
   // multiple times (each time it restocks 1000, letting execution resume).
   CHECK_NOTHROW(bkend.call("env", "call", uint32_t{20}));
   CHECK(yield_count > 0);
}

// Helper for the determinism test: instantiate a backend, set a fixed
// budget, call `call(depth)`, and return the counter value observed
// after the trap (by using a handler that snapshots the counter and
// restocks). Every backend with the same budget + same call should
// see the same final gas_counter value.
template <typename Impl>
int64_t gas_count_to_trap(psizam::wasm_code& code, int64_t budget,
                          uint32_t depth) {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, Impl>;
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   using ctx_t = typename Impl::context;
   static thread_local int64_t seen_counter = 0;
   seen_counter = 0;
   bkend.get_context().set_gas_strategy(psizam::gas_insertion_strategy::prepay_max);
   bkend.get_context().set_gas_handler(+[](void* ctx) {
      auto* c = static_cast<ctx_t*>(ctx);
      if (seen_counter == 0) {
         seen_counter = c->gas_counter().load(std::memory_order_relaxed);
      }
      c->restock_gas(psizam::gas_budget_unlimited);
   });
   bkend.get_context().set_gas_budget(budget);
   (void)bkend.call("env", "call", depth);
   return seen_counter;
}

TEST_CASE("gas: cross-backend determinism", "[gas][determinism]") {
   psizam::registered_host_functions<psizam::standalone_function_t>
      ::add<&gas_host_call>("env", "host.call");
   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);

   // Budget small enough to exhaust mid-recursion but large enough
   // to fire the handler at a well-defined point.
   constexpr int64_t budget = 5;
   constexpr uint32_t depth = 50;

   int64_t interp = gas_count_to_trap<psizam::interpreter>(code, budget, depth);
   INFO("interpreter trapped with counter=" << interp);
   REQUIRE(interp < 0);
#if defined(__x86_64__) || defined(__aarch64__)
   int64_t j1 = gas_count_to_trap<psizam::jit>(code, budget, depth);
   int64_t j2 = gas_count_to_trap<psizam::jit2>(code, budget, depth);
   INFO("jit trapped with counter=" << j1);
   INFO("jit2 trapped with counter=" << j2);
   CHECK(j1 == interp);
   CHECK(j2 == interp);
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
   int64_t jl = gas_count_to_trap<psizam::jit_llvm>(code, budget, depth);
   INFO("jit_llvm trapped with counter=" << jl);
   CHECK(jl == interp);
#endif
}

BACKEND_TEST_CASE("gas: external interrupt via atomic store(-1)", "[gas][interrupt]") {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, TestType>;
   rhf_t::add<&gas_host_call>("env", "host.call");

   psizam::wasm_code code(implementation_limits_wasm,
                          implementation_limits_wasm + implementation_limits_wasm_len);
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   bkend.get_context().set_gas_strategy(psizam::gas_insertion_strategy::prepay_max);
   bkend.get_context().set_gas_atomic(true);           // cross-thread visibility
   bkend.get_context().set_gas_budget(psizam::gas_budget_unlimited);
   bkend.get_context().set_gas_handler(nullptr);

   // Fire the interrupt immediately; the next function-entry check
   // observes the -1 counter and throws. No need to race with a live
   // thread — the counter store happens-before the call.
   bkend.get_context().gas_counter().store(-1, std::memory_order_relaxed);
   CHECK_THROWS_AS(bkend.call("env", "call", uint32_t{10}),
                   psizam::wasm_gas_exhausted_exception);
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

// Variant of gas_count_to_trap that targets an arbitrary export
// (keeps the yield-style handler semantics so the trap point is
// visible as a counter snapshot).
template <typename Impl>
int64_t gas_count_to_trap_export(psizam::wasm_code& code,
                                 const char* export_name,
                                 int64_t budget,
                                 uint32_t depth) {
   using rhf_t     = psizam::registered_host_functions<psizam::standalone_function_t>;
   using backend_t = psizam::backend<rhf_t, Impl>;
   psizam::wasm_allocator local_wa;
   backend_t bkend(code, &local_wa);
   rhf_t::resolve(bkend.get_module());

   using ctx_t = typename Impl::context;
   static thread_local int64_t seen_counter = 0;
   seen_counter = 0;
   bkend.get_context().set_gas_strategy(psizam::gas_insertion_strategy::prepay_max);
   bkend.get_context().set_gas_handler(+[](void* ctx) {
      auto* c = static_cast<ctx_t*>(ctx);
      if (seen_counter == 0) {
         seen_counter = c->gas_counter().load(std::memory_order_relaxed);
      }
      c->restock_gas(psizam::gas_budget_unlimited);
   });
   bkend.get_context().set_gas_budget(budget);
   (void)bkend.call("env", export_name, depth);
   return seen_counter;
}

// Prologue-extras cross-backend determinism.
//
// call.indirect's body contains one call_indirect outside any loop,
// so the parser folds +4 (call_indirect weight 5 − regular 1) into
// prologue_gas_extra. Every function-entry charge decrements the
// counter by (body_bytes + 4). With a small budget the first such
// charge traps immediately, and every backend sees the same
// counter value — that equality is what this test asserts.
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

   constexpr int64_t  budget = 5;
   constexpr uint32_t depth  = 50;

   int64_t interp = gas_count_to_trap_export<psizam::interpreter>(
      code, "call.indirect", budget, depth);
   INFO("interpreter trapped with counter=" << interp);
   REQUIRE(interp < 0);
#if defined(__x86_64__) || defined(__aarch64__)
   int64_t j1 = gas_count_to_trap_export<psizam::jit>(
      code, "call.indirect", budget, depth);
   int64_t j2 = gas_count_to_trap_export<psizam::jit2>(
      code, "call.indirect", budget, depth);
   INFO("jit trapped with counter=" << j1);
   INFO("jit2 trapped with counter=" << j2);
   CHECK(j1 == interp);
   CHECK(j2 == interp);
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
   int64_t jl = gas_count_to_trap_export<psizam::jit_llvm>(
      code, "call.indirect", budget, depth);
   INFO("jit_llvm trapped with counter=" << jl);
   CHECK(jl == interp);
#endif
}

// Per-backend weighted-charge sanity check.
//
// Running N iterations of gas_heavy_wasm's loop_div charges
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

   constexpr int64_t  budget_start    = psizam::gas_budget_unlimited;
   constexpr uint32_t iters           = 1000;
   constexpr int64_t  per_iter_extra  = psizam::gas_costs::div_rem
                                      - psizam::gas_costs::regular;  // 9
   constexpr int64_t  per_iter_weighted = 1 + per_iter_extra;         // 10

   bkend.get_context().set_gas_strategy(psizam::gas_insertion_strategy::prepay_max);
   bkend.get_context().set_gas_handler(nullptr);  // trap if we overrun
   bkend.get_context().set_gas_budget(budget_start);
   (void)bkend.call("env", "loop_div",
                    int64_t{1'000'000}, int64_t{3}, iters);

   int64_t consumed = budget_start
      - bkend.get_context().gas_counter().load(std::memory_order_relaxed);
   INFO("consumed=" << consumed << " iters=" << iters
        << " per_iter_weighted=" << per_iter_weighted);

   CHECK(consumed >= iters * per_iter_weighted);
   CHECK(consumed <= 3 * iters * per_iter_weighted);
}
