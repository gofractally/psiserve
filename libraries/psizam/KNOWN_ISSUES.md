# psizam Known Issues

## Test Summary: 87 failures / 16649 tests (99.5% pass rate)

### Pre-existing failures (12)

## Multi-module spec tests (12 failures: elem_59, elem_60, elem_68)

Three spec tests were generated from multi-module `.wast` files but the test
generator produced standalone single-module tests, which cannot work:

- **elem_59, elem_60**: Import a shared table from "module1" but the test calls
  exported functions (`call-7`, `call-8`, `call-9`) that only exist in module1.
  The generated `.wasm` has no export section.
- **elem_68**: Imports a funcref table from "module4" containing function `f`.
  Without module4, `call_indirect` hits uninitialized table entries (stack overflow).

Fix: requires multi-module linking support or regenerating tests with
multi-module test infrastructure.

### New failures from official spec tests (75)

## Multi-value return (8 failures: call_0, return_call_0, return_call_indirect_0)

The official spec tests for `call`, `return_call`, and `return_call_indirect`
include multi-value return tests (functions returning `(i32, f32)` etc.). Our
engine only returns the first value from `call_with_return`. Also,
`tailprint_i32_f32` and `call_tailprint` import print functions we don't provide.

## Tail call validation (20 failures: return_call_1/2/11, return_call_indirect_25/26)

The parser does not validate type compatibility for `return_call` and
`return_call_indirect` instructions. Malformed modules that should be rejected
at parse time (e.g., `return_call` targeting a function with incompatible
return type) are accepted without error.

## Table operation spec tests (44 failures: table_get/set/grow/size/fill)

The official spec table operation tests expose gaps in our table implementation:
- **table_get_0**: `is_null-funcref(2)` returns 1 (null) instead of 0 — funcref
  table initialization from elem segments not fully working
- **table_set_0**: Similar funcref initialization issues
- **table_grow_0..7**: Various failures in table grow with funcref/externref
  initialization, bounds checking, and size reporting
- **table_size_0**: Incorrect size after grow operations
- **table_fill_0**: Fill operation failures

## aarch64 JIT: SIMD float operations (~60 failures)

The aarch64 JIT backends (jit, jit2, jit_llvm) do not fully implement SIMD
floating-point operations with softfloat. The interpreter passes all of these.
