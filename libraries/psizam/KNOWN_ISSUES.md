# psizam Known Issues

Tracked from initial import (forked from eos-vm jit2 branch, 2026-04-09).
8162/8193 tests pass (99%).

## aarch64 JIT: SIMD float operations (28 failures)

The aarch64 JIT backend does not fully implement SIMD floating-point operations.
The interpreter passes all of these. Affected tests (all `psizam::jit` only):

- `simd_conversions_0_wasm`
- `simd_conversions_39_wasm`
- `simd_f32x4_0_wasm`
- `simd_f32x4_17_wasm`
- `simd_f32x4_arith_0_wasm`
- `simd_f32x4_arith_1_wasm`
- `simd_f32x4_arith_18_wasm`
- `simd_f32x4_cmp_0_wasm`
- `simd_f32x4_cmp_13_wasm`
- `simd_f32x4_pmin_pmax_0_wasm`
- `simd_f32x4_rounding_0_wasm`
- `simd_f64x2_0_wasm`
- `simd_f64x2_9_wasm`
- `simd_f64x2_arith_0_wasm`
- `simd_f64x2_arith_1_wasm`
- `simd_f64x2_arith_18_wasm`
- `simd_f64x2_cmp_0_wasm`
- `simd_f64x2_cmp_25_wasm`
- `simd_f64x2_pmin_pmax_0_wasm`
- `simd_f64x2_rounding_0_wasm`
- `simd_i32x4_trunc_sat_f32x4_0_wasm`
- `simd_i32x4_trunc_sat_f64x2_0_wasm`
- `simd_load_7_wasm` through `simd_load_11_wasm`
- `simd_splat_19_wasm`

## aarch64 JIT: call_depth bus error (1 failure)

`call_depth` test hits SIGBUS on the JIT backend. Likely a stack overflow
handling issue in the aarch64 JIT.

## call_return_stack_bytes (2 failures)

Fails on both interpreter and JIT. Pre-existing issue from upstream.

## jit2 backend: no aarch64 support

The jit2 (two-pass JIT with IR optimization) backend does not have a working
aarch64 code generator. Tests are currently only run against `interpreter`
and `jit` on ARM. See `tests/utils.hpp` `BACKEND_TEST_CASE` macro.
