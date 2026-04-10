# psizam Known Issues

14,872/14,994 tests pass (99.2%) across 4 backends: interpreter, jit, jit2, jit_llvm.

## aarch64 JIT: SIMD float operations (~60 failures)

The aarch64 JIT backends (jit, jit2, jit_llvm) do not fully implement SIMD
floating-point operations with softfloat. The interpreter passes all of these.

## Reference types: import table/global, externref (~52 failures)

The parser does not yet support:
- Importing tables (`import "spectest" "table" (table ...)`)
- Importing globals (`import "spectest" "global_i32" (global ...)`)
- `externref` type
- `ref.null` / `ref.func` instructions

These affect `elem_*` and `global_14` spec tests.

## call_depth / call_return_stack_bytes (~10 failures)

Pre-existing issues from upstream. `call_depth` hits SIGBUS on JIT backends.
`call_return_stack_bytes` fails on all backends.
