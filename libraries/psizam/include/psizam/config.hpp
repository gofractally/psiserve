#pragma once

namespace psizam {

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
