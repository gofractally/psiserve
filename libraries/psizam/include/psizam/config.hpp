#pragma once

namespace psizam {

// Floating-point execution mode. Selected per-instance by the embedder.
//
//  - fast             : hardware FP, no NaN canonicalization. Non-deterministic
//                       across platforms. Used by non-consensus code paths.
//  - hw_deterministic : hardware FP, but with NaN canonicalization and explicit
//                       WASM-spec handling of min/max / conversions / traps /
//                       rounding mode. MUST produce bit-identical results to
//                       softfloat on the same inputs; only the runtime cost
//                       differs. Used by consensus code paths.
//  - softfloat        : Berkeley-softfloat lane-wise. Slow but the reference
//                       oracle (used by differential fuzzing).
//
// Invariant: hw_deterministic(x) == softfloat(x) bit-for-bit. Only fast may
// diverge. Every backend (interpreter, JIT1, JIT2, LLVM) implements all three.
enum class fp_mode : unsigned char {
   fast             = 0,
   hw_deterministic = 1,
   softfloat        = 2,
};

// Legacy bool-pair → fp_mode. Used by code still carrying the old
// `softfloat` / `deterministic` booleans during the rollout.
constexpr fp_mode fp_mode_from_legacy(bool softfloat, bool deterministic) {
   if (softfloat)     return fp_mode::softfloat;
   if (deterministic) return fp_mode::hw_deterministic;
   return fp_mode::fast;
}

// Decompositions in the other direction, for code that hasn't migrated yet.
constexpr bool fp_mode_uses_softfloat(fp_mode m)    { return m == fp_mode::softfloat; }
constexpr bool fp_mode_is_deterministic(fp_mode m)  { return m != fp_mode::fast; }

#ifdef PSIZAM_SOFTFLOAT
   inline constexpr bool use_softfloat = true;
#else
   inline constexpr bool use_softfloat = false;
#endif

// When PSIZAM_NATIVE_FP is set, the JIT uses native FP instructions with
// hardware NaN canonicalization (FPCR.DN=1 on aarch64) instead of softfloat
// calls. Softfloat is still linked for interpreter SIMD float operations.
#ifdef PSIZAM_NATIVE_FP
   inline constexpr bool use_native_fp = true;
#else
   inline constexpr bool use_native_fp = false;
#endif

#ifdef PSIZAM_FULL_DEBUG
   inline constexpr bool psizam_debug = true;
#else
   inline constexpr bool psizam_debug = false;
#endif

#ifdef __x86_64__
   inline constexpr bool psizam_amd64 = true;
#else
   inline constexpr bool psizam_amd64 = false;
#endif

#ifdef __aarch64__
   inline constexpr bool psizam_arm64 = true;
#else
   inline constexpr bool psizam_arm64 = false;
#endif

} // namespace psizam
