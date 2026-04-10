// LLVM IR Translator implementation
//
// Translates psizam IR instructions to LLVM IR using the LLVM C++ API.
// Uses the alloca-based approach: every virtual register gets an alloca
// in the entry block; LLVM's mem2reg pass promotes these to SSA.
//
// Phase 4b: integers, comparisons, conversions, control flow, memory,
//           locals/globals, calls, select.

#include <psizam/llvm_ir_translator.hpp>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace psizam {

   struct llvm_ir_translator::impl {
      const module&             wasm_mod;
      llvm_translate_options    opts;
      std::unique_ptr<llvm::LLVMContext> ctx;
      std::unique_ptr<llvm::Module>      llvm_mod;

      // Type cache
      llvm::Type* void_ty   = nullptr;
      llvm::Type* i1_ty     = nullptr;
      llvm::Type* i8_ty     = nullptr;
      llvm::Type* i16_ty    = nullptr;
      llvm::Type* i32_ty    = nullptr;
      llvm::Type* i64_ty    = nullptr;
      llvm::Type* f32_ty    = nullptr;
      llvm::Type* f64_ty    = nullptr;
      llvm::Type* ptr_ty    = nullptr;

      // WASM function declarations (indexed by function index)
      std::vector<llvm::Function*> wasm_funcs;

      impl(const module& mod, const llvm_translate_options& options)
         : wasm_mod(mod), opts(options)
      {
         ctx = std::make_unique<llvm::LLVMContext>();
         llvm_mod = std::make_unique<llvm::Module>("psizam_wasm", *ctx);

         // Initialize type cache
         void_ty = llvm::Type::getVoidTy(*ctx);
         i1_ty   = llvm::Type::getInt1Ty(*ctx);
         i8_ty   = llvm::Type::getInt8Ty(*ctx);
         i16_ty  = llvm::Type::getInt16Ty(*ctx);
         i32_ty  = llvm::Type::getInt32Ty(*ctx);
         i64_ty  = llvm::Type::getInt64Ty(*ctx);
         f32_ty  = llvm::Type::getFloatTy(*ctx);
         f64_ty  = llvm::Type::getDoubleTy(*ctx);
         ptr_ty  = llvm::PointerType::get(*ctx, 0);

         // Pre-declare all WASM functions
         declare_functions();
         // Declare runtime helper functions
         declare_runtime_helpers();
      }

      // Runtime helper function declarations
      llvm::Function* rt_global_get    = nullptr;
      llvm::Function* rt_global_set    = nullptr;
      llvm::Function* rt_memory_size   = nullptr;
      llvm::Function* rt_memory_grow   = nullptr;
      llvm::Function* rt_call_host     = nullptr;
      llvm::Function* rt_memory_init   = nullptr;
      llvm::Function* rt_data_drop     = nullptr;
      llvm::Function* rt_memory_copy   = nullptr;
      llvm::Function* rt_memory_fill   = nullptr;
      llvm::Function* rt_table_init    = nullptr;
      llvm::Function* rt_elem_drop     = nullptr;
      llvm::Function* rt_table_copy    = nullptr;
      llvm::Function* rt_call_indirect = nullptr;
      llvm::Function* rt_trap           = nullptr;

      void declare_runtime_helpers() {
         auto decl = [&](const char* name, llvm::FunctionType* ty) -> llvm::Function* {
            return llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                           name, llvm_mod.get());
         };

         // int64_t __psizam_global_get(void* ctx, uint32_t idx)
         rt_global_get = decl("__psizam_global_get",
            llvm::FunctionType::get(i64_ty, {ptr_ty, i32_ty}, false));

         // void __psizam_global_set(void* ctx, uint32_t idx, int64_t value)
         rt_global_set = decl("__psizam_global_set",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i64_ty}, false));

         // int32_t __psizam_memory_size(void* ctx)
         rt_memory_size = decl("__psizam_memory_size",
            llvm::FunctionType::get(i32_ty, {ptr_ty}, false));

         // int32_t __psizam_memory_grow(void* ctx, int32_t pages, void** new_mem_out)
         rt_memory_grow = decl("__psizam_memory_grow",
            llvm::FunctionType::get(i32_ty, {ptr_ty, i32_ty, ptr_ty}, false));

         // int64_t __psizam_call_host(void* ctx, uint32_t func_idx, void* args_buf, uint32_t nargs)
         rt_call_host = decl("__psizam_call_host",
            llvm::FunctionType::get(i64_ty, {ptr_ty, i32_ty, ptr_ty, i32_ty}, false));

         // void __psizam_memory_init(void* ctx, uint32_t seg, uint32_t dest, uint32_t src, uint32_t n)
         rt_memory_init = decl("__psizam_memory_init",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty, i32_ty}, false));

         // void __psizam_data_drop(void* ctx, uint32_t seg)
         rt_data_drop = decl("__psizam_data_drop",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty}, false));

         // void __psizam_memory_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n)
         rt_memory_copy = decl("__psizam_memory_copy",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty}, false));

         // void __psizam_memory_fill(void* ctx, uint32_t dest, uint32_t val, uint32_t n)
         rt_memory_fill = decl("__psizam_memory_fill",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty}, false));

         // void __psizam_table_init(void* ctx, uint32_t elem, uint32_t dest, uint32_t src, uint32_t n)
         rt_table_init = decl("__psizam_table_init",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty, i32_ty}, false));

         // void __psizam_elem_drop(void* ctx, uint32_t seg)
         rt_elem_drop = decl("__psizam_elem_drop",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty}, false));

         // void __psizam_table_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n)
         rt_table_copy = decl("__psizam_table_copy",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty}, false));

         // int64_t __psizam_call_indirect(void* ctx, void* mem, uint32_t type_idx,
         //                                uint32_t table_elem, void* args_buf, uint32_t nargs)
         rt_call_indirect = decl("__psizam_call_indirect",
            llvm::FunctionType::get(i64_ty, {ptr_ty, ptr_ty, i32_ty, i32_ty, ptr_ty, i32_ty}, false));

         // void __psizam_trap(void* ctx, uint32_t trap_code)
         rt_trap = decl("__psizam_trap",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty}, false));
         rt_trap->setDoesNotReturn();
      }

      llvm::Type* wasm_type_to_llvm(uint8_t wt) {
         switch (wt) {
            case types::i32:  return i32_ty;
            case types::i64:  return i64_ty;
            case types::f32:  return f32_ty;
            case types::f64:  return f64_ty;
            default:          return i64_ty; // fallback
         }
      }

      void declare_functions() {
         uint32_t num_imports = wasm_mod.get_imported_functions_size();
         uint32_t total_funcs = num_imports + static_cast<uint32_t>(wasm_mod.code.size());
         wasm_funcs.resize(total_funcs, nullptr);

         for (uint32_t i = num_imports; i < total_funcs; i++) {
            const auto& ft = wasm_mod.get_function_type(i);

            // Build function type: (ptr ctx, ptr mem, params...) -> ret
            std::vector<llvm::Type*> params;
            params.push_back(ptr_ty); // ctx
            params.push_back(ptr_ty); // mem

            for (auto pt : ft.param_types) {
               params.push_back(wasm_type_to_llvm(pt));
            }

            llvm::Type* ret_ty = ft.return_count ? wasm_type_to_llvm(ft.return_type) : void_ty;
            auto* fn_ty = llvm::FunctionType::get(ret_ty, params, false);

            std::string name = "wasm_func_" + std::to_string(i);
            auto* fn = llvm::Function::Create(fn_ty, llvm::Function::InternalLinkage,
                                               name, llvm_mod.get());
            wasm_funcs[i] = fn;
         }
      }

      // ──── Translation helpers ────

      // Determine the LLVM type for a vreg based on the IR type byte
      llvm::Type* ir_type_to_llvm(uint8_t ty) {
         switch (ty) {
            case types::i32: return i32_ty;
            case types::i64: return i64_ty;
            case types::f32: return f32_ty;
            case types::f64: return f64_ty;
            default:         return i64_ty;
         }
      }

      // Create LLVM fshr/fshl intrinsic call for rotations
      llvm::Value* emit_rotate(llvm::IRBuilder<>& builder, llvm::Value* val,
                                llvm::Value* amt, llvm::Type* ty, bool right) {
         llvm::Function* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
            llvm_mod.get(),
            right ? llvm::Intrinsic::fshr : llvm::Intrinsic::fshl,
            {ty});
         // fshl(a, a, amt) = rotl(a, amt)
         // fshr(a, a, amt) = rotr(a, amt)
         return builder.CreateCall(intrinsic, {val, val, amt});
      }

      // Create LLVM ctlz/cttz/ctpop intrinsic call
      llvm::Value* emit_count_intrinsic(llvm::IRBuilder<>& builder, llvm::Value* val,
                                         llvm::Type* ty, llvm::Intrinsic::ID id) {
         llvm::Function* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
            llvm_mod.get(), id, {ty});
         if (id == llvm::Intrinsic::ctlz || id == llvm::Intrinsic::cttz) {
            // is_zero_poison = false (WASM spec: clz(0) = bit_width, ctz(0) = bit_width)
            return builder.CreateCall(intrinsic, {val, builder.getFalse()});
         }
         return builder.CreateCall(intrinsic, {val});
      }

      void translate(const ir_function& func) {
         uint32_t func_idx = func.func_index + wasm_mod.get_imported_functions_size();
         auto* fn = wasm_funcs[func_idx];
         if (!fn) return;

         llvm::IRBuilder<> builder(*ctx);

         // ── Entry block: allocas, parameters ──
         auto* entry_bb = llvm::BasicBlock::Create(*ctx, "entry", fn);
         builder.SetInsertPoint(entry_bb);

         // Map function parameters
         auto arg_it = fn->arg_begin();
         llvm::Value* ctx_ptr = &*arg_it++;
         ctx_ptr->setName("ctx");
         llvm::Value* mem_ptr = &*arg_it++;
         mem_ptr->setName("mem");

         // Determine vreg types by scanning instructions.
         // Take the first non-pseudo type assignment for each vreg.
         std::vector<uint8_t> vreg_types(func.next_vreg, 0); // 0 = unset
         for (uint32_t i = 0; i < func.inst_count; i++) {
            const auto& inst = func.insts[i];
            if (inst.dest != ir_vreg_none && inst.dest < func.next_vreg &&
                inst.opcode != ir_op::block_start && inst.opcode != ir_op::block_end &&
                inst.opcode != ir_op::br_table) {
               if (inst.type != types::pseudo && inst.type != types::ret_void &&
                   vreg_types[inst.dest] == 0) {
                  vreg_types[inst.dest] = inst.type;
               }
            }
            // Also infer type from src usage (for mov/select with pseudo type)
            if (inst.opcode == ir_op::mov && inst.dest != ir_vreg_none &&
                inst.dest < func.next_vreg && vreg_types[inst.dest] == 0) {
               if (inst.rr.src1 != ir_vreg_none && inst.rr.src1 < func.next_vreg &&
                   vreg_types[inst.rr.src1] != 0) {
                  vreg_types[inst.dest] = vreg_types[inst.rr.src1];
               }
            }
         }
         // Second pass: propagate through movs for any remaining unset vregs
         for (uint32_t i = 0; i < func.inst_count; i++) {
            const auto& inst = func.insts[i];
            if (inst.opcode == ir_op::mov && inst.dest != ir_vreg_none &&
                inst.dest < func.next_vreg && vreg_types[inst.dest] == 0) {
               if (inst.rr.src1 != ir_vreg_none && inst.rr.src1 < func.next_vreg &&
                   vreg_types[inst.rr.src1] != 0) {
                  vreg_types[inst.dest] = vreg_types[inst.rr.src1];
               }
            }
         }
         // Default any remaining to i64
         for (uint32_t i = 0; i < func.next_vreg; i++) {
            if (vreg_types[i] == 0) vreg_types[i] = types::i64;
         }

         // Create allocas for all virtual registers
         std::vector<llvm::AllocaInst*> vregs(func.next_vreg, nullptr);
         for (uint32_t i = 0; i < func.next_vreg; i++) {
            vregs[i] = builder.CreateAlloca(ir_type_to_llvm(vreg_types[i]), nullptr,
                                             "v" + std::to_string(i));
         }

         // Create allocas for local variables
         uint32_t num_locals = func.num_params + func.num_locals;
         std::vector<llvm::AllocaInst*> locals(num_locals, nullptr);
         // Get function type to determine local types
         const auto& ft = *func.type;
         for (uint32_t i = 0; i < num_locals; i++) {
            llvm::Type* local_ty;
            if (i < func.num_params) {
               local_ty = wasm_type_to_llvm(ft.param_types[i]);
            } else {
               // Local declarations - we need to determine their type.
               // WASM locals after params are zero-initialized.
               // The type is tracked in the module's code section.
               local_ty = i64_ty; // default; will be overridden by usage
            }
            locals[i] = builder.CreateAlloca(local_ty, nullptr, "local" + std::to_string(i));
         }

         // Store function parameters into param locals
         arg_it = fn->arg_begin();
         ++arg_it; ++arg_it; // skip ctx, mem
         for (uint32_t i = 0; i < func.num_params && arg_it != fn->arg_end(); ++i, ++arg_it) {
            builder.CreateStore(&*arg_it, locals[i]);
         }

         // Zero-initialize non-param locals
         for (uint32_t i = func.num_params; i < num_locals; i++) {
            llvm::Type* lt = locals[i]->getAllocatedType();
            builder.CreateStore(llvm::Constant::getNullValue(lt), locals[i]);
         }

         // ── Pre-scan: max host call args (for entry-block alloca) ──
         uint32_t max_host_args = 0;
         {
            uint32_t num_imports = wasm_mod.get_imported_functions_size();
            uint32_t pending_call_args = 0;
            for (uint32_t i = 0; i < func.inst_count; i++) {
               const auto& inst = func.insts[i];
               if (inst.opcode == ir_op::arg) {
                  pending_call_args++;
               } else if (inst.opcode == ir_op::call) {
                  if (inst.call.index < num_imports && pending_call_args > max_host_args)
                     max_host_args = pending_call_args;
                  pending_call_args = 0;
               } else if (inst.opcode == ir_op::call_indirect) {
                  if (pending_call_args > max_host_args)
                     max_host_args = pending_call_args;
                  pending_call_args = 0;
               }
            }
         }
         llvm::AllocaInst* host_args_alloca = nullptr;
         if (max_host_args > 0) {
            host_args_alloca = builder.CreateAlloca(i64_ty,
               builder.getInt32(max_host_args), "host_args");
         }

         // ── Pre-scan: which blocks have block_start instructions? ──
         // Blocks created by emit_block/emit_loop have block_start.
         // Blocks created by emit_if do NOT (only block_end).
         // Blocks created by emit_else for the else body DO.
         std::vector<bool> has_block_start(func.block_count, false);
         for (uint32_t i = 0; i < func.inst_count; i++) {
            if (func.insts[i].opcode == ir_op::block_start &&
                func.insts[i].dest < func.block_count) {
               has_block_start[func.insts[i].dest] = true;
            }
         }

         // ── Create LLVM basic blocks ──
         // Each psizam IR block gets TWO LLVM blocks:
         //   block_entries[N] — placed at block_start N (for loops / else bodies)
         //   block_exits[N]   — placed at block_end N (continuation point)
         //
         // br target=N (non-loop) → block_exits[N]
         // br target=N (loop) → block_entries[N]
         // if_ target=N (has else, N is else block with block_start) → block_entries[N]
         // if_ target=N (no else, N is if's own block) → block_exits[N]
         // else_ target=N → block_exits[N]
         std::vector<llvm::BasicBlock*> block_entries(func.block_count, nullptr);
         std::vector<llvm::BasicBlock*> block_exits(func.block_count, nullptr);
         for (uint32_t i = 0; i < func.block_count; i++) {
            if (has_block_start[i]) {
               block_entries[i] = llvm::BasicBlock::Create(*ctx,
                  "bb" + std::to_string(i) + "_entry", fn);
            }
            block_exits[i] = llvm::BasicBlock::Create(*ctx,
               "bb" + std::to_string(i) + "_exit", fn);
         }

         // Branch from entry to first block entry (or return if empty)
         if (func.block_count > 0 && block_entries[0]) {
            builder.CreateBr(block_entries[0]);
         } else if (func.block_count > 0) {
            // Block 0 has no block_start — shouldn't happen for function body
            builder.CreateBr(block_exits[0]);
         } else {
            if (fn->getReturnType()->isVoidTy())
               builder.CreateRetVoid();
            else
               builder.CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
            return;
         }

         // Helper lambdas for vreg load/store through allocas
         auto load_vreg = [&](uint32_t vreg) -> llvm::Value* {
            if (vreg >= vregs.size() || !vregs[vreg]) return nullptr;
            return builder.CreateLoad(vregs[vreg]->getAllocatedType(), vregs[vreg]);
         };

         // Convert value to target type, handling int/float size mismatches
         auto convert_type = [&](llvm::Value* val, llvm::Type* target) -> llvm::Value* {
            if (val->getType() == target) return val;
            auto* src = val->getType();
            unsigned src_bits = src->getPrimitiveSizeInBits();
            unsigned dst_bits = target->getPrimitiveSizeInBits();
            if (src->isIntegerTy() && target->isIntegerTy()) {
               if (src_bits < dst_bits) return builder.CreateZExt(val, target);
               else return builder.CreateTrunc(val, target);
            }
            if (src_bits == dst_bits) {
               return builder.CreateBitCast(val, target);
            }
            // Size mismatch between int and float: truncate/extend int first
            if (src->isIntegerTy() && target->isFloatingPointTy()) {
               llvm::Type* int_ty = builder.getIntNTy(dst_bits);
               val = (src_bits < dst_bits) ? builder.CreateZExt(val, int_ty)
                                            : builder.CreateTrunc(val, int_ty);
               return builder.CreateBitCast(val, target);
            }
            if (src->isFloatingPointTy() && target->isIntegerTy()) {
               llvm::Type* int_ty = builder.getIntNTy(src_bits);
               val = builder.CreateBitCast(val, int_ty);
               return (src_bits < dst_bits) ? builder.CreateZExt(val, target)
                                             : builder.CreateTrunc(val, target);
            }
            // Float to float (e.g., double to float)
            if (src->isFloatingPointTy() && target->isFloatingPointTy()) {
               if (src_bits > dst_bits) return builder.CreateFPTrunc(val, target);
               else return builder.CreateFPExt(val, target);
            }
            return val;
         };

         // Load a vreg and convert to a specific type
         auto load_vreg_as = [&](uint32_t vreg, llvm::Type* target_ty) -> llvm::Value* {
            llvm::Value* val = load_vreg(vreg);
            if (!val) return nullptr;
            return convert_type(val, target_ty);
         };

         auto store_vreg = [&](uint32_t vreg, llvm::Value* val) {
            if (vreg == ir_vreg_none || vreg >= vregs.size() || !vregs[vreg]) return;
            llvm::Type* alloca_ty = vregs[vreg]->getAllocatedType();
            if (val->getType() != alloca_ty) {
               val = convert_type(val, alloca_ty);
            }
            builder.CreateStore(val, vregs[vreg]);
         };

         // Track which block we're currently emitting into, for fallthrough
         uint32_t current_block = UINT32_MAX;

         // Helper: after emitting a terminator, create a dead block to absorb
         // any subsequent instructions (unreachable dead code in the IR stream).
         auto ensure_insertable = [&]() {
            auto* cur = builder.GetInsertBlock();
            if (cur && cur->getTerminator()) {
               auto* dead = llvm::BasicBlock::Create(*ctx, "dead", fn);
               builder.SetInsertPoint(dead);
            }
         };

         // Collect call arguments from 'arg' pseudo-instructions
         std::vector<llvm::Value*> call_args;

         // br_table state
         bool in_br_table = false;
         uint32_t br_table_size = 0;
         llvm::Value* br_table_index = nullptr;

         // if_ fixup stack: tracks the else/end block for if_ instructions
         struct if_fixup {
            llvm::BasicBlock* false_bb;  // where to go if condition is false
         };
         std::vector<if_fixup> if_fixups;

         // ── Translate instructions ──
         for (uint32_t ip = 0; ip < func.inst_count; ip++) {
            const auto& inst = func.insts[ip];

            // Skip dead instructions
            if (inst.flags & IR_DEAD) continue;

            // If the current block already has a terminator, create a dead block
            // to absorb subsequent dead code from the IR stream.
            // Exception: block_start handles its own insertion point.
            if (inst.opcode != ir_op::block_start) {
               ensure_insertable();
            }

            switch (inst.opcode) {
               case ir_op::nop:
                  break;

               case ir_op::unreachable:
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(0)});
                  builder.CreateUnreachable();
                  break;

               // ──── Block structure ────
               case ir_op::block_start: {
                  uint32_t block_idx = inst.dest;
                  if (block_idx < func.block_count && block_entries[block_idx]) {
                     auto* target = block_entries[block_idx];
                     auto* cur_bb = builder.GetInsertBlock();
                     if (cur_bb != target) {
                        // Add fallthrough if needed
                        if (cur_bb && !cur_bb->getTerminator()) {
                           builder.CreateBr(target);
                        }
                        builder.SetInsertPoint(target);
                     }
                     // If already at target (e.g., else_ just set us here), no-op
                     current_block = block_idx;
                  }
                  break;
               }

               case ir_op::block_end: {
                  // Transition to block's exit/continuation block.
                  // This is where br instructions targeting this block will land.
                  uint32_t block_idx = inst.dest;
                  if (block_idx < func.block_count && block_exits[block_idx]) {
                     auto* target = block_exits[block_idx];
                     auto* cur_bb = builder.GetInsertBlock();
                     if (cur_bb != target) {
                        if (cur_bb && !cur_bb->getTerminator()) {
                           builder.CreateBr(target);
                        }
                        builder.SetInsertPoint(target);
                     }
                  }
                  break;
               }

               // ──── Control flow ────
               case ir_op::if_: {
                  // Condition is in br.src1, target (else/end block) is in br.target
                  llvm::Value* cond = load_vreg(inst.br.src1);
                  if (!cond) break;
                  // WASM if: branch to else/end block if condition is zero
                  llvm::Value* cmp = builder.CreateICmpNE(cond,
                     llvm::Constant::getNullValue(cond->getType()));
                  // Determine the false branch target:
                  // - if target has block_start (else body): jump to block_entries[target]
                  // - if target has no block_start (no else): jump to block_exits[target]
                  llvm::BasicBlock* false_bb = nullptr;
                  uint32_t target = inst.br.target;
                  if (target < func.block_count) {
                     false_bb = has_block_start[target]
                        ? block_entries[target]
                        : block_exits[target];
                  } else {
                     false_bb = llvm::BasicBlock::Create(*ctx, "if_false", fn);
                  }
                  auto* then_bb = llvm::BasicBlock::Create(*ctx, "if_then", fn);
                  builder.CreateCondBr(cmp, then_bb, false_bb);
                  builder.SetInsertPoint(then_bb);
                  if_fixups.push_back({false_bb});
                  break;
               }

               case ir_op::else_: {
                  // else_ target = merge block (the if's block).
                  // Then-branch jumps to block_exits[target] (past the else body).
                  uint32_t merge_target = inst.br.target;
                  llvm::BasicBlock* merge_bb = nullptr;
                  if (merge_target < func.block_count) {
                     merge_bb = block_exits[merge_target];
                  }
                  auto* cur_bb = builder.GetInsertBlock();
                  if (cur_bb && !cur_bb->getTerminator() && merge_bb) {
                     builder.CreateBr(merge_bb);
                  }
                  // Pop the if_fixup and set insert point to the else body
                  // (the next instruction is block_start for the else block)
                  if (!if_fixups.empty()) {
                     auto fixup = if_fixups.back();
                     if_fixups.pop_back();
                     builder.SetInsertPoint(fixup.false_bb);
                  }
                  break;
               }

               case ir_op::br: {
                  if (in_br_table) break; // handled by br_table
                  uint32_t target = inst.br.target;
                  if (target < func.block_count) {
                     // For loops: branch to block_entries (loop header)
                     // For blocks: branch to block_exits (continuation)
                     auto* target_bb = (func.blocks[target].is_loop && block_entries[target])
                        ? block_entries[target]
                        : block_exits[target];
                     auto* cur_bb = builder.GetInsertBlock();
                     if (cur_bb && !cur_bb->getTerminator()) {
                        builder.CreateBr(target_bb);
                     }
                  }
                  break;
               }

               case ir_op::br_if: {
                  uint32_t target = inst.br.target;
                  llvm::Value* cond = load_vreg(inst.br.src1);
                  if (!cond || target >= func.block_count) break;
                  llvm::Value* cmp = builder.CreateICmpNE(cond,
                     llvm::Constant::getNullValue(cond->getType()));
                  auto* target_bb = (func.blocks[target].is_loop && block_entries[target])
                     ? block_entries[target]
                     : block_exits[target];
                  auto* cont_bb = llvm::BasicBlock::Create(*ctx, "br_if_cont", fn);
                  builder.CreateCondBr(cmp, target_bb, cont_bb);
                  builder.SetInsertPoint(cont_bb);
                  break;
               }

               case ir_op::br_table: {
                  // inst.rr.src1 = index vreg, inst.dest = table size
                  llvm::Value* bt_index = load_vreg(inst.rr.src1);
                  uint32_t bt_size = inst.dest;

                  // Helper to resolve br target to correct LLVM block
                  auto resolve_br_target = [&](uint32_t target) -> llvm::BasicBlock* {
                     if (target >= func.block_count) return nullptr;
                     return (func.blocks[target].is_loop && block_entries[target])
                        ? block_entries[target]
                        : block_exits[target];
                  };

                  // Scan subsequent instructions for [mov*, br] groups.
                  // Each case can have zero or more mov instructions before the br.
                  struct bt_case {
                     uint32_t br_target;
                     std::vector<std::pair<uint32_t, uint32_t>> movs; // dest, src pairs
                  };
                  std::vector<bt_case> cases;
                  uint32_t scan = ip + 1;
                  while (scan < func.inst_count && cases.size() <= bt_size) {
                     bt_case c;
                     // Collect movs before br
                     while (scan < func.inst_count && func.insts[scan].opcode == ir_op::mov) {
                        auto& mi = func.insts[scan];
                        c.movs.push_back({mi.dest, mi.rr.src1});
                        scan++;
                     }
                     // Expect a br
                     if (scan < func.inst_count && func.insts[scan].opcode == ir_op::br) {
                        c.br_target = func.insts[scan].br.target;
                        cases.push_back(std::move(c));
                        scan++;
                     } else {
                        break;
                     }
                  }

                  if (bt_index && cases.size() > bt_size) {
                     // Last case is the default
                     auto& default_case = cases.back();

                     // Create per-case blocks if any case has movs
                     bool need_case_blocks = false;
                     for (auto& c : cases) {
                        if (!c.movs.empty()) { need_case_blocks = true; break; }
                     }

                     if (need_case_blocks) {
                        // Create a case block for each entry (including default)
                        auto* default_case_bb = llvm::BasicBlock::Create(*ctx, "bt_default", fn);
                        auto* sw = builder.CreateSwitch(bt_index, default_case_bb,
                           static_cast<unsigned>(cases.size() - 1));

                        for (uint32_t c = 0; c < cases.size(); c++) {
                           llvm::BasicBlock* case_bb;
                           if (c == cases.size() - 1) {
                              case_bb = default_case_bb;
                           } else {
                              case_bb = llvm::BasicBlock::Create(*ctx,
                                 "bt_case" + std::to_string(c), fn);
                              sw->addCase(builder.getInt32(c), case_bb);
                           }
                           builder.SetInsertPoint(case_bb);
                           // Emit movs for this case
                           for (auto& [dest, src] : cases[c].movs) {
                              llvm::Value* val = load_vreg(src);
                              if (val) store_vreg(dest, val);
                           }
                           auto* target = resolve_br_target(cases[c].br_target);
                           if (target) builder.CreateBr(target);
                        }
                     } else {
                        // No movs — direct switch to targets
                        auto* default_bb = resolve_br_target(default_case.br_target);
                        if (default_bb) {
                           auto* sw = builder.CreateSwitch(bt_index, default_bb,
                              static_cast<unsigned>(cases.size() - 1));
                           for (uint32_t c = 0; c < cases.size() - 1; c++) {
                              auto* target = resolve_br_target(cases[c].br_target);
                              if (target) sw->addCase(builder.getInt32(c), target);
                           }
                        }
                     }
                  }

                  ip = scan - 1;
                  break;
               }

               case ir_op::return_: {
                  if (inst.rr.src1 != ir_vreg_none) {
                     llvm::Value* ret_val = load_vreg(inst.rr.src1);
                     if (ret_val) {
                        ret_val = convert_type(ret_val, fn->getReturnType());
                        builder.CreateRet(ret_val);
                     } else {
                        builder.CreateRetVoid();
                     }
                  } else {
                     if (fn->getReturnType()->isVoidTy())
                        builder.CreateRetVoid();
                     else
                        builder.CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
                  }
                  break;
               }

               // ──── Mov (phi-node merge) ────
               case ir_op::mov: {
                  llvm::Value* val = load_vreg(inst.rr.src1);
                  if (val) store_vreg(inst.dest, val);
                  break;
               }

               // ──── Constants ────
               case ir_op::const_i32:
                  store_vreg(inst.dest, builder.getInt32(
                     static_cast<uint32_t>(static_cast<int32_t>(inst.imm64))));
                  break;

               case ir_op::const_i64:
                  store_vreg(inst.dest, builder.getInt64(
                     static_cast<uint64_t>(inst.imm64)));
                  break;

               case ir_op::const_f32: {
                  float f;
                  std::memcpy(&f, &inst.immf32, sizeof(float));
                  // Use APFloat for exact bit pattern preservation
                  store_vreg(inst.dest,
                     llvm::ConstantFP::get(f32_ty, llvm::APFloat(f)));
                  break;
               }

               case ir_op::const_f64: {
                  double d;
                  std::memcpy(&d, &inst.immf64, sizeof(double));
                  store_vreg(inst.dest,
                     llvm::ConstantFP::get(f64_ty, llvm::APFloat(d)));
                  break;
               }

               // ──── Local / Global access ────
               case ir_op::local_get: {
                  uint32_t li = inst.local.index;
                  if (li < locals.size() && locals[li]) {
                     llvm::Value* val = builder.CreateLoad(
                        locals[li]->getAllocatedType(), locals[li]);
                     store_vreg(inst.dest, val);
                  }
                  break;
               }

               case ir_op::local_set: {
                  uint32_t li = inst.local.index;
                  llvm::Value* val = load_vreg(inst.local.src1);
                  if (li < locals.size() && locals[li] && val) {
                     llvm::Type* local_ty = locals[li]->getAllocatedType();
                     if (val->getType() != local_ty) {
                        if (val->getType()->isIntegerTy() && local_ty->isIntegerTy()) {
                           unsigned src_bits = val->getType()->getIntegerBitWidth();
                           unsigned dst_bits = local_ty->getIntegerBitWidth();
                           if (src_bits < dst_bits)
                              val = builder.CreateZExt(val, local_ty);
                           else if (src_bits > dst_bits)
                              val = builder.CreateTrunc(val, local_ty);
                        }
                     }
                     builder.CreateStore(val, locals[li]);
                  }
                  break;
               }

               case ir_op::local_tee: {
                  // Same as local_set but value stays on stack (already handled by ir_writer)
                  uint32_t li = inst.local.index;
                  llvm::Value* val = load_vreg(inst.local.src1);
                  if (li < locals.size() && locals[li] && val) {
                     llvm::Type* local_ty = locals[li]->getAllocatedType();
                     if (val->getType() != local_ty) {
                        if (val->getType()->isIntegerTy() && local_ty->isIntegerTy()) {
                           unsigned src_bits = val->getType()->getIntegerBitWidth();
                           unsigned dst_bits = local_ty->getIntegerBitWidth();
                           if (src_bits < dst_bits)
                              val = builder.CreateZExt(val, local_ty);
                           else if (src_bits > dst_bits)
                              val = builder.CreateTrunc(val, local_ty);
                        }
                     }
                     builder.CreateStore(val, locals[li]);
                  }
                  break;
               }

               case ir_op::global_get: {
                  uint32_t gidx = inst.local.index;
                  llvm::Value* raw = builder.CreateCall(rt_global_get,
                     {ctx_ptr, builder.getInt32(gidx)});
                  // Truncate/bitcast to the expected type
                  llvm::Type* target_ty = ir_type_to_llvm(inst.type);
                  llvm::Value* result;
                  if (target_ty == i32_ty) {
                     result = builder.CreateTrunc(raw, i32_ty);
                  } else if (target_ty == f32_ty) {
                     result = builder.CreateBitCast(builder.CreateTrunc(raw, i32_ty), f32_ty);
                  } else if (target_ty == f64_ty) {
                     result = builder.CreateBitCast(raw, f64_ty);
                  } else {
                     result = raw; // i64
                  }
                  store_vreg(inst.dest, result);
                  break;
               }

               case ir_op::global_set: {
                  uint32_t gidx = inst.local.index;
                  llvm::Value* val = load_vreg(inst.local.src1);
                  if (!val) break;
                  // Extend/bitcast to i64 for the runtime helper
                  llvm::Value* i64_val;
                  if (val->getType() == i32_ty) {
                     i64_val = builder.CreateZExt(val, i64_ty);
                  } else if (val->getType() == f32_ty) {
                     i64_val = builder.CreateZExt(builder.CreateBitCast(val, i32_ty), i64_ty);
                  } else if (val->getType() == f64_ty) {
                     i64_val = builder.CreateBitCast(val, i64_ty);
                  } else {
                     i64_val = val;
                  }
                  builder.CreateCall(rt_global_set,
                     {ctx_ptr, builder.getInt32(gidx), i64_val});
                  break;
               }

               // ──── Parametric ────
               case ir_op::drop:
                  // No-op in LLVM (values are SSA, unused values are dead)
                  break;

               case ir_op::select: {
                  llvm::Value* cond = load_vreg(inst.sel.cond);
                  llvm::Value* val1 = load_vreg(inst.sel.val1);
                  llvm::Value* val2 = load_vreg(inst.sel.val2);
                  if (cond && val1 && val2) {
                     llvm::Value* cmp = builder.CreateICmpNE(cond,
                        llvm::Constant::getNullValue(cond->getType()));
                     llvm::Value* result = builder.CreateSelect(cmp, val1, val2);
                     store_vreg(inst.dest, result);
                  }
                  break;
               }

               // ──── Call argument pseudo-instruction ────
               case ir_op::arg: {
                  llvm::Value* val = load_vreg(inst.rr.src1);
                  if (val) call_args.push_back(val);
                  break;
               }

               // ──── Function calls ────
               case ir_op::call: {
                  uint32_t funcnum = inst.call.index;
                  uint32_t num_imports = wasm_mod.get_imported_functions_size();

                  if (funcnum < num_imports) {
                     // Host function call — go through runtime helper
                     uint32_t nargs = static_cast<uint32_t>(call_args.size());
                     // Use entry-block alloca (avoids stack growth in loops)
                     llvm::Value* args_array = host_args_alloca;
                     if (!args_array) {
                        // No host calls were expected — shouldn't happen, but be safe
                        args_array = builder.CreateAlloca(i64_ty, builder.getInt32(1));
                     }
                     for (uint32_t a = 0; a < nargs; a++) {
                        llvm::Value* slot = builder.CreateGEP(i64_ty, args_array,
                           builder.getInt32(a));
                        llvm::Value* v = call_args[a];
                        // Extend/bitcast to i64
                        if (v->getType() == i32_ty)
                           v = builder.CreateZExt(v, i64_ty);
                        else if (v->getType() == f32_ty)
                           v = builder.CreateZExt(builder.CreateBitCast(v, i32_ty), i64_ty);
                        else if (v->getType() == f64_ty)
                           v = builder.CreateBitCast(v, i64_ty);
                        builder.CreateStore(v, slot);
                     }
                     call_args.clear();

                     llvm::Value* raw = builder.CreateCall(rt_call_host,
                        {ctx_ptr, builder.getInt32(funcnum), args_array,
                         builder.getInt32(nargs)});

                     if (inst.dest != ir_vreg_none) {
                        llvm::Type* target_ty = ir_type_to_llvm(inst.type);
                        llvm::Value* result;
                        if (target_ty == i32_ty)
                           result = builder.CreateTrunc(raw, i32_ty);
                        else if (target_ty == f32_ty)
                           result = builder.CreateBitCast(builder.CreateTrunc(raw, i32_ty), f32_ty);
                        else if (target_ty == f64_ty)
                           result = builder.CreateBitCast(raw, f64_ty);
                        else
                           result = raw;
                        store_vreg(inst.dest, result);
                     }
                     // Reload mem_ptr after host call (it might have grown memory)
                     // We can't easily do this without a helper, so we rely on
                     // the caller to pass mem_ptr again. For now, keep existing ptr.
                  } else {
                     // WASM-to-WASM call
                     std::vector<llvm::Value*> args;
                     args.push_back(ctx_ptr);
                     args.push_back(mem_ptr);

                     // Convert each arg to match callee's parameter types
                     if (funcnum < wasm_funcs.size() && wasm_funcs[funcnum]) {
                        auto* callee_ty = wasm_funcs[funcnum]->getFunctionType();
                        for (uint32_t a = 0; a < call_args.size(); a++) {
                           uint32_t param_idx = a + 2; // skip ctx, mem
                           if (param_idx < callee_ty->getNumParams()) {
                              call_args[a] = convert_type(call_args[a], callee_ty->getParamType(param_idx));
                           }
                        }
                     }

                     for (auto* a : call_args) args.push_back(a);
                     call_args.clear();

                     if (funcnum < wasm_funcs.size() && wasm_funcs[funcnum]) {
                        auto* result = builder.CreateCall(wasm_funcs[funcnum], args);
                        if (inst.dest != ir_vreg_none && !result->getType()->isVoidTy()) {
                           store_vreg(inst.dest, result);
                        }
                     }
                  }
                  break;
               }

               case ir_op::call_indirect: {
                  uint32_t type_idx = inst.call.index;
                  // The table index is the last call_arg (condition/index value)
                  // In the IR, call_indirect's call_args include the table index as the last arg
                  llvm::Value* table_elem = nullptr;
                  if (!call_args.empty()) {
                     table_elem = call_args.back();
                     call_args.pop_back();
                  }
                  if (!table_elem) {
                     call_args.clear();
                     break;
                  }

                  uint32_t nargs = static_cast<uint32_t>(call_args.size());
                  // Use entry-block alloca (avoids stack growth in loops)
                  llvm::Value* args_array = host_args_alloca;
                  if (!args_array) {
                     args_array = builder.CreateAlloca(i64_ty, builder.getInt32(1));
                  }
                  for (uint32_t a = 0; a < nargs; a++) {
                     llvm::Value* slot = builder.CreateGEP(i64_ty, args_array,
                        builder.getInt32(a));
                     llvm::Value* v = call_args[a];
                     if (v->getType() == i32_ty)
                        v = builder.CreateZExt(v, i64_ty);
                     else if (v->getType() == f32_ty)
                        v = builder.CreateZExt(builder.CreateBitCast(v, i32_ty), i64_ty);
                     else if (v->getType() == f64_ty)
                        v = builder.CreateBitCast(v, i64_ty);
                     builder.CreateStore(v, slot);
                  }
                  call_args.clear();

                  // Ensure table_elem is i32
                  if (table_elem->getType() != i32_ty) {
                     if (table_elem->getType()->isIntegerTy())
                        table_elem = builder.CreateTrunc(table_elem, i32_ty);
                  }

                  llvm::Value* raw = builder.CreateCall(rt_call_indirect,
                     {ctx_ptr, mem_ptr, builder.getInt32(type_idx),
                      table_elem, args_array, builder.getInt32(nargs)});

                  if (inst.dest != ir_vreg_none) {
                     llvm::Type* target_ty = ir_type_to_llvm(inst.type);
                     llvm::Value* result;
                     if (target_ty == i32_ty)
                        result = builder.CreateTrunc(raw, i32_ty);
                     else if (target_ty == f32_ty)
                        result = builder.CreateBitCast(builder.CreateTrunc(raw, i32_ty), f32_ty);
                     else if (target_ty == f64_ty)
                        result = builder.CreateBitCast(raw, f64_ty);
                     else
                        result = raw;
                     store_vreg(inst.dest, result);
                  }
                  break;
               }

               // ──── Memory loads ────
               case ir_op::i32_load: case ir_op::i64_load:
               case ir_op::f32_load: case ir_op::f64_load:
               case ir_op::i32_load8_s: case ir_op::i32_load8_u:
               case ir_op::i32_load16_s: case ir_op::i32_load16_u:
               case ir_op::i64_load8_s: case ir_op::i64_load8_u:
               case ir_op::i64_load16_s: case ir_op::i64_load16_u:
               case ir_op::i64_load32_s: case ir_op::i64_load32_u: {
                  llvm::Value* addr = load_vreg(inst.ri.src1);
                  if (!addr) break;
                  uint32_t offset = static_cast<uint32_t>(inst.ri.imm);

                  // Compute effective address: mem_ptr + addr + offset
                  llvm::Value* eff_addr = builder.CreateZExt(addr, i64_ty);
                  if (offset != 0) {
                     eff_addr = builder.CreateAdd(eff_addr, builder.getInt64(static_cast<uint64_t>(offset)));
                  }
                  llvm::Value* ptr = builder.CreateGEP(i8_ty, mem_ptr, eff_addr);

                  // Determine load type
                  llvm::Type* load_ty;
                  bool sign_extend = false;
                  llvm::Type* result_ty;
                  switch (inst.opcode) {
                     case ir_op::i32_load:    load_ty = i32_ty; result_ty = i32_ty; break;
                     case ir_op::i64_load:    load_ty = i64_ty; result_ty = i64_ty; break;
                     case ir_op::f32_load:    load_ty = f32_ty; result_ty = f32_ty; break;
                     case ir_op::f64_load:    load_ty = f64_ty; result_ty = f64_ty; break;
                     case ir_op::i32_load8_s: load_ty = i8_ty;  result_ty = i32_ty; sign_extend = true; break;
                     case ir_op::i32_load8_u: load_ty = i8_ty;  result_ty = i32_ty; break;
                     case ir_op::i32_load16_s:load_ty = i16_ty; result_ty = i32_ty; sign_extend = true; break;
                     case ir_op::i32_load16_u:load_ty = i16_ty; result_ty = i32_ty; break;
                     case ir_op::i64_load8_s: load_ty = i8_ty;  result_ty = i64_ty; sign_extend = true; break;
                     case ir_op::i64_load8_u: load_ty = i8_ty;  result_ty = i64_ty; break;
                     case ir_op::i64_load16_s:load_ty = i16_ty; result_ty = i64_ty; sign_extend = true; break;
                     case ir_op::i64_load16_u:load_ty = i16_ty; result_ty = i64_ty; break;
                     case ir_op::i64_load32_s:load_ty = i32_ty; result_ty = i64_ty; sign_extend = true; break;
                     case ir_op::i64_load32_u:load_ty = i32_ty; result_ty = i64_ty; break;
                     default: load_ty = i32_ty; result_ty = i32_ty; break;
                  }

                  auto* load_inst = builder.CreateLoad(load_ty, ptr);
                  // Non-volatile: LLVM won't speculate loads from non-dereferenceable
                  // pointers (isSafeToSpeculativelyExecute returns false), so guard
                  // pages still catch OOB access. Removing volatile enables GVN,
                  // load forwarding, and addressing mode folding.
                  llvm::Value* loaded = load_inst;
                  // Extend if needed
                  if (load_ty != result_ty) {
                     if (sign_extend)
                        loaded = builder.CreateSExt(loaded, result_ty);
                     else
                        loaded = builder.CreateZExt(loaded, result_ty);
                  }
                  store_vreg(inst.dest, loaded);
                  break;
               }

               // ──── Memory stores ────
               case ir_op::i32_store: case ir_op::i64_store:
               case ir_op::f32_store: case ir_op::f64_store:
               case ir_op::i32_store8: case ir_op::i32_store16:
               case ir_op::i64_store8: case ir_op::i64_store16:
               case ir_op::i64_store32: {
                  llvm::Value* addr = load_vreg(inst.ri.src1);
                  // For stores, inst.dest holds the value vreg
                  llvm::Value* val = load_vreg(inst.dest);
                  if (!addr || !val) break;
                  uint32_t offset = static_cast<uint32_t>(inst.ri.imm);

                  // Compute effective address
                  llvm::Value* eff_addr = builder.CreateZExt(addr, i64_ty);
                  if (offset != 0) {
                     eff_addr = builder.CreateAdd(eff_addr, builder.getInt64(static_cast<uint64_t>(offset)));
                  }
                  llvm::Value* ptr = builder.CreateGEP(i8_ty, mem_ptr, eff_addr);

                  // Truncate value if needed
                  switch (inst.opcode) {
                     case ir_op::i32_store8:
                     case ir_op::i64_store8:
                        val = builder.CreateTrunc(val, i8_ty);
                        break;
                     case ir_op::i32_store16:
                     case ir_op::i64_store16:
                        val = builder.CreateTrunc(val, i16_ty);
                        break;
                     case ir_op::i64_store32:
                        val = builder.CreateTrunc(val, i32_ty);
                        break;
                     default:
                        break;
                  }
                  auto* store_inst = builder.CreateStore(val, ptr);
                  break;
               }

               // ──── Memory management ────
               case ir_op::memory_size: {
                  llvm::Value* sz = builder.CreateCall(rt_memory_size, {ctx_ptr});
                  store_vreg(inst.dest, sz);
                  break;
               }

               case ir_op::memory_grow: {
                  llvm::Value* pages = load_vreg(inst.rr.src1);
                  if (!pages) break;
                  // Allocate space on stack for the new memory pointer
                  llvm::Value* mem_out = builder.CreateAlloca(ptr_ty);
                  llvm::Value* result = builder.CreateCall(rt_memory_grow,
                     {ctx_ptr, pages, mem_out});
                  // Update mem_ptr to the potentially new base
                  mem_ptr = builder.CreateLoad(ptr_ty, mem_out);
                  store_vreg(inst.dest, result);
                  break;
               }

               // ──── i32 arithmetic ────
               case ir_op::i32_add:
                  store_vreg(inst.dest, builder.CreateAdd(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_sub:
                  store_vreg(inst.dest, builder.CreateSub(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_mul:
                  store_vreg(inst.dest, builder.CreateMul(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_div_s: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i32_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i32_ty);
                  // Trap on divide by zero
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt32(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "div_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "div_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  // Trap on INT_MIN / -1 overflow
                  auto* is_min = builder.CreateICmpEQ(lhs, builder.getInt32(INT32_MIN));
                  auto* is_neg1 = builder.CreateICmpEQ(rhs, builder.getInt32(-1));
                  auto* is_overflow = builder.CreateAnd(is_min, is_neg1);
                  auto* ovf_bb = llvm::BasicBlock::Create(*ctx, "div_ovf", fn);
                  auto* safe_bb = llvm::BasicBlock::Create(*ctx, "div_safe", fn);
                  builder.CreateCondBr(is_overflow, ovf_bb, safe_bb);
                  builder.SetInsertPoint(ovf_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(2)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(safe_bb);
                  store_vreg(inst.dest, builder.CreateSDiv(lhs, rhs));
                  break;
               }
               case ir_op::i32_div_u: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i32_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i32_ty);
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt32(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "divu_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "divu_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  store_vreg(inst.dest, builder.CreateUDiv(lhs, rhs));
                  break;
               }
               case ir_op::i32_rem_s: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i32_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i32_ty);
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt32(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "rems_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "rems_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  // INT_MIN % -1 = 0 (not a trap, but avoid UB)
                  auto* is_min = builder.CreateICmpEQ(lhs, builder.getInt32(INT32_MIN));
                  auto* is_neg1 = builder.CreateICmpEQ(rhs, builder.getInt32(-1));
                  auto* is_special = builder.CreateAnd(is_min, is_neg1);
                  auto* special_bb = llvm::BasicBlock::Create(*ctx, "rems_special", fn);
                  auto* normal_bb = llvm::BasicBlock::Create(*ctx, "rems_normal", fn);
                  auto* merge_bb = llvm::BasicBlock::Create(*ctx, "rems_merge", fn);
                  builder.CreateCondBr(is_special, special_bb, normal_bb);
                  builder.SetInsertPoint(special_bb);
                  builder.CreateBr(merge_bb);
                  builder.SetInsertPoint(normal_bb);
                  auto* rem_val = builder.CreateSRem(lhs, rhs);
                  builder.CreateBr(merge_bb);
                  builder.SetInsertPoint(merge_bb);
                  auto* phi = builder.CreatePHI(i32_ty, 2);
                  phi->addIncoming(builder.getInt32(0), special_bb);
                  phi->addIncoming(rem_val, normal_bb);
                  store_vreg(inst.dest, phi);
                  break;
               }
               case ir_op::i32_rem_u: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i32_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i32_ty);
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt32(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "remu_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "remu_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  store_vreg(inst.dest, builder.CreateURem(lhs, rhs));
                  break;
               }
               case ir_op::i32_and:
                  store_vreg(inst.dest, builder.CreateAnd(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_or:
                  store_vreg(inst.dest, builder.CreateOr(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_xor:
                  store_vreg(inst.dest, builder.CreateXor(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_shl:
                  store_vreg(inst.dest, builder.CreateShl(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_shr_s:
                  store_vreg(inst.dest, builder.CreateAShr(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_shr_u:
                  store_vreg(inst.dest, builder.CreateLShr(
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)));
                  break;
               case ir_op::i32_rotl:
                  store_vreg(inst.dest, emit_rotate(builder,
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty), i32_ty, false));
                  break;
               case ir_op::i32_rotr:
                  store_vreg(inst.dest, emit_rotate(builder,
                     load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty), i32_ty, true));
                  break;

               // ──── i64 arithmetic ────
               case ir_op::i64_add:
                  store_vreg(inst.dest, builder.CreateAdd(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_sub:
                  store_vreg(inst.dest, builder.CreateSub(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_mul:
                  store_vreg(inst.dest, builder.CreateMul(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_div_s: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i64_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i64_ty);
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt64(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "div64_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "div64_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  auto* is_min = builder.CreateICmpEQ(lhs, builder.getInt64(INT64_MIN));
                  auto* is_neg1 = builder.CreateICmpEQ(rhs, builder.getInt64(-1));
                  auto* is_overflow = builder.CreateAnd(is_min, is_neg1);
                  auto* ovf_bb = llvm::BasicBlock::Create(*ctx, "div64_ovf", fn);
                  auto* safe_bb = llvm::BasicBlock::Create(*ctx, "div64_safe", fn);
                  builder.CreateCondBr(is_overflow, ovf_bb, safe_bb);
                  builder.SetInsertPoint(ovf_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(2)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(safe_bb);
                  store_vreg(inst.dest, builder.CreateSDiv(lhs, rhs));
                  break;
               }
               case ir_op::i64_div_u: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i64_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i64_ty);
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt64(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "divu64_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "divu64_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  store_vreg(inst.dest, builder.CreateUDiv(lhs, rhs));
                  break;
               }
               case ir_op::i64_rem_s: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i64_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i64_ty);
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt64(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "rems64_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "rems64_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  auto* is_min = builder.CreateICmpEQ(lhs, builder.getInt64(INT64_MIN));
                  auto* is_neg1 = builder.CreateICmpEQ(rhs, builder.getInt64(-1));
                  auto* is_special = builder.CreateAnd(is_min, is_neg1);
                  auto* special_bb = llvm::BasicBlock::Create(*ctx, "rems64_special", fn);
                  auto* normal_bb = llvm::BasicBlock::Create(*ctx, "rems64_normal", fn);
                  auto* merge_bb = llvm::BasicBlock::Create(*ctx, "rems64_merge", fn);
                  builder.CreateCondBr(is_special, special_bb, normal_bb);
                  builder.SetInsertPoint(special_bb);
                  builder.CreateBr(merge_bb);
                  builder.SetInsertPoint(normal_bb);
                  auto* rem_val = builder.CreateSRem(lhs, rhs);
                  builder.CreateBr(merge_bb);
                  builder.SetInsertPoint(merge_bb);
                  auto* phi = builder.CreatePHI(i64_ty, 2);
                  phi->addIncoming(builder.getInt64(0), special_bb);
                  phi->addIncoming(rem_val, normal_bb);
                  store_vreg(inst.dest, phi);
                  break;
               }
               case ir_op::i64_rem_u: {
                  auto* lhs = load_vreg_as(inst.rr.src1, i64_ty);
                  auto* rhs = load_vreg_as(inst.rr.src2, i64_ty);
                  auto* is_zero = builder.CreateICmpEQ(rhs, builder.getInt64(0));
                  auto* trap_bb = llvm::BasicBlock::Create(*ctx, "remu64_trap", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "remu64_ok", fn);
                  builder.CreateCondBr(is_zero, trap_bb, ok_bb);
                  builder.SetInsertPoint(trap_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(1)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);
                  store_vreg(inst.dest, builder.CreateURem(lhs, rhs));
                  break;
               }
               case ir_op::i64_and:
                  store_vreg(inst.dest, builder.CreateAnd(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_or:
                  store_vreg(inst.dest, builder.CreateOr(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_xor:
                  store_vreg(inst.dest, builder.CreateXor(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_shl:
                  store_vreg(inst.dest, builder.CreateShl(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_shr_s:
                  store_vreg(inst.dest, builder.CreateAShr(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_shr_u:
                  store_vreg(inst.dest, builder.CreateLShr(
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)));
                  break;
               case ir_op::i64_rotl:
                  store_vreg(inst.dest, emit_rotate(builder,
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty), i64_ty, false));
                  break;
               case ir_op::i64_rotr:
                  store_vreg(inst.dest, emit_rotate(builder,
                     load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty), i64_ty, true));
                  break;

               // ──── Unary integer operations ────
               case ir_op::i32_clz:
                  store_vreg(inst.dest, emit_count_intrinsic(builder,
                     load_vreg_as(inst.rr.src1, i32_ty), i32_ty, llvm::Intrinsic::ctlz));
                  break;
               case ir_op::i32_ctz:
                  store_vreg(inst.dest, emit_count_intrinsic(builder,
                     load_vreg_as(inst.rr.src1, i32_ty), i32_ty, llvm::Intrinsic::cttz));
                  break;
               case ir_op::i32_popcnt:
                  store_vreg(inst.dest, emit_count_intrinsic(builder,
                     load_vreg_as(inst.rr.src1, i32_ty), i32_ty, llvm::Intrinsic::ctpop));
                  break;
               case ir_op::i64_clz:
                  store_vreg(inst.dest, emit_count_intrinsic(builder,
                     load_vreg_as(inst.rr.src1, i64_ty), i64_ty, llvm::Intrinsic::ctlz));
                  break;
               case ir_op::i64_ctz:
                  store_vreg(inst.dest, emit_count_intrinsic(builder,
                     load_vreg_as(inst.rr.src1, i64_ty), i64_ty, llvm::Intrinsic::cttz));
                  break;
               case ir_op::i64_popcnt:
                  store_vreg(inst.dest, emit_count_intrinsic(builder,
                     load_vreg_as(inst.rr.src1, i64_ty), i64_ty, llvm::Intrinsic::ctpop));
                  break;

               // ──── i32 comparisons ────
               case ir_op::i32_eqz:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpEQ(load_vreg_as(inst.rr.src1, i32_ty), builder.getInt32(0)), i32_ty));
                  break;
               case ir_op::i32_eq:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpEQ(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_ne:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpNE(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_lt_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSLT(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_lt_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpULT(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_gt_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSGT(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_gt_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpUGT(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_le_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSLE(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_le_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpULE(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_ge_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSGE(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;
               case ir_op::i32_ge_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpUGE(load_vreg_as(inst.rr.src1, i32_ty), load_vreg_as(inst.rr.src2, i32_ty)), i32_ty));
                  break;

               // ──── i64 comparisons ────
               case ir_op::i64_eqz:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpEQ(load_vreg_as(inst.rr.src1, i64_ty), builder.getInt64(0)), i32_ty));
                  break;
               case ir_op::i64_eq:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpEQ(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_ne:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpNE(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_lt_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSLT(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_lt_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpULT(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_gt_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSGT(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_gt_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpUGT(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_le_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSLE(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_le_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpULE(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_ge_s:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpSGE(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;
               case ir_op::i64_ge_u:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateICmpUGE(load_vreg_as(inst.rr.src1, i64_ty), load_vreg_as(inst.rr.src2, i64_ty)), i32_ty));
                  break;

               // ──── f32 comparisons ────
               case ir_op::f32_eq:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOEQ(load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)), i32_ty));
                  break;
               case ir_op::f32_ne:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpUNE(load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)), i32_ty));
                  break;
               case ir_op::f32_lt:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOLT(load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)), i32_ty));
                  break;
               case ir_op::f32_gt:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOGT(load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)), i32_ty));
                  break;
               case ir_op::f32_le:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOLE(load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)), i32_ty));
                  break;
               case ir_op::f32_ge:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOGE(load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)), i32_ty));
                  break;

               // ──── f64 comparisons ────
               case ir_op::f64_eq:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOEQ(load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)), i32_ty));
                  break;
               case ir_op::f64_ne:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpUNE(load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)), i32_ty));
                  break;
               case ir_op::f64_lt:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOLT(load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)), i32_ty));
                  break;
               case ir_op::f64_gt:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOGT(load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)), i32_ty));
                  break;
               case ir_op::f64_le:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOLE(load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)), i32_ty));
                  break;
               case ir_op::f64_ge:
                  store_vreg(inst.dest, builder.CreateZExt(
                     builder.CreateFCmpOGE(load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)), i32_ty));
                  break;

               // ──── f32 unary ────
               case ir_op::f32_abs:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fabs, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::f32_neg:
                  store_vreg(inst.dest, builder.CreateFNeg(load_vreg_as(inst.rr.src1, f32_ty)));
                  break;
               case ir_op::f32_ceil:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ceil, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::f32_floor:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::floor, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::f32_trunc:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::trunc, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::f32_nearest:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::nearbyint, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::f32_sqrt:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sqrt, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;

               // ──── f64 unary ────
               case ir_op::f64_abs:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fabs, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::f64_neg:
                  store_vreg(inst.dest, builder.CreateFNeg(load_vreg_as(inst.rr.src1, f64_ty)));
                  break;
               case ir_op::f64_ceil:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ceil, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::f64_floor:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::floor, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::f64_trunc:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::trunc, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::f64_nearest:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::nearbyint, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::f64_sqrt:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sqrt, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;

               // ──── f32 binary arithmetic ────
               case ir_op::f32_add:
                  store_vreg(inst.dest, builder.CreateFAdd(
                     load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  break;
               case ir_op::f32_sub:
                  store_vreg(inst.dest, builder.CreateFSub(
                     load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  break;
               case ir_op::f32_mul:
                  store_vreg(inst.dest, builder.CreateFMul(
                     load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  break;
               case ir_op::f32_div:
                  store_vreg(inst.dest, builder.CreateFDiv(
                     load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  break;
               case ir_op::f32_min:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::minimum, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)}));
                  break;
               case ir_op::f32_max:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::maximum, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)}));
                  break;
               case ir_op::f32_copysign:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::copysign, {f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)}));
                  break;

               // ──── f64 binary arithmetic ────
               case ir_op::f64_add:
                  store_vreg(inst.dest, builder.CreateFAdd(
                     load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  break;
               case ir_op::f64_sub:
                  store_vreg(inst.dest, builder.CreateFSub(
                     load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  break;
               case ir_op::f64_mul:
                  store_vreg(inst.dest, builder.CreateFMul(
                     load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  break;
               case ir_op::f64_div:
                  store_vreg(inst.dest, builder.CreateFDiv(
                     load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  break;
               case ir_op::f64_min:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::minimum, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)}));
                  break;
               case ir_op::f64_max:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::maximum, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)}));
                  break;
               case ir_op::f64_copysign:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::copysign, {f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)}));
                  break;

               // ──── Type conversions ────
               case ir_op::i32_wrap_i64:
                  store_vreg(inst.dest, builder.CreateTrunc(load_vreg_as(inst.rr.src1, i64_ty), i32_ty));
                  break;
               case ir_op::i64_extend_s_i32:
                  store_vreg(inst.dest, builder.CreateSExt(load_vreg_as(inst.rr.src1, i32_ty), i64_ty));
                  break;
               case ir_op::i64_extend_u_i32:
                  store_vreg(inst.dest, builder.CreateZExt(load_vreg_as(inst.rr.src1, i32_ty), i64_ty));
                  break;

               // Integer truncation from floats (with trap checks)
               case ir_op::i32_trunc_s_f32:
               case ir_op::i32_trunc_u_f32:
               case ir_op::i32_trunc_s_f64:
               case ir_op::i32_trunc_u_f64:
               case ir_op::i64_trunc_s_f32:
               case ir_op::i64_trunc_u_f32:
               case ir_op::i64_trunc_s_f64:
               case ir_op::i64_trunc_u_f64: {
                  bool is_signed = (inst.opcode == ir_op::i32_trunc_s_f32 ||
                                    inst.opcode == ir_op::i32_trunc_s_f64 ||
                                    inst.opcode == ir_op::i64_trunc_s_f32 ||
                                    inst.opcode == ir_op::i64_trunc_s_f64);
                  bool src_f32 = (inst.opcode == ir_op::i32_trunc_s_f32 ||
                                  inst.opcode == ir_op::i32_trunc_u_f32 ||
                                  inst.opcode == ir_op::i64_trunc_s_f32 ||
                                  inst.opcode == ir_op::i64_trunc_u_f32);
                  bool dst_i32 = (inst.opcode == ir_op::i32_trunc_s_f32 ||
                                  inst.opcode == ir_op::i32_trunc_u_f32 ||
                                  inst.opcode == ir_op::i32_trunc_s_f64 ||
                                  inst.opcode == ir_op::i32_trunc_u_f64);
                  llvm::Type* src_ty = src_f32 ? f32_ty : f64_ty;
                  llvm::Type* dst_ty = dst_i32 ? i32_ty : i64_ty;
                  auto* val = load_vreg_as(inst.rr.src1, src_ty);
                  if (!val) break;

                  // Check for NaN
                  auto* is_nan = builder.CreateFCmpUNO(val, val);
                  auto* nan_bb = llvm::BasicBlock::Create(*ctx, "trunc_nan", fn);
                  auto* chk_bb = llvm::BasicBlock::Create(*ctx, "trunc_chk", fn);
                  builder.CreateCondBr(is_nan, nan_bb, chk_bb);
                  builder.SetInsertPoint(nan_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(3)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(chk_bb);

                  // Determine bounds based on destination type, signedness, and source precision.
                  // WASM spec: trunc is valid when truncated value is in range.
                  // Use the actual FP representation to avoid precision-rounding issues.
                  llvm::Value* out_of_range;
                  if (dst_i32 && is_signed) {
                     // Valid range: [-2147483648, 2147483647]
                     // Trap if val >= 2147483648.0 or val <= -2147483649.0
                     // For f32 source: -2147483649.0 rounds to -2147483648.0f,
                     // so use strict < against -2147483648.0 instead
                     auto* hi_val = llvm::ConstantFP::get(src_ty, 2147483648.0);
                     auto* too_high = builder.CreateFCmpOGE(val, hi_val);
                     llvm::Value* too_low;
                     if (src_f32) {
                        auto* lo_val = llvm::ConstantFP::get(src_ty, -2147483648.0);
                        too_low = builder.CreateFCmpOLT(val, lo_val);
                     } else {
                        auto* lo_val = llvm::ConstantFP::get(src_ty, -2147483649.0);
                        too_low = builder.CreateFCmpOLE(val, lo_val);
                     }
                     out_of_range = builder.CreateOr(too_low, too_high);
                  } else if (dst_i32 && !is_signed) {
                     auto* hi_val = llvm::ConstantFP::get(src_ty, 4294967296.0);
                     auto* lo_val = llvm::ConstantFP::get(src_ty, -1.0);
                     auto* too_high = builder.CreateFCmpOGE(val, hi_val);
                     auto* too_low = builder.CreateFCmpOLE(val, lo_val);
                     out_of_range = builder.CreateOr(too_low, too_high);
                  } else if (!dst_i32 && is_signed) {
                     // i64 signed: valid [-9223372036854775808, 9223372036854775807]
                     // 9223372036854775808.0 is exactly 2^63 as a double
                     auto* hi_val = llvm::ConstantFP::get(src_ty, 9223372036854775808.0);
                     auto* too_high = builder.CreateFCmpOGE(val, hi_val);
                     // For f64: -9223372036854775808.0 is exactly representable, and valid
                     // For f32: -9223372036854775808.0f is also representable
                     // The lower bound: any value < -2^63 should trap.
                     // -9223372036854775808.0 (= -2^63) is the min valid value.
                     // We need val < -2^63, but -2^63 is exactly representable.
                     // So trap if val < -9223372036854775808.0
                     auto* lo_val = llvm::ConstantFP::get(src_ty, -9223372036854775808.0);
                     auto* too_low = builder.CreateFCmpOLT(val, lo_val);
                     out_of_range = builder.CreateOr(too_low, too_high);
                  } else {
                     // i64 unsigned
                     auto* hi_val = llvm::ConstantFP::get(src_ty, 18446744073709551616.0);
                     auto* lo_val = llvm::ConstantFP::get(src_ty, -1.0);
                     auto* too_high = builder.CreateFCmpOGE(val, hi_val);
                     auto* too_low = builder.CreateFCmpOLE(val, lo_val);
                     out_of_range = builder.CreateOr(too_low, too_high);
                  }
                  auto* ovf_bb = llvm::BasicBlock::Create(*ctx, "trunc_ovf", fn);
                  auto* ok_bb = llvm::BasicBlock::Create(*ctx, "trunc_ok", fn);
                  builder.CreateCondBr(out_of_range, ovf_bb, ok_bb);
                  builder.SetInsertPoint(ovf_bb);
                  builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(3)});
                  builder.CreateUnreachable();
                  builder.SetInsertPoint(ok_bb);

                  llvm::Value* result = is_signed
                     ? builder.CreateFPToSI(val, dst_ty)
                     : builder.CreateFPToUI(val, dst_ty);
                  store_vreg(inst.dest, result);
                  break;
               }

               // Float conversions from integers
               case ir_op::f32_convert_s_i32:
                  store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i32_ty), f32_ty));
                  break;
               case ir_op::f32_convert_u_i32:
                  store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i32_ty), f32_ty));
                  break;
               case ir_op::f32_convert_s_i64:
                  store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i64_ty), f32_ty));
                  break;
               case ir_op::f32_convert_u_i64:
                  store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i64_ty), f32_ty));
                  break;
               case ir_op::f64_convert_s_i32:
                  store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i32_ty), f64_ty));
                  break;
               case ir_op::f64_convert_u_i32:
                  store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i32_ty), f64_ty));
                  break;
               case ir_op::f64_convert_s_i64:
                  store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i64_ty), f64_ty));
                  break;
               case ir_op::f64_convert_u_i64:
                  store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i64_ty), f64_ty));
                  break;

               // Float promotions/demotions
               case ir_op::f32_demote_f64:
                  store_vreg(inst.dest, builder.CreateFPTrunc(load_vreg_as(inst.rr.src1, f64_ty), f32_ty));
                  break;
               case ir_op::f64_promote_f32:
                  store_vreg(inst.dest, builder.CreateFPExt(load_vreg_as(inst.rr.src1, f32_ty), f64_ty));
                  break;

               // Reinterpretations (bitcast)
               case ir_op::i32_reinterpret_f32:
                  store_vreg(inst.dest, builder.CreateBitCast(load_vreg_as(inst.rr.src1, f32_ty), i32_ty));
                  break;
               case ir_op::i64_reinterpret_f64:
                  store_vreg(inst.dest, builder.CreateBitCast(load_vreg_as(inst.rr.src1, f64_ty), i64_ty));
                  break;
               case ir_op::f32_reinterpret_i32:
                  store_vreg(inst.dest, builder.CreateBitCast(load_vreg_as(inst.rr.src1, i32_ty), f32_ty));
                  break;
               case ir_op::f64_reinterpret_i64:
                  store_vreg(inst.dest, builder.CreateBitCast(load_vreg_as(inst.rr.src1, i64_ty), f64_ty));
                  break;

               // Saturating truncations
               case ir_op::i32_trunc_sat_f32_s:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptosi_sat, {i32_ty, f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::i32_trunc_sat_f32_u:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptoui_sat, {i32_ty, f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::i32_trunc_sat_f64_s:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptosi_sat, {i32_ty, f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::i32_trunc_sat_f64_u:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptoui_sat, {i32_ty, f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::i64_trunc_sat_f32_s:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptosi_sat, {i64_ty, f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::i64_trunc_sat_f32_u:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptoui_sat, {i64_ty, f32_ty}),
                     {load_vreg_as(inst.rr.src1, f32_ty)}));
                  break;
               case ir_op::i64_trunc_sat_f64_s:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptosi_sat, {i64_ty, f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;
               case ir_op::i64_trunc_sat_f64_u:
                  store_vreg(inst.dest, builder.CreateCall(
                     llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptoui_sat, {i64_ty, f64_ty}),
                     {load_vreg_as(inst.rr.src1, f64_ty)}));
                  break;

               // ──── Sign extensions ────
               case ir_op::i32_extend8_s: {
                  auto* v = builder.CreateTrunc(load_vreg_as(inst.rr.src1, i32_ty), i8_ty);
                  store_vreg(inst.dest, builder.CreateSExt(v, i32_ty));
                  break;
               }
               case ir_op::i32_extend16_s: {
                  auto* v = builder.CreateTrunc(load_vreg_as(inst.rr.src1, i32_ty), i16_ty);
                  store_vreg(inst.dest, builder.CreateSExt(v, i32_ty));
                  break;
               }
               case ir_op::i64_extend8_s: {
                  auto* v = builder.CreateTrunc(load_vreg_as(inst.rr.src1, i64_ty), i8_ty);
                  store_vreg(inst.dest, builder.CreateSExt(v, i64_ty));
                  break;
               }
               case ir_op::i64_extend16_s: {
                  auto* v = builder.CreateTrunc(load_vreg_as(inst.rr.src1, i64_ty), i16_ty);
                  store_vreg(inst.dest, builder.CreateSExt(v, i64_ty));
                  break;
               }
               case ir_op::i64_extend32_s: {
                  auto* v = builder.CreateTrunc(load_vreg_as(inst.rr.src1, i64_ty), i32_ty);
                  store_vreg(inst.dest, builder.CreateSExt(v, i64_ty));
                  break;
               }

               // ──── Bulk memory operations ────
               case ir_op::memory_init: {
                  // call_args: [dest, src, count]; inst.ri.imm = segment index
                  if (call_args.size() >= 3) {
                     builder.CreateCall(rt_memory_init,
                        {ctx_ptr, builder.getInt32(inst.ri.imm),
                         call_args[0], call_args[1], call_args[2]});
                  }
                  call_args.clear();
                  break;
               }

               case ir_op::data_drop:
                  builder.CreateCall(rt_data_drop,
                     {ctx_ptr, builder.getInt32(inst.ri.imm)});
                  break;

               case ir_op::memory_copy: {
                  if (call_args.size() >= 3) {
                     builder.CreateCall(rt_memory_copy,
                        {ctx_ptr, call_args[0], call_args[1], call_args[2]});
                  }
                  call_args.clear();
                  break;
               }

               case ir_op::memory_fill: {
                  if (call_args.size() >= 3) {
                     builder.CreateCall(rt_memory_fill,
                        {ctx_ptr, call_args[0], call_args[1], call_args[2]});
                  }
                  call_args.clear();
                  break;
               }

               case ir_op::table_init: {
                  if (call_args.size() >= 3) {
                     builder.CreateCall(rt_table_init,
                        {ctx_ptr, builder.getInt32(inst.ri.imm),
                         call_args[0], call_args[1], call_args[2]});
                  }
                  call_args.clear();
                  break;
               }

               case ir_op::elem_drop:
                  builder.CreateCall(rt_elem_drop,
                     {ctx_ptr, builder.getInt32(inst.ri.imm)});
                  break;

               case ir_op::table_copy: {
                  if (call_args.size() >= 3) {
                     builder.CreateCall(rt_table_copy,
                        {ctx_ptr, call_args[0], call_args[1], call_args[2]});
                  }
                  call_args.clear();
                  break;
               }

               // ──── SIMD (stub) ────
               case ir_op::v128_op:
               case ir_op::const_v128:
                  // TODO: Phase 4e — SIMD support
                  break;

               default:
                  // Unknown operation — skip
                  break;
            }
         }

         // Ensure ALL blocks have terminators.
         // block_exits[0] is the function return point — return v0.
         // All other unterminated blocks are dead/unreachable.
         for (auto& bb : *fn) {
            if (bb.getTerminator()) continue;
            builder.SetInsertPoint(&bb);
            if (func.block_count > 0 && &bb == block_exits[0]) {
               // Function exit block — return merge vreg v0
               if (fn->getReturnType()->isVoidTy()) {
                  builder.CreateRetVoid();
               } else {
                  llvm::Value* ret_val = load_vreg(0);
                  if (ret_val) {
                     ret_val = convert_type(ret_val, fn->getReturnType());
                     builder.CreateRet(ret_val);
                  } else {
                     builder.CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
                  }
               }
            } else {
               // Dead/unreachable block — return null
               if (fn->getReturnType()->isVoidTy()) {
                  builder.CreateRetVoid();
               } else {
                  builder.CreateRet(llvm::Constant::getNullValue(fn->getReturnType()));
               }
            }
         }
      }

      // Generate entry wrappers for all translated functions.
      // Entry wrapper signature: i64 @wasm_entry_N(ptr %ctx, ptr %mem, ptr %args)
      // Unpacks native_value args from the array and calls the real function.
      void generate_entry_wrappers() {
         uint32_t num_imports = wasm_mod.get_imported_functions_size();
         // Entry wrapper type: i64(ptr, ptr, ptr)
         auto* entry_ty = llvm::FunctionType::get(i64_ty, {ptr_ty, ptr_ty, ptr_ty}, false);

         for (uint32_t i = num_imports; i < static_cast<uint32_t>(wasm_funcs.size()); i++) {
            auto* real_fn = wasm_funcs[i];
            if (!real_fn || real_fn->isDeclaration()) continue;

            std::string entry_name = "wasm_entry_" + std::to_string(i);
            auto* entry_fn = llvm::Function::Create(entry_ty, llvm::Function::ExternalLinkage,
                                                     entry_name, llvm_mod.get());

            auto* bb = llvm::BasicBlock::Create(*ctx, "entry", entry_fn);
            llvm::IRBuilder<> b(bb);

            auto args_it = entry_fn->arg_begin();
            llvm::Value* ctx_arg = &*args_it++;  // ptr %ctx
            llvm::Value* mem_arg = &*args_it++;  // ptr %mem
            llvm::Value* arr_arg = &*args_it++;  // ptr %args (native_value array)

            const auto& ft = wasm_mod.get_function_type(i);

            // Build call args: ctx, mem, then unpack params from args array
            std::vector<llvm::Value*> call_args;
            call_args.push_back(ctx_arg);
            call_args.push_back(mem_arg);

            for (uint32_t p = 0; p < ft.param_types.size(); p++) {
               // Each element in the args array is a native_value (8 bytes / i64)
               auto* elem_ptr = b.CreateGEP(i64_ty, arr_arg, b.getInt32(p));
               llvm::Value* raw = b.CreateLoad(i64_ty, elem_ptr);

               // Convert to the expected parameter type
               llvm::Type* param_ty = wasm_type_to_llvm(ft.param_types[p]);
               if (param_ty == i32_ty) {
                  raw = b.CreateTrunc(raw, i32_ty);
               } else if (param_ty == f32_ty) {
                  auto* as_i32 = b.CreateTrunc(raw, i32_ty);
                  raw = b.CreateBitCast(as_i32, f32_ty);
               } else if (param_ty == f64_ty) {
                  raw = b.CreateBitCast(raw, f64_ty);
               }
               // i64 stays as-is
               call_args.push_back(raw);
            }

            auto* call_result = b.CreateCall(real_fn, call_args);

            // Convert return value to i64
            if (ft.return_count == 0) {
               b.CreateRet(llvm::ConstantInt::get(i64_ty, 0));
            } else {
               llvm::Value* ret = call_result;
               llvm::Type* ret_ty = wasm_type_to_llvm(ft.return_type);
               if (ret_ty == i32_ty) {
                  ret = b.CreateZExt(ret, i64_ty);
               } else if (ret_ty == f32_ty) {
                  auto* as_i32 = b.CreateBitCast(ret, i32_ty);
                  ret = b.CreateZExt(as_i32, i64_ty);
               } else if (ret_ty == f64_ty) {
                  ret = b.CreateBitCast(ret, i64_ty);
               }
               b.CreateRet(ret);
            }
         }
      }
   };

   // ──── Public interface ────

   llvm_ir_translator::llvm_ir_translator(const module& mod, const llvm_translate_options& opts)
      : _impl(std::make_unique<impl>(mod, opts)) {}

   llvm_ir_translator::~llvm_ir_translator() = default;
   llvm_ir_translator::llvm_ir_translator(llvm_ir_translator&&) noexcept = default;
   llvm_ir_translator& llvm_ir_translator::operator=(llvm_ir_translator&&) noexcept = default;

   void llvm_ir_translator::translate_function(const ir_function& func) {
      _impl->translate(func);
   }

   void llvm_ir_translator::finalize() {
      // Generate entry wrappers for all functions
      _impl->generate_entry_wrappers();

      // Verify the module
      std::string err;
      llvm::raw_string_ostream err_stream(err);
      if (llvm::verifyModule(*_impl->llvm_mod, &err_stream)) {
         throw std::runtime_error("LLVM module verification failed: " + err);
      }

      // Run optimization passes
      if (_impl->opts.opt_level > 0) {
         llvm::PassBuilder pb;
         llvm::LoopAnalysisManager lam;
         llvm::FunctionAnalysisManager fam;
         llvm::CGSCCAnalysisManager cgam;
         llvm::ModuleAnalysisManager mam;

         pb.registerModuleAnalyses(mam);
         pb.registerCGSCCAnalyses(cgam);
         pb.registerFunctionAnalyses(fam);
         pb.registerLoopAnalyses(lam);
         pb.crossRegisterProxies(lam, fam, cgam, mam);

         if (_impl->opts.opt_level == 5) {
            // Standard LLVM pipeline (opt_level 5 = use generic O2)
            auto mpm = pb.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O2);
            mpm.run(*_impl->llvm_mod, mam);
         } else {
            // Custom WASM-tuned pipeline: only passes that matter for translated WASM IR.
            // ~10 passes vs ~60 in the generic pipeline — compiles ~45% faster with
            // identical or slightly better execution speed.
            llvm::ModulePassManager mpm;
            llvm::FunctionPassManager fpm;

            // Phase 1: Canonicalize — cheap, huge IR reduction.
            // mem2reg is critical: promotes alloca-per-register to SSA.
            // SROA breaks up the host_args alloca when it can.
            fpm.addPass(llvm::PromotePass());
            fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
            fpm.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/false));
            fpm.addPass(llvm::InstCombinePass());
            fpm.addPass(llvm::SimplifyCFGPass());

            // Phase 2: Optimize — moderate cost, good speedup.
            // Reassociate enables better constant folding.
            // GVN eliminates redundant loads (e.g., repeated ctx pointer loads).
            // LICM hoists loop-invariant computations (loop pass, needs adaptor).
            fpm.addPass(llvm::ReassociatePass());
            fpm.addPass(llvm::GVNPass());
            {
               llvm::LoopPassManager lpm;
               lpm.addPass(llvm::LICMPass(llvm::LICMOptions()));
               fpm.addPass(llvm::createFunctionToLoopPassAdaptor(std::move(lpm), /*UseMemorySSA=*/true));
            }

            // Phase 3: Cleanup — removes dead code exposed by earlier passes.
            fpm.addPass(llvm::ADCEPass());
            fpm.addPass(llvm::InstCombinePass());
            fpm.addPass(llvm::SimplifyCFGPass());

            mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
            mpm.run(*_impl->llvm_mod, mam);
         }
      }
   }

   llvm::Module* llvm_ir_translator::get_module() const {
      return _impl->llvm_mod.get();
   }

   std::unique_ptr<llvm::Module> llvm_ir_translator::take_module() {
      return std::move(_impl->llvm_mod);
   }

   std::unique_ptr<llvm::LLVMContext> llvm_ir_translator::take_context() {
      return std::move(_impl->ctx);
   }

   std::string llvm_ir_translator::dump_ir() const {
      std::string result;
      llvm::raw_string_ostream os(result);
      _impl->llvm_mod->print(os, nullptr);
      return result;
   }

} // namespace psizam
