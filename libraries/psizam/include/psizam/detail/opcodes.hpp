#pragma once
#include <psizam/detail/opcodes_def.hpp>
#include <psizam/detail/variant.hpp>

#include <map>

namespace psizam {
   enum opcodes {
      PSIZAM_CONTROL_FLOW_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_BR_TABLE_OP(PSIZAM_CREATE_ENUM)
      PSIZAM_RETURN_OP(PSIZAM_CREATE_ENUM)
      PSIZAM_CALL_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_CALL_IMM_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_EH_CATCH_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_PARAMETRIC_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_VARIABLE_ACCESS_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_MEMORY_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_I32_CONSTANT_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_I64_CONSTANT_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_F32_CONSTANT_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_F64_CONSTANT_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_COMPARISON_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_NUMERIC_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_CONVERSION_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_EXIT_OP(PSIZAM_CREATE_ENUM)
      PSIZAM_REF_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_EXTENDED_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_EMPTY_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_ERROR_OPS(PSIZAM_CREATE_ENUM)
   };

   enum ext_opcodes {
       PSIZAM_DATA_OPS(PSIZAM_CREATE_ENUM)
       PSIZAM_EXT_OPS(PSIZAM_CREATE_ENUM)
   };

   enum vec_opcodes {
      PSIZAM_VEC_MEMORY_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_VEC_LANE_MEMORY_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_VEC_CONSTANT_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_VEC_SHUFFLE_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_VEC_LANE_OPS(PSIZAM_CREATE_ENUM)
      PSIZAM_VEC_NUMERIC_OPS(PSIZAM_CREATE_ENUM)
   };

   struct opcode_utils {
      std::map<uint16_t, std::string> opcode_map{
         PSIZAM_CONTROL_FLOW_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_BR_TABLE_OP(PSIZAM_CREATE_MAP)
         PSIZAM_RETURN_OP(PSIZAM_CREATE_MAP)
         PSIZAM_CALL_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_CALL_IMM_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_EH_CATCH_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_PARAMETRIC_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_VARIABLE_ACCESS_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_MEMORY_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_I32_CONSTANT_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_I64_CONSTANT_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_F32_CONSTANT_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_F64_CONSTANT_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_COMPARISON_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_NUMERIC_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_CONVERSION_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_EXIT_OP(PSIZAM_CREATE_MAP)
         PSIZAM_REF_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_EXTENDED_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_EMPTY_OPS(PSIZAM_CREATE_MAP)
         PSIZAM_ERROR_OPS(PSIZAM_CREATE_MAP)
      };
   };

   enum imm_types {
      none,
      block_imm,
      varuint32_imm,
      br_table_imm,
   };


   PSIZAM_CONTROL_FLOW_OPS(PSIZAM_CREATE_CONTROL_FLOW_TYPES)
   PSIZAM_BR_TABLE_OP(PSIZAM_CREATE_BR_TABLE_TYPE)
   PSIZAM_RETURN_OP(PSIZAM_CREATE_CONTROL_FLOW_TYPES)
   PSIZAM_CALL_OPS(PSIZAM_CREATE_CALL_TYPES)
   PSIZAM_CALL_IMM_OPS(PSIZAM_CREATE_CALL_IMM_TYPES)
   PSIZAM_EH_CATCH_OPS(PSIZAM_CREATE_CONTROL_FLOW_TYPES)
   PSIZAM_PARAMETRIC_OPS(PSIZAM_CREATE_TYPES)
   PSIZAM_VARIABLE_ACCESS_OPS(PSIZAM_CREATE_VARIABLE_ACCESS_TYPES)
   PSIZAM_MEMORY_OPS(PSIZAM_CREATE_MEMORY_TYPES)
   PSIZAM_I32_CONSTANT_OPS(PSIZAM_CREATE_I32_CONSTANT_TYPE)
   PSIZAM_I64_CONSTANT_OPS(PSIZAM_CREATE_I64_CONSTANT_TYPE)
   PSIZAM_F32_CONSTANT_OPS(PSIZAM_CREATE_F32_CONSTANT_TYPE)
   PSIZAM_F64_CONSTANT_OPS(PSIZAM_CREATE_F64_CONSTANT_TYPE)
   PSIZAM_COMPARISON_OPS(PSIZAM_CREATE_TYPES)
   PSIZAM_NUMERIC_OPS(PSIZAM_CREATE_TYPES)
   PSIZAM_CONVERSION_OPS(PSIZAM_CREATE_TYPES)
   PSIZAM_EXIT_OP(PSIZAM_CREATE_EXIT_TYPE)

   // Reference type opcodes — manually defined since they have different layouts
   struct ref_null_t { uint8_t type; static constexpr uint8_t opcode = 0xD0; };
   struct ref_is_null_t { static constexpr uint8_t opcode = 0xD1; };
   struct ref_func_t { uint32_t index; static constexpr uint8_t opcode = 0xD2; };

   PSIZAM_EMPTY_OPS(PSIZAM_CREATE_TYPES)
   PSIZAM_ERROR_OPS(PSIZAM_CREATE_TYPES)
   PSIZAM_DATA_OPS(PSIZAM_CREATE_DATA_TYPES)
   PSIZAM_EXT_OPS(PSIZAM_CREATE_TYPES)
   PSIZAM_VEC_MEMORY_OPS(PSIZAM_CREATE_VEC_MEMORY_TYPES)
   PSIZAM_VEC_LANE_MEMORY_OPS(PSIZAM_CREATE_VEC_LANE_MEMORY_TYPES)
   PSIZAM_VEC_CONSTANT_OPS(PSIZAM_CREATE_V128_CONSTANT_TYPE)
   PSIZAM_VEC_SHUFFLE_OPS(PSIZAM_CREATE_VEC_SHUFFLE_TYPE)
   PSIZAM_VEC_LANE_OPS(PSIZAM_CREATE_VEC_LANE_TYPES)
   PSIZAM_VEC_NUMERIC_OPS(PSIZAM_CREATE_VEC_TYPES)
   PSIZAM_VEC_RELAXED_OPS(PSIZAM_CREATE_VEC_TYPES)

   // Atomic sub-opcodes (0xFE prefix)
   enum class atomic_sub : uint8_t {
      memory_atomic_notify   = 0x00,
      memory_atomic_wait32   = 0x01,
      memory_atomic_wait64   = 0x02,
      atomic_fence           = 0x03,

      i32_atomic_load        = 0x10,
      i64_atomic_load        = 0x11,
      i32_atomic_load8_u     = 0x12,
      i32_atomic_load16_u    = 0x13,
      i64_atomic_load8_u     = 0x14,
      i64_atomic_load16_u    = 0x15,
      i64_atomic_load32_u    = 0x16,

      i32_atomic_store       = 0x17,
      i64_atomic_store       = 0x18,
      i32_atomic_store8      = 0x19,
      i32_atomic_store16     = 0x1A,
      i64_atomic_store8      = 0x1B,
      i64_atomic_store16     = 0x1C,
      i64_atomic_store32     = 0x1D,

      i32_atomic_rmw_add     = 0x1E,
      i64_atomic_rmw_add     = 0x1F,
      i32_atomic_rmw8_add_u  = 0x20,
      i32_atomic_rmw16_add_u = 0x21,
      i64_atomic_rmw8_add_u  = 0x22,
      i64_atomic_rmw16_add_u = 0x23,
      i64_atomic_rmw32_add_u = 0x24,

      i32_atomic_rmw_sub     = 0x25,
      i64_atomic_rmw_sub     = 0x26,
      i32_atomic_rmw8_sub_u  = 0x27,
      i32_atomic_rmw16_sub_u = 0x28,
      i64_atomic_rmw8_sub_u  = 0x29,
      i64_atomic_rmw16_sub_u = 0x2A,
      i64_atomic_rmw32_sub_u = 0x2B,

      i32_atomic_rmw_and     = 0x2C,
      i64_atomic_rmw_and     = 0x2D,
      i32_atomic_rmw8_and_u  = 0x2E,
      i32_atomic_rmw16_and_u = 0x2F,
      i64_atomic_rmw8_and_u  = 0x30,
      i64_atomic_rmw16_and_u = 0x31,
      i64_atomic_rmw32_and_u = 0x32,

      i32_atomic_rmw_or      = 0x33,
      i64_atomic_rmw_or      = 0x34,
      i32_atomic_rmw8_or_u   = 0x35,
      i32_atomic_rmw16_or_u  = 0x36,
      i64_atomic_rmw8_or_u   = 0x37,
      i64_atomic_rmw16_or_u  = 0x38,
      i64_atomic_rmw32_or_u  = 0x39,

      i32_atomic_rmw_xor     = 0x3A,
      i64_atomic_rmw_xor     = 0x3B,
      i32_atomic_rmw8_xor_u  = 0x3C,
      i32_atomic_rmw16_xor_u = 0x3D,
      i64_atomic_rmw8_xor_u  = 0x3E,
      i64_atomic_rmw16_xor_u = 0x3F,
      i64_atomic_rmw32_xor_u = 0x40,

      i32_atomic_rmw_xchg     = 0x41,
      i64_atomic_rmw_xchg     = 0x42,
      i32_atomic_rmw8_xchg_u  = 0x43,
      i32_atomic_rmw16_xchg_u = 0x44,
      i64_atomic_rmw8_xchg_u  = 0x45,
      i64_atomic_rmw16_xchg_u = 0x46,
      i64_atomic_rmw32_xchg_u = 0x47,

      i32_atomic_rmw_cmpxchg     = 0x48,
      i64_atomic_rmw_cmpxchg     = 0x49,
      i32_atomic_rmw8_cmpxchg_u  = 0x4A,
      i32_atomic_rmw16_cmpxchg_u = 0x4B,
      i64_atomic_rmw8_cmpxchg_u  = 0x4C,
      i64_atomic_rmw16_cmpxchg_u = 0x4D,
      i64_atomic_rmw32_cmpxchg_u = 0x4E,
   };

   struct atomic_op_t {
      atomic_sub sub;
      uint32_t   flags_align;
      uint32_t   offset;
      static constexpr uint8_t opcode_prefix = 0xFE;
   };

   using opcode = variant<
      PSIZAM_CONTROL_FLOW_OPS(PSIZAM_IDENTITY)
      PSIZAM_BR_TABLE_OP(PSIZAM_IDENTITY)
      PSIZAM_RETURN_OP(PSIZAM_IDENTITY)
      PSIZAM_CALL_OPS(PSIZAM_IDENTITY)
      PSIZAM_CALL_IMM_OPS(PSIZAM_IDENTITY)
      PSIZAM_EH_CATCH_OPS(PSIZAM_IDENTITY)
      PSIZAM_PARAMETRIC_OPS(PSIZAM_IDENTITY)
      PSIZAM_VARIABLE_ACCESS_OPS(PSIZAM_IDENTITY)
      PSIZAM_MEMORY_OPS(PSIZAM_IDENTITY)
      PSIZAM_I32_CONSTANT_OPS(PSIZAM_IDENTITY)
      PSIZAM_I64_CONSTANT_OPS(PSIZAM_IDENTITY)
      PSIZAM_F32_CONSTANT_OPS(PSIZAM_IDENTITY)
      PSIZAM_F64_CONSTANT_OPS(PSIZAM_IDENTITY)
      PSIZAM_COMPARISON_OPS(PSIZAM_IDENTITY)
      PSIZAM_NUMERIC_OPS(PSIZAM_IDENTITY)
      PSIZAM_CONVERSION_OPS(PSIZAM_IDENTITY)
      PSIZAM_EXIT_OP(PSIZAM_IDENTITY)
      ref_null_t, ref_is_null_t, ref_func_t,
      PSIZAM_EMPTY_OPS(PSIZAM_IDENTITY)
      PSIZAM_DATA_OPS(PSIZAM_IDENTITY)
      PSIZAM_EXT_OPS(PSIZAM_IDENTITY)
      PSIZAM_VEC_MEMORY_OPS(PSIZAM_IDENTITY)
      PSIZAM_VEC_LANE_MEMORY_OPS(PSIZAM_IDENTITY)
      PSIZAM_VEC_CONSTANT_OPS(PSIZAM_IDENTITY)
      PSIZAM_VEC_SHUFFLE_OPS(PSIZAM_IDENTITY)
      PSIZAM_VEC_LANE_OPS(PSIZAM_IDENTITY)
      PSIZAM_VEC_NUMERIC_OPS(PSIZAM_IDENTITY)
      PSIZAM_VEC_RELAXED_OPS(PSIZAM_IDENTITY)
      psizam::atomic_op_t,
      PSIZAM_ERROR_OPS(PSIZAM_IDENTITY_END)
      >;
} // namespace psizam
