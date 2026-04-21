#pragma once

// IR definition for the two-pass optimizing JIT (jit2).
// Each WASM function is lowered to a flat array of ir_inst with virtual registers.
//
// Transient data (IR, regalloc, codegen scratch) uses jit_scratch_allocator.
// Only native code output goes through growable_allocator.
// No malloc/free anywhere in the hot path.

#include <psizam/allocator.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/types.hpp>
#include <psizam/detail/vector.hpp>

#include <cstdint>

namespace psizam::detail {

   // Bump allocator for JIT scratch data (IR, regalloc, codegen aux).
   // Wraps a growable_allocator, using a saved/restored watermark so that
   // scratch allocations are reclaimed in bulk without affecting permanent data.
   // No extra mmap — reuses the existing allocator's warm pages.
   class jit_scratch_allocator {
    public:
      explicit jit_scratch_allocator(growable_allocator& alloc)
         : _alloc(alloc), _watermark(alloc._offset) {}

      ~jit_scratch_allocator() {
         if (_armed) _alloc._offset = _watermark;
      }

      // Prevent destructor from restoring the watermark.
      // Call before _allocator.reset() to avoid undoing the reset.
      void disarm() { _armed = false; }

      jit_scratch_allocator(const jit_scratch_allocator&) = delete;
      jit_scratch_allocator& operator=(const jit_scratch_allocator&) = delete;

      template <typename T>
      T* alloc(std::size_t count) {
         return _alloc.alloc<T>(count);
      }

      void reset() { _alloc._offset = _watermark; }

    private:
      growable_allocator& _alloc;
      std::size_t _watermark;
      bool _armed = true;
   };

   // IR opcodes. These mirror WASM operations but in register-transfer form.
   enum class ir_op : uint16_t {
      // Meta
      nop,
      unreachable,
      // Constants
      const_i32,
      const_i64,
      const_f32,
      const_f64,
      const_v128,
      // Local/global variable access
      local_get,
      local_set,
      local_tee,
      global_get,
      global_set,
      // Memory loads (dest = load(src1 + offset))
      i32_load, i64_load, f32_load, f64_load,
      i32_load8_s, i32_load8_u, i32_load16_s, i32_load16_u,
      i64_load8_s, i64_load8_u, i64_load16_s, i64_load16_u,
      i64_load32_s, i64_load32_u,
      // Memory stores (void: store src2 to [src1 + offset])
      i32_store, i64_store, f32_store, f64_store,
      i32_store8, i32_store16,
      i64_store8, i64_store16, i64_store32,
      // Memory management
      memory_size, memory_grow,
      // Parametric
      drop,
      select,
      // Comparison (result is i32 0 or 1)
      i32_eqz, i32_eq, i32_ne, i32_lt_s, i32_lt_u, i32_gt_s, i32_gt_u,
      i32_le_s, i32_le_u, i32_ge_s, i32_ge_u,
      i64_eqz, i64_eq, i64_ne, i64_lt_s, i64_lt_u, i64_gt_s, i64_gt_u,
      i64_le_s, i64_le_u, i64_ge_s, i64_ge_u,
      f32_eq, f32_ne, f32_lt, f32_gt, f32_le, f32_ge,
      f64_eq, f64_ne, f64_lt, f64_gt, f64_le, f64_ge,
      // Unary integer
      i32_clz, i32_ctz, i32_popcnt,
      i64_clz, i64_ctz, i64_popcnt,
      // Binary integer
      i32_add, i32_sub, i32_mul, i32_div_s, i32_div_u, i32_rem_s, i32_rem_u,
      i32_and, i32_or, i32_xor, i32_shl, i32_shr_s, i32_shr_u, i32_rotl, i32_rotr,
      i64_add, i64_sub, i64_mul, i64_div_s, i64_div_u, i64_rem_s, i64_rem_u,
      i64_and, i64_or, i64_xor, i64_shl, i64_shr_s, i64_shr_u, i64_rotl, i64_rotr,
      // Unary float
      f32_abs, f32_neg, f32_ceil, f32_floor, f32_trunc, f32_nearest, f32_sqrt,
      f64_abs, f64_neg, f64_ceil, f64_floor, f64_trunc, f64_nearest, f64_sqrt,
      // Binary float
      f32_add, f32_sub, f32_mul, f32_div, f32_min, f32_max, f32_copysign,
      f64_add, f64_sub, f64_mul, f64_div, f64_min, f64_max, f64_copysign,
      // Conversions
      i32_wrap_i64,
      i32_trunc_s_f32, i32_trunc_u_f32, i32_trunc_s_f64, i32_trunc_u_f64,
      i64_extend_s_i32, i64_extend_u_i32,
      i64_trunc_s_f32, i64_trunc_u_f32, i64_trunc_s_f64, i64_trunc_u_f64,
      f32_convert_s_i32, f32_convert_u_i32, f32_convert_s_i64, f32_convert_u_i64,
      f32_demote_f64,
      f64_convert_s_i32, f64_convert_u_i32, f64_convert_s_i64, f64_convert_u_i64,
      f64_promote_f32,
      i32_reinterpret_f32, i64_reinterpret_f64, f32_reinterpret_i32, f64_reinterpret_i64,
      // Saturating truncations
      i32_trunc_sat_f32_s, i32_trunc_sat_f32_u, i32_trunc_sat_f64_s, i32_trunc_sat_f64_u,
      i64_trunc_sat_f32_s, i64_trunc_sat_f32_u, i64_trunc_sat_f64_s, i64_trunc_sat_f64_u,
      // Sign extensions
      i32_extend8_s, i32_extend16_s,
      i64_extend8_s, i64_extend16_s, i64_extend32_s,
      // Control flow
      block,
      loop,
      block_start,  // pseudo: dest = block_idx, marks block entry point for codegen
      block_end,    // pseudo: dest = block_idx, flags bit 0 = is_if
      br,
      br_if,
      br_table,
      if_,
      else_,
      end,
      return_,
      // Calls
      call,
      call_indirect,
      tail_call,
      tail_call_indirect,
      // Move (used at control flow merge points)
      mov,
      // Call argument (pseudo-instruction)
      arg,
      // SIMD (generic with sub-opcode in imm field)
      v128_op,
      // Bulk memory
      memory_init,
      data_drop,
      memory_copy,
      memory_fill,
      table_init,
      elem_drop,
      table_copy,
      table_get,
      table_set,
      table_grow,
      table_size,
      table_fill,
      // Multi-value return: store value to _multi_return buffer
      // ri.src1 = value vreg, ri.imm = byte offset in buffer
      multi_return_store,
      // Multi-value call return: load value from _multi_return buffer after a call
      // dest = loaded vreg, ri.imm = byte offset in buffer
      multi_return_load,
      // Atomic operations (0xFE prefix)
      // simd.sub field carries atomic_sub, simd.offset carries memarg offset, simd.addr carries addr vreg
      atomic_op,
      // Exception handling
      // eh_enter: call __psizam_eh_enter, returns jmpbuf ptr.
      //   dest = jmpbuf vreg (i64/ptr), ri.imm = eh_data_index, ri.src1 = catch_count
      eh_enter,
      // eh_setjmp: call __psizam_setjmp on jmpbuf. Returns 0 (normal) or non-zero (longjmp).
      //   dest = result vreg (i32), rr.src1 = jmpbuf vreg
      eh_setjmp,
      // eh_leave: call __psizam_eh_leave (normal try_table exit)
      eh_leave,
      // eh_throw: throw WASM exception. Payload via preceding arg ops.
      //   ri.imm = tag_index. Does not return.
      eh_throw,
      // eh_throw_ref: re-throw via exnref. rr.src1 = exnref vreg. Does not return.
      eh_throw_ref,
      // eh_get_match: get matched catch index after longjmp. dest = i32 result.
      eh_get_match,
      // eh_get_payload: get payload value at index. dest = i64 result, ri.imm = index.
      eh_get_payload,
      // eh_get_exnref: get exnref index. dest = i64 result.
      eh_get_exnref,
   };

   static constexpr uint32_t ir_vreg_none = UINT32_MAX;

   // SIMD sub-opcodes carried in v128_op's ri.imm field.
   // These identify the specific SIMD operation for the codegen.
   enum class simd_sub : int32_t {
      // Constants
      v128_const = 0,
      // Memory operations
      v128_load, v128_store,
      v128_load8x8_s, v128_load8x8_u,
      v128_load16x4_s, v128_load16x4_u,
      v128_load32x2_s, v128_load32x2_u,
      v128_load8_splat, v128_load16_splat, v128_load32_splat, v128_load64_splat,
      v128_load32_zero, v128_load64_zero,
      v128_load8_lane, v128_load16_lane, v128_load32_lane, v128_load64_lane,
      v128_store8_lane, v128_store16_lane, v128_store32_lane, v128_store64_lane,
      // Shuffle/Swizzle
      i8x16_shuffle, i8x16_swizzle,
      // Extract/Replace lane
      i8x16_extract_lane_s, i8x16_extract_lane_u, i8x16_replace_lane,
      i16x8_extract_lane_s, i16x8_extract_lane_u, i16x8_replace_lane,
      i32x4_extract_lane, i32x4_replace_lane,
      i64x2_extract_lane, i64x2_replace_lane,
      f32x4_extract_lane, f32x4_replace_lane,
      f64x2_extract_lane, f64x2_replace_lane,
      // Splat
      i8x16_splat, i16x8_splat, i32x4_splat, i64x2_splat, f32x4_splat, f64x2_splat,
      // Comparisons
      i8x16_eq, i8x16_ne, i8x16_lt_s, i8x16_lt_u, i8x16_gt_s, i8x16_gt_u,
      i8x16_le_s, i8x16_le_u, i8x16_ge_s, i8x16_ge_u,
      i16x8_eq, i16x8_ne, i16x8_lt_s, i16x8_lt_u, i16x8_gt_s, i16x8_gt_u,
      i16x8_le_s, i16x8_le_u, i16x8_ge_s, i16x8_ge_u,
      i32x4_eq, i32x4_ne, i32x4_lt_s, i32x4_lt_u, i32x4_gt_s, i32x4_gt_u,
      i32x4_le_s, i32x4_le_u, i32x4_ge_s, i32x4_ge_u,
      i64x2_eq, i64x2_ne, i64x2_lt_s, i64x2_gt_s, i64x2_le_s, i64x2_ge_s,
      f32x4_eq, f32x4_ne, f32x4_lt, f32x4_gt, f32x4_le, f32x4_ge,
      f64x2_eq, f64x2_ne, f64x2_lt, f64x2_gt, f64x2_le, f64x2_ge,
      // Logical
      v128_not, v128_and, v128_andnot, v128_or, v128_xor,
      v128_bitselect, v128_any_true,
      // i8x16 arithmetic
      i8x16_abs, i8x16_neg, i8x16_popcnt,
      i8x16_all_true, i8x16_bitmask,
      i8x16_narrow_i16x8_s, i8x16_narrow_i16x8_u,
      i8x16_shl, i8x16_shr_s, i8x16_shr_u,
      i8x16_add, i8x16_add_sat_s, i8x16_add_sat_u,
      i8x16_sub, i8x16_sub_sat_s, i8x16_sub_sat_u,
      i8x16_min_s, i8x16_min_u, i8x16_max_s, i8x16_max_u, i8x16_avgr_u,
      // i16x8 arithmetic
      i16x8_extadd_pairwise_i8x16_s, i16x8_extadd_pairwise_i8x16_u,
      i16x8_abs, i16x8_neg, i16x8_q15mulr_sat_s,
      i16x8_all_true, i16x8_bitmask,
      i16x8_narrow_i32x4_s, i16x8_narrow_i32x4_u,
      i16x8_extend_low_i8x16_s, i16x8_extend_high_i8x16_s,
      i16x8_extend_low_i8x16_u, i16x8_extend_high_i8x16_u,
      i16x8_shl, i16x8_shr_s, i16x8_shr_u,
      i16x8_add, i16x8_add_sat_s, i16x8_add_sat_u,
      i16x8_sub, i16x8_sub_sat_s, i16x8_sub_sat_u,
      i16x8_mul,
      i16x8_min_s, i16x8_min_u, i16x8_max_s, i16x8_max_u, i16x8_avgr_u,
      i16x8_extmul_low_i8x16_s, i16x8_extmul_high_i8x16_s,
      i16x8_extmul_low_i8x16_u, i16x8_extmul_high_i8x16_u,
      // i32x4 arithmetic
      i32x4_extadd_pairwise_i16x8_s, i32x4_extadd_pairwise_i16x8_u,
      i32x4_abs, i32x4_neg,
      i32x4_all_true, i32x4_bitmask,
      i32x4_extend_low_i16x8_s, i32x4_extend_high_i16x8_s,
      i32x4_extend_low_i16x8_u, i32x4_extend_high_i16x8_u,
      i32x4_shl, i32x4_shr_s, i32x4_shr_u,
      i32x4_add, i32x4_sub, i32x4_mul,
      i32x4_min_s, i32x4_min_u, i32x4_max_s, i32x4_max_u,
      i32x4_dot_i16x8_s,
      i32x4_extmul_low_i16x8_s, i32x4_extmul_high_i16x8_s,
      i32x4_extmul_low_i16x8_u, i32x4_extmul_high_i16x8_u,
      // i64x2 arithmetic
      i64x2_abs, i64x2_neg,
      i64x2_all_true, i64x2_bitmask,
      i64x2_extend_low_i32x4_s, i64x2_extend_high_i32x4_s,
      i64x2_extend_low_i32x4_u, i64x2_extend_high_i32x4_u,
      i64x2_shl, i64x2_shr_s, i64x2_shr_u,
      i64x2_add, i64x2_sub, i64x2_mul,
      i64x2_extmul_low_i32x4_s, i64x2_extmul_high_i32x4_s,
      i64x2_extmul_low_i32x4_u, i64x2_extmul_high_i32x4_u,
      // f32x4
      f32x4_ceil, f32x4_floor, f32x4_trunc, f32x4_nearest,
      f32x4_abs, f32x4_neg, f32x4_sqrt,
      f32x4_add, f32x4_sub, f32x4_mul, f32x4_div,
      f32x4_min, f32x4_max, f32x4_pmin, f32x4_pmax,
      // f64x2
      f64x2_ceil, f64x2_floor, f64x2_trunc, f64x2_nearest,
      f64x2_abs, f64x2_neg, f64x2_sqrt,
      f64x2_add, f64x2_sub, f64x2_mul, f64x2_div,
      f64x2_min, f64x2_max, f64x2_pmin, f64x2_pmax,
      // Conversions
      i32x4_trunc_sat_f32x4_s, i32x4_trunc_sat_f32x4_u,
      f32x4_convert_i32x4_s, f32x4_convert_i32x4_u,
      i32x4_trunc_sat_f64x2_s_zero, i32x4_trunc_sat_f64x2_u_zero,
      f64x2_convert_low_i32x4_s, f64x2_convert_low_i32x4_u,
      f32x4_demote_f64x2_zero, f64x2_promote_low_f32x4,
      // Relaxed SIMD
      f32x4_relaxed_madd, f32x4_relaxed_nmadd,
      f64x2_relaxed_madd, f64x2_relaxed_nmadd,
      i16x8_relaxed_dot_i8x16_i7x16_s,
      i32x4_relaxed_dot_i8x16_i7x16_add_s,
   };

   // Returns true if a v128_op's simd.offset field holds a vreg number
   // (shift amount or scalar value), false if it holds a literal memory offset.
   inline bool simd_offset_is_vreg(simd_sub sub) {
      switch (sub) {
      // Shift ops store shift-amount vreg in offset
      case simd_sub::i8x16_shl: case simd_sub::i8x16_shr_s: case simd_sub::i8x16_shr_u:
      case simd_sub::i16x8_shl: case simd_sub::i16x8_shr_s: case simd_sub::i16x8_shr_u:
      case simd_sub::i32x4_shl: case simd_sub::i32x4_shr_s: case simd_sub::i32x4_shr_u:
      case simd_sub::i64x2_shl: case simd_sub::i64x2_shr_s: case simd_sub::i64x2_shr_u:
      // Replace-lane ops store scalar vreg in offset
      case simd_sub::i8x16_replace_lane: case simd_sub::i16x8_replace_lane:
      case simd_sub::i32x4_replace_lane: case simd_sub::i64x2_replace_lane:
      case simd_sub::f32x4_replace_lane: case simd_sub::f64x2_replace_lane:
         return true;
      default:
         return false;
      }
   }

   // Returns true if a v128_op's codegen emits a BLR/CALL to a softfloat helper
   // (the aarch64 and x86_64 backends both route float SIMD through softfloat).
   // Regalloc uses this to mark the op as a call point so caller-saved registers
   // holding live scalar vregs get spilled across the helper call.
   inline bool simd_sub_calls_helper(simd_sub sub) {
      switch (sub) {
      // f32x4 / f64x2 comparisons
      case simd_sub::f32x4_eq: case simd_sub::f32x4_ne:
      case simd_sub::f32x4_lt: case simd_sub::f32x4_gt:
      case simd_sub::f32x4_le: case simd_sub::f32x4_ge:
      case simd_sub::f64x2_eq: case simd_sub::f64x2_ne:
      case simd_sub::f64x2_lt: case simd_sub::f64x2_gt:
      case simd_sub::f64x2_le: case simd_sub::f64x2_ge:
      // f32x4 arithmetic
      case simd_sub::f32x4_ceil: case simd_sub::f32x4_floor:
      case simd_sub::f32x4_trunc: case simd_sub::f32x4_nearest:
      case simd_sub::f32x4_abs: case simd_sub::f32x4_neg: case simd_sub::f32x4_sqrt:
      case simd_sub::f32x4_add: case simd_sub::f32x4_sub:
      case simd_sub::f32x4_mul: case simd_sub::f32x4_div:
      case simd_sub::f32x4_min: case simd_sub::f32x4_max:
      case simd_sub::f32x4_pmin: case simd_sub::f32x4_pmax:
      // f64x2 arithmetic
      case simd_sub::f64x2_ceil: case simd_sub::f64x2_floor:
      case simd_sub::f64x2_trunc: case simd_sub::f64x2_nearest:
      case simd_sub::f64x2_abs: case simd_sub::f64x2_neg: case simd_sub::f64x2_sqrt:
      case simd_sub::f64x2_add: case simd_sub::f64x2_sub:
      case simd_sub::f64x2_mul: case simd_sub::f64x2_div:
      case simd_sub::f64x2_min: case simd_sub::f64x2_max:
      case simd_sub::f64x2_pmin: case simd_sub::f64x2_pmax:
      // Float/int SIMD conversions
      case simd_sub::i32x4_trunc_sat_f32x4_s: case simd_sub::i32x4_trunc_sat_f32x4_u:
      case simd_sub::f32x4_convert_i32x4_s:   case simd_sub::f32x4_convert_i32x4_u:
      case simd_sub::i32x4_trunc_sat_f64x2_s_zero:
      case simd_sub::i32x4_trunc_sat_f64x2_u_zero:
      case simd_sub::f64x2_convert_low_i32x4_s: case simd_sub::f64x2_convert_low_i32x4_u:
      case simd_sub::f32x4_demote_f64x2_zero:   case simd_sub::f64x2_promote_low_f32x4:
      // Relaxed SIMD
      case simd_sub::f32x4_relaxed_madd: case simd_sub::f32x4_relaxed_nmadd:
      case simd_sub::f64x2_relaxed_madd: case simd_sub::f64x2_relaxed_nmadd:
      case simd_sub::i16x8_relaxed_dot_i8x16_i7x16_s:
      case simd_sub::i32x4_relaxed_dot_i8x16_i7x16_add_s:
         return true;
      default:
         return false;
      }
   }

   // Returns true if a v128_op produces a scalar (i32/i64) result rather than v128.
   // For these ops, the result vreg is stored in simd.addr.
   inline bool simd_produces_scalar(simd_sub sub) {
      switch (sub) {
      case simd_sub::v128_any_true:
      case simd_sub::i8x16_all_true: case simd_sub::i8x16_bitmask:
      case simd_sub::i16x8_all_true: case simd_sub::i16x8_bitmask:
      case simd_sub::i32x4_all_true: case simd_sub::i32x4_bitmask:
      case simd_sub::i64x2_all_true: case simd_sub::i64x2_bitmask:
      case simd_sub::i8x16_extract_lane_s: case simd_sub::i8x16_extract_lane_u:
      case simd_sub::i16x8_extract_lane_s: case simd_sub::i16x8_extract_lane_u:
      case simd_sub::i32x4_extract_lane: case simd_sub::i64x2_extract_lane:
      case simd_sub::f32x4_extract_lane: case simd_sub::f64x2_extract_lane:
         return true;
      default:
         return false;
      }
   }

   enum ir_flags : uint8_t {
      IR_NONE             = 0,
      IR_SIDE_EFFECT      = 1 << 0,
      IR_COMMUTATIVE      = 1 << 1,
      IR_DEAD             = 1 << 2,
      IR_FUSE_NEXT        = 1 << 3,  // Comparison: skip setcc, fuse with next if_/br_if
      IR_BRANCH_LIKELY    = 1 << 4,  // Branch hint: branch is likely taken
      IR_BRANCH_UNLIKELY  = 1 << 5,  // Branch hint: branch is unlikely taken
   };

   // IR instruction. POD type — no constructor/destructor.
   struct ir_inst {
      ir_op    opcode;
      uint8_t  type;
      uint8_t  flags;
      uint32_t dest;
      union {
         struct { uint32_t src1; uint32_t src2; } rr;
         struct { uint32_t src1; int32_t  imm; }  ri;
         int64_t  imm64;
         double   immf64;
         float    immf32;
         struct { uint32_t target; uint32_t src1; } br;
         struct { uint32_t index; uint32_t src1; }  call;
         struct { uint32_t index; uint32_t src1; }  local;
         struct { uint32_t val1; uint32_t val2; uint32_t cond; } sel;
         v128_t   immv128;
         // For v128_op: sub-opcode goes in dest field (cast from simd_sub).
         // offset: memarg offset or scalar vreg (shifts/replace_lane)
         // addr: address vreg (loads/stores) or scalar result vreg (test/extract)
         // v_src1/v_src2: v128 operand vregs (for XMM register lookup)
         // v_dest: v128 result vreg (for XMM register store)
         struct { uint32_t offset; uint32_t addr; uint8_t lane; uint8_t _pad2;
                  uint16_t v_src1; uint16_t v_src2; uint16_t v_dest; } simd;
      };
   };
   static_assert(std::is_trivially_copyable_v<ir_inst>);
   static_assert(std::is_trivially_destructible_v<ir_inst>);

   struct ir_basic_block {
      uint32_t start;
      uint32_t end;
      uint32_t successors[2];
      uint8_t  num_successors;
      uint8_t  is_loop;
      uint8_t  is_if;
      uint8_t  branch_to_entry; // When set, br/br_if targeting this block go to entry, not exit
      uint32_t loop_depth;
      // Phase 4 gas-metering: sum of heavy-opcode extras the parser
      // accumulated for opcodes inside this loop. Added to the
      // per-iteration gas_charge(1) emitted at the loop header during
      // codegen. Only meaningful when is_loop == 1.
      int64_t  loop_gas_extra;
   };
   static_assert(std::is_trivially_copyable_v<ir_basic_block>);
   static_assert(std::is_trivially_destructible_v<ir_basic_block>);

   // Live interval for register allocation. POD.
   struct ir_live_interval {
      uint32_t vreg;
      uint32_t start;
      uint32_t end;
      int8_t   phys_reg;
      int8_t   phys_xmm;
      int16_t  spill_slot;
      uint8_t  type;
      // True if the live range crosses an ir_op::eh_setjmp. Standard
      // setjmp/longjmp preserves callee-saved GPRs, but a callee-saved reg
      // that's reused for different vregs on either side of a setjmp would
      // have its setjmp-time value after longjmp — not the value the
      // post-setjmp / catch-handler code expects. Force-spill these vregs
      // to memory so every use reads the current definition, not a stale
      // register. XMM regs are caller-saved on SysV (all XMM) so they are
      // never safe across setjmp regardless.
      uint8_t  crosses_setjmp = 0;
   };
   static_assert(std::is_trivially_copyable_v<ir_live_interval>);

   // Exception handling catch clause metadata for IR try_table.
   // Stored in ir_function::eh_data side table, indexed by eh_enter's ri.imm.
   struct ir_eh_catch_info {
      uint8_t  kind;        // catch_kind (0=catch_tag, 1=catch_tag_ref, 2=catch_all, 3=catch_all_ref)
      uint32_t tag_index;   // tag to match (for catch_tag/catch_tag_ref)
   };
   struct ir_eh_data {
      uint32_t catch_count;
      ir_eh_catch_info catches[16]; // max 16 catch clauses per try_table
   };

   // Control stack entry for IR construction. POD.
   struct ir_control_entry {
      uint32_t block_idx;
      uint32_t stack_depth;
      uint8_t  result_type;
      uint8_t  is_loop;
      uint8_t  is_function;
      uint8_t  entered_unreachable; // true if control entered in unreachable code
      uint32_t merge_block;
      uint32_t merge_vreg;  // For if/else with result: vreg that both branches write to
      uint8_t  result_count;       // Number of results (0 or 1 for single-value, >1 for multi-value)
      uint8_t  param_count;        // Number of block parameters (multi-value blocks)
      uint8_t  is_try_table;      // true for try_table blocks (emit eh_leave at end)
      uint8_t  result_types[16];   // Types of each result (multi-value)
      uint32_t merge_vregs[16];    // One merge vreg per result (multi-value)
      uint32_t param_vregs[16];    // Saved parameter vregs (for else branch re-push)
   };
   static_assert(std::is_trivially_copyable_v<ir_control_entry>);

   // Per-function IR representation.
   // All buffers are backed by growable_allocator — no malloc/free.
   // Capacity is bounded by source_bytes at construction.
   struct ir_function {
      // Raw pointer + count arrays (allocated from growable_allocator)
      ir_inst*        insts       = nullptr;
      uint32_t        inst_count  = 0;
      uint32_t        inst_cap    = 0;

      ir_basic_block* blocks      = nullptr;
      uint32_t        block_count = 0;
      uint32_t        block_cap   = 0;

      // Virtual operand stack (bounded by source_bytes / min_instruction_size)
      uint32_t*       vstack      = nullptr;
      uint32_t        vstack_top  = 0;
      uint32_t        vstack_cap  = 0;

      // Control flow stack (bounded by max nesting depth)
      ir_control_entry* ctrl_stack     = nullptr;
      uint32_t          ctrl_stack_top = 0;
      uint32_t          ctrl_stack_cap = 0;

      // Live intervals (allocated lazily during regalloc, count = next_vreg)
      ir_live_interval* intervals      = nullptr;
      uint32_t          interval_count = 0;

      // SSA use chains (allocated by optimizer, count = next_vreg).
      // use_count[v] = number of instructions that read vreg v.
      // def_inst[v]  = instruction index that defines vreg v (UINT32_MAX if none).
      uint16_t*       use_count   = nullptr;
      uint32_t*       def_inst    = nullptr;

      // Compile-time constant info (allocated by optimizer Phase 1, count = next_vreg).
      // is_const[v] = 1 if vreg v is a known compile-time constant.
      // const_val[v] = the constant value (only valid when is_const[v] == 1).
      uint8_t*        is_const    = nullptr;
      int64_t*        const_val   = nullptr;

      uint32_t        next_vreg   = 0;
      uint32_t        num_params  = 0;
      uint32_t        num_locals  = 0;
      uint32_t        func_index  = 0;
      const func_type* type       = nullptr;
      uint32_t        num_spill_slots = 0;
      uint32_t        callee_saved_used = 0;  // bitmask of callee-saved regs assigned by regalloc
      bool            has_simd = false;       // true if function contains v128_op instructions

      // Phase 4 gas-metering: sum of heavy-opcode extras the parser
      // accumulated for opcodes outside any loop (the "prepay"). Added
      // to the per-function body-bytes cost emitted in the prologue
      // gas_charge during codegen.
      int64_t         prologue_gas_extra = 0;

      // Exception handling data (indexed by eh_enter's ri.imm)
      ir_eh_data*     eh_data       = nullptr;
      uint32_t        eh_data_count = 0;
      uint32_t        eh_data_cap   = 0;

      // Initialize with bounded capacity from scratch allocator.
      // source_bytes = size of this function's WASM bytecode.
      void init(jit_scratch_allocator& alloc, std::size_t source_bytes) {
         // Each WASM byte produces at most 3 IR instructions (e.g. store = addr+store+arg).
         // Add a minimum to handle tiny functions.
         inst_cap = static_cast<uint32_t>(source_bytes * 3 + 16);
         insts = alloc.alloc<ir_inst>(inst_cap);
         inst_count = 0;

         // Each control flow instruction creates at most 2 blocks.
         // source_bytes is an upper bound on instruction count.
         // Multiply by 2 since if/else creates 2 blocks, plus minimum for tiny functions.
         block_cap = static_cast<uint32_t>(source_bytes * 2 + 16);
         blocks = alloc.alloc<ir_basic_block>(block_cap);
         block_count = 0;

         // Virtual stack is bounded by source_bytes (each push needs at least 1 byte of WASM).
         // v128 values use 2 slots each, so multiply by 2 for safety.
         vstack_cap = static_cast<uint32_t>(source_bytes * 2 + 16);
         vstack = alloc.alloc<uint32_t>(vstack_cap);
         vstack_top = 0;

         // Control stack is bounded by nesting depth. source_bytes is a safe upper bound.
         // In practice, WASM validators enforce max nesting depth.
         ctrl_stack_cap = static_cast<uint32_t>(source_bytes + 4);
         ctrl_stack = alloc.alloc<ir_control_entry>(ctrl_stack_cap);
         ctrl_stack_top = 0;

         // EH data: bounded by number of try_table instructions (at most source_bytes)
         eh_data_cap = static_cast<uint32_t>(source_bytes / 2 + 4);
         eh_data = alloc.alloc<ir_eh_data>(eh_data_cap);
         eh_data_count = 0;

         next_vreg = 0;
         intervals = nullptr;
         interval_count = 0;
         num_spill_slots = 0;
         prologue_gas_extra = 0;
      }

      // No per-function release needed — scratch allocator frees all in bulk.

      uint32_t alloc_vreg(uint8_t /*ty*/) {
         return next_vreg++;
      }

      void emit(ir_inst inst) {
         PSIZAM_ASSERT(inst_count < inst_cap, wasm_parse_exception, "IR instruction buffer overflow");
         insts[inst_count++] = inst;
      }

      uint32_t current_inst_index() const {
         return inst_count;
      }

      uint32_t new_block() {
         PSIZAM_ASSERT(block_count < block_cap, wasm_parse_exception, "IR block buffer overflow");
         uint32_t idx = block_count++;
         auto& b = blocks[idx];
         b.start = UINT32_MAX;  // UINT32_MAX means "not set"
         b.end = UINT32_MAX;
         b.successors[0] = b.successors[1] = UINT32_MAX;
         b.num_successors = 0;
         b.is_loop = 0;
         b.is_if = 0;
         b.branch_to_entry = 0;
         b.loop_depth = 0;
         b.loop_gas_extra = 0;
         return idx;
      }

      void start_block(uint32_t block_idx, uint32_t v128_result_bytes = 0) {
         ir_inst inst{};
         inst.opcode = ir_op::block_start;
         inst.type = 0;
         inst.flags = IR_SIDE_EFFECT; // prevent DCE (dest = block_idx, not a vreg)
         inst.dest = block_idx;
         inst.rr.src1 = v128_result_bytes;
         inst.rr.src2 = ir_vreg_none;
         emit(inst);
      }

      void end_block(uint32_t block_idx, uint32_t v128_result_bytes = 0) {
         ir_inst inst{};
         inst.opcode = ir_op::block_end;
         inst.type = 0;
         inst.flags = (block_idx < block_count && blocks[block_idx].is_if) ? 1 : 0;
         inst.dest = block_idx;
         inst.rr.src1 = v128_result_bytes;
         inst.rr.src2 = ir_vreg_none;
         emit(inst);
      }

      // Virtual operand stack operations (bounded, no malloc)
      void vpush(uint32_t vreg) {
         PSIZAM_ASSERT(vstack_top < vstack_cap, wasm_parse_exception, "IR virtual stack overflow");
         vstack[vstack_top++] = vreg;
      }

      uint32_t vpop() {
         PSIZAM_ASSERT(vstack_top > 0, wasm_parse_exception, "IR virtual stack underflow");
         return vstack[--vstack_top];
      }

      void vstack_resize(uint32_t depth) {
         // Allow upward resize: in unreachable code, stack may have been
         // popped below the block entry depth.  Callers push results afterward.
         PSIZAM_ASSERT(depth <= vstack_cap, wasm_parse_exception, "IR virtual stack resize overflow");
         vstack_top = depth;
      }

      uint32_t vstack_depth() const { return vstack_top; }
      uint32_t vstack_back() const {
         PSIZAM_ASSERT(vstack_top > 0, wasm_parse_exception, "IR virtual stack empty");
         return vstack[vstack_top - 1];
      }

      // Control flow stack operations (bounded, no malloc)
      void ctrl_push(ir_control_entry entry) {
         PSIZAM_ASSERT(ctrl_stack_top < ctrl_stack_cap, wasm_parse_exception, "IR control stack overflow");
         ctrl_stack[ctrl_stack_top++] = entry;
      }

      ir_control_entry ctrl_pop() {
         PSIZAM_ASSERT(ctrl_stack_top > 0, wasm_parse_exception, "IR control stack underflow");
         return ctrl_stack[--ctrl_stack_top];
      }

      ir_control_entry& ctrl_back() {
         PSIZAM_ASSERT(ctrl_stack_top > 0, wasm_parse_exception, "IR control stack empty");
         return ctrl_stack[ctrl_stack_top - 1];
      }

      ir_control_entry& ctrl_at(uint32_t depth) {
         PSIZAM_ASSERT(depth < ctrl_stack_top, wasm_parse_exception, "IR control stack depth out of range");
         return ctrl_stack[ctrl_stack_top - 1 - depth];
      }

      // Add catch clause metadata for a try_table, returns index into eh_data.
      uint32_t add_eh_data(uint32_t catch_count, const catch_clause* clauses) {
         PSIZAM_ASSERT(eh_data_count < eh_data_cap, wasm_parse_exception, "EH data overflow");
         uint32_t idx = eh_data_count++;
         auto& d = eh_data[idx];
         d.catch_count = catch_count < 16 ? catch_count : 16;
         for (uint32_t i = 0; i < d.catch_count; i++) {
            d.catches[i].kind = clauses[i].kind;
            d.catches[i].tag_index = clauses[i].tag_index;
         }
         return idx;
      }
   };

} // namespace psizam::detail
