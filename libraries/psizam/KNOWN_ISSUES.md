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

## call_depth / call_return_stack_bytes (~10 failures)

Pre-existing issues from upstream. `call_depth` hits SIGBUS on JIT backends.
`call_return_stack_bytes` fails on all backends.
