// Gas metering — BACKEND_TEST_CASE suite covering trap, user-handler,
// external-interrupt (atomic mode), unlimited, and cross-backend
// determinism. Exercises the paths that the bench never touches.
//
// The WASM module is implementation_limits_wasm's recursive `call`
// function: takes an i32 depth, recurses depth+1 times before
// returning. Each call pays gas_charge once (function entry), so
// depth=N → N+1 charges.

#include "implementation_limits.hpp"
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
