# psizam Known Issues

## Multi-module spec tests (12 failures: elem_59, elem_60, elem_68)

Three spec tests were generated from multi-module `.wast` files but the test
generator produced standalone single-module tests, which cannot work:

- **elem_59, elem_60**: Import a shared table from "module1" but the test calls
  exported functions (`call-7`, `call-8`, `call-9`) that only exist in module1.
  The generated `.wasm` has no export section.
- **elem_68**: Imports a funcref global from "module4" that should reference a
  function returning 42. Without module4, the global is uninitialized.

Fix: regenerate these tests with multi-module support or build multi-module
test infrastructure (see task #13).

## aarch64 JIT: SIMD float operations (~60 failures)

The aarch64 JIT backends (jit, jit2, jit_llvm) do not fully implement SIMD
floating-point operations with softfloat. The interpreter passes all of these.

## growable_allocator (4 failures)

The allocator tests expect `wasm_bad_alloc` exceptions for allocations above
certain size limits, but on systems with sufficient address space (64-bit with
overcommit), `mmap` succeeds and no exception is thrown. These are
platform-dependent false expectations, not real bugs.

## max_stack_bytes (2 failures: jit2, jit_llvm)

The `max_stack_bytes` option (byte-level stack accounting) is not fully
implemented for jit2 and jit_llvm backends. These backends don't emit the
per-function stack usage checks that the `stack_limit_is_bytes` mode requires.
Calls that should throw "stack overflow" succeed instead, and vice versa.

## call_return_stack_bytes (4 failures: all backends)

`test_double_deref` returns 0 instead of 42. The test uses `stack_limit_is_bytes`
mode which changes how the operand stack is laid out. The `i32.load` at the
double-deref level appears to get a stale or zero value. Likely a stack
accounting bug in the byte-counting mode, not related to normal execution.
