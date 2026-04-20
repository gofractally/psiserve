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
// Capability bits (`meter_cap`) classify which features a compiled
// module supports. The runtime uses `compatible()` / `missing()` to
// validate instance policies at instantiation time, and `shape_for()`
// to reduce the bit set to one of three JIT codegen shapes.
//
// See libraries/psizam/docs/gas-metering-design.md for the full design
// and .issues/psizam-gas-state-redesign.md for the ongoing revision.

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

// ═════════════════════════════════════════════════════════════════════
// Metering capability bits
// ═════════════════════════════════════════════════════════════════════
//
// Orthogonal feature flags that classify a metering configuration.
// Used two ways:
//
//   compile-time — what the injector + codegen built into this module.
//                  Stored on the prepared template.
//   runtime      — what the instance policy asks the handler to use.
//
// Compatibility rule: `runtime_caps` must be a subset of `compile_caps`.
// Reverse is fine (module may have check sites the instance doesn't
// exercise — correctness unaffected, tiny perf cost).
//
// Not every bit combination is meaningful; `is_valid()` rejects the
// nonsensical ones (e.g. `yield` without any budget to yield against).
enum class meter_cap : uint16_t {
   none         = 0,
   gas_budget   = 1 << 0,  // injector emits consumed-counter updates
   wall_budget  = 1 << 1,  // handler checks wall-clock deadline
   yield        = 1 << 2,  // handler may advance deadline (vs trap only)
   interrupt    = 1 << 3,  // back-edge sites refresh atomic deadline
   pool         = 1 << 4,  // handler debits from a shared gas_pool
};

constexpr meter_cap operator|(meter_cap a, meter_cap b) noexcept {
   return meter_cap(uint16_t(a) | uint16_t(b));
}
constexpr meter_cap operator&(meter_cap a, meter_cap b) noexcept {
   return meter_cap(uint16_t(a) & uint16_t(b));
}
constexpr meter_cap operator^(meter_cap a, meter_cap b) noexcept {
   return meter_cap(uint16_t(a) ^ uint16_t(b));
}
constexpr meter_cap operator~(meter_cap a) noexcept {
   return meter_cap(~uint16_t(a));
}
constexpr meter_cap& operator|=(meter_cap& a, meter_cap b) noexcept { return a = a | b; }
constexpr meter_cap& operator&=(meter_cap& a, meter_cap b) noexcept { return a = a & b; }
constexpr meter_cap& operator^=(meter_cap& a, meter_cap b) noexcept { return a = a ^ b; }

// Every bit in `bits` is set in `mask`.
constexpr bool has(meter_cap mask, meter_cap bits) noexcept {
   return (mask & bits) == bits;
}
// Any bit is set.
constexpr bool any(meter_cap m) noexcept {
   return uint16_t(m) != 0;
}

// Rejects internally-inconsistent combinations:
//  * `yield`     — requires a budget it can advance
//  * `interrupt` — requires a budget an external thread can shorten
//  * `pool`      — pool debits in gas, so `gas_budget` is mandatory
constexpr bool is_valid(meter_cap f) noexcept {
   const bool any_budget = any(f & (meter_cap::gas_budget | meter_cap::wall_budget));
   if (has(f, meter_cap::yield)     && !any_budget)                  return false;
   if (has(f, meter_cap::interrupt) && !any_budget)                  return false;
   if (has(f, meter_cap::pool)      && !has(f, meter_cap::gas_budget)) return false;
   return true;
}

// Returns the subset of `want` that `have` does NOT cover. Empty result
// means `want` is fully supported by `have`. Use for instantiation-time
// error messages: "module was compiled without { interrupt, pool }".
constexpr meter_cap missing(meter_cap have, meter_cap want) noexcept {
   return want & ~have;
}
constexpr bool compatible(meter_cap have, meter_cap want) noexcept {
   return missing(have, want) == meter_cap::none;
}

// Returns the subset of compile-time caps that the runtime is not using.
// Useful for debug-build warnings ("module overcompiled — consider a
// cheaper shape") and never a correctness issue.
constexpr meter_cap unused(meter_cap have, meter_cap want) noexcept {
   return have & ~want;
}

// Named presets. Bitmasks are authoritative — these are just sugar.
namespace meter_presets {
   inline constexpr meter_cap off               = meter_cap::none;
   inline constexpr meter_cap trap              = meter_cap::gas_budget;
   inline constexpr meter_cap yield             = meter_cap::gas_budget  | meter_cap::yield;
   inline constexpr meter_cap timeout           = meter_cap::wall_budget | meter_cap::yield;
   inline constexpr meter_cap trap_interrupt    = meter_cap::gas_budget  | meter_cap::interrupt;
   inline constexpr meter_cap yield_interrupt   = meter_cap::gas_budget  | meter_cap::yield | meter_cap::interrupt;
   inline constexpr meter_cap timeout_interrupt = meter_cap::wall_budget | meter_cap::yield | meter_cap::interrupt;
   inline constexpr meter_cap both_poll         = meter_cap::gas_budget  | meter_cap::wall_budget
                                                | meter_cap::yield       | meter_cap::interrupt;
   inline constexpr meter_cap both_poll_pooled  = both_poll              | meter_cap::pool;
}

// ═════════════════════════════════════════════════════════════════════
// Codegen shape — how JIT backends emit the per-site check
// ═════════════════════════════════════════════════════════════════════
//
// The full mode-matrix collapses to three distinct emission shapes.
// Selected at prepare time from `shape_for(caps)`; each JIT backend
// switches on this rather than on the full cap bit set, so adding a
// new mode that reuses an existing shape is a zero-codegen-change.
enum class codegen_shape : uint8_t {
   none,               // no injection
   dtd_only,           // DTD register at every site: `sub rDtd, cost; js slow`
   dtd_with_refresh,   // DTD at forward sites; back-edge re-reads atomic deadline
};

constexpr codegen_shape shape_for(meter_cap f) noexcept {
   if (!any(f & (meter_cap::gas_budget | meter_cap::wall_budget)))
      return codegen_shape::none;
   if (has(f, meter_cap::interrupt))
      return codegen_shape::dtd_with_refresh;
   return codegen_shape::dtd_only;
}

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
