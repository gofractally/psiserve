// Differential test for psizam's three fp_modes (fast, hw_deterministic,
// softfloat) on the interpreter backend.
//
// Strategy:
//   * Instantiate one interpreter backend per WASM module.
//   * For each (function, args) vector, call the backend three times,
//     switching fp_mode between calls. On the interpreter, set_fp_mode
//     takes effect immediately.
//   * Compare results with NaN-aware equality. The two deterministic
//     modes (hw_deterministic, softfloat) MUST agree exactly (modulo NaN
//     payload bits that are all required to be quiet). `fast` is allowed
//     to diverge but on the same host we expect matches for finite
//     results; for NaN results we only verify the quiet-NaN invariant.
//
// This test targets only the interpreter. JIT backends bake the fp_mode
// at construction time, so the runtime switch does not retroactively
// specialize emitted code.

#include <cmath>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

#include <catch2/catch.hpp>
#include <psizam/backend.hpp>
#include <psizam/config.hpp>

#include "utils.hpp"

using namespace psizam;

namespace {

// ── NaN bit patterns ────────────────────────────────────────────────────────
constexpr uint32_t F32_SIGN      = 0x80000000u;
constexpr uint32_t F32_EXP       = 0x7F800000u;
constexpr uint32_t F32_QUIET_BIT = 0x00400000u;
constexpr uint32_t F32_MANT      = 0x007FFFFFu;
constexpr uint32_t F32_CANON_NAN = 0x7FC00000u;

constexpr uint64_t F64_SIGN      = 0x8000000000000000ULL;
constexpr uint64_t F64_EXP       = 0x7FF0000000000000ULL;
constexpr uint64_t F64_QUIET_BIT = 0x0008000000000000ULL;
constexpr uint64_t F64_MANT      = 0x000FFFFFFFFFFFFFULL;
constexpr uint64_t F64_CANON_NAN = 0x7FF8000000000000ULL;

inline bool is_f32_nan(uint32_t bits) {
   return (bits & F32_EXP) == F32_EXP && (bits & F32_MANT) != 0;
}
inline bool is_f64_nan(uint64_t bits) {
   return (bits & F64_EXP) == F64_EXP && (bits & F64_MANT) != 0;
}
inline bool is_f32_quiet_nan(uint32_t bits) {
   return is_f32_nan(bits) && (bits & F32_QUIET_BIT) != 0;
}
inline bool is_f64_quiet_nan(uint64_t bits) {
   return is_f64_nan(bits) && (bits & F64_QUIET_BIT) != 0;
}

inline uint32_t f32_bits(float f) {
   uint32_t u; std::memcpy(&u, &f, sizeof u); return u;
}
inline uint64_t f64_bits(double d) {
   uint64_t u; std::memcpy(&u, &d, sizeof u); return u;
}
inline float    f32_from_bits(uint32_t u) {
   float f; std::memcpy(&f, &u, sizeof f); return f;
}
inline double   f64_from_bits(uint64_t u) {
   double d; std::memcpy(&d, &u, sizeof d); return d;
}

// WASM-spec-compatible NaN-aware equality for f32.
//   - If both are NaN: PASS iff both are quiet NaNs.
//   - Otherwise: strict bit-wise equality.
inline bool f32_wasm_equal(uint32_t a, uint32_t b) {
   bool a_nan = is_f32_nan(a);
   bool b_nan = is_f32_nan(b);
   if (a_nan != b_nan) return false;
   if (a_nan) return is_f32_quiet_nan(a) && is_f32_quiet_nan(b);
   return a == b;
}
inline bool f64_wasm_equal(uint64_t a, uint64_t b) {
   bool a_nan = is_f64_nan(a);
   bool b_nan = is_f64_nan(b);
   if (a_nan != b_nan) return false;
   if (a_nan) return is_f64_quiet_nan(a) && is_f64_quiet_nan(b);
   return a == b;
}

// ── WASM module builder ─────────────────────────────────────────────────────
// Builds a module exporting a fixed set of FP functions (f32 + f64 flavors of
// add/sub/mul/div/min/max plus unary sqrt/ceil/floor).
//
// Type layout:
//   type 0: (f32, f32) -> f32     [binary f32]
//   type 1: (f32)      -> f32     [unary  f32]
//   type 2: (f64, f64) -> f64     [binary f64]
//   type 3: (f64)      -> f64     [unary  f64]

constexpr uint8_t VT_F32 = 0x7d;
constexpr uint8_t VT_F64 = 0x7c;

// f32 opcodes
constexpr uint8_t OP_F32_CEIL  = 0x8d;
constexpr uint8_t OP_F32_FLOOR = 0x8e;
constexpr uint8_t OP_F32_SQRT  = 0x91;
constexpr uint8_t OP_F32_ADD   = 0x92;
constexpr uint8_t OP_F32_SUB   = 0x93;
constexpr uint8_t OP_F32_MUL   = 0x94;
constexpr uint8_t OP_F32_DIV   = 0x95;
constexpr uint8_t OP_F32_MIN   = 0x96;
constexpr uint8_t OP_F32_MAX   = 0x97;
// f64 opcodes
constexpr uint8_t OP_F64_CEIL  = 0x9b;
constexpr uint8_t OP_F64_FLOOR = 0x9c;
constexpr uint8_t OP_F64_SQRT  = 0x9f;
constexpr uint8_t OP_F64_ADD   = 0xa0;
constexpr uint8_t OP_F64_SUB   = 0xa1;
constexpr uint8_t OP_F64_MUL   = 0xa2;
constexpr uint8_t OP_F64_DIV   = 0xa3;
constexpr uint8_t OP_F64_MIN   = 0xa4;
constexpr uint8_t OP_F64_MAX   = 0xa5;

struct FnSpec {
   const char* name;
   uint8_t     type_idx;  // 0..3
   uint8_t     opcode;    // the single FP op run after loading locals
};

inline void push_u8(std::vector<uint8_t>& v, uint8_t b) { v.push_back(b); }

// Encode a uint32 as unsigned LEB128.
inline void push_uleb128(std::vector<uint8_t>& v, uint32_t x) {
   do {
      uint8_t byte = static_cast<uint8_t>(x & 0x7fu);
      x >>= 7;
      if (x) byte |= 0x80;
      v.push_back(byte);
   } while (x);
}

// Encode a section (id, body) with proper LEB128 length.
inline void emit_section(std::vector<uint8_t>& out, uint8_t id, const std::vector<uint8_t>& body) {
   push_u8(out, id);
   push_uleb128(out, static_cast<uint32_t>(body.size()));
   out.insert(out.end(), body.begin(), body.end());
}

std::vector<uint8_t> build_fp_module(const std::vector<FnSpec>& fns) {
   std::vector<uint8_t> mod;
   // Magic + version
   mod.insert(mod.end(), {0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00});

   // Type section: 4 types
   std::vector<uint8_t> types;
   push_u8(types, 0x04); // 4 types
   // type 0: (f32, f32) -> f32
   types.insert(types.end(), {0x60, 0x02, VT_F32, VT_F32, 0x01, VT_F32});
   // type 1: (f32) -> f32
   types.insert(types.end(), {0x60, 0x01, VT_F32,        0x01, VT_F32});
   // type 2: (f64, f64) -> f64
   types.insert(types.end(), {0x60, 0x02, VT_F64, VT_F64, 0x01, VT_F64});
   // type 3: (f64) -> f64
   types.insert(types.end(), {0x60, 0x01, VT_F64,        0x01, VT_F64});
   emit_section(mod, 0x01, types);

   // Function section: N fns, each with its type index.
   std::vector<uint8_t> funcs;
   push_uleb128(funcs, static_cast<uint32_t>(fns.size()));
   for (auto& f : fns) push_u8(funcs, f.type_idx);
   emit_section(mod, 0x03, funcs);

   // Export section: N function exports.
   std::vector<uint8_t> exports;
   push_uleb128(exports, static_cast<uint32_t>(fns.size()));
   for (uint32_t i = 0; i < fns.size(); ++i) {
      auto& f = fns[i];
      size_t nlen = std::strlen(f.name);
      push_uleb128(exports, static_cast<uint32_t>(nlen));
      for (size_t j = 0; j < nlen; ++j) push_u8(exports, static_cast<uint8_t>(f.name[j]));
      push_u8(exports, 0x00); // kind = func
      push_uleb128(exports, i); // func idx
   }
   emit_section(mod, 0x07, exports);

   // Code section: N bodies.
   std::vector<uint8_t> code;
   push_uleb128(code, static_cast<uint32_t>(fns.size()));
   for (auto& f : fns) {
      // body: 0 local decls, local.get 0, [local.get 1 if binary,] op, end
      bool binary = (f.type_idx == 0 || f.type_idx == 2);
      std::vector<uint8_t> body;
      push_u8(body, 0x00);             // 0 local decls
      push_u8(body, 0x20); push_u8(body, 0x00); // local.get 0
      if (binary) {
         push_u8(body, 0x20); push_u8(body, 0x01); // local.get 1
      }
      push_u8(body, f.opcode);
      push_u8(body, 0x0b);             // end
      push_uleb128(code, static_cast<uint32_t>(body.size()));
      code.insert(code.end(), body.begin(), body.end());
   }
   emit_section(mod, 0x0a, code);
   return mod;
}

// ── Module + test vectors ───────────────────────────────────────────────────
const std::vector<FnSpec>& fp_fns() {
   static const std::vector<FnSpec> v = {
      // Binary f32
      {"f32_add",   0, OP_F32_ADD},
      {"f32_sub",   0, OP_F32_SUB},
      {"f32_mul",   0, OP_F32_MUL},
      {"f32_div",   0, OP_F32_DIV},
      {"f32_min",   0, OP_F32_MIN},
      {"f32_max",   0, OP_F32_MAX},
      // Unary f32
      {"f32_sqrt",  1, OP_F32_SQRT},
      {"f32_ceil",  1, OP_F32_CEIL},
      {"f32_floor", 1, OP_F32_FLOOR},
      // Binary f64
      {"f64_add",   2, OP_F64_ADD},
      {"f64_sub",   2, OP_F64_SUB},
      {"f64_mul",   2, OP_F64_MUL},
      {"f64_div",   2, OP_F64_DIV},
      {"f64_min",   2, OP_F64_MIN},
      {"f64_max",   2, OP_F64_MAX},
      // Unary f64
      {"f64_sqrt",  3, OP_F64_SQRT},
      {"f64_ceil",  3, OP_F64_CEIL},
      {"f64_floor", 3, OP_F64_FLOOR},
   };
   return v;
}

// Evaluate a function under a specific fp_mode on the provided backend.
// Returns the raw bit pattern of the f32/f64 result.
template <typename Backend>
uint32_t run_f32_unary(Backend& bk, const char* fn, uint32_t a_bits, fp_mode m) {
   bk.set_fp_mode(m);
   auto r = bk.call_with_return("env", fn, f32_from_bits(a_bits));
   return f32_bits(r->to_f32());
}
template <typename Backend>
uint32_t run_f32_binary(Backend& bk, const char* fn, uint32_t a_bits, uint32_t b_bits, fp_mode m) {
   bk.set_fp_mode(m);
   auto r = bk.call_with_return("env", fn, f32_from_bits(a_bits), f32_from_bits(b_bits));
   return f32_bits(r->to_f32());
}
template <typename Backend>
uint64_t run_f64_unary(Backend& bk, const char* fn, uint64_t a_bits, fp_mode m) {
   bk.set_fp_mode(m);
   auto r = bk.call_with_return("env", fn, f64_from_bits(a_bits));
   return f64_bits(r->to_f64());
}
template <typename Backend>
uint64_t run_f64_binary(Backend& bk, const char* fn, uint64_t a_bits, uint64_t b_bits, fp_mode m) {
   bk.set_fp_mode(m);
   auto r = bk.call_with_return("env", fn, f64_from_bits(a_bits), f64_from_bits(b_bits));
   return f64_bits(r->to_f64());
}

} // anonymous namespace

TEST_CASE("fp_mode differential: interpreter three-way value compare", "[fp_mode_diff]") {
   using backend_t = backend<std::nullptr_t, psizam::interpreter>;
   auto wasm_bytes = build_fp_module(fp_fns());
   backend_t bk(wasm_bytes, &wa);

   // ── f32 test vectors ─────────────────────────────────────────────────────
   // Finite (includes subnormals) and NaN inputs. Specific NaN payloads are
   // chosen to exercise: canonical NaN, arithmetic (already-quiet) NaN, and a
   // signalling NaN (becomes quiet in any non-pass-through op).
   constexpr uint32_t F32_1p0   = 0x3F800000u;  //  1.0
   constexpr uint32_t F32_2p0   = 0x40000000u;  //  2.0
   constexpr uint32_t F32_0p5   = 0x3F000000u;  //  0.5
   constexpr uint32_t F32_NEG1  = 0xBF800000u;  // -1.0
   constexpr uint32_t F32_INF   = 0x7F800000u;  // +inf
   constexpr uint32_t F32_NINF  = 0xFF800000u;  // -inf
   constexpr uint32_t F32_SUBN  = 0x00000001u;  // smallest subnormal
   constexpr uint32_t F32_ZERO  = 0x00000000u;
   constexpr uint32_t F32_NZERO = 0x80000000u;
   constexpr uint32_t F32_PI    = 0x40490fdbu;  //  ~3.1415927
   constexpr uint32_t F32_NEG_PI_OVER_2 = 0xBFC90FDBu;
   constexpr uint32_t F32_NAN_CAN = F32_CANON_NAN;                 // 0x7FC00000
   constexpr uint32_t F32_NAN_ARI = F32_CANON_NAN | 0x12345u;      // quiet, custom payload
   constexpr uint32_t F32_NAN_SIG = 0x7F800001u;                   // signalling NaN
   constexpr uint32_t F32_NAN_NEG = 0xFFC54321u;                   // quiet, sign=1

   // f64 counterparts
   constexpr uint64_t F64_1p0   = 0x3FF0000000000000ULL;
   constexpr uint64_t F64_2p0   = 0x4000000000000000ULL;
   constexpr uint64_t F64_0p5   = 0x3FE0000000000000ULL;
   constexpr uint64_t F64_NEG1  = 0xBFF0000000000000ULL;
   constexpr uint64_t F64_INF   = 0x7FF0000000000000ULL;
   constexpr uint64_t F64_NINF  = 0xFFF0000000000000ULL;
   constexpr uint64_t F64_SUBN  = 0x0000000000000001ULL;
   constexpr uint64_t F64_ZERO  = 0x0000000000000000ULL;
   constexpr uint64_t F64_NZERO = 0x8000000000000000ULL;
   constexpr uint64_t F64_PI    = 0x400921FB54442D18ULL;
   constexpr uint64_t F64_NAN_CAN = F64_CANON_NAN;                        // 0x7FF8...
   constexpr uint64_t F64_NAN_ARI = F64_CANON_NAN | 0x123456789ULL;       // quiet payload
   constexpr uint64_t F64_NAN_SIG = 0x7FF0000000000001ULL;                // signalling NaN
   constexpr uint64_t F64_NAN_NEG = 0xFFF8ABCDEF012345ULL;                // quiet, sign=1

   struct f32_bin_vec { const char* fn; uint32_t a, b; };
   std::vector<f32_bin_vec> f32_binary = {
      {"f32_add",   F32_1p0,       F32_2p0},
      {"f32_sub",   F32_2p0,       F32_1p0},
      {"f32_mul",   F32_PI,        F32_0p5},
      {"f32_div",   F32_1p0,       F32_2p0},
      {"f32_div",   F32_1p0,       F32_ZERO},       // +inf
      {"f32_div",   F32_NEG1,      F32_ZERO},       // -inf
      {"f32_div",   F32_ZERO,      F32_ZERO},       // NaN (canonical)
      {"f32_min",   F32_NEG1,      F32_1p0},
      {"f32_max",   F32_NEG1,      F32_1p0},
      {"f32_min",   F32_NZERO,     F32_ZERO},       // -0 < +0 in wasm min
      {"f32_max",   F32_NZERO,     F32_ZERO},
      {"f32_add",   F32_NAN_CAN,   F32_1p0},        // NaN prop
      {"f32_add",   F32_NAN_ARI,   F32_2p0},
      {"f32_mul",   F32_NAN_SIG,   F32_1p0},        // sNaN -> quiet after op
      {"f32_sub",   F32_NAN_NEG,   F32_NAN_ARI},
      {"f32_min",   F32_NAN_ARI,   F32_1p0},        // min with NaN -> NaN
      {"f32_max",   F32_NAN_ARI,   F32_INF},
      {"f32_add",   F32_INF,       F32_NINF},       // inf + -inf -> NaN
      {"f32_sub",   F32_INF,       F32_INF},        // inf - inf  -> NaN
      {"f32_mul",   F32_ZERO,      F32_INF},        // 0 * inf    -> NaN
      {"f32_add",   F32_SUBN,      F32_SUBN},
   };
   struct f32_un_vec { const char* fn; uint32_t a; };
   std::vector<f32_un_vec> f32_unary = {
      {"f32_sqrt",  F32_2p0},
      {"f32_sqrt",  F32_ZERO},
      {"f32_sqrt",  F32_NEG1},      // negative -> NaN
      {"f32_sqrt",  F32_NAN_CAN},
      {"f32_sqrt",  F32_NAN_ARI},
      {"f32_sqrt",  F32_NAN_SIG},
      {"f32_sqrt",  F32_INF},
      {"f32_ceil",  F32_PI},
      {"f32_ceil",  F32_NEG_PI_OVER_2},
      {"f32_ceil",  F32_NAN_ARI},
      {"f32_floor", F32_PI},
      {"f32_floor", F32_NEG_PI_OVER_2},
      {"f32_floor", F32_NAN_SIG},
   };

   struct f64_bin_vec { const char* fn; uint64_t a, b; };
   std::vector<f64_bin_vec> f64_binary = {
      {"f64_add",   F64_1p0,       F64_2p0},
      {"f64_sub",   F64_2p0,       F64_1p0},
      {"f64_mul",   F64_PI,        F64_0p5},
      {"f64_div",   F64_1p0,       F64_2p0},
      {"f64_div",   F64_1p0,       F64_ZERO},
      {"f64_div",   F64_NEG1,      F64_ZERO},
      {"f64_div",   F64_ZERO,      F64_ZERO},
      {"f64_min",   F64_NEG1,      F64_1p0},
      {"f64_max",   F64_NEG1,      F64_1p0},
      {"f64_min",   F64_NZERO,     F64_ZERO},
      {"f64_max",   F64_NZERO,     F64_ZERO},
      {"f64_add",   F64_NAN_CAN,   F64_1p0},
      {"f64_add",   F64_NAN_ARI,   F64_2p0},
      {"f64_mul",   F64_NAN_SIG,   F64_1p0},
      {"f64_sub",   F64_NAN_NEG,   F64_NAN_ARI},
      {"f64_min",   F64_NAN_ARI,   F64_1p0},
      {"f64_max",   F64_NAN_ARI,   F64_INF},
      {"f64_add",   F64_INF,       F64_NINF},
      {"f64_sub",   F64_INF,       F64_INF},
      {"f64_mul",   F64_ZERO,      F64_INF},
      {"f64_add",   F64_SUBN,      F64_SUBN},
   };
   struct f64_un_vec { const char* fn; uint64_t a; };
   std::vector<f64_un_vec> f64_unary = {
      {"f64_sqrt",  F64_2p0},
      {"f64_sqrt",  F64_ZERO},
      {"f64_sqrt",  F64_NEG1},
      {"f64_sqrt",  F64_NAN_CAN},
      {"f64_sqrt",  F64_NAN_ARI},
      {"f64_sqrt",  F64_NAN_SIG},
      {"f64_sqrt",  F64_INF},
      {"f64_ceil",  F64_PI},
      {"f64_ceil",  F64_NAN_ARI},
      {"f64_floor", F64_PI},
      {"f64_floor", F64_NAN_SIG},
   };

   // ── Core checker ─────────────────────────────────────────────────────────
   int  total_vectors      = 0;
   int  mismatch_det       = 0;  // hw_deterministic vs softfloat mismatches
   int  mismatch_fast      = 0;  // fast vs deterministic mismatches (informational)
   int  nan_from_fast_bad  = 0;  // fast produced a non-quiet NaN (illegal even for fast)

   auto check_f32 = [&](const char* fn, uint32_t a, const uint32_t* b,
                         uint32_t r_fast, uint32_t r_hwdet, uint32_t r_soft) {
      ++total_vectors;
      INFO("fn=" << fn
         << " a=0x" << std::hex << a
         << (b ? std::string(" b=0x") + [&]{ std::string s; char buf[32]; std::snprintf(buf, sizeof buf, "%08x", *b); s = buf; return s; }() : std::string())
         << " fast=0x" << r_fast
         << " hwdet=0x" << r_hwdet
         << " soft=0x" << r_soft);
      // Strict determinism between the two WASM-spec modes.
      bool det_eq = f32_wasm_equal(r_hwdet, r_soft);
      if (!det_eq) ++mismatch_det;
      CHECK(det_eq);
      // Fast NaN sanity: if fast returned NaN, the quiet bit MUST be set
      // (this is required by WASM even in non-deterministic mode).
      if (is_f32_nan(r_fast) && !is_f32_quiet_nan(r_fast)) {
         ++nan_from_fast_bad;
         FAIL("fast produced a signalling NaN: 0x" << std::hex << r_fast);
      }
      // Fast vs deterministic: informational only (fast is allowed to diverge).
      if (!f32_wasm_equal(r_fast, r_hwdet)) ++mismatch_fast;
   };

   auto check_f64 = [&](const char* fn, uint64_t a, const uint64_t* b,
                         uint64_t r_fast, uint64_t r_hwdet, uint64_t r_soft) {
      ++total_vectors;
      INFO("fn=" << fn
         << " a=0x" << std::hex << a
         << (b ? std::string(" b=0x") + [&]{ std::string s; char buf[32]; std::snprintf(buf, sizeof buf, "%016llx", (unsigned long long)*b); s = buf; return s; }() : std::string())
         << " fast=0x" << r_fast
         << " hwdet=0x" << r_hwdet
         << " soft=0x" << r_soft);
      bool det_eq = f64_wasm_equal(r_hwdet, r_soft);
      if (!det_eq) ++mismatch_det;
      CHECK(det_eq);
      if (is_f64_nan(r_fast) && !is_f64_quiet_nan(r_fast)) {
         ++nan_from_fast_bad;
         FAIL("fast produced a signalling NaN: 0x" << std::hex << r_fast);
      }
      if (!f64_wasm_equal(r_fast, r_hwdet)) ++mismatch_fast;
   };

   // ── Drive the interpreter for each vector, in each fp_mode ───────────────
   for (auto& v : f32_binary) {
      uint32_t rf = run_f32_binary(bk, v.fn, v.a, v.b, fp_mode::fast);
      uint32_t rh = run_f32_binary(bk, v.fn, v.a, v.b, fp_mode::hw_deterministic);
      uint32_t rs = run_f32_binary(bk, v.fn, v.a, v.b, fp_mode::softfloat);
      check_f32(v.fn, v.a, &v.b, rf, rh, rs);
   }
   for (auto& v : f32_unary) {
      uint32_t rf = run_f32_unary(bk, v.fn, v.a, fp_mode::fast);
      uint32_t rh = run_f32_unary(bk, v.fn, v.a, fp_mode::hw_deterministic);
      uint32_t rs = run_f32_unary(bk, v.fn, v.a, fp_mode::softfloat);
      check_f32(v.fn, v.a, nullptr, rf, rh, rs);
   }
   for (auto& v : f64_binary) {
      uint64_t rf = run_f64_binary(bk, v.fn, v.a, v.b, fp_mode::fast);
      uint64_t rh = run_f64_binary(bk, v.fn, v.a, v.b, fp_mode::hw_deterministic);
      uint64_t rs = run_f64_binary(bk, v.fn, v.a, v.b, fp_mode::softfloat);
      check_f64(v.fn, v.a, &v.b, rf, rh, rs);
   }
   for (auto& v : f64_unary) {
      uint64_t rf = run_f64_unary(bk, v.fn, v.a, fp_mode::fast);
      uint64_t rh = run_f64_unary(bk, v.fn, v.a, fp_mode::hw_deterministic);
      uint64_t rs = run_f64_unary(bk, v.fn, v.a, fp_mode::softfloat);
      check_f64(v.fn, v.a, nullptr, rf, rh, rs);
   }

   // Summary (visible in Catch2 output when run with --success or on failure).
   INFO("total vectors: " << total_vectors
      << "  det mismatches: " << mismatch_det
      << "  fast-vs-det mismatches: " << mismatch_fast
      << "  fast bad NaNs: " << nan_from_fast_bad);
   CHECK(mismatch_det == 0);
   CHECK(nan_from_fast_bad == 0);
   // mismatch_fast is expected to be 0 on the same host for our vectors, but
   // we treat it as a warning rather than a hard failure -- it is not a
   // correctness violation for fast to diverge in principle. Report via INFO
   // above, don't fail the test on it.
}
