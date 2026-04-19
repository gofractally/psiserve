#pragma once

// Shared gas-injection state machine.
//
// Every backend's parse-to-emit pipeline faces the same bookkeeping
// problem: walk the function body once, accumulate the extra weight
// of heavy opcodes into per-scope buckets (function-entry prepay vs
// per-loop iteration), then annotate the appropriate gas-charge
// emission point with the final number once the scope closes.
//
// This state machine owns that bookkeeping so every backend solves
// it the same way. Backends only supply the byte-level primitives —
// reserve a patchable gas-charge slot, and patch the immediate when
// the final cost is known.
//
// See libraries/psizam/docs/gas-metering-design.md.

#include <cstdint>
#include <vector>

#include <psizam/gas.hpp>
#include <psizam/detail/opcodes.hpp>

namespace psizam::detail {

   // Extra weight above the regular=1 baseline for a single opcode.
   // Zero for the flat-weight majority. Kept as a small constexpr
   // switch rather than a table so the compiler folds it at call
   // sites and the branch predictor pins the common zero case.
   //
   // Extended-prefix opcodes (memory.copy/fill, table.copy/init,
   // memory.grow, table.grow) are operand-scaled and will emit their
   // own dynamic charges at the opcode site; they are not folded
   // into the pre-computed per-scope accumulator.
   constexpr int64_t gas_heavy_extra_for(uint8_t op) {
      switch (op) {
         case opcodes::i64_div_s:
         case opcodes::i64_div_u:
         case opcodes::i64_rem_s:
         case opcodes::i64_rem_u:
         case opcodes::f32_div:
         case opcodes::f64_div:
            return gas_costs::div_rem - gas_costs::regular;
         case opcodes::call_indirect:
            return gas_costs::call_indirect - gas_costs::regular;
         default:
            return 0;
      }
   }

   // Cost accumulator shared by every backend's parser pipeline.
   //
   // Lifecycle per function body:
   //    reset()          — on enter
   //    on_opcode(b)     — per opcode, before backend dispatch
   //    on_loop_enter()  — on LOOP opcode
   //    on_loop_exit()   — on matching END, returns the loop's extra
   //
   // Outside any loop, heavy-op extras flow into prepay_extra (added
   // to the function's entry charge). Inside a loop, they flow into
   // the top of loop_stack and come back out when the loop closes.
   //
   // Always active. The runtime insertion_strategy lives on the
   // execution context and gates the charge at execution time, not
   // here — so the same compiled module can be run with metering on
   // or off without re-parsing.
   struct gas_injection_state {
      std::vector<int64_t> loop_stack;
      int64_t              prepay_extra = 0;

      void reset() {
         prepay_extra = 0;
         loop_stack.clear();
      }

      void on_opcode(uint8_t op) {
         const int64_t extra = gas_heavy_extra_for(op);
         if (extra == 0) return;
         if (!loop_stack.empty()) loop_stack.back() += extra;
         else                     prepay_extra     += extra;
      }

      void on_loop_enter() {
         loop_stack.push_back(0);
      }

      // Returns the extra weight accumulated inside the loop that
      // just closed. Caller patches the loop's gas-charge emission
      // with (1 + returned_extra) so each iteration pays it.
      int64_t on_loop_exit() {
         if (loop_stack.empty()) return 0;
         const int64_t extra = loop_stack.back();
         loop_stack.pop_back();
         return extra;
      }
   };

}  // namespace psizam::detail
