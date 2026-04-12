#pragma once

#include <psizam/detail/interpret_visitor.hpp>
#include <psizam/detail/opcodes.hpp>

#include <iostream>

#define DBG_VISIT(name, code)                                                                                          \
   void operator()(const PSIZAM_OPCODE_T(name)& op) {                                                                  \
      std::cout << "Found " << #name << " at " << get_context().get_pc() << "\n";                                      \
      interpret_visitor<ExecutionCTX>::operator()(op);                                                                 \
      get_context().print_stack();                                                                                     \
   }

#define DBG2_VISIT(name, code)                                                                                         \
   void operator()(const PSIZAM_OPCODE_T(name)& op) { std::cout << "Found " << #name << "\n"; }

namespace psizam::detail {

template <typename ExecutionCTX>
struct debug_visitor : public interpret_visitor<ExecutionCTX> {
   using interpret_visitor<ExecutionCTX>::operator();
   debug_visitor(ExecutionCTX& ctx) : interpret_visitor<ExecutionCTX>(ctx) {}
   ExecutionCTX& get_context() { return interpret_visitor<ExecutionCTX>::get_context(); }
   PSIZAM_CONTROL_FLOW_OPS(DBG_VISIT)
   PSIZAM_BR_TABLE_OP(DBG_VISIT)
   PSIZAM_RETURN_OP(DBG_VISIT)
   PSIZAM_CALL_OPS(DBG_VISIT)
   PSIZAM_CALL_IMM_OPS(DBG_VISIT)
   PSIZAM_PARAMETRIC_OPS(DBG_VISIT)
   PSIZAM_VARIABLE_ACCESS_OPS(DBG_VISIT)
   PSIZAM_MEMORY_OPS(DBG_VISIT)
   PSIZAM_I32_CONSTANT_OPS(DBG_VISIT)
   PSIZAM_I64_CONSTANT_OPS(DBG_VISIT)
   PSIZAM_F32_CONSTANT_OPS(DBG_VISIT)
   PSIZAM_F64_CONSTANT_OPS(DBG_VISIT)
   PSIZAM_COMPARISON_OPS(DBG_VISIT)
   PSIZAM_NUMERIC_OPS(DBG_VISIT)
   PSIZAM_CONVERSION_OPS(DBG_VISIT)
   PSIZAM_EXIT_OP(DBG_VISIT)
   PSIZAM_ERROR_OPS(DBG_VISIT)
};

struct debug_visitor2 {
   PSIZAM_CONTROL_FLOW_OPS(DBG2_VISIT)
   PSIZAM_BR_TABLE_OP(DBG2_VISIT)
   PSIZAM_RETURN_OP(DBG2_VISIT)
   PSIZAM_CALL_OPS(DBG2_VISIT)
   PSIZAM_CALL_IMM_OPS(DBG2_VISIT)
   PSIZAM_PARAMETRIC_OPS(DBG2_VISIT)
   PSIZAM_VARIABLE_ACCESS_OPS(DBG2_VISIT)
   PSIZAM_MEMORY_OPS(DBG2_VISIT)
   PSIZAM_I32_CONSTANT_OPS(DBG2_VISIT)
   PSIZAM_I64_CONSTANT_OPS(DBG2_VISIT)
   PSIZAM_F32_CONSTANT_OPS(DBG2_VISIT)
   PSIZAM_F64_CONSTANT_OPS(DBG2_VISIT)
   PSIZAM_COMPARISON_OPS(DBG2_VISIT)
   PSIZAM_NUMERIC_OPS(DBG2_VISIT)
   PSIZAM_CONVERSION_OPS(DBG2_VISIT)
   PSIZAM_EXIT_OP(DBG2_VISIT)
   PSIZAM_ERROR_OPS(DBG2_VISIT)
};
#undef DBG_VISIT
#undef DBG2_VISIT

#undef DBG_VISIT
#undef DBG2_VISIT

} // namespace psizam::detail
