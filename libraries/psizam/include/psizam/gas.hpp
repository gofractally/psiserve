#pragma once

// Gas metering — public types and the compile-time cost table.
//
// Gas metering is always compiled in. It is inactive by default:
// the counter starts at INT64_MAX, the handler is null, and the
// load-time injection pass only runs when a caller sets an
// insertion_strategy other than `off` in their options. So the
// runtime cost of the feature is effectively zero for anyone who
// doesn't opt in.
//
// Runtime state (counter, handler) lives on execution_context_base.
// The load-time injection pass and the per-backend peepholes are
// additive layers built on top of these primitives.
//
// See libraries/psizam/docs/gas-metering-design.md for the full design.

#include <cstdint>
#include <limits>

namespace psizam {

// Where the load-time injector places gas checks.
enum class gas_insertion_strategy : uint8_t {
   off        = 0,  // no injection; unmetered baseline for perf comparison
   prepay_max = 1,  // function entry (max-path prepay) + loop header
   per_block  = 2,  // every basic-block boundary (Wasmer / Near style)
   hybrid     = 3,  // prepay up to threshold; per_block above it
};

// Invoked when an instance's gas counter transitions to negative.
// The argument is an opaque pointer to the execution context; the
// handler must know the concrete context type to interpret it.
// A null handler is treated as "throw wasm_gas_exhausted_exception".
using gas_handler_t = void(*)(void* ctx);

// Compile-time per-opcode weight table.
//
// Indexed by our internal dispatch numbering in bitcode; parser-side
// callers instead use weight_for_opcode(wasm_op), which maps raw WASM
// opcodes to weights. The shape matches Near's wasm_regular_op_cost:
// flat 1 for regular ops, 0 for structural no-ops, higher values for
// intrinsically expensive ops.
//
// These numbers are part of the engine's ABI — changing them changes
// the gas charged by every previously-compiled module.
struct gas_costs {
   // Default weight for anything not otherwise listed.
   static constexpr int64_t regular = 1;

   // Structural / no-op opcodes: block, loop, end, else, nop, drop.
   static constexpr int64_t structural = 0;

   // Integer div/rem/f-div are noticeably more expensive than add/sub/mul
   // even on native hardware; keep a single number to avoid micro-tuning.
   static constexpr int64_t div_rem = 10;

   // Indirect call dispatch (table lookup + type check).
   static constexpr int64_t call_indirect = 5;

   // memory.grow / table.grow have a fixed component here plus a
   // dynamic operand-scaled component charged at runtime.
   static constexpr int64_t grow_fixed = 100;

   // bulk-memory ops: base cost; the per-byte charge is dynamic.
   static constexpr int64_t bulk_memory_fixed = 10;
};

// Default budgets / slice sizes. Callers override per instance.
inline constexpr int64_t gas_budget_unlimited = std::numeric_limits<int64_t>::max();
inline constexpr int64_t gas_slice_default    = 10'000;

} // namespace psizam
