# Gas Metering + Interrupt + Yield Design

Status: design (2026-04-18). Not yet implemented.

## Overview

A unified mechanism that provides gas metering, timeout interrupts, and
cooperative fiber yielding through a single `sub + branch` at two
insertion points: function entry and loop back-edges. The "out of gas"
handler is a policy callback — the same JIT code supports trap, yield,
timeout check, or unlimited execution depending on the handler installed
at instantiation time.

## Insertion Points

### Function entry

Prepay the function's worst-case path cost (max instruction count across
all branches). Overpays if a cheaper branch is taken — conservative and
safe. No gas check at any forward branch within the function.

```asm
function_entry:
   sub  r_gas, r_gas, #total_function_cost
   b.mi gas_exhausted
   ; ... function body (fully optimizable, no gas checks) ...
```

### Loop back-edges

Pay the loop body cost per iteration. This is a delta — the initial
traversal of the loop body was already paid at function entry.

```asm
loop_header:
   sub  r_gas, r_gas, #loop_body_cost
   b.mi gas_exhausted
   ; ... loop body ...
   b    loop_header
```

### What is NOT instrumented

- Forward branches (if/else) — paid at function entry
- Straight-line code — paid at function entry
- Function returns — no check needed

## Cost Calculation

Gas costs are computed AFTER optimization. The JIT optimizes freely
(unroll, merge, vectorize, eliminate dead code), then counts native
instructions per block as a final post-pass.

### For function entry

`total_function_cost` = max over all paths through the function,
summing the native instruction count of each basic block on the path.
Exclude loop body instructions (those are paid per-iteration).

### For back-edges

`loop_body_cost` = native instruction count of the loop body's basic
blocks for one iteration (after optimization, after unrolling).

### Patching

The gas immediate is patched after code generation:

1. Emit `sub r_gas, r_gas, #0` (placeholder) at each insertion point
2. Generate and optimize the function body
3. Count native instructions per block
4. Compute function cost and loop body costs
5. Patch the immediates

For JIT backends that support immediate patching (jit1, jit2): patch
in place. For LLVM: use a final pass after instruction selection.

## Gas Handler (Policy Callback)

```cpp
using gas_handler_t = void(*)(execution_context* ctx);
```

Installed per-instance at instantiation time. Called when `r_gas` goes
negative. The handler can:

### Trap (default)
```cpp
void handler_trap(execution_context* ctx) {
   throw wasm_resource_exception("gas exhausted");
}
```

### Yield (cooperative multitasking)
```cpp
void handler_yield(execution_context* ctx) {
   ctx->restock_gas(budget_per_slice);
   ctx->fiber.yield();  // suspend fiber, resume later
}
```

Cooperative scheduling falls out for free. Set `budget_per_slice` to
~10K instructions. The WASM runs a slice, yields, another fiber runs.
No async transforms, no special yield points.

### Timeout check
```cpp
void handler_check_timeout(execution_context* ctx) {
   if (wall_clock_elapsed() > ctx->timeout)
      throw wasm_resource_exception("timeout");
   ctx->restock_gas(budget_per_slice);
}
```

Wall-clock timeout without signals or mprotect. Checked at gas
boundaries (back-edges + function calls). Worst-case latency to
detect timeout = one slice (~10K instructions ≈ microseconds).

### Unlimited
```cpp
void handler_unlimited(execution_context* ctx) {
   ctx->restock_gas(INT64_MAX);
}
```

Effectively disables metering. For trusted/replay mode.

### External interrupt
```cpp
// From another thread or the timeout watchdog:
instance.gas_counter.store(-1);
// The next back-edge or function call traps into the handler.
```

No signal, no mprotect, no VMA modification. Just a store to a
shared atomic. The handler decides what to do (trap, yield, etc.).

## CPU Cost Analysis

### JIT (jit1, jit2, jit_llvm)

The `sub r_gas, #cost` has no data dependency on the function's real
work — `r_gas` is a dedicated register nothing else reads. The CPU
executes it in parallel via out-of-order execution. The `b.mi` is
predicted not-taken (correct 99.999% of the time). The pipeline never
stalls.

**Effective cost: 0 cycles on the hot path.**

The only time you pay is when gas actually runs out — a branch
misprediction (~20 cycles) plus the handler call. This happens once
per slice (yield) or once per execution (trap/timeout).

### Interpreter

The interpreter already pays ~5-10ns per opcode for dispatch (indirect
branch, memory loads). Adding `gas -= cost; if (gas < 0) handler();`
at back-edge and call opcodes adds ~0.5ns — roughly 5-10% overhead
on something already 10-50× slower than JIT. Negligible in practice.

The interpreter checks are in the opcode handlers for `br`, `br_if`,
`br_table` (when the target is a back-edge), `call`, `call_indirect`,
and `return_call`.

## Interaction with Optimization

**Zero interference.** Both insertion points are optimization barriers:

- Function entry: the optimizer works within the function body freely.
  No gas instructions inside the body to constrain reordering, block
  merging, dead code elimination, or loop unrolling.

- Back-edges: the optimizer works within the loop body freely. An
  unrolled loop has fewer back-edges (one per N original iterations),
  so unrolled code pays LESS gas — correctly reflecting that it does
  the same work in fewer iterations.

The gas `sub` immediate is patched AFTER optimization, so the cost
reflects the actual optimized native instruction count.

## Implementation Plan

### execution_context changes

```cpp
// In execution_context (or jit_execution_context):
int64_t          _gas_counter = INT64_MAX;  // default: unlimited
gas_handler_t    _gas_handler = nullptr;    // null = trap
```

For JIT: `r_gas` is a dedicated callee-saved register (e.g., x20 on
aarch64, r13 on x86_64). Spilled/restored at host call boundaries.

### JIT changes (per backend)

#### jit1 (aarch64 + x86_64)
- At function prologue: emit `sub x20, x20, #0` + `b.mi gas_stub`
- At back-edge (br/br_if/br_table to earlier offset): same
- gas_stub: save state, call `gas_handler(ctx)`, restore, return
- Post-emit: patch the `#0` immediates with computed costs

#### jit2 (x86_64, two-pass IR)
- IR pass: insert gas_check IR node at function entry + back-edges
- Code gen: emit `sub r13, cost` + `js gas_stub`
- Cost computed from final native instruction count

#### jit_llvm
- Add a FunctionPass that inserts gas instrumentation after all
  optimization passes, during the machine-code emission phase
- Or: insert IR-level calls to a `__gas_check(cost)` intrinsic at
  back-edges and function entry, let LLVM optimize around them

### Interpreter changes

In `interpret_visitor`:
- `op_br` / `op_br_if` / `op_br_table`: if target < current pc
  (back-edge), decrement gas counter and check
- `op_call` / `op_call_indirect` / `op_return_call`: decrement gas
  counter and check
- Gas handler called when counter goes negative

### instance_policy integration

```cpp
struct instance_policy {
   // ... existing fields ...
   int64_t        gas_budget  = INT64_MAX;  // initial gas
   int64_t        gas_slice   = 10000;      // restock amount for yield
   gas_handler_t  gas_handler = nullptr;    // null = default trap
};
```

### Dedicated register allocation

| Architecture | Gas register | Notes |
|---|---|---|
| aarch64 | x20 | callee-saved, not used by WASM |
| x86_64 | r13 | callee-saved, not used by WASM |

The register must be callee-saved so it survives host function calls.
It must not conflict with registers used by the WASM operand stack or
locals. Both x20 and r13 are available in the current JIT allocators.

## Testing

- Unit test: verify gas counter decrements correctly at function entry
  and back-edges across all backends
- Test yield handler: fiber-based test that yields every N instructions
  and verifies progress
- Test timeout handler: wall-clock timeout detection
- Test interrupt: external thread sets gas to -1, verify trap
- Benchmark: measure overhead of gas metering vs unmetered execution
  across interpreter, jit1, jit2, jit_llvm
- Determinism: verify same WASM with same gas budget produces same
  result across backends (gas exhaustion at same logical point)
