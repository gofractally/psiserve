// LLVM IR Translator implementation
//
// Translates psizam IR instructions to LLVM IR using the LLVM C++ API.
// Uses the alloca-based approach: every virtual register gets an alloca
// in the entry block; LLVM's mem2reg pass promotes these to SSA.
//
// Phase 4b: integers, comparisons, conversions, control flow, memory,
//           locals/globals, calls, select.

#include <psizam/detail/llvm_ir_translator.hpp>
#include <psizam/detail/execution_context.hpp>
#include <psizam/exceptions.hpp>

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/MDBuilder.h>
#include <cstdlib>
#include <iostream>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

// Optimization passes — individual includes instead of PassBuilder to avoid
// pulling in all registered passes (saves ~20MB in WASM binary).
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/ADCE.h>
#include <llvm/Transforms/Scalar/EarlyCSE.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/Transforms/Scalar/LICM.h>
#include <llvm/Transforms/Scalar/Reassociate.h>
#include <llvm/Transforms/Scalar/SimplifyCFG.h>
#include <llvm/Transforms/Scalar/SROA.h>
#include <llvm/Transforms/Utils/Mem2Reg.h>

// Analysis passes — only what our 10 passes actually need.
// PassBuilder::registerFunctionAnalyses() registers ALL 36+ analyses from
// PassRegistry.def, keeping ~20MB of IPO/Vectorize/Coroutines/etc. alive.
// Manual registration of just these 13 analyses cuts the dependency graph.
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/BasicAliasAnalysis.h>
#include <llvm/Analysis/BlockFrequencyInfo.h>
#include <llvm/Analysis/CGSCCPassManager.h>
#include <llvm/Analysis/GlobalsModRef.h>
#include <llvm/Analysis/LastRunTrackingAnalysis.h>
#include <llvm/Analysis/LoopAnalysisManager.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/MemoryDependenceAnalysis.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/Analysis/OptimizationRemarkEmitter.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/TargetTransformInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Transforms/Scalar/LoopPassManager.h>

#include <cassert>
#include <climits>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace psizam::detail {

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
      llvm::Type* i128_ty   = nullptr;
      // SIMD vector types
      llvm::VectorType* v128_ty    = nullptr; // <2 x i64> — storage type
      llvm::VectorType* v16xi8_ty  = nullptr;
      llvm::VectorType* v8xi16_ty  = nullptr;
      llvm::VectorType* v4xi32_ty  = nullptr;
      llvm::VectorType* v2xi64_ty  = nullptr;
      llvm::VectorType* v4xf32_ty  = nullptr;
      llvm::VectorType* v2xf64_ty  = nullptr;

      // WASM function declarations (indexed by function index). Lazy: entry
      // is null until `get_or_declare_wasm_func(i)` creates the declaration.
      std::vector<llvm::Function*> wasm_funcs;

      // Function indices actually translated in this module (in translation
      // order). Used by generate_entry_wrappers to avoid iterating over
      // unused external declarations.
      std::vector<uint32_t> translated_funcs;

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
         i128_ty = llvm::Type::getInt128Ty(*ctx);
         v128_ty   = llvm::FixedVectorType::get(i64_ty, 2);
         v16xi8_ty = llvm::FixedVectorType::get(i8_ty, 16);
         v8xi16_ty = llvm::FixedVectorType::get(i16_ty, 8);
         v4xi32_ty = llvm::FixedVectorType::get(i32_ty, 4);
         v2xi64_ty = llvm::FixedVectorType::get(i64_ty, 2);
         v4xf32_ty = llvm::FixedVectorType::get(f32_ty, 4);
         v2xf64_ty = llvm::FixedVectorType::get(f64_ty, 2);

         // Pre-declare all WASM functions
         declare_functions();
         // Declare runtime helper functions
         declare_runtime_helpers();
         // Softfloat helpers are used in both softfloat and hw_deterministic
         // modes. hw_deterministic mode calls the softfloat helpers for
         // NaN-payload-sensitive ops (f64_promote_f32, f32_demote_f64) to match
         // the interpreter's bit-exact behavior — see interpret_visitor.hpp
         // which calls _psizam_f32_promote whenever fp != fast. Only `fast`
         // mode skips softfloat entirely and uses native HW ops.
         if (opts.fp != fp_mode::fast) {
            declare_softfloat_helpers();
         }
         if (opts.enable_backtrace) {
            frameaddress_intrinsic = llvm::Intrinsic::getOrInsertDeclaration(
               llvm_mod.get(), llvm::Intrinsic::frameaddress, {ptr_ty});
         }
      }

      // Runtime helper function declarations
      llvm::Function* rt_global_get       = nullptr;
      llvm::Function* rt_global_set       = nullptr;
      llvm::Function* rt_global_get_v128  = nullptr;
      llvm::Function* rt_global_set_v128  = nullptr;
      llvm::Function* rt_memory_size   = nullptr;
      llvm::Function* rt_memory_grow   = nullptr;
      llvm::Function* rt_call_host     = nullptr;
      llvm::Function* rt_call_host_full = nullptr; // Combined: depth_dec + call + depth_inc + get_memory
      llvm::Function* rt_memory_init   = nullptr;
      llvm::Function* rt_data_drop     = nullptr;
      llvm::Function* rt_memory_copy   = nullptr;
      llvm::Function* rt_memory_fill   = nullptr;
      llvm::Function* rt_table_init    = nullptr;
      llvm::Function* rt_elem_drop     = nullptr;
      llvm::Function* rt_table_copy    = nullptr;
      llvm::Function* rt_call_indirect = nullptr;
      llvm::Function* rt_table_get     = nullptr;
      llvm::Function* rt_table_set     = nullptr;
      llvm::Function* rt_table_grow    = nullptr;
      llvm::Function* rt_table_size    = nullptr;
      llvm::Function* rt_table_fill    = nullptr;
      llvm::Function* rt_trap           = nullptr;
      llvm::Function* rt_call_depth_dec = nullptr;
      llvm::Function* rt_gas_charge = nullptr;
      llvm::Function* rt_gas_exhausted_check = nullptr;
      llvm::Function* rt_call_depth_inc = nullptr;
      llvm::Function* rt_get_memory     = nullptr;

      // Exception handling runtime helpers
      llvm::Function* rt_eh_enter       = nullptr;
      llvm::Function* rt_eh_leave       = nullptr;
      llvm::Function* rt_eh_throw       = nullptr;
      llvm::Function* rt_eh_throw_ref   = nullptr;
      llvm::Function* rt_eh_get_match   = nullptr;
      llvm::Function* rt_eh_get_payload = nullptr;
      llvm::Function* rt_eh_get_exnref  = nullptr;
      llvm::Function* rt_setjmp         = nullptr;

      // Backtrace intrinsic
      llvm::Function* frameaddress_intrinsic = nullptr;

      // Softfloat function declarations (only populated when (opts.fp == fp_mode::softfloat))
      // f32 arithmetic: i32(i32, i32)
      llvm::Function* sf_f32_add = nullptr;
      llvm::Function* sf_f32_sub = nullptr;
      llvm::Function* sf_f32_mul = nullptr;
      llvm::Function* sf_f32_div = nullptr;
      llvm::Function* sf_f32_min = nullptr;
      llvm::Function* sf_f32_max = nullptr;
      llvm::Function* sf_f32_copysign = nullptr;
      // f32 unary: i32(i32)
      llvm::Function* sf_f32_abs = nullptr;
      llvm::Function* sf_f32_neg = nullptr;
      llvm::Function* sf_f32_sqrt = nullptr;
      llvm::Function* sf_f32_ceil = nullptr;
      llvm::Function* sf_f32_floor = nullptr;
      llvm::Function* sf_f32_trunc = nullptr;
      llvm::Function* sf_f32_nearest = nullptr;
      // f64 arithmetic: i64(i64, i64)
      llvm::Function* sf_f64_add = nullptr;
      llvm::Function* sf_f64_sub = nullptr;
      llvm::Function* sf_f64_mul = nullptr;
      llvm::Function* sf_f64_div = nullptr;
      llvm::Function* sf_f64_min = nullptr;
      llvm::Function* sf_f64_max = nullptr;
      llvm::Function* sf_f64_copysign = nullptr;
      // f64 unary: i64(i64)
      llvm::Function* sf_f64_abs = nullptr;
      llvm::Function* sf_f64_neg = nullptr;
      llvm::Function* sf_f64_sqrt = nullptr;
      llvm::Function* sf_f64_ceil = nullptr;
      llvm::Function* sf_f64_floor = nullptr;
      llvm::Function* sf_f64_trunc = nullptr;
      llvm::Function* sf_f64_nearest = nullptr;
      // Conversions
      llvm::Function* sf_f32_convert_i32s = nullptr;
      llvm::Function* sf_f32_convert_i32u = nullptr;
      llvm::Function* sf_f32_convert_i64s = nullptr;
      llvm::Function* sf_f32_convert_i64u = nullptr;
      llvm::Function* sf_f64_convert_i32s = nullptr;
      llvm::Function* sf_f64_convert_i32u = nullptr;
      llvm::Function* sf_f64_convert_i64s = nullptr;
      llvm::Function* sf_f64_convert_i64u = nullptr;
      llvm::Function* sf_f32_demote_f64 = nullptr;
      llvm::Function* sf_f64_promote_f32 = nullptr;

      // gas_state redesign — universal memory-deadline shape. Used at
      // function entry and at every loop header so each iteration pays
      // gas. Leaves the builder positioned at gas_done so callers
      // resume from a well-defined point.
      //
      //   consumed = *(ctx + consumed_off)     ; plain u64 load
      //   consumed += cost
      //   *(ctx + consumed_off) = consumed
      //   deadline = *(ctx + deadline_off)     ; relaxed atomic load (plain u64 on x86/arm)
      //   if (consumed < deadline) goto done
      //   call __psizam_gas_exhausted_check(ctx)
      //   done:
      void emit_gas_prologue_check(llvm::IRBuilder<>& builder,
                                   llvm::Value* ctx_ptr, int64_t cost) {
         auto* fn = builder.GetInsertBlock()->getParent();
         const std::size_t consumed_off = detail::jit_execution_context<false>::gas_consumed_offset();
         const std::size_t deadline_off = detail::jit_execution_context<false>::gas_deadline_offset();

         auto* gas_trap_bb = llvm::BasicBlock::Create(*ctx, "gas_trap", fn);
         auto* gas_done_bb = llvm::BasicBlock::Create(*ctx, "gas_done", fn);

         auto* consumed_gep = builder.CreateConstGEP1_64(i8_ty, ctx_ptr, consumed_off, "consumed_ptr");
         auto* cur = builder.CreateLoad(i64_ty, consumed_gep, "consumed_cur");
         auto* next = builder.CreateAdd(cur,
            llvm::ConstantInt::get(i64_ty, cost, /*signed*/true), "consumed_next");
         builder.CreateStore(next, consumed_gep);

         auto* deadline_gep = builder.CreateConstGEP1_64(i8_ty, ctx_ptr, deadline_off, "deadline_ptr");
         auto* deadline_val = builder.CreateLoad(i64_ty, deadline_gep, "deadline");
         // Unsigned compare: consumed < deadline → skip (both are u64).
         auto* ok = builder.CreateICmpULT(next, deadline_val, "gas_ok");
         builder.CreateCondBr(ok, gas_done_bb, gas_trap_bb);

         builder.SetInsertPoint(gas_trap_bb);
         builder.CreateCall(rt_gas_exhausted_check, {ctx_ptr});
         builder.CreateBr(gas_done_bb);

         builder.SetInsertPoint(gas_done_bb);
      }

      // ── Checked memory mode helpers ──

      bool is_checked() const { return opts.mem_mode == mem_safety::checked; }
      bool is_checked_strict() const { return opts.checked_kind == checked_mode::strict; }

      // Emit watermark update for a read access. eff_addr is the i64 effective
      // address (already zext'd), access_size is the number of bytes read.
      // watermark_alloca must be a valid i64 alloca.
      void emit_read_watermark_update(llvm::IRBuilder<>& builder,
                                      llvm::Value* eff_addr, uint32_t access_size,
                                      llvm::AllocaInst* watermark_alloca) {
         if (!is_checked() || !watermark_alloca) return;
         llvm::Value* end = eff_addr;
         if (access_size != 0) {
            end = builder.CreateAdd(eff_addr,
               builder.getInt64(static_cast<uint64_t>(access_size)), "read_end");
         }
         llvm::Value* cur = builder.CreateLoad(i64_ty, watermark_alloca, "wm_cur");
         if (is_checked_strict()) {
            auto* umax_fn = llvm::Intrinsic::getOrInsertDeclaration(
               builder.GetInsertBlock()->getModule(), llvm::Intrinsic::umax, {i64_ty});
            llvm::Value* new_wm = builder.CreateCall(umax_fn, {cur, end}, "wm_max");
            builder.CreateStore(new_wm, watermark_alloca);
         } else {
            llvm::Value* new_wm = builder.CreateOr(cur, end, "wm_or");
            builder.CreateStore(new_wm, watermark_alloca);
         }
      }

      // Direct per-read bounds check (alternative to deferred watermark).
      // Same pattern as write bounds check but for loads.
      void emit_read_bounds_check(llvm::IRBuilder<>& builder,
                                  llvm::Value* ctx_ptr, llvm::Value* mem_ptr_val,
                                  llvm::Value* eff_addr, uint32_t access_size,
                                  llvm::AllocaInst* mem_size_cache = nullptr) {
         if (!is_checked()) return;
         auto* fn = builder.GetInsertBlock()->getParent();
         llvm::Value* end = builder.CreateAdd(eff_addr,
            builder.getInt64(static_cast<uint64_t>(access_size)), "read_end");
         llvm::Value* mem_size;
         if (mem_size_cache) {
            mem_size = builder.CreateLoad(i64_ty, mem_size_cache, "mem_size");
         } else {
            const int32_t ms_off = wasm_allocator::mem_size_offset();
            llvm::Value* ms_ptr = builder.CreateConstGEP1_64(i8_ty, mem_ptr_val,
               static_cast<int64_t>(ms_off), "mem_size_ptr");
            mem_size = builder.CreateLoad(i64_ty, ms_ptr, "mem_size");
         }
         llvm::Value* oob = builder.CreateICmpUGT(end, mem_size, "read_oob");

         auto* trap_bb = llvm::BasicBlock::Create(*ctx, "load_oob_trap", fn);
         auto* ok_bb   = llvm::BasicBlock::Create(*ctx, "load_ok", fn);
         auto* br = builder.CreateCondBr(oob, trap_bb, ok_bb);
         br->setMetadata(llvm::LLVMContext::MD_prof,
            llvm::MDBuilder(*ctx).createBranchWeights(1, 10000));

         builder.SetInsertPoint(trap_bb);
         builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(5)});
         builder.CreateUnreachable();

         builder.SetInsertPoint(ok_bb);
      }

      // Emit immediate bounds check for a write access. eff_addr is the i64
      // effective address, access_size is the number of bytes written.
      // Traps with trap_code 5 (memory OOB) if out of bounds.
      void emit_write_bounds_check(llvm::IRBuilder<>& builder,
                                   llvm::Value* ctx_ptr, llvm::Value* mem_ptr_val,
                                   llvm::Value* eff_addr, uint32_t access_size,
                                   llvm::AllocaInst* mem_size_cache = nullptr) {
         if (!is_checked()) return;
         auto* fn = builder.GetInsertBlock()->getParent();
         llvm::Value* end = builder.CreateAdd(eff_addr,
            builder.getInt64(static_cast<uint64_t>(access_size)), "write_end");
         llvm::Value* mem_size;
         if (mem_size_cache) {
            mem_size = builder.CreateLoad(i64_ty, mem_size_cache, "mem_size");
         } else {
            const int32_t ms_off = wasm_allocator::mem_size_offset();
            llvm::Value* ms_ptr = builder.CreateConstGEP1_64(i8_ty, mem_ptr_val,
               static_cast<int64_t>(ms_off), "mem_size_ptr");
            mem_size = builder.CreateLoad(i64_ty, ms_ptr, "mem_size");
         }
         llvm::Value* oob = builder.CreateICmpUGT(end, mem_size, "write_oob");

         auto* trap_bb = llvm::BasicBlock::Create(*ctx, "store_oob_trap", fn);
         auto* ok_bb   = llvm::BasicBlock::Create(*ctx, "store_ok", fn);
         auto* br = builder.CreateCondBr(oob, trap_bb, ok_bb);
         // Mark OOB as extremely unlikely so LLVM moves trap BBs to cold sections
         auto* weights = llvm::MDBuilder(*ctx).createBranchWeights(1, 10000);
         br->setMetadata(llvm::LLVMContext::MD_prof, weights);

         builder.SetInsertPoint(trap_bb);
         builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(5)});
         builder.CreateUnreachable();

         builder.SetInsertPoint(ok_bb);
      }

      // Emit watermark validation: if watermark > mem_size, trap.
      // Resets watermark to 0 afterwards. One branch (always predicted not-taken).
      void emit_watermark_validate(llvm::IRBuilder<>& builder,
                                   llvm::Value* ctx_ptr, llvm::Value* mem_ptr_val,
                                   llvm::AllocaInst* watermark_alloca,
                                   llvm::AllocaInst* mem_size_cache = nullptr) {
         if (!is_checked() || !watermark_alloca) return;
         auto* fn = builder.GetInsertBlock()->getParent();
         llvm::Value* wm = builder.CreateLoad(i64_ty, watermark_alloca, "wm_val");

         llvm::Value* mem_size;
         if (mem_size_cache) {
            mem_size = builder.CreateLoad(i64_ty, mem_size_cache, "mem_size");
         } else {
            const int32_t ms_off = wasm_allocator::mem_size_offset();
            llvm::Value* ms_ptr = builder.CreateConstGEP1_64(i8_ty, mem_ptr_val,
               static_cast<int64_t>(ms_off), "wm_ms_ptr");
            mem_size = builder.CreateLoad(i64_ty, ms_ptr, "wm_ms");
         }
         llvm::Value* oob = builder.CreateICmpUGT(wm, mem_size, "wm_oob");

         auto* trap_bb = llvm::BasicBlock::Create(*ctx, "wm_trap", fn);
         auto* ok_bb   = llvm::BasicBlock::Create(*ctx, "wm_ok", fn);
         auto* br = builder.CreateCondBr(oob, trap_bb, ok_bb);
         br->setMetadata(llvm::LLVMContext::MD_prof,
            llvm::MDBuilder(*ctx).createBranchWeights(1, 10000));

         builder.SetInsertPoint(trap_bb);
         builder.CreateCall(rt_trap, {ctx_ptr, builder.getInt32(5)});
         builder.CreateUnreachable();

         builder.SetInsertPoint(ok_bb);
         builder.CreateStore(builder.getInt64(0), watermark_alloca);
      }

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

         // void __psizam_global_get_v128(void* ctx, uint32_t idx, void* out)
         rt_global_get_v128 = decl("__psizam_global_get_v128",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, ptr_ty}, false));

         // void __psizam_global_set_v128(void* ctx, uint32_t idx, const void* in)
         rt_global_set_v128 = decl("__psizam_global_set_v128",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, ptr_ty}, false));

         // int32_t __psizam_memory_size(void* ctx)
         rt_memory_size = decl("__psizam_memory_size",
            llvm::FunctionType::get(i32_ty, {ptr_ty}, false));

         // int32_t __psizam_memory_grow(void* ctx, int32_t pages, void** new_mem_out)
         rt_memory_grow = decl("__psizam_memory_grow",
            llvm::FunctionType::get(i32_ty, {ptr_ty, i32_ty, ptr_ty}, false));

         // int64_t __psizam_call_host[_nothrow](void* ctx, uint32_t func_idx, void* args_buf, uint32_t nargs)
         rt_call_host = decl(opts.nothrow_host_calls ? "__psizam_call_host_nothrow" : "__psizam_call_host",
            llvm::FunctionType::get(i64_ty, {ptr_ty, i32_ty, ptr_ty, i32_ty}, false));

         // int64_t __psizam_call_host_full(void* ctx, uint32_t func_idx, void* args, uint32_t nargs, void** mem_out)
         // Combined: depth_dec + call + depth_inc + memory reload in one extern call
         if (opts.nothrow_host_calls) {
            rt_call_host_full = decl("__psizam_call_host_full",
               llvm::FunctionType::get(i64_ty, {ptr_ty, i32_ty, ptr_ty, i32_ty, ptr_ty}, false));
         }

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

         // void __psizam_table_init(void* ctx, uint32_t elem, uint32_t dest, uint32_t src, uint32_t n,
         //                          uint32_t table_idx)
         rt_table_init = decl("__psizam_table_init",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty, i32_ty, i32_ty}, false));

         // void __psizam_elem_drop(void* ctx, uint32_t seg)
         rt_elem_drop = decl("__psizam_elem_drop",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty}, false));

         // void __psizam_table_copy(void* ctx, uint32_t dest, uint32_t src, uint32_t n,
         //                          uint32_t dst_table, uint32_t src_table)
         rt_table_copy = decl("__psizam_table_copy",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty, i32_ty, i32_ty}, false));

         // int64_t __psizam_call_indirect[_nothrow](void* ctx, void* mem, uint32_t type_idx,
         //                                         uint32_t table_elem, void* args_buf, uint32_t nargs)
         rt_call_indirect = decl(opts.nothrow_host_calls ? "__psizam_call_indirect_nothrow" : "__psizam_call_indirect",
            llvm::FunctionType::get(i64_ty, {ptr_ty, ptr_ty, i32_ty, i32_ty, ptr_ty, i32_ty}, false));

         // uint32_t __psizam_table_get(void* ctx, uint32_t table_idx, uint32_t elem_idx)
         rt_table_get = decl("__psizam_table_get",
            llvm::FunctionType::get(i32_ty, {ptr_ty, i32_ty, i32_ty}, false));

         // void __psizam_table_set(void* ctx, uint32_t table_idx, uint32_t elem_idx, uint32_t val)
         rt_table_set = decl("__psizam_table_set",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty}, false));

         // uint32_t __psizam_table_grow(void* ctx, uint32_t table_idx, uint32_t delta, uint32_t init_val)
         rt_table_grow = decl("__psizam_table_grow",
            llvm::FunctionType::get(i32_ty, {ptr_ty, i32_ty, i32_ty, i32_ty}, false));

         // uint32_t __psizam_table_size(void* ctx, uint32_t table_idx)
         rt_table_size = decl("__psizam_table_size",
            llvm::FunctionType::get(i32_ty, {ptr_ty, i32_ty}, false));

         // void __psizam_table_fill(void* ctx, uint32_t table_idx, uint32_t i, uint32_t val, uint32_t n)
         rt_table_fill = decl("__psizam_table_fill",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, i32_ty, i32_ty, i32_ty}, false));

         // void __psizam_trap(void* ctx, uint32_t trap_code)
         rt_trap = decl("__psizam_trap",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty}, false));
         rt_trap->setDoesNotReturn();

         // void __psizam_call_depth_dec(void* ctx) — decrements and traps on overflow
         rt_call_depth_dec = decl("__psizam_call_depth_dec",
            llvm::FunctionType::get(void_ty, {ptr_ty}, false));

         // void __psizam_call_depth_inc(void* ctx) — increments on return
         rt_call_depth_inc = decl("__psizam_call_depth_inc",
            llvm::FunctionType::get(void_ty, {ptr_ty}, false));

         // void __psizam_gas_charge(void* ctx, int64_t cost) — Phase 2a
         // placeholder call at every function entry. Helper early-returns
         // when the context's strategy is off.
         rt_gas_charge = decl("__psizam_gas_charge",
            llvm::FunctionType::get(void_ty, {ptr_ty, i64_ty}, false));

         // void __psizam_gas_exhausted_check(void* ctx) — Phase 3 slow
         // path for the inline-decrement version. Called after the JIT
         // has already decremented the counter into negative territory.
         rt_gas_exhausted_check = decl("__psizam_gas_exhausted_check",
            llvm::FunctionType::get(void_ty, {ptr_ty}, false));

         // void* __psizam_get_memory(void* ctx) — returns current linear memory base
         rt_get_memory = decl("__psizam_get_memory",
            llvm::FunctionType::get(ptr_ty, {ptr_ty}, false));

         // ── Exception Handling runtime helpers ──

         // void* __psizam_eh_enter(void* ctx, uint32_t catch_count, const uint64_t* catch_data)
         rt_eh_enter = decl("__psizam_eh_enter",
            llvm::FunctionType::get(ptr_ty, {ptr_ty, i32_ty, ptr_ty}, false));

         // void __psizam_eh_leave(void* ctx)
         rt_eh_leave = decl("__psizam_eh_leave",
            llvm::FunctionType::get(void_ty, {ptr_ty}, false));

         // void __psizam_eh_throw(void* ctx, uint32_t tag_index, const uint64_t* payload, uint32_t count)
         rt_eh_throw = decl("__psizam_eh_throw",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty, ptr_ty, i32_ty}, false));
         rt_eh_throw->setDoesNotReturn();

         // void __psizam_eh_throw_ref(void* ctx, uint32_t exnref_idx)
         rt_eh_throw_ref = decl("__psizam_eh_throw_ref",
            llvm::FunctionType::get(void_ty, {ptr_ty, i32_ty}, false));
         rt_eh_throw_ref->setDoesNotReturn();

         // uint32_t __psizam_eh_get_match(void* ctx)
         rt_eh_get_match = decl("__psizam_eh_get_match",
            llvm::FunctionType::get(i32_ty, {ptr_ty}, false));

         // uint64_t __psizam_eh_get_payload(void* ctx, uint32_t index)
         rt_eh_get_payload = decl("__psizam_eh_get_payload",
            llvm::FunctionType::get(i64_ty, {ptr_ty, i32_ty}, false));

         // uint64_t __psizam_eh_get_exnref(void* ctx)
         rt_eh_get_exnref = decl("__psizam_eh_get_exnref",
            llvm::FunctionType::get(i64_ty, {ptr_ty}, false));

         // int __psizam_setjmp(void* jmpbuf) — wrapper for setjmp
         rt_setjmp = decl("__psizam_setjmp",
            llvm::FunctionType::get(i32_ty, {ptr_ty}, false));
         rt_setjmp->addFnAttr(llvm::Attribute::ReturnsTwice);
      }

      void declare_softfloat_helpers() {
         auto decl = [&](const char* name, llvm::FunctionType* ty) -> llvm::Function* {
            return llvm::Function::Create(ty, llvm::Function::ExternalLinkage,
                                           name, llvm_mod.get());
         };

         // Match the C implementation signatures (softfloat.hpp) so LLVM lowers
         // arguments/returns via the hardware float ABI that matches the registered
         // C function. Using integer types here would route values through the
         // wrong register class (int regs vs. SSE/NEON regs) on x86_64 SysV and
         // ARM64 AAPCS, silently corrupting every softfloat call.
         auto f32_binop_ty = llvm::FunctionType::get(f32_ty, {f32_ty, f32_ty}, false);
         auto f32_unop_ty  = llvm::FunctionType::get(f32_ty, {f32_ty}, false);
         auto f64_binop_ty = llvm::FunctionType::get(f64_ty, {f64_ty, f64_ty}, false);
         auto f64_unop_ty  = llvm::FunctionType::get(f64_ty, {f64_ty}, false);

         sf_f32_add = decl("__psizam_sf_f32_add", f32_binop_ty);
         sf_f32_sub = decl("__psizam_sf_f32_sub", f32_binop_ty);
         sf_f32_mul = decl("__psizam_sf_f32_mul", f32_binop_ty);
         sf_f32_div = decl("__psizam_sf_f32_div", f32_binop_ty);
         sf_f32_min = decl("__psizam_sf_f32_min", f32_binop_ty);
         sf_f32_max = decl("__psizam_sf_f32_max", f32_binop_ty);
         sf_f32_copysign = decl("__psizam_sf_f32_copysign", f32_binop_ty);

         sf_f32_abs     = decl("__psizam_sf_f32_abs", f32_unop_ty);
         sf_f32_neg     = decl("__psizam_sf_f32_neg", f32_unop_ty);
         sf_f32_sqrt    = decl("__psizam_sf_f32_sqrt", f32_unop_ty);
         sf_f32_ceil    = decl("__psizam_sf_f32_ceil", f32_unop_ty);
         sf_f32_floor   = decl("__psizam_sf_f32_floor", f32_unop_ty);
         sf_f32_trunc   = decl("__psizam_sf_f32_trunc", f32_unop_ty);
         sf_f32_nearest = decl("__psizam_sf_f32_nearest", f32_unop_ty);

         sf_f64_add = decl("__psizam_sf_f64_add", f64_binop_ty);
         sf_f64_sub = decl("__psizam_sf_f64_sub", f64_binop_ty);
         sf_f64_mul = decl("__psizam_sf_f64_mul", f64_binop_ty);
         sf_f64_div = decl("__psizam_sf_f64_div", f64_binop_ty);
         sf_f64_min = decl("__psizam_sf_f64_min", f64_binop_ty);
         sf_f64_max = decl("__psizam_sf_f64_max", f64_binop_ty);
         sf_f64_copysign = decl("__psizam_sf_f64_copysign", f64_binop_ty);

         sf_f64_abs     = decl("__psizam_sf_f64_abs", f64_unop_ty);
         sf_f64_neg     = decl("__psizam_sf_f64_neg", f64_unop_ty);
         sf_f64_sqrt    = decl("__psizam_sf_f64_sqrt", f64_unop_ty);
         sf_f64_ceil    = decl("__psizam_sf_f64_ceil", f64_unop_ty);
         sf_f64_floor   = decl("__psizam_sf_f64_floor", f64_unop_ty);
         sf_f64_trunc   = decl("__psizam_sf_f64_trunc", f64_unop_ty);
         sf_f64_nearest = decl("__psizam_sf_f64_nearest", f64_unop_ty);

         // Conversions: int -> float. Return type is float/double to match
         // the registered C implementations (see softfloat.hpp).
         sf_f32_convert_i32s = decl("__psizam_sf_f32_convert_i32s",
            llvm::FunctionType::get(f32_ty, {i32_ty}, false));
         sf_f32_convert_i32u = decl("__psizam_sf_f32_convert_i32u",
            llvm::FunctionType::get(f32_ty, {i32_ty}, false));
         sf_f32_convert_i64s = decl("__psizam_sf_f32_convert_i64s",
            llvm::FunctionType::get(f32_ty, {i64_ty}, false));
         sf_f32_convert_i64u = decl("__psizam_sf_f32_convert_i64u",
            llvm::FunctionType::get(f32_ty, {i64_ty}, false));
         sf_f64_convert_i32s = decl("__psizam_sf_f64_convert_i32s",
            llvm::FunctionType::get(f64_ty, {i32_ty}, false));
         sf_f64_convert_i32u = decl("__psizam_sf_f64_convert_i32u",
            llvm::FunctionType::get(f64_ty, {i32_ty}, false));
         sf_f64_convert_i64s = decl("__psizam_sf_f64_convert_i64s",
            llvm::FunctionType::get(f64_ty, {i64_ty}, false));
         sf_f64_convert_i64u = decl("__psizam_sf_f64_convert_i64u",
            llvm::FunctionType::get(f64_ty, {i64_ty}, false));
         // float -> float
         sf_f32_demote_f64  = decl("__psizam_sf_f32_demote_f64",
            llvm::FunctionType::get(f32_ty, {f64_ty}, false));
         sf_f64_promote_f32 = decl("__psizam_sf_f64_promote_f32",
            llvm::FunctionType::get(f64_ty, {f32_ty}, false));
      }

      // Helpers: coerce arbitrary vreg values to a float/double so they match
      // the softfloat helper's float ABI declaration. If the caller passed a
      // value of a different width (can happen with unreachable-code uses where
      // load_vreg_as returns a zero of the target type), convert it first.
      llvm::Value* to_f32(llvm::IRBuilder<>& builder, llvm::Value* a) {
         auto* t = a->getType();
         if (t == f32_ty) return a;
         if (t == i32_ty) return builder.CreateBitCast(a, f32_ty);
         if (t == f64_ty) return builder.CreateFPTrunc(a, f32_ty);
         if (t == i64_ty) return builder.CreateBitCast(builder.CreateTrunc(a, i32_ty), f32_ty);
         return llvm::Constant::getNullValue(f32_ty);
      }
      llvm::Value* to_f64(llvm::IRBuilder<>& builder, llvm::Value* a) {
         auto* t = a->getType();
         if (t == f64_ty) return a;
         if (t == i64_ty) return builder.CreateBitCast(a, f64_ty);
         if (t == f32_ty) return builder.CreateFPExt(a, f64_ty);
         if (t == i32_ty) return builder.CreateBitCast(builder.CreateZExt(a, i64_ty), f64_ty);
         return llvm::Constant::getNullValue(f64_ty);
      }
      llvm::Value* call_sf_f32_binop(llvm::IRBuilder<>& builder, llvm::Function* fn, llvm::Value* a, llvm::Value* b) {
         return builder.CreateCall(fn, {to_f32(builder, a), to_f32(builder, b)});
      }
      llvm::Value* call_sf_f32_unop(llvm::IRBuilder<>& builder, llvm::Function* fn, llvm::Value* a) {
         return builder.CreateCall(fn, {to_f32(builder, a)});
      }
      llvm::Value* call_sf_f64_binop(llvm::IRBuilder<>& builder, llvm::Function* fn, llvm::Value* a, llvm::Value* b) {
         return builder.CreateCall(fn, {to_f64(builder, a), to_f64(builder, b)});
      }
      llvm::Value* call_sf_f64_unop(llvm::IRBuilder<>& builder, llvm::Function* fn, llvm::Value* a) {
         return builder.CreateCall(fn, {to_f64(builder, a)});
      }

      // hw_deterministic NaN canonicalization — replace any NaN result with the WASM
      // canonical NaN (f32: 0x7FC00000, f64: 0x7FF8000000000000). Applied only in
      // hw_deterministic mode, and only to ops that can produce NaN. Guarantees
      // cross-platform bit-identical results for NaN-producing inputs.
      llvm::Value* canonicalize_f32(llvm::IRBuilder<>& builder, llvm::Value* v) {
         auto* is_nan = builder.CreateFCmpUNO(v, v);
         auto* canon_i = builder.getInt32(0x7FC00000);
         auto* canon_f = builder.CreateBitCast(canon_i, f32_ty);
         return builder.CreateSelect(is_nan, canon_f, v);
      }
      llvm::Value* canonicalize_f64(llvm::IRBuilder<>& builder, llvm::Value* v) {
         auto* is_nan = builder.CreateFCmpUNO(v, v);
         auto* canon_i = builder.getInt64(0x7FF8000000000000ULL);
         auto* canon_f = builder.CreateBitCast(canon_i, f64_ty);
         return builder.CreateSelect(is_nan, canon_f, v);
      }
      llvm::Value* canonicalize_v4xf32(llvm::IRBuilder<>& builder, llvm::Value* v) {
         auto* is_nan = builder.CreateFCmpUNO(v, v);
         auto* canon = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(4),
                         llvm::ConstantFP::get(f32_ty,
                            llvm::APFloat(llvm::APFloat::IEEEsingle(),
                               llvm::APInt(32, 0x7FC00000))));
         return builder.CreateSelect(is_nan, canon, v);
      }
      llvm::Value* canonicalize_v2xf64(llvm::IRBuilder<>& builder, llvm::Value* v) {
         auto* is_nan = builder.CreateFCmpUNO(v, v);
         auto* canon = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(2),
                         llvm::ConstantFP::get(f64_ty,
                            llvm::APFloat(llvm::APFloat::IEEEdouble(),
                               llvm::APInt(64, 0x7FF8000000000000ULL))));
         return builder.CreateSelect(is_nan, canon, v);
      }
      // Conditional canonicalization: applied only in hw_deterministic mode.
      // In fast mode, returns v unchanged (native HW NaN — may vary across platforms).
      // In softfloat mode, the value came from a softfloat helper which already
      // produces a WASM-spec arithmetic NaN, so no extra canonicalization needed.
      llvm::Value* maybe_canon_f32(llvm::IRBuilder<>& builder, llvm::Value* v) {
         return (opts.fp == fp_mode::hw_deterministic) ? canonicalize_f32(builder, v) : v;
      }
      llvm::Value* maybe_canon_f64(llvm::IRBuilder<>& builder, llvm::Value* v) {
         return (opts.fp == fp_mode::hw_deterministic) ? canonicalize_f64(builder, v) : v;
      }
      llvm::Value* maybe_canon_v4xf32(llvm::IRBuilder<>& builder, llvm::Value* v) {
         return (opts.fp == fp_mode::hw_deterministic) ? canonicalize_v4xf32(builder, v) : v;
      }
      llvm::Value* maybe_canon_v2xf64(llvm::IRBuilder<>& builder, llvm::Value* v) {
         return (opts.fp == fp_mode::hw_deterministic) ? canonicalize_v2xf64(builder, v) : v;
      }

      // SIMD softfloat scalarization — extract each lane, call scalar sf_*, insert back.
      // Only used when opts.fp == fp_mode::softfloat. Slow (lane-by-lane host calls)
      // but guarantees bit-identical results to scalar softfloat backends.
      llvm::Value* simd_sf_f32x4_binop(llvm::IRBuilder<>& builder, llvm::Function* fn,
                                       llvm::Value* a, llvm::Value* b) {
         llvm::Value* result = llvm::UndefValue::get(v4xf32_ty);
         for (int j = 0; j < 4; ++j) {
            auto* idx = builder.getInt32(j);
            auto* ae  = builder.CreateExtractElement(a, idx);
            auto* be  = builder.CreateExtractElement(b, idx);
            result = builder.CreateInsertElement(result,
                       call_sf_f32_binop(builder, fn, ae, be), idx);
         }
         return result;
      }
      llvm::Value* simd_sf_f32x4_unop(llvm::IRBuilder<>& builder, llvm::Function* fn,
                                      llvm::Value* a) {
         llvm::Value* result = llvm::UndefValue::get(v4xf32_ty);
         for (int j = 0; j < 4; ++j) {
            auto* idx = builder.getInt32(j);
            auto* ae  = builder.CreateExtractElement(a, idx);
            result = builder.CreateInsertElement(result,
                       call_sf_f32_unop(builder, fn, ae), idx);
         }
         return result;
      }
      llvm::Value* simd_sf_f64x2_binop(llvm::IRBuilder<>& builder, llvm::Function* fn,
                                       llvm::Value* a, llvm::Value* b) {
         llvm::Value* result = llvm::UndefValue::get(v2xf64_ty);
         for (int j = 0; j < 2; ++j) {
            auto* idx = builder.getInt32(j);
            auto* ae  = builder.CreateExtractElement(a, idx);
            auto* be  = builder.CreateExtractElement(b, idx);
            result = builder.CreateInsertElement(result,
                       call_sf_f64_binop(builder, fn, ae, be), idx);
         }
         return result;
      }
      llvm::Value* simd_sf_f64x2_unop(llvm::IRBuilder<>& builder, llvm::Function* fn,
                                      llvm::Value* a) {
         llvm::Value* result = llvm::UndefValue::get(v2xf64_ty);
         for (int j = 0; j < 2; ++j) {
            auto* idx = builder.getInt32(j);
            auto* ae  = builder.CreateExtractElement(a, idx);
            result = builder.CreateInsertElement(result,
                       call_sf_f64_unop(builder, fn, ae), idx);
         }
         return result;
      }

      llvm::Type* wasm_type_to_llvm(uint8_t wt) {
         switch (wt) {
            case types::i32:  return i32_ty;
            case types::i64:  return i64_ty;
            case types::f32:  return f32_ty;
            case types::f64:  return f64_ty;
            case types::v128: return v128_ty;
            default:          return i64_ty; // fallback
         }
      }

      // Lazy declaration: wasm_funcs[i] starts null; we only create the
      // llvm::Function when something actually references it. For batch
      // compilation this avoids paying for 100k+ unused declarations in
      // every batch (each batch typically references only its own functions
      // plus their direct callees).
      void declare_functions() {
         uint32_t num_imports = wasm_mod.get_imported_functions_size();
         uint32_t total_funcs = num_imports + static_cast<uint32_t>(wasm_mod.code.size());
         wasm_funcs.resize(total_funcs, nullptr);
      }

      llvm::Function* get_or_declare_wasm_func(uint32_t i) {
         uint32_t num_imports = wasm_mod.get_imported_functions_size();
         if (i < num_imports) return nullptr; // imports have no wasm_func_N
         if (i >= wasm_funcs.size()) return nullptr; // out of range
         if (wasm_funcs[i]) return wasm_funcs[i];

         const auto& ft = wasm_mod.get_function_type(i);
         std::vector<llvm::Type*> params;
         params.push_back(ptr_ty); // ctx
         params.push_back(ptr_ty); // mem
         for (auto pt : ft.param_types) {
            params.push_back(wasm_type_to_llvm(pt));
         }
         llvm::Type* ret_ty = ft.return_count ? wasm_type_to_llvm(ft.return_type) : void_ty;
         auto* fn_ty = llvm::FunctionType::get(ret_ty, params, false);

         std::string name = "wasm_func_" + std::to_string(i);
         auto linkage = opts.per_function
            ? llvm::Function::ExternalLinkage
            : llvm::Function::InternalLinkage;
         auto* fn = llvm::Function::Create(fn_ty, linkage, name, llvm_mod.get());
         if (opts.fp != fp_mode::fast)
            fn->addFnAttr(llvm::Attribute::StrictFP);
         if (opts.enable_backtrace)
            fn->addFnAttr("frame-pointer", "all");
         wasm_funcs[i] = fn;
         return fn;
      }

      // ──── Translation helpers ────

      // Determine the LLVM type for a vreg based on the IR type byte
      llvm::Type* ir_type_to_llvm(uint8_t ty) {
         switch (ty) {
            case types::i32:  return i32_ty;
            case types::i64:  return i64_ty;
            case types::f32:  return f32_ty;
            case types::f64:  return f64_ty;
            // v128 uses separate storage (v128_slots), not scalar vregs
            default:          return i64_ty;
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

      // ──── Backtrace helpers ────
      // Store current frame address to ctx->_top_frame (offset 8)
      void emit_backtrace_set_top(llvm::IRBuilder<>& builder, llvm::Value* ctx_ptr) {
         if (!opts.enable_backtrace) return;
         auto* fp = builder.CreateCall(frameaddress_intrinsic, {builder.getInt32(0)});
         auto* slot = builder.CreateGEP(i8_ty, ctx_ptr, builder.getInt64(8));
         builder.CreateStore(fp, slot, /*isVolatile=*/true);
      }
      // Clear ctx->_top_frame (offset 8)
      void emit_backtrace_clear_top(llvm::IRBuilder<>& builder, llvm::Value* ctx_ptr) {
         if (!opts.enable_backtrace) return;
         auto* slot = builder.CreateGEP(i8_ty, ctx_ptr, builder.getInt64(8));
         builder.CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_ty)),
                             slot, /*isVolatile=*/true);
      }
      // Store current frame address to ctx->_bottom_frame (offset 0)
      void emit_backtrace_set_bottom(llvm::IRBuilder<>& builder, llvm::Value* ctx_ptr) {
         if (!opts.enable_backtrace) return;
         auto* fp = builder.CreateCall(frameaddress_intrinsic, {builder.getInt32(0)});
         builder.CreateStore(fp, ctx_ptr, /*isVolatile=*/true);
      }
      // Clear ctx->_bottom_frame (offset 0)
      void emit_backtrace_clear_bottom(llvm::IRBuilder<>& builder, llvm::Value* ctx_ptr) {
         if (!opts.enable_backtrace) return;
         builder.CreateStore(llvm::ConstantPointerNull::get(llvm::cast<llvm::PointerType>(ptr_ty)),
                             ctx_ptr, /*isVolatile=*/true);
      }

      void translate(const ir_function& func) {
         uint32_t func_idx = func.func_index + wasm_mod.get_imported_functions_size();
         auto* fn = get_or_declare_wasm_func(func_idx);
         if (!fn) return;
         translated_funcs.push_back(func_idx);


         llvm::IRBuilder<> builder(*ctx);

         // In deterministic or softfloat mode, use constrained FP to prevent LLVM
         // from folding identity float ops (x-0.0 → x, x*1.0 → x) that would
         // skip sNaN → qNaN quieting required by the WASM spec.
         if (opts.fp != fp_mode::fast) {
            builder.setIsFPConstrained(true);
            builder.setDefaultConstrainedExcept(llvm::fp::ebStrict);
            builder.setDefaultConstrainedRounding(llvm::RoundingMode::NearestTiesToEven);
         }

         // ── Entry block: allocas, parameters ──
         auto* entry_bb = llvm::BasicBlock::Create(*ctx, "entry", fn);
         builder.SetInsertPoint(entry_bb);

         // Map function parameters
         auto arg_it = fn->arg_begin();
         llvm::Value* ctx_ptr = &*arg_it++;
         ctx_ptr->setName("ctx");
         llvm::Value* mem_arg = &*arg_it++;
         mem_arg->setName("mem");

         // Gas metering (Phase 4): three-way branch at function entry
         // with cost = body byte size + prepay_extra (heavy-op weights
         // the parser accumulated for opcodes outside any loop). Since
         // LLVM codegen runs after the full parser pass, the extras are
         // already on the IR and the final ConstantInt is known here —
         // no post-hoc patching needed.
         {
            const auto& fb = wasm_mod.code[func.func_index];
            emit_gas_prologue_check(builder, ctx_ptr,
                                    static_cast<int64_t>(fb.wasm_body_bytes)
                                    + func.prologue_gas_extra);
         }

         // Store mem_ptr in an alloca so memory_grow can update it and LLVM's
         // mem2reg pass inserts PHI nodes at control flow merges. Without this,
         // reassigning the C++ variable after memory_grow creates a Load in one
         // basic block that doesn't dominate uses in other blocks.
         auto* mem_ptr_alloca = builder.CreateAlloca(ptr_ty, nullptr, "mem_ptr");
         builder.CreateStore(mem_arg, mem_ptr_alloca);
         auto load_mem_ptr = [&]() -> llvm::Value* {
            return builder.CreateLoad(ptr_ty, mem_ptr_alloca, "mem");
         };

         // Determine vreg types by scanning instructions.
         // Take the first non-pseudo type assignment for each vreg.
         std::vector<uint8_t> vreg_types(func.next_vreg, 0); // 0 = unset
         // Pre-seed v0 with the function's return type. ir_writer's
         // emit_function_prologue allocates v0 first via alloc_vreg(result_type)
         // so v0 is the merge vreg for a single-result function return. If we
         // let inference pick the type from the first dest-write (which may be
         // an i32 store for a truncated f64 merge), the alloca ends up 32-bit
         // and f64 consts get silently truncated on the way out.
         // v128 is handled separately via is_v128[0] / v128_slots.
         {
            const auto& fnty = *func.type;
            if (fnty.return_count == 1 && func.next_vreg > 0 &&
                fnty.return_type != types::v128) {
               vreg_types[0] = fnty.return_type;
            }
         }
         for (uint32_t i = 0; i < func.inst_count; i++) {
            const auto& inst = func.insts[i];
            // Skip ops that repurpose inst.dest as a non-vreg integer payload
            // (depth-change for br/br_if, result_count for multi-value return_,
            // simd_sub enum for v128_op, etc.). If we don't skip them, a numeric
            // collision with a real vreg index wrongly imprints the op's
            // inst.type onto an unrelated vreg (observed: br with rt=f32 and
            // dc=9 typing v9 as f32, triggering `trunc float to i32` verify
            // failures downstream).
            if (inst.dest != ir_vreg_none && inst.dest < func.next_vreg &&
                inst.opcode != ir_op::block_start && inst.opcode != ir_op::block_end &&
                inst.opcode != ir_op::br_table && inst.opcode != ir_op::br &&
                inst.opcode != ir_op::br_if && inst.opcode != ir_op::return_) {
               if (inst.type != types::pseudo && inst.type != types::ret_void &&
                   inst.type != types::v128 && vreg_types[inst.dest] == 0) {
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

         // Create allocas for all virtual registers (scalar)
         std::vector<llvm::AllocaInst*> vregs(func.next_vreg, nullptr);
         for (uint32_t i = 0; i < func.next_vreg; i++) {
            vregs[i] = builder.CreateAlloca(ir_type_to_llvm(vreg_types[i]), nullptr,
                                             "v" + std::to_string(i));
         }

         // Separate v128 storage, pre-allocated for all SIMD vregs
         std::vector<bool> is_v128(func.next_vreg, false);
         {
            // Mark vreg v0 as v128 if function returns v128 (v0 is the merge result vreg)
            if (fn->getReturnType() == v128_ty && func.next_vreg > 0) {
               is_v128[0] = true;
            }
            // Pass 1: mark direct SIMD vregs
            for (uint32_t i = 0; i < func.inst_count; i++) {
               const auto& sinst = func.insts[i];
               if (sinst.opcode == ir_op::const_v128) {
                  if (sinst.dest < func.next_vreg) is_v128[sinst.dest] = true;
               } else if (sinst.opcode == ir_op::v128_op) {
                  auto ssub = static_cast<simd_sub>(sinst.dest);
                  if (sinst.simd.v_src1 < func.next_vreg) is_v128[sinst.simd.v_src1] = true;
                  if (sinst.simd.v_src2 < func.next_vreg && sinst.simd.v_src2 != 0xFFFF)
                     is_v128[sinst.simd.v_src2] = true;
                  if (!simd_produces_scalar(ssub) && sinst.simd.v_dest < func.next_vreg)
                     is_v128[sinst.simd.v_dest] = true;
                  // bitselect and relaxed ternary ops store mask/third operand in addr
                  if (ssub == simd_sub::v128_bitselect ||
                      ssub == simd_sub::f32x4_relaxed_madd || ssub == simd_sub::f32x4_relaxed_nmadd ||
                      ssub == simd_sub::f64x2_relaxed_madd || ssub == simd_sub::f64x2_relaxed_nmadd ||
                      ssub == simd_sub::i32x4_relaxed_dot_i8x16_i7x16_add_s) {
                     if (sinst.simd.addr < func.next_vreg) is_v128[sinst.simd.addr] = true;
                  }
               } else if (sinst.opcode == ir_op::nop && sinst.type == types::v128 &&
                           sinst.dest != ir_vreg_none && sinst.dest < func.next_vreg) {
                  is_v128[sinst.dest] = true;
               } else if (sinst.opcode == ir_op::arg && sinst.type == types::v128 &&
                           sinst.rr.src1 < func.next_vreg) {
                  is_v128[sinst.rr.src1] = true;
               } else if (sinst.opcode == ir_op::local_get && sinst.type == types::v128 &&
                           sinst.dest != ir_vreg_none && sinst.dest < func.next_vreg) {
                  is_v128[sinst.dest] = true;
               } else if ((sinst.opcode == ir_op::call || sinst.opcode == ir_op::call_indirect) &&
                           sinst.type == types::v128 &&
                           sinst.dest != ir_vreg_none && sinst.dest < func.next_vreg) {
                  is_v128[sinst.dest] = true;
               } else if (sinst.opcode == ir_op::global_get && sinst.type == types::v128 &&
                           sinst.dest != ir_vreg_none && sinst.dest < func.next_vreg) {
                  is_v128[sinst.dest] = true;
               } else if (sinst.opcode == ir_op::select && sinst.type == types::v128) {
                  if (sinst.dest != ir_vreg_none && sinst.dest < func.next_vreg)
                     is_v128[sinst.dest] = true;
                  if (sinst.sel.val1 < func.next_vreg) is_v128[sinst.sel.val1] = true;
                  if (sinst.sel.val2 < func.next_vreg) is_v128[sinst.sel.val2] = true;
               } else if (sinst.opcode == ir_op::multi_return_load && sinst.type == types::v128 &&
                           sinst.dest != ir_vreg_none && sinst.dest < func.next_vreg) {
                  is_v128[sinst.dest] = true;
               } else if (sinst.opcode == ir_op::multi_return_store && sinst.type == types::v128 &&
                           sinst.ri.src1 != ir_vreg_none && sinst.ri.src1 < func.next_vreg) {
                  is_v128[sinst.ri.src1] = true;
               }
            }
            // Pass 2: propagate through movs (repeat until stable)
            bool changed = true;
            while (changed) {
               changed = false;
               for (uint32_t i = 0; i < func.inst_count; i++) {
                  const auto& sinst = func.insts[i];
                  if (sinst.opcode == ir_op::mov &&
                      sinst.rr.src1 < func.next_vreg && is_v128[sinst.rr.src1] &&
                      sinst.dest != ir_vreg_none && sinst.dest < func.next_vreg &&
                      !is_v128[sinst.dest]) {
                     is_v128[sinst.dest] = true;
                     changed = true;
                  }
               }
            }
         }
         // Create v128 allocas
         std::vector<llvm::AllocaInst*> v128_slots(func.next_vreg, nullptr);
         for (uint32_t i = 0; i < func.next_vreg; i++) {
            if (is_v128[i])
               v128_slots[i] = builder.CreateAlloca(v128_ty, nullptr,
                  "s" + std::to_string(i));
         }

         // Build a flat array of WASM types for all locals (params + declared locals)
         uint32_t num_locals = func.num_params + func.num_locals;
         const auto& ft = *func.type;
         std::vector<uint8_t> local_wasm_types(num_locals, types::i64);
         for (uint32_t i = 0; i < func.num_params; i++)
            local_wasm_types[i] = ft.param_types[i];
         {
            const auto& body_locals = wasm_mod.code[func.func_index].locals;
            uint32_t idx = func.num_params;
            for (const auto& le : body_locals) {
               for (uint32_t c = 0; c < le.count && idx < num_locals; c++, idx++)
                  local_wasm_types[idx] = le.type;
            }
         }

         // Create allocas for local variables
         std::vector<llvm::AllocaInst*> locals(num_locals, nullptr);
         for (uint32_t i = 0; i < num_locals; i++) {
            locals[i] = builder.CreateAlloca(wasm_type_to_llvm(local_wasm_types[i]), nullptr,
                                              "local" + std::to_string(i));
         }

         // Store function parameters into param locals
         arg_it = fn->arg_begin();
         ++arg_it; ++arg_it; // skip ctx, mem
         for (uint32_t i = 0; i < func.num_params && arg_it != fn->arg_end(); ++i, ++arg_it) {
            builder.CreateStore(&*arg_it, locals[i]);
         }

         // Initialize non-param locals (ref types to UINT32_MAX null sentinel, others to zero)
         for (uint32_t i = func.num_params; i < num_locals; i++) {
            llvm::Type* lt = locals[i]->getAllocatedType();
            uint8_t wt = local_wasm_types[i];
            if (wt == types::funcref || wt == types::externref || wt == types::exnref) {
               builder.CreateStore(llvm::ConstantInt::get(lt, UINT32_MAX), locals[i]);
            } else {
               builder.CreateStore(llvm::Constant::getNullValue(lt), locals[i]);
            }
         }

         // ── Pre-scan: max host call args (for entry-block alloca) ──
         // The alloca must also fit return values written back by the callee
         // (e.g. entry wrapper stores v128 return — 2 i64 slots — into args).
         uint32_t max_host_args = 0;
         {
            uint32_t num_imports = wasm_mod.get_imported_functions_size();
            uint32_t pending_call_args = 0;
            auto update_for_call = [&](bool uses_args_buf, uint32_t type_idx_or_funcnum, bool is_indirect) {
               if (uses_args_buf && pending_call_args > max_host_args)
                  max_host_args = pending_call_args;
               // Return value can be written back via the args buffer (v128 = 2 slots).
               const func_type* ft = nullptr;
               if (is_indirect) {
                  uint32_t tidx = type_idx_or_funcnum & 0xFFFF;
                  if (tidx < wasm_mod.types.size())
                     ft = &wasm_mod.types[tidx];
               } else if (type_idx_or_funcnum < wasm_mod.get_functions_total()) {
                  ft = &wasm_mod.get_function_type(type_idx_or_funcnum);
               }
               if (ft && ft->return_count > 0 && ft->return_type == types::v128 && max_host_args < 2)
                  max_host_args = 2;
            };
            for (uint32_t i = 0; i < func.inst_count; i++) {
               const auto& inst = func.insts[i];
               if (inst.opcode == ir_op::arg) {
                  pending_call_args++;
               } else if (inst.opcode == ir_op::call) {
                  update_for_call(inst.call.index < num_imports, inst.call.index, false);
                  pending_call_args = 0;
               } else if (inst.opcode == ir_op::call_indirect) {
                  update_for_call(true, inst.call.index, true);
                  pending_call_args = 0;
               } else if (inst.opcode == ir_op::tail_call) {
                  update_for_call(inst.call.index < num_imports, inst.call.index, false);
                  pending_call_args = 0;
               } else if (inst.opcode == ir_op::tail_call_indirect) {
                  update_for_call(true, inst.call.index, true);
                  pending_call_args = 0;
               }
            }
         }
         llvm::AllocaInst* host_args_alloca = nullptr;
         llvm::AllocaInst* mem_out_alloca = nullptr;  // For rt_call_host_full's memory output
         // Always allocate at least 1 slot in the entry block.
         // Without this, zero-arg host calls (e.g. host_noop()) would fall through
         // to CreateAlloca inside the loop body, growing the stack on every iteration
         // until the native stack overflows.
         {
            uint32_t alloca_size = max_host_args > 0 ? max_host_args : 1;
            host_args_alloca = builder.CreateAlloca(i64_ty,
               builder.getInt32(alloca_size), "host_args");
         }
         // Allocate mem_out in the entry block for any function that might call host
         if (rt_call_host_full && wasm_mod.get_imported_functions_size() > 0) {
            mem_out_alloca = builder.CreateAlloca(ptr_ty, nullptr, "mem_out");
         }

         // Checked memory mode: reads use deferred watermark (umax accumulation,
         // zero branches per read). Validated at loop headers and before host calls.
         // Writes use immediate per-access bounds checks.
         llvm::AllocaInst* watermark_alloca = nullptr;
         if (is_checked()) {
            watermark_alloca = builder.CreateAlloca(i64_ty, nullptr, "watermark");
            builder.CreateStore(builder.getInt64(0), watermark_alloca);
         }

         // Checked mode: cache mem_size in a local alloca. LLVM's SROA/mem2reg
         // promotes this to a register, avoiding repeated loads from the prefix
         // page on every bounds check. Reloaded after memory.grow / host calls.
         llvm::AllocaInst* mem_size_cache = nullptr;
         if (is_checked()) {
            mem_size_cache = builder.CreateAlloca(i64_ty, nullptr, "mem_size_cache");
            const int32_t ms_off = wasm_allocator::mem_size_offset();
            llvm::Value* ms_ptr = builder.CreateConstGEP1_64(i8_ty, load_mem_ptr(),
               static_cast<int64_t>(ms_off), "init_ms_ptr");
            builder.CreateStore(builder.CreateLoad(i64_ty, ms_ptr, "init_ms"), mem_size_cache);
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
            // Vector to scalar: bitcast to same-width integer, then truncate
            if (src->isVectorTy() && !target->isVectorTy()) {
               auto* int_wide = builder.getIntNTy(src_bits);
               val = builder.CreateBitCast(val, int_wide);
               if (target->isIntegerTy()) {
                  return (src_bits == dst_bits) ? val : builder.CreateTrunc(val, target);
               }
               if (target->isFloatingPointTy()) {
                  val = builder.CreateTrunc(val, builder.getIntNTy(dst_bits));
                  return builder.CreateBitCast(val, target);
               }
            }
            return val;
         };

         // Load a vreg and convert to a specific type.
         // Returns a zero constant of the target type if the vreg is unallocated
         // (e.g. in unreachable code): this keeps the IR builder from calling
         // methods on a null Value* and matches what other backends produce for
         // a dead-code use (garbage is fine; the op will never actually execute).
         auto load_vreg_as = [&](uint32_t vreg, llvm::Type* target_ty) -> llvm::Value* {
            llvm::Value* val = load_vreg(vreg);
            if (!val) return llvm::Constant::getNullValue(target_ty);
            auto* out = convert_type(val, target_ty);
            // Defensive: if convert_type didn't actually produce the requested
            // type (edge case with unexpected source types), fall back to a
            // same-size int bitcast or a zero constant of the target type.
            if (out->getType() != target_ty) {
               unsigned out_bits = out->getType()->getPrimitiveSizeInBits();
               unsigned dst_bits = target_ty->getPrimitiveSizeInBits();
               if (out_bits == dst_bits)
                  out = builder.CreateBitCast(out, target_ty);
               else
                  out = llvm::Constant::getNullValue(target_ty);
            }
            return out;
         };

         auto store_vreg = [&](uint32_t vreg, llvm::Value* val) {
            if (vreg == ir_vreg_none || vreg >= vregs.size() || !vregs[vreg]) return;
            llvm::Type* alloca_ty = vregs[vreg]->getAllocatedType();
            if (val->getType() != alloca_ty) {
               val = convert_type(val, alloca_ty);
            }
            builder.CreateStore(val, vregs[vreg]);
         };

         // ── v128 helpers ──
         // Load a v128 vreg as the specified vector type (from separate v128 storage)
         auto load_v128 = [&](uint16_t vreg, llvm::VectorType* as_ty) -> llvm::Value* {
            if (vreg >= v128_slots.size() || !v128_slots[vreg])
               return llvm::Constant::getNullValue(as_ty);
            auto* raw = builder.CreateLoad(v128_ty, v128_slots[vreg]);
            if (raw->getType() == as_ty) return raw;
            return builder.CreateBitCast(raw, as_ty);
         };
         auto store_v128 = [&](uint16_t vreg, llvm::Value* val) {
            if (vreg >= v128_slots.size() || !v128_slots[vreg]) return;
            if (val->getType() != v128_ty)
               val = builder.CreateBitCast(val, v128_ty);
            builder.CreateStore(val, v128_slots[vreg]);
         };
         // Compute effective memory address: mem_ptr + zext(addr, i64) + offset
         // Uses i64 arithmetic to avoid i32 wrap-around (WASM requires i33 effective address)
         // access_size > 0 with is_store=false: track read watermark (checked mode)
         // access_size > 0 with is_store=true:  immediate bounds check (checked mode)
         auto simd_mem_addr = [&](uint32_t addr_vreg, uint32_t offset,
                                  uint32_t access_size = 0, bool is_store = false) -> llvm::Value* {
            auto* addr = load_vreg(addr_vreg);
            if (!addr) addr = builder.getInt64(0);
            auto* eff_addr = builder.CreateZExt(builder.CreateTrunc(addr, i32_ty), i64_ty);
            if (offset != 0)
               eff_addr = builder.CreateAdd(eff_addr, builder.getInt64(static_cast<uint64_t>(offset)));
            if (access_size > 0) {
               if (is_store)
                  emit_write_bounds_check(builder, ctx_ptr, load_mem_ptr(), eff_addr, access_size, mem_size_cache);
               else
                  emit_read_watermark_update(builder, eff_addr, access_size, watermark_alloca);
            }
            return builder.CreateGEP(i8_ty, load_mem_ptr(), eff_addr);
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

         // Pending v128 result from shuffle (whose immv128 overlay prevents using simd.v_dest)
         llvm::Value* pending_v128_result = nullptr;

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
                  // Consume pending v128 result from shuffle (whose immv128 overlay prevents simd.v_dest)
                  if (pending_v128_result && inst.dest != ir_vreg_none) {
                     store_v128(static_cast<uint16_t>(inst.dest), pending_v128_result);
                     pending_v128_result = nullptr;
                  }
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
                     // Loop-header gas metering: back-branches to this
                     // block land at `target`, so emitting the gas check
                     // here means every iteration (including the first)
                     // charges gas. Cost = 1 per iteration + heavy-op
                     // extras the parser accumulated inside this loop
                     // (Phase 4).
                     if (func.blocks && func.blocks[block_idx].is_loop) {
                        // Validate watermark before gas charge (checked mode)
                        emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);
                        emit_gas_prologue_check(builder, ctx_ptr,
                           1 + func.blocks[block_idx].loop_gas_extra);
                     }
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
                  // Determine the false branch target. emit_if (ir_writer) now
                  // emits block_start for both no-else and has-else cases, so
                  // has_block_start[target] is always true — use is_if instead:
                  //   is_if=1 → no else, target is the if's own body; skip it
                  //             by branching to block_exits[target] (end merge).
                  //   is_if=0 → has else, target is a separate else_block whose
                  //             start emits into block_entries[target].
                  llvm::BasicBlock* false_bb = nullptr;
                  uint32_t target = inst.br.target;
                  if (target < func.block_count) {
                     false_bb = (func.blocks && func.blocks[target].is_if)
                        ? block_exits[target]
                        : block_entries[target];
                     if (!false_bb) false_bb = block_exits[target];
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
                     auto* target_bb = ((func.blocks[target].is_loop || func.blocks[target].branch_to_entry) && block_entries[target])
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
                  // High 16 bits of inst.dest hold eh_leave_count for try_table
                  // scopes crossed by this branch. Must emit eh_leave on the
                  // taken path so the catch frame is popped before we jump.
                  uint32_t eh_count = inst.dest >> 16;
                  llvm::Value* cond = load_vreg(inst.br.src1);
                  if (!cond || target >= func.block_count) break;
                  llvm::Value* cmp = builder.CreateICmpNE(cond,
                     llvm::Constant::getNullValue(cond->getType()));
                  auto* target_bb = ((func.blocks[target].is_loop || func.blocks[target].branch_to_entry) && block_entries[target])
                     ? block_entries[target]
                     : block_exits[target];
                  auto* cont_bb = llvm::BasicBlock::Create(*ctx, "br_if_cont", fn);
                  if (eh_count > 0) {
                     auto* taken_bb = llvm::BasicBlock::Create(*ctx, "br_if_taken", fn);
                     builder.CreateCondBr(cmp, taken_bb, cont_bb);
                     builder.SetInsertPoint(taken_bb);
                     for (uint32_t i = 0; i < eh_count; ++i) {
                        builder.CreateCall(rt_eh_leave, {ctx_ptr});
                     }
                     builder.CreateBr(target_bb);
                  } else {
                     builder.CreateCondBr(cmp, target_bb, cont_bb);
                  }
                  builder.SetInsertPoint(cont_bb);
                  break;
               }

               case ir_op::br_table: {
                  // inst.rr.src1 = index vreg, inst.dest = table size
                  llvm::Value* bt_index = load_vreg(inst.rr.src1);
                  uint32_t bt_size = inst.dest;
                  // WASM br_table's index is i32. If the vreg happens to be
                  // i64 (e.g. a cross-type reuse in unreachable code), LLVM's
                  // SwitchInst rejects mismatched case/value types. Truncate
                  // to i32 so CreateSwitch + addCase(getInt32(c)) validates.
                  if (bt_index && bt_index->getType() == i64_ty) {
                     bt_index = builder.CreateTrunc(bt_index, i32_ty);
                  } else if (bt_index && bt_index->getType() != i32_ty) {
                     bt_index = convert_type(bt_index, i32_ty);
                  }

                  // Helper to resolve br target to correct LLVM block
                  auto resolve_br_target = [&](uint32_t target) -> llvm::BasicBlock* {
                     if (target >= func.block_count) return nullptr;
                     return ((func.blocks[target].is_loop || func.blocks[target].branch_to_entry) && block_entries[target])
                        ? block_entries[target]
                        : block_exits[target];
                  };

                  // Scan subsequent instructions for per-case prologue groups
                  // terminated by a br.  A case prologue may contain mov ops
                  // (for block merge_vregs) and/or multi_return_store ops
                  // (for function-exit multi-value returns), in the order
                  // emitted by ir_writer::br_table_parser::emit_case.
                  struct bt_case {
                     uint32_t br_target;
                     uint32_t eh_count; // high 16 bits of each br's dest field
                     std::vector<uint32_t> prologue; // inst indices to replay
                  };
                  std::vector<bt_case> cases;
                  uint32_t scan = ip + 1;
                  while (scan < func.inst_count && cases.size() <= bt_size) {
                     bt_case c{};
                     // Collect prologue ops (mov / multi_return_store) before br
                     while (scan < func.inst_count &&
                            (func.insts[scan].opcode == ir_op::mov ||
                             func.insts[scan].opcode == ir_op::multi_return_store)) {
                        c.prologue.push_back(scan);
                        scan++;
                     }
                     // Expect a br
                     if (scan < func.inst_count && func.insts[scan].opcode == ir_op::br) {
                        c.br_target = func.insts[scan].br.target;
                        c.eh_count = func.insts[scan].dest >> 16;
                        cases.push_back(std::move(c));
                        scan++;
                     } else {
                        break;
                     }
                  }

                  if (bt_index && cases.size() > bt_size) {
                     // WASM br_table index is i32; IR may surface it wider. Coerce so
                     // LLVM's switch and the getInt32 case constants agree on type.
                     if (bt_index->getType() != builder.getInt32Ty() &&
                         bt_index->getType()->isIntegerTy()) {
                        bt_index = builder.CreateIntCast(bt_index, builder.getInt32Ty(), false);
                     }
                     // Last case is the default
                     auto& default_case = cases.back();

                     // Create per-case blocks if any case has prologue ops OR
                     // crosses try_table scopes (needs eh_leave calls).
                     bool need_case_blocks = false;
                     for (auto& c : cases) {
                        if (!c.prologue.empty() || c.eh_count > 0) { need_case_blocks = true; break; }
                     }

                     if (need_case_blocks) {
                        // Emit a prologue op (mov or multi_return_store) in
                        // the current insert point, mirroring the main-loop
                        // handlers for these opcodes.
                        auto emit_prologue_op = [&](uint32_t inst_idx) {
                           auto& pi = func.insts[inst_idx];
                           if (pi.opcode == ir_op::mov) {
                              if (pi.rr.src1 < v128_slots.size() && v128_slots[pi.rr.src1]) {
                                 auto* val = load_v128(static_cast<uint16_t>(pi.rr.src1), v128_ty);
                                 store_v128(static_cast<uint16_t>(pi.dest), val);
                              } else {
                                 llvm::Value* val = load_vreg(pi.rr.src1);
                                 if (val) store_vreg(pi.dest, val);
                              }
                           } else { // multi_return_store
                              auto* val = load_vreg(pi.ri.src1);
                              if (!val) return;
                              auto* t = val->getType();
                              llvm::Value* val_i64 = val;
                              if (t == f32_ty) {
                                 val_i64 = builder.CreateZExt(builder.CreateBitCast(val, i32_ty), i64_ty);
                              } else if (t == f64_ty) {
                                 val_i64 = builder.CreateBitCast(val, i64_ty);
                              } else if (t == i32_ty) {
                                 val_i64 = builder.CreateZExt(val, i64_ty);
                              } else if (t != i64_ty) {
                                 val_i64 = llvm::ConstantInt::get(i64_ty, 0);
                              }
                              int32_t offset = 24 + pi.ri.imm;
                              auto* ptr = builder.CreateGEP(builder.getInt8Ty(), ctx_ptr,
                                 builder.getInt32(offset));
                              auto* typed_ptr = builder.CreateBitCast(ptr,
                                 llvm::PointerType::getUnqual(builder.getInt64Ty()));
                              builder.CreateStore(val_i64, typed_ptr);
                           }
                        };

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
                           // Emit prologue ops (movs / multi_return_stores) for this case
                           for (uint32_t pidx : cases[c].prologue) {
                              emit_prologue_op(pidx);
                           }
                           // Pop try_table catch frames this branch crosses
                           for (uint32_t i = 0; i < cases[c].eh_count; ++i) {
                              builder.CreateCall(rt_eh_leave, {ctx_ptr});
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
                  emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);
                  if (inst.rr.src1 != ir_vreg_none) {
                     llvm::Value* ret_val;
                     // Check if returning a v128 value
                     if (fn->getReturnType() == v128_ty &&
                         (inst.type == types::v128 ||
                          (inst.rr.src1 < v128_slots.size() && v128_slots[inst.rr.src1]))) {
                        ret_val = load_v128(static_cast<uint16_t>(inst.rr.src1), v128_ty);
                     } else {
                        ret_val = load_vreg(inst.rr.src1);
                     }
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
                  // Check if source is a v128 vreg — propagate through v128 storage
                  if (inst.rr.src1 < v128_slots.size() && v128_slots[inst.rr.src1]) {
                     auto* val = load_v128(static_cast<uint16_t>(inst.rr.src1), v128_ty);
                     store_v128(static_cast<uint16_t>(inst.dest), val);
                  } else {
                     llvm::Value* val = load_vreg(inst.rr.src1);
                     if (val) store_vreg(inst.dest, val);
                  }
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
                     if (inst.type == types::v128 && inst.dest != ir_vreg_none) {
                        store_v128(static_cast<uint16_t>(inst.dest), val);
                     } else {
                        store_vreg(inst.dest, val);
                     }
                  }
                  break;
               }

               case ir_op::local_set: {
                  uint32_t li = inst.local.index;
                  if (li < locals.size() && locals[li]) {
                     if (inst.type == types::v128) {
                        auto* val = load_v128(static_cast<uint16_t>(inst.local.src1), v128_ty);
                        builder.CreateStore(val, locals[li]);
                     } else {
                        llvm::Value* val = load_vreg(inst.local.src1);
                        if (val) {
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
                     }
                  }
                  break;
               }

               case ir_op::local_tee: {
                  // Same as local_set but value stays on stack (already handled by ir_writer)
                  uint32_t li = inst.local.index;
                  if (li < locals.size() && locals[li]) {
                     if (inst.type == types::v128) {
                        auto* val = load_v128(static_cast<uint16_t>(inst.local.src1), v128_ty);
                        builder.CreateStore(val, locals[li]);
                     } else {
                        llvm::Value* val = load_vreg(inst.local.src1);
                        if (val) {
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
                     }
                  }
                  break;
               }

               case ir_op::global_get: {
                  uint32_t gidx = inst.local.index;
                  if (inst.type == types::v128) {
                     // v128 global: use pointer-based helper
                     auto* tmp = builder.CreateAlloca(v128_ty);
                     builder.CreateCall(rt_global_get_v128,
                        {ctx_ptr, builder.getInt32(gidx), tmp});
                     auto* val = builder.CreateLoad(v128_ty, tmp);
                     store_v128(static_cast<uint16_t>(inst.dest), val);
                  } else {
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
                  }
                  break;
               }

               case ir_op::global_set: {
                  uint32_t gidx = inst.local.index;
                  if (inst.type == types::v128) {
                     // v128 global: use pointer-based helper
                     auto* val = load_v128(static_cast<uint16_t>(inst.local.src1), v128_ty);
                     auto* tmp = builder.CreateAlloca(v128_ty);
                     builder.CreateStore(val, tmp);
                     builder.CreateCall(rt_global_set_v128,
                        {ctx_ptr, builder.getInt32(gidx), tmp});
                  } else {
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
                  }
                  break;
               }

               // ──── Parametric ────
               case ir_op::drop:
                  // No-op in LLVM (values are SSA, unused values are dead)
                  break;

               case ir_op::select: {
                  llvm::Value* cond = load_vreg(inst.sel.cond);
                  if (inst.type == types::v128) {
                     auto* v128_ty_local = llvm::FixedVectorType::get(builder.getInt64Ty(), 2);
                     llvm::Value* val1 = load_v128(inst.sel.val1, v128_ty_local);
                     llvm::Value* val2 = load_v128(inst.sel.val2, v128_ty_local);
                     if (cond && val1 && val2) {
                        llvm::Value* cmp = builder.CreateICmpNE(cond,
                           llvm::Constant::getNullValue(cond->getType()));
                        llvm::Value* result = builder.CreateSelect(cmp, val1, val2);
                        store_v128(static_cast<uint16_t>(inst.dest), result);
                     }
                  } else {
                     llvm::Value* val1 = load_vreg(inst.sel.val1);
                     llvm::Value* val2 = load_vreg(inst.sel.val2);
                     if (cond && val1 && val2) {
                        // Ensure both operands have matching types for LLVM select
                        if (val1->getType() != val2->getType()) {
                           val2 = convert_type(val2, val1->getType());
                        }
                        llvm::Value* cmp = builder.CreateICmpNE(cond,
                           llvm::Constant::getNullValue(cond->getType()));
                        llvm::Value* result = builder.CreateSelect(cmp, val1, val2);
                        store_vreg(inst.dest, result);
                     }
                  }
                  break;
               }

               // ──── Call argument pseudo-instruction ────
               case ir_op::arg: {
                  llvm::Value* val;
                  if (inst.type == types::v128 ||
                      (inst.rr.src1 < is_v128.size() && is_v128[inst.rr.src1])) {
                     // v128 arg — load from v128 storage
                     val = load_v128(static_cast<uint16_t>(inst.rr.src1), v128_ty);
                  } else {
                     val = load_vreg(inst.rr.src1);
                  }
                  if (val) call_args.push_back(val);
                  break;
               }

               // ──── Function calls ────
               case ir_op::call: {
                  uint32_t funcnum = inst.call.index;
                  uint32_t num_imports = wasm_mod.get_imported_functions_size();

                  if (funcnum < num_imports && mem_out_alloca) {
                     // Fast path: combined call (depth_dec + host_call + depth_inc + get_memory)
                     // Reduces 4 extern calls to 1 for host function calls.
                     uint32_t nargs = static_cast<uint32_t>(call_args.size());
                     for (uint32_t a = 0; a < nargs; a++) {
                        llvm::Value* slot = builder.CreateGEP(i64_ty, host_args_alloca,
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

                     // Checked mode: validate read watermark before host call
                     emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);

                     emit_backtrace_set_top(builder, ctx_ptr);
                     emit_backtrace_set_bottom(builder, ctx_ptr);

                     llvm::Value* raw = builder.CreateCall(rt_call_host_full,
                        {ctx_ptr, builder.getInt32(funcnum), host_args_alloca,
                         builder.getInt32(nargs), mem_out_alloca});

                     emit_backtrace_clear_bottom(builder, ctx_ptr);
                     emit_backtrace_clear_top(builder, ctx_ptr);

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
                     // Load updated memory pointer from the combined call
                     auto* new_mem = builder.CreateLoad(ptr_ty, mem_out_alloca);
                     builder.CreateStore(new_mem, mem_ptr_alloca);
                     if (mem_size_cache) {
                        const int32_t ms_off = wasm_allocator::mem_size_offset();
                        auto* ms_ptr = builder.CreateConstGEP1_64(i8_ty, new_mem,
                           static_cast<int64_t>(ms_off), "host_ms_ptr");
                        builder.CreateStore(builder.CreateLoad(i64_ty, ms_ptr, "host_ms"), mem_size_cache);
                     }
                  } else {
                     // Standard path: separate call_depth tracking
                     // Checked mode: validate read watermark before call
                     emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);
                     builder.CreateCall(rt_call_depth_dec, {ctx_ptr});

                     if (funcnum < num_imports) {
                        // Host function call — go through runtime helper
                        uint32_t nargs = static_cast<uint32_t>(call_args.size());
                        for (uint32_t a = 0; a < nargs; a++) {
                           llvm::Value* slot = builder.CreateGEP(i64_ty, host_args_alloca,
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

                        emit_backtrace_set_top(builder, ctx_ptr);
                        emit_backtrace_set_bottom(builder, ctx_ptr);

                        llvm::Value* raw = builder.CreateCall(rt_call_host,
                           {ctx_ptr, builder.getInt32(funcnum), host_args_alloca,
                            builder.getInt32(nargs)});

                        emit_backtrace_clear_bottom(builder, ctx_ptr);
                        emit_backtrace_clear_top(builder, ctx_ptr);

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
                        auto* new_mem = builder.CreateCall(rt_get_memory, {ctx_ptr});
                        builder.CreateStore(new_mem, mem_ptr_alloca);
                        if (mem_size_cache) {
                           const int32_t ms_off = wasm_allocator::mem_size_offset();
                           auto* ms_ptr = builder.CreateConstGEP1_64(i8_ty, new_mem,
                              static_cast<int64_t>(ms_off), "host_ms_ptr");
                           builder.CreateStore(builder.CreateLoad(i64_ty, ms_ptr, "host_ms"), mem_size_cache);
                        }
                     } else {
                        // WASM-to-WASM call
                        std::vector<llvm::Value*> args;
                        args.push_back(ctx_ptr);
                        args.push_back(load_mem_ptr());

                        llvm::Function* callee = get_or_declare_wasm_func(funcnum);

                        // Convert each arg to match callee's parameter types
                        if (callee) {
                           auto* callee_ty = callee->getFunctionType();
                           for (uint32_t a = 0; a < call_args.size(); a++) {
                              uint32_t param_idx = a + 2; // skip ctx, mem
                              if (param_idx < callee_ty->getNumParams()) {
                                 call_args[a] = convert_type(call_args[a], callee_ty->getParamType(param_idx));
                              }
                           }
                        }

                        for (auto* a : call_args) args.push_back(a);
                        call_args.clear();

                        if (callee) {
                           emit_backtrace_set_top(builder, ctx_ptr);
                           auto* result = builder.CreateCall(callee, args);
                           emit_backtrace_clear_top(builder, ctx_ptr);
                           if (inst.dest != ir_vreg_none && !result->getType()->isVoidTy()) {
                              if (inst.type == types::v128) {
                                 store_v128(static_cast<uint16_t>(inst.dest), result);
                              } else {
                                 store_vreg(inst.dest, result);
                              }
                           }
                           // Reload mem_ptr after WASM-to-WASM call (callee may grow memory)
                           auto* new_mem = builder.CreateCall(rt_get_memory, {ctx_ptr});
                           builder.CreateStore(new_mem, mem_ptr_alloca);
                           if (mem_size_cache) {
                              const int32_t ms_off = wasm_allocator::mem_size_offset();
                              auto* ms_ptr = builder.CreateConstGEP1_64(i8_ty, new_mem,
                                 static_cast<int64_t>(ms_off), "call_ms_ptr");
                              builder.CreateStore(builder.CreateLoad(i64_ty, ms_ptr, "call_ms"), mem_size_cache);
                           }
                        }
                     }
                     // Restore call depth after either host or WASM call
                     builder.CreateCall(rt_call_depth_inc, {ctx_ptr});
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
                  for (uint32_t a = 0; a < nargs; a++) {
                     llvm::Value* slot = builder.CreateGEP(i64_ty, host_args_alloca,
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

                  // Checked mode: validate read watermark before indirect call
                  emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);
                  builder.CreateCall(rt_call_depth_dec, {ctx_ptr});
                  emit_backtrace_set_top(builder, ctx_ptr);
                  emit_backtrace_set_bottom(builder, ctx_ptr);

                  llvm::Value* raw = builder.CreateCall(rt_call_indirect,
                     {ctx_ptr, load_mem_ptr(), builder.getInt32(type_idx),
                      table_elem, host_args_alloca, builder.getInt32(nargs)});

                  emit_backtrace_clear_bottom(builder, ctx_ptr);
                  emit_backtrace_clear_top(builder, ctx_ptr);
                  builder.CreateCall(rt_call_depth_inc, {ctx_ptr});

                  // Reload mem_ptr after call_indirect (callee may grow memory)
                  {
                     auto* new_mem = builder.CreateCall(rt_get_memory, {ctx_ptr});
                     builder.CreateStore(new_mem, mem_ptr_alloca);
                     if (mem_size_cache) {
                        const int32_t ms_off = wasm_allocator::mem_size_offset();
                        auto* ms_ptr = builder.CreateConstGEP1_64(i8_ty, new_mem,
                           static_cast<int64_t>(ms_off), "calli_ms_ptr");
                        builder.CreateStore(builder.CreateLoad(i64_ty, ms_ptr, "calli_ms"), mem_size_cache);
                     }
                  }

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

               // ──── Tail calls (emit as call + return) ────
               case ir_op::tail_call: {
                  uint32_t funcnum = inst.call.index;
                  uint32_t num_imports = wasm_mod.get_imported_functions_size();

                  // Checked mode: validate read watermark before tail call
                  emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);
                  builder.CreateCall(rt_call_depth_dec, {ctx_ptr});

                  if (funcnum < num_imports) {
                     // Host function tail call
                     uint32_t nargs = static_cast<uint32_t>(call_args.size());
                     for (uint32_t a = 0; a < nargs; a++) {
                        llvm::Value* slot = builder.CreateGEP(i64_ty, host_args_alloca, builder.getInt32(a));
                        llvm::Value* v = call_args[a];
                        if (v->getType() == i32_ty) v = builder.CreateZExt(v, i64_ty);
                        else if (v->getType() == f32_ty) v = builder.CreateZExt(builder.CreateBitCast(v, i32_ty), i64_ty);
                        else if (v->getType() == f64_ty) v = builder.CreateBitCast(v, i64_ty);
                        builder.CreateStore(v, slot);
                     }
                     call_args.clear();
                     emit_backtrace_set_top(builder, ctx_ptr);
                     emit_backtrace_set_bottom(builder, ctx_ptr);
                     llvm::Value* raw = builder.CreateCall(rt_call_host,
                        {ctx_ptr, builder.getInt32(funcnum), host_args_alloca, builder.getInt32(nargs)});
                     emit_backtrace_clear_bottom(builder, ctx_ptr);
                     emit_backtrace_clear_top(builder, ctx_ptr);
                     builder.CreateCall(rt_call_depth_inc, {ctx_ptr});
                     builder.CreateStore(builder.CreateCall(rt_get_memory, {ctx_ptr}), mem_ptr_alloca);
                     // Return the result
                     if (fn->getReturnType()->isVoidTy()) {
                        builder.CreateRetVoid();
                     } else if (fn->getReturnType() == v128_ty) {
                        // Host trampoline returns scalar low i64; the full v128
                        // is not currently written back by host calls. Fall back
                        // to splatting the low half into the vector so the IR
                        // typechecks. Host v128 returns are a known gap.
                        auto* vec = builder.CreateInsertElement(
                           llvm::UndefValue::get(v128_ty), raw, builder.getInt32(0));
                        vec = builder.CreateInsertElement(vec,
                           llvm::ConstantInt::get(i64_ty, 0), builder.getInt32(1));
                        builder.CreateRet(vec);
                     } else {
                        llvm::Value* result = convert_type(raw, fn->getReturnType());
                        builder.CreateRet(result);
                     }
                  } else {
                     // WASM-to-WASM tail call
                     std::vector<llvm::Value*> args;
                     args.push_back(ctx_ptr);
                     args.push_back(load_mem_ptr());
                     llvm::Function* callee = get_or_declare_wasm_func(funcnum);
                     if (callee) {
                        auto* callee_ty = callee->getFunctionType();
                        for (uint32_t a = 0; a < call_args.size(); a++) {
                           uint32_t param_idx = a + 2;
                           if (param_idx < callee_ty->getNumParams())
                              call_args[a] = convert_type(call_args[a], callee_ty->getParamType(param_idx));
                        }
                     }
                     for (auto* a : call_args) args.push_back(a);
                     call_args.clear();
                     if (callee) {
                        emit_backtrace_set_top(builder, ctx_ptr);
                        auto* result = builder.CreateCall(callee, args);
                        result->setTailCallKind(llvm::CallInst::TCK_Tail);
                        emit_backtrace_clear_top(builder, ctx_ptr);
                        builder.CreateCall(rt_call_depth_inc, {ctx_ptr});
                        if (fn->getReturnType()->isVoidTy()) {
                           builder.CreateRetVoid();
                        } else if (!result->getType()->isVoidTy()) {
                           builder.CreateRet(convert_type(result, fn->getReturnType()));
                        } else {
                           builder.CreateRetVoid();
                        }
                     } else {
                        builder.CreateCall(rt_call_depth_inc, {ctx_ptr});
                        builder.CreateRetVoid();
                     }
                  }
                  break;
               }
               case ir_op::tail_call_indirect: {
                  uint32_t type_idx = inst.call.index;
                  llvm::Value* table_elem = nullptr;
                  if (!call_args.empty()) {
                     table_elem = call_args.back();
                     call_args.pop_back();
                  }
                  if (!table_elem) { call_args.clear(); break; }
                  uint32_t nargs = static_cast<uint32_t>(call_args.size());
                  for (uint32_t a = 0; a < nargs; a++) {
                     llvm::Value* slot = builder.CreateGEP(i64_ty, host_args_alloca, builder.getInt32(a));
                     llvm::Value* v = call_args[a];
                     if (v->getType() == i32_ty) v = builder.CreateZExt(v, i64_ty);
                     else if (v->getType() == f32_ty) v = builder.CreateZExt(builder.CreateBitCast(v, i32_ty), i64_ty);
                     else if (v->getType() == f64_ty) v = builder.CreateBitCast(v, i64_ty);
                     builder.CreateStore(v, slot);
                  }
                  call_args.clear();
                  if (table_elem->getType() != i32_ty) {
                     if (table_elem->getType()->isIntegerTy())
                        table_elem = builder.CreateTrunc(table_elem, i32_ty);
                  }
                  // Checked mode: validate read watermark before tail call indirect
                  emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);
                  builder.CreateCall(rt_call_depth_dec, {ctx_ptr});
                  emit_backtrace_set_top(builder, ctx_ptr);
                  emit_backtrace_set_bottom(builder, ctx_ptr);
                  llvm::Value* raw = builder.CreateCall(rt_call_indirect,
                     {ctx_ptr, load_mem_ptr(), builder.getInt32(type_idx),
                      table_elem, host_args_alloca, builder.getInt32(nargs)});
                  emit_backtrace_clear_bottom(builder, ctx_ptr);
                  emit_backtrace_clear_top(builder, ctx_ptr);
                  builder.CreateCall(rt_call_depth_inc, {ctx_ptr});
                  builder.CreateStore(builder.CreateCall(rt_get_memory, {ctx_ptr}), mem_ptr_alloca);
                  // Return
                  if (fn->getReturnType()->isVoidTy()) {
                     builder.CreateRetVoid();
                  } else if (fn->getReturnType() == v128_ty) {
                     // Callee's entry wrapper stores the v128 first-return into
                     // host_args_alloca before returning the scalar low i64.
                     // Reload the full vector to honor this function's v128 ABI.
                     auto* vec = builder.CreateAlignedLoad(v128_ty, host_args_alloca, llvm::Align(8));
                     builder.CreateRet(vec);
                  } else {
                     builder.CreateRet(convert_type(raw, fn->getReturnType()));
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

                  // Determine load type and access size
                  llvm::Type* load_ty;
                  bool sign_extend = false;
                  llvm::Type* result_ty;
                  uint32_t access_size;
                  switch (inst.opcode) {
                     case ir_op::i32_load:    load_ty = i32_ty; result_ty = i32_ty; access_size = 4; break;
                     case ir_op::i64_load:    load_ty = i64_ty; result_ty = i64_ty; access_size = 8; break;
                     case ir_op::f32_load:    load_ty = f32_ty; result_ty = f32_ty; access_size = 4; break;
                     case ir_op::f64_load:    load_ty = f64_ty; result_ty = f64_ty; access_size = 8; break;
                     case ir_op::i32_load8_s: load_ty = i8_ty;  result_ty = i32_ty; access_size = 1; sign_extend = true; break;
                     case ir_op::i32_load8_u: load_ty = i8_ty;  result_ty = i32_ty; access_size = 1; break;
                     case ir_op::i32_load16_s:load_ty = i16_ty; result_ty = i32_ty; access_size = 2; sign_extend = true; break;
                     case ir_op::i32_load16_u:load_ty = i16_ty; result_ty = i32_ty; access_size = 2; break;
                     case ir_op::i64_load8_s: load_ty = i8_ty;  result_ty = i64_ty; access_size = 1; sign_extend = true; break;
                     case ir_op::i64_load8_u: load_ty = i8_ty;  result_ty = i64_ty; access_size = 1; break;
                     case ir_op::i64_load16_s:load_ty = i16_ty; result_ty = i64_ty; access_size = 2; sign_extend = true; break;
                     case ir_op::i64_load16_u:load_ty = i16_ty; result_ty = i64_ty; access_size = 2; break;
                     case ir_op::i64_load32_s:load_ty = i32_ty; result_ty = i64_ty; access_size = 4; sign_extend = true; break;
                     case ir_op::i64_load32_u:load_ty = i32_ty; result_ty = i64_ty; access_size = 4; break;
                     default: load_ty = i32_ty; result_ty = i32_ty; access_size = 4; break;
                  }

                  // Checked mode: deferred watermark (umax, no branch per read).
                  // Validated at loop headers and before host calls.
                  emit_read_watermark_update(builder, eff_addr, access_size, watermark_alloca);

                  llvm::Value* ptr = builder.CreateGEP(i8_ty, load_mem_ptr(), eff_addr);
                  auto* load_inst = builder.CreateLoad(load_ty, ptr);
                  // All modes: loads must be volatile to prevent LLVM from
                  // eliminating OOB accesses that would hit guard pages.
                  load_inst->setVolatile(true);
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

                  // Determine store access size for bounds check
                  uint32_t store_access_size;
                  switch (inst.opcode) {
                     case ir_op::i32_store8:
                     case ir_op::i64_store8:  store_access_size = 1; break;
                     case ir_op::i32_store16:
                     case ir_op::i64_store16: store_access_size = 2; break;
                     case ir_op::i32_store:
                     case ir_op::f32_store:   store_access_size = 4; break;
                     case ir_op::i64_store32: store_access_size = 4; break;
                     case ir_op::i64_store:
                     case ir_op::f64_store:   store_access_size = 8; break;
                     default:                 store_access_size = 4; break;
                  }

                  emit_write_bounds_check(builder, ctx_ptr, load_mem_ptr(),
                                          eff_addr, store_access_size, mem_size_cache);

                  llvm::Value* ptr = builder.CreateGEP(i8_ty, load_mem_ptr(), eff_addr);

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
                  store_inst->setVolatile(true);
                  break;
               }

               // ──── Memory management ────
               case ir_op::memory_size: {
                  llvm::Value* sz = builder.CreateCall(rt_memory_size, {ctx_ptr});
                  store_vreg(inst.dest, sz);
                  break;
               }

               case ir_op::memory_grow: {
                  // rt_memory_grow expects i32 pages; vreg may be stored as i64
                  // (ir_writer widens i32 vregs in some paths), so coerce.
                  llvm::Value* pages = load_vreg_as(inst.rr.src1, i32_ty);
                  if (!pages) break;
                  // Allocate space on stack for the new memory pointer
                  llvm::Value* mem_out = builder.CreateAlloca(ptr_ty);
                  llvm::Value* result = builder.CreateCall(rt_memory_grow,
                     {ctx_ptr, pages, mem_out});
                  // Update mem_ptr alloca — mem2reg will insert PHI nodes
                  llvm::Value* new_mem = builder.CreateLoad(ptr_ty, mem_out);
                  builder.CreateStore(new_mem, mem_ptr_alloca);
                  if (mem_size_cache) {
                     const int32_t ms_off = wasm_allocator::mem_size_offset();
                     llvm::Value* ms_ptr = builder.CreateConstGEP1_64(i8_ty, new_mem,
                        static_cast<int64_t>(ms_off), "grow_ms_ptr");
                     builder.CreateStore(builder.CreateLoad(i64_ty, ms_ptr, "grow_ms"), mem_size_cache);
                  }
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
               case ir_op::i32_shl: {
                  // WASM masks the shift amount (mod 32). LLVM's shl/ashr/lshr
                  // produce poison when the shift amount is >= bitwidth, so we
                  // must mask explicitly — otherwise constant folding can turn
                  // a legal WASM `shl i32 0, 256` into `poison`.
                  auto* amt = builder.CreateAnd(load_vreg_as(inst.rr.src2, i32_ty), builder.getInt32(31));
                  store_vreg(inst.dest, builder.CreateShl(load_vreg_as(inst.rr.src1, i32_ty), amt));
                  break;
               }
               case ir_op::i32_shr_s: {
                  auto* amt = builder.CreateAnd(load_vreg_as(inst.rr.src2, i32_ty), builder.getInt32(31));
                  store_vreg(inst.dest, builder.CreateAShr(load_vreg_as(inst.rr.src1, i32_ty), amt));
                  break;
               }
               case ir_op::i32_shr_u: {
                  auto* amt = builder.CreateAnd(load_vreg_as(inst.rr.src2, i32_ty), builder.getInt32(31));
                  store_vreg(inst.dest, builder.CreateLShr(load_vreg_as(inst.rr.src1, i32_ty), amt));
                  break;
               }
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
               case ir_op::i64_shl: {
                  auto* amt = builder.CreateAnd(load_vreg_as(inst.rr.src2, i64_ty), builder.getInt64(63));
                  store_vreg(inst.dest, builder.CreateShl(load_vreg_as(inst.rr.src1, i64_ty), amt));
                  break;
               }
               case ir_op::i64_shr_s: {
                  auto* amt = builder.CreateAnd(load_vreg_as(inst.rr.src2, i64_ty), builder.getInt64(63));
                  store_vreg(inst.dest, builder.CreateAShr(load_vreg_as(inst.rr.src1, i64_ty), amt));
                  break;
               }
               case ir_op::i64_shr_u: {
                  auto* amt = builder.CreateAnd(load_vreg_as(inst.rr.src2, i64_ty), builder.getInt64(63));
                  store_vreg(inst.dest, builder.CreateLShr(load_vreg_as(inst.rr.src1, i64_ty), amt));
                  break;
               }
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
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_unop(builder, sf_f32_abs,
                        load_vreg_as(inst.rr.src1, f32_ty)));
                  } else {
                     store_vreg(inst.dest, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fabs, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty)}));
                  }
                  break;
               case ir_op::f32_neg:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_unop(builder, sf_f32_neg,
                        load_vreg_as(inst.rr.src1, f32_ty)));
                  } else {
                     store_vreg(inst.dest, builder.CreateFNeg(load_vreg_as(inst.rr.src1, f32_ty)));
                  }
                  break;
               case ir_op::f32_ceil:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_unop(builder, sf_f32_ceil,
                        load_vreg_as(inst.rr.src1, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ceil, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty)})));
                  }
                  break;
               case ir_op::f32_floor:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_unop(builder, sf_f32_floor,
                        load_vreg_as(inst.rr.src1, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::floor, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty)})));
                  }
                  break;
               case ir_op::f32_trunc:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_unop(builder, sf_f32_trunc,
                        load_vreg_as(inst.rr.src1, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::trunc, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty)})));
                  }
                  break;
               case ir_op::f32_nearest:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_unop(builder, sf_f32_nearest,
                        load_vreg_as(inst.rr.src1, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::nearbyint, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty)})));
                  }
                  break;
               case ir_op::f32_sqrt:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_unop(builder, sf_f32_sqrt,
                        load_vreg_as(inst.rr.src1, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sqrt, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty)})));
                  }
                  break;

               // ──── f64 unary ────
               case ir_op::f64_abs:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_unop(builder, sf_f64_abs,
                        load_vreg_as(inst.rr.src1, f64_ty)));
                  } else {
                     store_vreg(inst.dest, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fabs, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty)}));
                  }
                  break;
               case ir_op::f64_neg:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_unop(builder, sf_f64_neg,
                        load_vreg_as(inst.rr.src1, f64_ty)));
                  } else {
                     store_vreg(inst.dest, builder.CreateFNeg(load_vreg_as(inst.rr.src1, f64_ty)));
                  }
                  break;
               case ir_op::f64_ceil:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_unop(builder, sf_f64_ceil,
                        load_vreg_as(inst.rr.src1, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ceil, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty)})));
                  }
                  break;
               case ir_op::f64_floor:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_unop(builder, sf_f64_floor,
                        load_vreg_as(inst.rr.src1, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::floor, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty)})));
                  }
                  break;
               case ir_op::f64_trunc:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_unop(builder, sf_f64_trunc,
                        load_vreg_as(inst.rr.src1, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::trunc, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty)})));
                  }
                  break;
               case ir_op::f64_nearest:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_unop(builder, sf_f64_nearest,
                        load_vreg_as(inst.rr.src1, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::nearbyint, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty)})));
                  }
                  break;
               case ir_op::f64_sqrt:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_unop(builder, sf_f64_sqrt,
                        load_vreg_as(inst.rr.src1, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sqrt, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty)})));
                  }
                  break;

               // ──── f32 binary arithmetic ────
               case ir_op::f32_add:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_binop(builder, sf_f32_add,
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateFAdd(
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty))));
                  }
                  break;
               case ir_op::f32_sub:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_binop(builder, sf_f32_sub,
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateFSub(
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty))));
                  }
                  break;
               case ir_op::f32_mul:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_binop(builder, sf_f32_mul,
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateFMul(
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty))));
                  }
                  break;
               case ir_op::f32_div:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_binop(builder, sf_f32_div,
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateFDiv(
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty))));
                  }
                  break;
               case ir_op::f32_min:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_binop(builder, sf_f32_min,
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::minimum, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)})));
                  }
                  break;
               case ir_op::f32_max:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_binop(builder, sf_f32_max,
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f32(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::maximum, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)})));
                  }
                  break;
               case ir_op::f32_copysign:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f32_binop(builder, sf_f32_copysign,
                        load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)));
                  } else {
                     store_vreg(inst.dest, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::copysign, {f32_ty}),
                        {load_vreg_as(inst.rr.src1, f32_ty), load_vreg_as(inst.rr.src2, f32_ty)}));
                  }
                  break;

               // ──── f64 binary arithmetic ────
               case ir_op::f64_add:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_binop(builder, sf_f64_add,
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateFAdd(
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty))));
                  }
                  break;
               case ir_op::f64_sub:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_binop(builder, sf_f64_sub,
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateFSub(
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty))));
                  }
                  break;
               case ir_op::f64_mul:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_binop(builder, sf_f64_mul,
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateFMul(
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty))));
                  }
                  break;
               case ir_op::f64_div:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_binop(builder, sf_f64_div,
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateFDiv(
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty))));
                  }
                  break;
               case ir_op::f64_min:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_binop(builder, sf_f64_min,
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::minimum, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)})));
                  }
                  break;
               case ir_op::f64_max:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_binop(builder, sf_f64_max,
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  } else {
                     store_vreg(inst.dest, maybe_canon_f64(builder, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::maximum, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)})));
                  }
                  break;
               case ir_op::f64_copysign:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, call_sf_f64_binop(builder, sf_f64_copysign,
                        load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)));
                  } else {
                     store_vreg(inst.dest, builder.CreateCall(
                        llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::copysign, {f64_ty}),
                        {load_vreg_as(inst.rr.src1, f64_ty), load_vreg_as(inst.rr.src2, f64_ty)}));
                  }
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
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f32_convert_i32s, {load_vreg_as(inst.rr.src1, i32_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i32_ty), f32_ty));
                  }
                  break;
               case ir_op::f32_convert_u_i32:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f32_convert_i32u, {load_vreg_as(inst.rr.src1, i32_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i32_ty), f32_ty));
                  }
                  break;
               case ir_op::f32_convert_s_i64:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f32_convert_i64s, {load_vreg_as(inst.rr.src1, i64_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i64_ty), f32_ty));
                  }
                  break;
               case ir_op::f32_convert_u_i64:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f32_convert_i64u, {load_vreg_as(inst.rr.src1, i64_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i64_ty), f32_ty));
                  }
                  break;
               case ir_op::f64_convert_s_i32:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f64_convert_i32s, {load_vreg_as(inst.rr.src1, i32_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i32_ty), f64_ty));
                  }
                  break;
               case ir_op::f64_convert_u_i32:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f64_convert_i32u, {load_vreg_as(inst.rr.src1, i32_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i32_ty), f64_ty));
                  }
                  break;
               case ir_op::f64_convert_s_i64:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f64_convert_i64s, {load_vreg_as(inst.rr.src1, i64_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateSIToFP(load_vreg_as(inst.rr.src1, i64_ty), f64_ty));
                  }
                  break;
               case ir_op::f64_convert_u_i64:
                  if ((opts.fp == fp_mode::softfloat)) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f64_convert_i64u, {load_vreg_as(inst.rr.src1, i64_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateUIToFP(load_vreg_as(inst.rr.src1, i64_ty), f64_ty));
                  }
                  break;

               // Float promotions/demotions. Route through the softfloat helper in
               // both softfloat and hw_deterministic modes: the interpreter uses the
               // softfloat promote/demote for any `fp != fast` mode (interpret_visitor.hpp),
               // and the resulting NaN payload is not equivalent to native FPExt/FPTrunc
               // + NaN canonicalization. Matching the interpreter bit-for-bit requires
               // calling the same softfloat helper here. `fast` mode keeps native ops.
               case ir_op::f32_demote_f64:
                  if (opts.fp != fp_mode::fast) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f32_demote_f64, {load_vreg_as(inst.rr.src1, f64_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateFPTrunc(load_vreg_as(inst.rr.src1, f64_ty), f32_ty));
                  }
                  break;
               case ir_op::f64_promote_f32:
                  if (opts.fp != fp_mode::fast) {
                     store_vreg(inst.dest, builder.CreateCall(sf_f64_promote_f32, {load_vreg_as(inst.rr.src1, f32_ty)}));
                  } else {
                     store_vreg(inst.dest, builder.CreateFPExt(load_vreg_as(inst.rr.src1, f32_ty), f64_ty));
                  }
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
                     auto* dest = convert_type(call_args[0], i32_ty);
                     auto* src  = convert_type(call_args[1], i32_ty);
                     auto* n    = convert_type(call_args[2], i32_ty);
                     builder.CreateCall(rt_memory_init,
                        {ctx_ptr, builder.getInt32(inst.ri.imm), dest, src, n});
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
                     auto* dest = convert_type(call_args[0], i32_ty);
                     auto* src  = convert_type(call_args[1], i32_ty);
                     auto* n    = convert_type(call_args[2], i32_ty);
                     builder.CreateCall(rt_memory_copy, {ctx_ptr, dest, src, n});
                  }
                  call_args.clear();
                  break;
               }

               case ir_op::memory_fill: {
                  if (call_args.size() >= 3) {
                     auto* dest = convert_type(call_args[0], i32_ty);
                     auto* val  = convert_type(call_args[1], i32_ty);
                     auto* n    = convert_type(call_args[2], i32_ty);
                     builder.CreateCall(rt_memory_fill, {ctx_ptr, dest, val, n});
                  }
                  call_args.clear();
                  break;
               }

               case ir_op::table_init: {
                  if (call_args.size() >= 3) {
                     uint32_t elem_idx = static_cast<uint32_t>(inst.ri.imm) & 0xFFFF;
                     uint32_t table_idx = static_cast<uint32_t>(inst.ri.imm) >> 16;
                     auto* dest = convert_type(call_args[0], i32_ty);
                     auto* src  = convert_type(call_args[1], i32_ty);
                     auto* n    = convert_type(call_args[2], i32_ty);
                     builder.CreateCall(rt_table_init,
                        {ctx_ptr, builder.getInt32(elem_idx),
                         dest, src, n,
                         builder.getInt32(table_idx)});
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
                     uint32_t dst_table = static_cast<uint32_t>(inst.ri.imm) & 0xFFFF;
                     uint32_t src_table = static_cast<uint32_t>(inst.ri.imm) >> 16;
                     auto* dest = convert_type(call_args[0], i32_ty);
                     auto* src  = convert_type(call_args[1], i32_ty);
                     auto* n    = convert_type(call_args[2], i32_ty);
                     builder.CreateCall(rt_table_copy,
                        {ctx_ptr, dest, src, n,
                         builder.getInt32(dst_table), builder.getInt32(src_table)});
                  }
                  call_args.clear();
                  break;
               }

               case ir_op::table_get: {
                  // Operand comes via preceding arg instruction
                  if (call_args.size() >= 1) {
                     auto* idx = convert_type(call_args[0], i32_ty);
                     auto* result = builder.CreateCall(rt_table_get,
                        {ctx_ptr, builder.getInt32(static_cast<uint32_t>(inst.ri.imm)), idx});
                     store_vreg(inst.dest, result);
                  }
                  call_args.clear();
                  break;
               }
               case ir_op::table_set: {
                  // Operands come via preceding arg instructions: [idx, val]
                  if (call_args.size() >= 2) {
                     auto* idx = convert_type(call_args[0], i32_ty);
                     auto* val = convert_type(call_args[1], i32_ty);
                     builder.CreateCall(rt_table_set,
                        {ctx_ptr, builder.getInt32(static_cast<uint32_t>(inst.ri.imm)),
                         idx, val});
                  }
                  call_args.clear();
                  break;
               }
               case ir_op::table_grow: {
                  // Operands come via preceding arg instructions: [init_val, delta]
                  if (call_args.size() >= 2) {
                     auto* init_val = convert_type(call_args[0], i32_ty);
                     auto* delta = convert_type(call_args[1], i32_ty);
                     auto* result = builder.CreateCall(rt_table_grow,
                        {ctx_ptr, builder.getInt32(static_cast<uint32_t>(inst.ri.imm)),
                         delta, init_val});
                     store_vreg(inst.dest, result);
                  }
                  call_args.clear();
                  break;
               }
               case ir_op::table_size: {
                  auto* result = builder.CreateCall(rt_table_size,
                     {ctx_ptr, builder.getInt32(static_cast<uint32_t>(inst.ri.imm))});
                  store_vreg(inst.dest, result);
                  break;
               }
               case ir_op::table_fill: {
                  if (call_args.size() >= 3) {
                     auto* i = convert_type(call_args[0], i32_ty);
                     auto* val = convert_type(call_args[1], i32_ty);
                     auto* n = convert_type(call_args[2], i32_ty);
                     builder.CreateCall(rt_table_fill,
                        {ctx_ptr, builder.getInt32(static_cast<uint32_t>(inst.ri.imm)),
                         i, val, n});
                  }
                  call_args.clear();
                  break;
               }

               // ──── Multi-value return store ────
               case ir_op::multi_return_store: {
                  int32_t offset = 24 + inst.ri.imm; // multi_return_offset = 24
                  if (inst.type == types::v128) {
                     // Load v128 from separate slot, split into low/high i64s,
                     // store packed: low at offset, high at offset+8.
                     auto* vec = load_v128(static_cast<uint16_t>(inst.ri.src1),
                                           v128_ty);
                     if (!vec) break;
                     auto* lo = builder.CreateExtractElement(vec, builder.getInt32(0));
                     auto* hi = builder.CreateExtractElement(vec, builder.getInt32(1));
                     auto* lo_ptr = builder.CreateGEP(builder.getInt8Ty(), ctx_ptr,
                        builder.getInt32(offset));
                     auto* hi_ptr = builder.CreateGEP(builder.getInt8Ty(), ctx_ptr,
                        builder.getInt32(offset + 8));
                     auto* lo_typed = builder.CreateBitCast(lo_ptr,
                        llvm::PointerType::getUnqual(builder.getInt64Ty()));
                     auto* hi_typed = builder.CreateBitCast(hi_ptr,
                        llvm::PointerType::getUnqual(builder.getInt64Ty()));
                     builder.CreateStore(lo, lo_typed);
                     builder.CreateStore(hi, hi_typed);
                     break;
                  }
                  // In unreachable code the source vreg may not have been
                  // allocated; skip the store in that case (the op won't
                  // actually execute at runtime).
                  auto* val = load_vreg(inst.ri.src1);
                  if (!val) break;
                  // _multi_return slots are uint64_t. Widen the vreg's raw
                  // bits to i64 (size-preserving bitcast for float types,
                  // zext for narrower ints) rather than letting CreateStore
                  // emit a size-mismatched bitcast.
                  auto* t = val->getType();
                  llvm::Value* val_i64 = val;
                  if (t == f32_ty) {
                     val_i64 = builder.CreateZExt(builder.CreateBitCast(val, i32_ty), i64_ty);
                  } else if (t == f64_ty) {
                     val_i64 = builder.CreateBitCast(val, i64_ty);
                  } else if (t == i32_ty) {
                     val_i64 = builder.CreateZExt(val, i64_ty);
                  } else if (t != i64_ty) {
                     // Unknown type — best-effort zero
                     val_i64 = llvm::ConstantInt::get(i64_ty, 0);
                  }
                  auto* ptr = builder.CreateGEP(builder.getInt8Ty(), ctx_ptr,
                     builder.getInt32(offset));
                  auto* typed_ptr = builder.CreateBitCast(ptr,
                     llvm::PointerType::getUnqual(builder.getInt64Ty()));
                  builder.CreateStore(val_i64, typed_ptr);
                  break;
               }

               // ──── Multi-value call return load ────
               case ir_op::multi_return_load: {
                  int32_t offset = 24 + inst.ri.imm; // multi_return_offset = 24
                  if (inst.type == types::v128) {
                     // Load packed low/high i64s from offset and offset+8, combine
                     // into v128 vector, store to v128 slot.
                     auto* lo_ptr = builder.CreateGEP(builder.getInt8Ty(), ctx_ptr,
                        builder.getInt32(offset));
                     auto* hi_ptr = builder.CreateGEP(builder.getInt8Ty(), ctx_ptr,
                        builder.getInt32(offset + 8));
                     auto* lo_typed = builder.CreateBitCast(lo_ptr,
                        llvm::PointerType::getUnqual(builder.getInt64Ty()));
                     auto* hi_typed = builder.CreateBitCast(hi_ptr,
                        llvm::PointerType::getUnqual(builder.getInt64Ty()));
                     auto* lo = builder.CreateLoad(builder.getInt64Ty(), lo_typed);
                     auto* hi = builder.CreateLoad(builder.getInt64Ty(), hi_typed);
                     llvm::Value* vec = llvm::UndefValue::get(v128_ty);
                     vec = builder.CreateInsertElement(vec, lo, builder.getInt32(0));
                     vec = builder.CreateInsertElement(vec, hi, builder.getInt32(1));
                     store_v128(static_cast<uint16_t>(inst.dest), vec);
                     break;
                  }
                  auto* ptr = builder.CreateGEP(builder.getInt8Ty(), ctx_ptr,
                     builder.getInt32(offset));
                  auto* typed_ptr = builder.CreateBitCast(ptr,
                     llvm::PointerType::getUnqual(builder.getInt64Ty()));
                  auto* val = builder.CreateLoad(builder.getInt64Ty(), typed_ptr);
                  store_vreg(inst.dest, val);
                  break;
               }

               // ──── SIMD: v128 constant ────
               case ir_op::const_v128: {
                  // immv128 stores {low, high} as two i64s
                  llvm::Constant* lo = builder.getInt64(inst.immv128.low);
                  llvm::Constant* hi = builder.getInt64(inst.immv128.high);
                  llvm::Value* vec = llvm::ConstantVector::get({lo, hi});
                  store_v128(static_cast<uint16_t>(inst.dest), vec);
                  break;
               }

               // ──── SIMD: all v128 operations ────
               case ir_op::v128_op: {
                  auto sub = static_cast<simd_sub>(inst.dest);

                  // ── Memory loads ──
                  switch (sub) {
                  case simd_sub::v128_load: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 16, false);
                     auto* ld = builder.CreateAlignedLoad(v128_ty, ptr, llvm::Align(1));
                     ld->setVolatile(true);
                     store_v128(inst.simd.v_dest, ld);
                     break;
                  }
                  case simd_sub::v128_store: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 16, true);
                     auto* val = load_v128(inst.simd.v_src1, v128_ty);
                     auto* st = builder.CreateAlignedStore(val, ptr, llvm::Align(1));
                     st->setVolatile(true);
                     break;
                  }
                  case simd_sub::v128_load8x8_s: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* v8 = builder.CreateAlignedLoad(llvm::FixedVectorType::get(i8_ty, 8), ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(v8)->setVolatile(true);
                     auto* ext = builder.CreateSExt(v8, v8xi16_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(ext, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load8x8_u: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* v8 = builder.CreateAlignedLoad(llvm::FixedVectorType::get(i8_ty, 8), ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(v8)->setVolatile(true);
                     auto* ext = builder.CreateZExt(v8, v8xi16_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(ext, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load16x4_s: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* v4 = builder.CreateAlignedLoad(llvm::FixedVectorType::get(i16_ty, 4), ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(v4)->setVolatile(true);
                     auto* ext = builder.CreateSExt(v4, v4xi32_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(ext, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load16x4_u: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* v4 = builder.CreateAlignedLoad(llvm::FixedVectorType::get(i16_ty, 4), ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(v4)->setVolatile(true);
                     auto* ext = builder.CreateZExt(v4, v4xi32_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(ext, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load32x2_s: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* v2 = builder.CreateAlignedLoad(llvm::FixedVectorType::get(i32_ty, 2), ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(v2)->setVolatile(true);
                     auto* ext = builder.CreateSExt(v2, v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(ext, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load32x2_u: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* v2 = builder.CreateAlignedLoad(llvm::FixedVectorType::get(i32_ty, 2), ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(v2)->setVolatile(true);
                     auto* ext = builder.CreateZExt(v2, v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(ext, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load8_splat: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 1, false);
                     auto* scalar = builder.CreateLoad(i8_ty, ptr);
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     llvm::Value* vec = builder.CreateVectorSplat(16, scalar);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(vec, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load16_splat: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 2, false);
                     auto* scalar = builder.CreateAlignedLoad(i16_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     llvm::Value* vec = builder.CreateVectorSplat(8, scalar);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(vec, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load32_splat: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 4, false);
                     auto* scalar = builder.CreateAlignedLoad(i32_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     llvm::Value* vec = builder.CreateVectorSplat(4, scalar);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(vec, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load64_splat: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* scalar = builder.CreateAlignedLoad(i64_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     llvm::Value* vec = builder.CreateVectorSplat(2, scalar);
                     store_v128(inst.simd.v_dest, vec);
                     break;
                  }
                  case simd_sub::v128_load32_zero: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 4, false);
                     auto* scalar = builder.CreateAlignedLoad(i32_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     auto* zero = llvm::Constant::getNullValue(v4xi32_ty);
                     auto* vec = builder.CreateInsertElement(zero, scalar, builder.getInt32(0));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(vec, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load64_zero: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* scalar = builder.CreateAlignedLoad(i64_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     auto* zero = llvm::Constant::getNullValue(v2xi64_ty);
                     auto* vec = builder.CreateInsertElement(zero, scalar, builder.getInt32(0));
                     store_v128(inst.simd.v_dest, vec);
                     break;
                  }
                  case simd_sub::v128_load8_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 1, false);
                     auto* scalar = builder.CreateLoad(i8_ty, ptr);
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     auto* vec = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* result = builder.CreateInsertElement(vec, scalar, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load16_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 2, false);
                     auto* scalar = builder.CreateAlignedLoad(i16_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     auto* vec = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* result = builder.CreateInsertElement(vec, scalar, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load32_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 4, false);
                     auto* scalar = builder.CreateAlignedLoad(i32_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     auto* vec = load_v128(inst.simd.v_src1, v4xi32_ty);
                     auto* result = builder.CreateInsertElement(vec, scalar, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::v128_load64_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, false);
                     auto* scalar = builder.CreateAlignedLoad(i64_ty, ptr, llvm::Align(1));
                     cast<llvm::LoadInst>(scalar)->setVolatile(true);
                     auto* vec = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* result = builder.CreateInsertElement(vec, scalar, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, result);
                     break;
                  }
                  case simd_sub::v128_store8_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 1, true);
                     auto* vec = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* scalar = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     auto* st = builder.CreateStore(scalar, ptr);
                     st->setVolatile(true);
                     break;
                  }
                  case simd_sub::v128_store16_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 2, true);
                     auto* vec = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* scalar = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     auto* st = builder.CreateAlignedStore(scalar, ptr, llvm::Align(1));
                     st->setVolatile(true);
                     break;
                  }
                  case simd_sub::v128_store32_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 4, true);
                     auto* vec = load_v128(inst.simd.v_src1, v4xi32_ty);
                     auto* scalar = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     auto* st = builder.CreateAlignedStore(scalar, ptr, llvm::Align(1));
                     st->setVolatile(true);
                     break;
                  }
                  case simd_sub::v128_store64_lane: {
                     auto* ptr = simd_mem_addr(inst.simd.addr, inst.simd.offset, 8, true);
                     auto* vec = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* scalar = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     auto* st = builder.CreateAlignedStore(scalar, ptr, llvm::Align(1));
                     st->setVolatile(true);
                     break;
                  }

                  // ── Shuffle/Swizzle ──
                  case simd_sub::i8x16_shuffle: {
                     // Shuffle uses immv128 overlay (not simd struct) for the 16 mask bytes.
                     // Two preceding arg instructions pushed v128 sources into call_args.
                     llvm::Value* src1_raw = nullptr;
                     llvm::Value* src2_raw = nullptr;
                     if (call_args.size() >= 2) {
                        src1_raw = call_args[call_args.size() - 2];
                        src2_raw = call_args[call_args.size() - 1];
                        call_args.resize(call_args.size() - 2);
                     }
                     auto to_v16i8 = [&](llvm::Value* v) -> llvm::Value* {
                        if (!v) return llvm::Constant::getNullValue(v16xi8_ty);
                        if (v->getType() == v16xi8_ty) return v;
                        if (v->getType()->isVectorTy() || v->getType() == v128_ty)
                           return builder.CreateBitCast(v, v16xi8_ty);
                        // Scalar i64 vreg — load from its alloca as v128
                        return llvm::Constant::getNullValue(v16xi8_ty);
                     };
                     auto* a = to_v16i8(src1_raw);
                     auto* b = to_v16i8(src2_raw);
                     // Read 16 mask bytes from immv128
                     uint8_t mask_bytes[16];
                     std::memcpy(mask_bytes, &inst.immv128, 16);
                     int mask[16];
                     for (int j = 0; j < 16; j++)
                        mask[j] = static_cast<int>(mask_bytes[j]);
                     auto* result = builder.CreateShuffleVector(a, b, llvm::ArrayRef<int>(mask, 16));
                     // Can't use simd.v_dest (immv128 overlay) — store for next nop marker
                     pending_v128_result = builder.CreateBitCast(result, v128_ty);
                     break;
                  }
                  case simd_sub::i8x16_swizzle: {
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* idx = load_v128(inst.simd.v_src2, v16xi8_ty);
                     // WASM swizzle: for each lane, if index >= 16, result is 0
                     // Use LLVM intrinsic or manual implementation
                     // Manual: clamp indices, do shuffle, then zero out-of-range lanes
                     auto* sixteen = builder.CreateVectorSplat(16, builder.getInt8(16));
                     auto* oob = builder.CreateICmpUGE(idx, sixteen);
                     // Use intrinsic: aarch64.neon.tbl1 or x86 pshufb
                     // Portable approach: element-by-element extraction
                     // Actually, we can use @llvm.experimental.vector.extract or build manually.
                     // Simplest portable: alloca, store, indexed load
                     auto* arr = builder.CreateAlloca(v16xi8_ty, nullptr, "swiz_src");
                     builder.CreateStore(a, arr);
                     auto* arr_ptr = builder.CreateBitCast(arr, ptr_ty);
                     llvm::Value* result = llvm::Constant::getNullValue(v16xi8_ty);
                     for (int j = 0; j < 16; j++) {
                        auto* lane_idx = builder.CreateExtractElement(idx, builder.getInt32(j));
                        auto* clamped = builder.CreateAnd(lane_idx, builder.getInt8(15));
                        auto* elem_ptr = builder.CreateGEP(i8_ty, arr_ptr, builder.CreateZExt(clamped, i32_ty));
                        auto* elem = builder.CreateLoad(i8_ty, elem_ptr);
                        auto* is_oob = builder.CreateExtractElement(oob, builder.getInt32(j));
                        auto* val = builder.CreateSelect(is_oob, builder.getInt8(0), elem);
                        result = builder.CreateInsertElement(result, val, builder.getInt32(j));
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── Extract lane ──
                  case simd_sub::i8x16_extract_lane_s: {
                     auto* vec = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, builder.CreateZExt(builder.CreateSExt(elem, i32_ty), i64_ty));
                     break;
                  }
                  case simd_sub::i8x16_extract_lane_u: {
                     auto* vec = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, builder.CreateZExt(elem, i64_ty));
                     break;
                  }
                  case simd_sub::i16x8_extract_lane_s: {
                     auto* vec = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, builder.CreateZExt(builder.CreateSExt(elem, i32_ty), i64_ty));
                     break;
                  }
                  case simd_sub::i16x8_extract_lane_u: {
                     auto* vec = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, builder.CreateZExt(elem, i64_ty));
                     break;
                  }
                  case simd_sub::i32x4_extract_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v4xi32_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, builder.CreateZExt(elem, i64_ty));
                     break;
                  }
                  case simd_sub::i64x2_extract_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, elem);
                     break;
                  }
                  case simd_sub::f32x4_extract_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v4xf32_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, builder.CreateBitCast(elem, i32_ty));
                     break;
                  }
                  case simd_sub::f64x2_extract_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v2xf64_ty);
                     auto* elem = builder.CreateExtractElement(vec, builder.getInt32(inst.simd.lane));
                     store_vreg(inst.simd.addr, builder.CreateBitCast(elem, i64_ty));
                     break;
                  }

                  // ── Replace lane ──
                  case simd_sub::i8x16_replace_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* scalar = load_vreg(inst.simd.offset); // offset holds scalar vreg
                     auto* elem = convert_type(scalar, i8_ty);
                     auto* result = builder.CreateInsertElement(vec, elem, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i16x8_replace_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* scalar = load_vreg(inst.simd.offset);
                     auto* elem = convert_type(scalar, i16_ty);
                     auto* result = builder.CreateInsertElement(vec, elem, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i32x4_replace_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v4xi32_ty);
                     auto* scalar = load_vreg(inst.simd.offset);
                     auto* elem = convert_type(scalar, i32_ty);
                     auto* result = builder.CreateInsertElement(vec, elem, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i64x2_replace_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* scalar = load_vreg(inst.simd.offset);
                     auto* result = builder.CreateInsertElement(vec, scalar, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, result);
                     break;
                  }
                  case simd_sub::f32x4_replace_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v4xf32_ty);
                     auto* scalar = load_vreg(inst.simd.offset);
                     auto* elem = convert_type(scalar, f32_ty);
                     auto* result = builder.CreateInsertElement(vec, elem, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::f64x2_replace_lane: {
                     auto* vec = load_v128(inst.simd.v_src1, v2xf64_ty);
                     auto* scalar = load_vreg(inst.simd.offset);
                     auto* elem = convert_type(scalar, f64_ty);
                     auto* result = builder.CreateInsertElement(vec, elem, builder.getInt32(inst.simd.lane));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── Splat ──
                  case simd_sub::i8x16_splat: {
                     auto* scalar = load_vreg(inst.simd.addr);
                     auto* elem = convert_type(scalar, i8_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateVectorSplat(16, elem), v128_ty));
                     break;
                  }
                  case simd_sub::i16x8_splat: {
                     auto* scalar = load_vreg(inst.simd.addr);
                     auto* elem = convert_type(scalar, i16_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateVectorSplat(8, elem), v128_ty));
                     break;
                  }
                  case simd_sub::i32x4_splat: {
                     auto* scalar = load_vreg(inst.simd.addr);
                     auto* elem = convert_type(scalar, i32_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateVectorSplat(4, elem), v128_ty));
                     break;
                  }
                  case simd_sub::i64x2_splat: {
                     auto* scalar = load_vreg(inst.simd.addr);
                     auto* elem = convert_type(scalar, i64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateVectorSplat(2, elem), v128_ty));
                     break;
                  }
                  case simd_sub::f32x4_splat: {
                     auto* scalar = load_vreg(inst.simd.addr);
                     auto* f = convert_type(scalar, f32_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateVectorSplat(4, f), v128_ty));
                     break;
                  }
                  case simd_sub::f64x2_splat: {
                     auto* scalar = load_vreg(inst.simd.addr);
                     auto* f = convert_type(scalar, f64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateVectorSplat(2, f), v128_ty));
                     break;
                  }

                  // ── Logical ──
                  case simd_sub::v128_not: {
                     auto* a = load_v128(inst.simd.v_src1, v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateNot(a));
                     break;
                  }
                  case simd_sub::v128_and: {
                     auto* a = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* b = load_v128(inst.simd.v_src2, v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateAnd(a, b));
                     break;
                  }
                  case simd_sub::v128_andnot: {
                     auto* a = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* b = load_v128(inst.simd.v_src2, v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateAnd(a, builder.CreateNot(b)));
                     break;
                  }
                  case simd_sub::v128_or: {
                     auto* a = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* b = load_v128(inst.simd.v_src2, v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateOr(a, b));
                     break;
                  }
                  case simd_sub::v128_xor: {
                     auto* a = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* b = load_v128(inst.simd.v_src2, v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateXor(a, b));
                     break;
                  }
                  case simd_sub::v128_bitselect: {
                     // WASM bitselect: result = (val1 & mask) | (val2 & ~mask)
                     // IR layout: v_src1=val1, v_src2=val2, addr=mask vreg
                     auto* val1 = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* val2 = load_v128(inst.simd.v_src2, v2xi64_ty);
                     auto* mask = load_v128(static_cast<uint16_t>(inst.simd.addr), v2xi64_ty);
                     store_v128(inst.simd.v_dest, builder.CreateOr(
                        builder.CreateAnd(val1, mask),
                        builder.CreateAnd(val2, builder.CreateNot(mask))));
                     break;
                  }
                  case simd_sub::v128_any_true: {
                     auto* vec = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* or_val = builder.CreateOr(
                        builder.CreateExtractElement(vec, builder.getInt32(0)),
                        builder.CreateExtractElement(vec, builder.getInt32(1)));
                     auto* cmp = builder.CreateICmpNE(or_val, builder.getInt64(0));
                     store_vreg(inst.simd.addr, builder.CreateZExt(cmp, i64_ty));
                     break;
                  }

                  // ── Integer comparisons ──
                  // i8x16
                  case simd_sub::i8x16_eq: case simd_sub::i8x16_ne:
                  case simd_sub::i8x16_lt_s: case simd_sub::i8x16_lt_u:
                  case simd_sub::i8x16_gt_s: case simd_sub::i8x16_gt_u:
                  case simd_sub::i8x16_le_s: case simd_sub::i8x16_le_u:
                  case simd_sub::i8x16_ge_s: case simd_sub::i8x16_ge_u:
                  // i16x8
                  case simd_sub::i16x8_eq: case simd_sub::i16x8_ne:
                  case simd_sub::i16x8_lt_s: case simd_sub::i16x8_lt_u:
                  case simd_sub::i16x8_gt_s: case simd_sub::i16x8_gt_u:
                  case simd_sub::i16x8_le_s: case simd_sub::i16x8_le_u:
                  case simd_sub::i16x8_ge_s: case simd_sub::i16x8_ge_u:
                  // i32x4
                  case simd_sub::i32x4_eq: case simd_sub::i32x4_ne:
                  case simd_sub::i32x4_lt_s: case simd_sub::i32x4_lt_u:
                  case simd_sub::i32x4_gt_s: case simd_sub::i32x4_gt_u:
                  case simd_sub::i32x4_le_s: case simd_sub::i32x4_le_u:
                  case simd_sub::i32x4_ge_s: case simd_sub::i32x4_ge_u:
                  // i64x2
                  case simd_sub::i64x2_eq: case simd_sub::i64x2_ne:
                  case simd_sub::i64x2_lt_s: case simd_sub::i64x2_gt_s:
                  case simd_sub::i64x2_le_s: case simd_sub::i64x2_ge_s: {
                     // Determine vector type based on sub-opcode
                     llvm::VectorType* vty;
                     if (sub >= simd_sub::i8x16_eq && sub <= simd_sub::i8x16_ge_u) vty = v16xi8_ty;
                     else if (sub >= simd_sub::i16x8_eq && sub <= simd_sub::i16x8_ge_u) vty = v8xi16_ty;
                     else if (sub >= simd_sub::i32x4_eq && sub <= simd_sub::i32x4_ge_u) vty = v4xi32_ty;
                     else vty = v2xi64_ty;

                     auto* a = load_v128(inst.simd.v_src1, vty);
                     auto* b = load_v128(inst.simd.v_src2, vty);
                     llvm::Value* cmp;
                     switch (sub) {
                     case simd_sub::i8x16_eq: case simd_sub::i16x8_eq:
                     case simd_sub::i32x4_eq: case simd_sub::i64x2_eq:
                        cmp = builder.CreateICmpEQ(a, b); break;
                     case simd_sub::i8x16_ne: case simd_sub::i16x8_ne:
                     case simd_sub::i32x4_ne: case simd_sub::i64x2_ne:
                        cmp = builder.CreateICmpNE(a, b); break;
                     case simd_sub::i8x16_lt_s: case simd_sub::i16x8_lt_s:
                     case simd_sub::i32x4_lt_s: case simd_sub::i64x2_lt_s:
                        cmp = builder.CreateICmpSLT(a, b); break;
                     case simd_sub::i8x16_lt_u: case simd_sub::i16x8_lt_u:
                     case simd_sub::i32x4_lt_u:
                        cmp = builder.CreateICmpULT(a, b); break;
                     case simd_sub::i8x16_gt_s: case simd_sub::i16x8_gt_s:
                     case simd_sub::i32x4_gt_s: case simd_sub::i64x2_gt_s:
                        cmp = builder.CreateICmpSGT(a, b); break;
                     case simd_sub::i8x16_gt_u: case simd_sub::i16x8_gt_u:
                     case simd_sub::i32x4_gt_u:
                        cmp = builder.CreateICmpUGT(a, b); break;
                     case simd_sub::i8x16_le_s: case simd_sub::i16x8_le_s:
                     case simd_sub::i32x4_le_s: case simd_sub::i64x2_le_s:
                        cmp = builder.CreateICmpSLE(a, b); break;
                     case simd_sub::i8x16_le_u: case simd_sub::i16x8_le_u:
                     case simd_sub::i32x4_le_u:
                        cmp = builder.CreateICmpULE(a, b); break;
                     case simd_sub::i8x16_ge_s: case simd_sub::i16x8_ge_s:
                     case simd_sub::i32x4_ge_s: case simd_sub::i64x2_ge_s:
                        cmp = builder.CreateICmpSGE(a, b); break;
                     case simd_sub::i8x16_ge_u: case simd_sub::i16x8_ge_u:
                     case simd_sub::i32x4_ge_u:
                        cmp = builder.CreateICmpUGE(a, b); break;
                     default: cmp = builder.CreateICmpEQ(a, b); break;
                     }
                     // WASM: comparison results are all-ones (-1) for true, all-zeros for false
                     auto* result = builder.CreateSExt(cmp, vty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── Float comparisons ──
                  case simd_sub::f32x4_eq: case simd_sub::f32x4_ne:
                  case simd_sub::f32x4_lt: case simd_sub::f32x4_gt:
                  case simd_sub::f32x4_le: case simd_sub::f32x4_ge:
                  case simd_sub::f64x2_eq: case simd_sub::f64x2_ne:
                  case simd_sub::f64x2_lt: case simd_sub::f64x2_gt:
                  case simd_sub::f64x2_le: case simd_sub::f64x2_ge: {
                     bool is_f64 = (sub >= simd_sub::f64x2_eq && sub <= simd_sub::f64x2_ge);
                     auto* fvty = is_f64 ? (llvm::VectorType*)v2xf64_ty : (llvm::VectorType*)v4xf32_ty;
                     auto* ivty = is_f64 ? (llvm::VectorType*)v2xi64_ty : (llvm::VectorType*)v4xi32_ty;
                     auto* a = load_v128(inst.simd.v_src1, fvty);
                     auto* b = load_v128(inst.simd.v_src2, fvty);
                     llvm::Value* cmp;
                     switch (sub) {
                     case simd_sub::f32x4_eq: case simd_sub::f64x2_eq:
                        cmp = builder.CreateFCmpOEQ(a, b); break;
                     case simd_sub::f32x4_ne: case simd_sub::f64x2_ne:
                        cmp = builder.CreateFCmpUNE(a, b); break;
                     case simd_sub::f32x4_lt: case simd_sub::f64x2_lt:
                        cmp = builder.CreateFCmpOLT(a, b); break;
                     case simd_sub::f32x4_gt: case simd_sub::f64x2_gt:
                        cmp = builder.CreateFCmpOGT(a, b); break;
                     case simd_sub::f32x4_le: case simd_sub::f64x2_le:
                        cmp = builder.CreateFCmpOLE(a, b); break;
                     case simd_sub::f32x4_ge: case simd_sub::f64x2_ge:
                        cmp = builder.CreateFCmpOGE(a, b); break;
                     default: cmp = builder.CreateFCmpOEQ(a, b); break;
                     }
                     auto* result = builder.CreateSExt(cmp, ivty);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── Integer arithmetic: abs, neg ──
                  case simd_sub::i8x16_abs: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* neg = builder.CreateNeg(a); auto* cmp = builder.CreateICmpSLT(a, llvm::Constant::getNullValue(v16xi8_ty)); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSelect(cmp, neg, a), v128_ty)); break; }
                  case simd_sub::i16x8_abs: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* neg = builder.CreateNeg(a); auto* cmp = builder.CreateICmpSLT(a, llvm::Constant::getNullValue(v8xi16_ty)); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSelect(cmp, neg, a), v128_ty)); break; }
                  case simd_sub::i32x4_abs: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* neg = builder.CreateNeg(a); auto* cmp = builder.CreateICmpSLT(a, llvm::Constant::getNullValue(v4xi32_ty)); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSelect(cmp, neg, a), v128_ty)); break; }
                  case simd_sub::i64x2_abs: { auto* a = load_v128(inst.simd.v_src1, v2xi64_ty); auto* neg = builder.CreateNeg(a); auto* cmp = builder.CreateICmpSLT(a, llvm::Constant::getNullValue(v2xi64_ty)); store_v128(inst.simd.v_dest, builder.CreateSelect(cmp, neg, a)); break; }
                  case simd_sub::i8x16_neg: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateNeg(a), v128_ty)); break; }
                  case simd_sub::i16x8_neg: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateNeg(a), v128_ty)); break; }
                  case simd_sub::i32x4_neg: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateNeg(a), v128_ty)); break; }
                  case simd_sub::i64x2_neg: { auto* a = load_v128(inst.simd.v_src1, v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateNeg(a)); break; }

                  // ── i8x16 popcnt ──
                  case simd_sub::i8x16_popcnt: {
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* intrinsic = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ctpop, {v16xi8_ty});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(intrinsic, {a}), v128_ty));
                     break;
                  }

                  // ── all_true, bitmask ──
                  case simd_sub::i8x16_all_true: {
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* zero = llvm::Constant::getNullValue(v16xi8_ty);
                     auto* cmp = builder.CreateICmpNE(a, zero);
                     // Reduce AND all lanes
                     llvm::Value* result = builder.CreateExtractElement(cmp, builder.getInt32(0));
                     for (int j = 1; j < 16; j++)
                        result = builder.CreateAnd(result, builder.CreateExtractElement(cmp, builder.getInt32(j)));
                     store_vreg(inst.simd.addr, builder.CreateZExt(result, i64_ty));
                     break;
                  }
                  case simd_sub::i16x8_all_true: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* zero = llvm::Constant::getNullValue(v8xi16_ty);
                     auto* cmp = builder.CreateICmpNE(a, zero);
                     llvm::Value* result = builder.CreateExtractElement(cmp, builder.getInt32(0));
                     for (int j = 1; j < 8; j++)
                        result = builder.CreateAnd(result, builder.CreateExtractElement(cmp, builder.getInt32(j)));
                     store_vreg(inst.simd.addr, builder.CreateZExt(result, i64_ty));
                     break;
                  }
                  case simd_sub::i32x4_all_true: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     auto* zero = llvm::Constant::getNullValue(v4xi32_ty);
                     auto* cmp = builder.CreateICmpNE(a, zero);
                     llvm::Value* result = builder.CreateExtractElement(cmp, builder.getInt32(0));
                     for (int j = 1; j < 4; j++)
                        result = builder.CreateAnd(result, builder.CreateExtractElement(cmp, builder.getInt32(j)));
                     store_vreg(inst.simd.addr, builder.CreateZExt(result, i64_ty));
                     break;
                  }
                  case simd_sub::i64x2_all_true: {
                     auto* a = load_v128(inst.simd.v_src1, v2xi64_ty);
                     auto* zero = llvm::Constant::getNullValue(v2xi64_ty);
                     auto* cmp = builder.CreateICmpNE(a, zero);
                     auto* r = builder.CreateAnd(
                        builder.CreateExtractElement(cmp, builder.getInt32(0)),
                        builder.CreateExtractElement(cmp, builder.getInt32(1)));
                     store_vreg(inst.simd.addr, builder.CreateZExt(r, i64_ty));
                     break;
                  }
                  case simd_sub::i8x16_bitmask: {
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     llvm::Value* mask = builder.getInt32(0);
                     for (int j = 0; j < 16; j++) {
                        auto* elem = builder.CreateExtractElement(a, builder.getInt32(j));
                        auto* bit = builder.CreateICmpSLT(elem, builder.getInt8(0));
                        auto* shifted = builder.CreateShl(builder.CreateZExt(bit, i32_ty), builder.getInt32(j));
                        mask = builder.CreateOr(mask, shifted);
                     }
                     store_vreg(inst.simd.addr, builder.CreateZExt(mask, i64_ty));
                     break;
                  }
                  case simd_sub::i16x8_bitmask: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     llvm::Value* mask = builder.getInt32(0);
                     for (int j = 0; j < 8; j++) {
                        auto* elem = builder.CreateExtractElement(a, builder.getInt32(j));
                        auto* bit = builder.CreateICmpSLT(elem, builder.getInt16(0));
                        auto* shifted = builder.CreateShl(builder.CreateZExt(bit, i32_ty), builder.getInt32(j));
                        mask = builder.CreateOr(mask, shifted);
                     }
                     store_vreg(inst.simd.addr, builder.CreateZExt(mask, i64_ty));
                     break;
                  }
                  case simd_sub::i32x4_bitmask: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     llvm::Value* mask = builder.getInt32(0);
                     for (int j = 0; j < 4; j++) {
                        auto* elem = builder.CreateExtractElement(a, builder.getInt32(j));
                        auto* bit = builder.CreateICmpSLT(elem, builder.getInt32(0));
                        auto* shifted = builder.CreateShl(builder.CreateZExt(bit, i32_ty), builder.getInt32(j));
                        mask = builder.CreateOr(mask, shifted);
                     }
                     store_vreg(inst.simd.addr, builder.CreateZExt(mask, i64_ty));
                     break;
                  }
                  case simd_sub::i64x2_bitmask: {
                     auto* a = load_v128(inst.simd.v_src1, v2xi64_ty);
                     llvm::Value* mask = builder.getInt32(0);
                     for (int j = 0; j < 2; j++) {
                        auto* elem = builder.CreateExtractElement(a, builder.getInt32(j));
                        auto* bit = builder.CreateICmpSLT(elem, builder.getInt64(0));
                        auto* shifted = builder.CreateShl(builder.CreateZExt(bit, i32_ty), builder.getInt32(j));
                        mask = builder.CreateOr(mask, shifted);
                     }
                     store_vreg(inst.simd.addr, builder.CreateZExt(mask, i64_ty));
                     break;
                  }

                  // ── Narrow ──
                  case simd_sub::i8x16_narrow_i16x8_s: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* b = load_v128(inst.simd.v_src2, v8xi16_ty);
                     // Saturate each i16 to [-128,127] then truncate
                     auto* lo = builder.getInt16(-128); auto* hi = builder.getInt16(127);
                     auto* lo_v = builder.CreateVectorSplat(8, lo); auto* hi_v = builder.CreateVectorSplat(8, hi);
                     auto* clamped_a = builder.CreateSelect(builder.CreateICmpSLT(a, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(a, hi_v), hi_v, a));
                     auto* clamped_b = builder.CreateSelect(builder.CreateICmpSLT(b, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(b, hi_v), hi_v, b));
                     auto* trunc_a = builder.CreateTrunc(clamped_a, llvm::FixedVectorType::get(i8_ty, 8));
                     auto* trunc_b = builder.CreateTrunc(clamped_b, llvm::FixedVectorType::get(i8_ty, 8));
                     auto* result = builder.CreateShuffleVector(trunc_a, trunc_b, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i8x16_narrow_i16x8_u: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* b = load_v128(inst.simd.v_src2, v8xi16_ty);
                     auto* lo = builder.getInt16(0); auto* hi = builder.getInt16(255);
                     auto* lo_v = builder.CreateVectorSplat(8, lo); auto* hi_v = builder.CreateVectorSplat(8, hi);
                     auto* clamped_a = builder.CreateSelect(builder.CreateICmpSLT(a, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(a, hi_v), hi_v, a));
                     auto* clamped_b = builder.CreateSelect(builder.CreateICmpSLT(b, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(b, hi_v), hi_v, b));
                     auto* trunc_a = builder.CreateTrunc(clamped_a, llvm::FixedVectorType::get(i8_ty, 8));
                     auto* trunc_b = builder.CreateTrunc(clamped_b, llvm::FixedVectorType::get(i8_ty, 8));
                     auto* result = builder.CreateShuffleVector(trunc_a, trunc_b, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i16x8_narrow_i32x4_s: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     auto* b = load_v128(inst.simd.v_src2, v4xi32_ty);
                     auto* lo_v = builder.CreateVectorSplat(4, builder.getInt32(-32768));
                     auto* hi_v = builder.CreateVectorSplat(4, builder.getInt32(32767));
                     auto* clamped_a = builder.CreateSelect(builder.CreateICmpSLT(a, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(a, hi_v), hi_v, a));
                     auto* clamped_b = builder.CreateSelect(builder.CreateICmpSLT(b, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(b, hi_v), hi_v, b));
                     auto* trunc_a = builder.CreateTrunc(clamped_a, llvm::FixedVectorType::get(i16_ty, 4));
                     auto* trunc_b = builder.CreateTrunc(clamped_b, llvm::FixedVectorType::get(i16_ty, 4));
                     auto* result = builder.CreateShuffleVector(trunc_a, trunc_b, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i16x8_narrow_i32x4_u: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     auto* b = load_v128(inst.simd.v_src2, v4xi32_ty);
                     auto* lo_v = builder.CreateVectorSplat(4, builder.getInt32(0));
                     auto* hi_v = builder.CreateVectorSplat(4, builder.getInt32(65535));
                     auto* clamped_a = builder.CreateSelect(builder.CreateICmpSLT(a, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(a, hi_v), hi_v, a));
                     auto* clamped_b = builder.CreateSelect(builder.CreateICmpSLT(b, lo_v), lo_v, builder.CreateSelect(builder.CreateICmpSGT(b, hi_v), hi_v, b));
                     auto* trunc_a = builder.CreateTrunc(clamped_a, llvm::FixedVectorType::get(i16_ty, 4));
                     auto* trunc_b = builder.CreateTrunc(clamped_b, llvm::FixedVectorType::get(i16_ty, 4));
                     auto* result = builder.CreateShuffleVector(trunc_a, trunc_b, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── Shifts ──
                  case simd_sub::i8x16_shl: case simd_sub::i8x16_shr_s: case simd_sub::i8x16_shr_u:
                  case simd_sub::i16x8_shl: case simd_sub::i16x8_shr_s: case simd_sub::i16x8_shr_u:
                  case simd_sub::i32x4_shl: case simd_sub::i32x4_shr_s: case simd_sub::i32x4_shr_u:
                  case simd_sub::i64x2_shl: case simd_sub::i64x2_shr_s: case simd_sub::i64x2_shr_u: {
                     llvm::VectorType* vty;
                     unsigned lanes, bits;
                     if (sub >= simd_sub::i8x16_shl && sub <= simd_sub::i8x16_shr_u) { vty = v16xi8_ty; lanes = 16; bits = 8; }
                     else if (sub >= simd_sub::i16x8_shl && sub <= simd_sub::i16x8_shr_u) { vty = v8xi16_ty; lanes = 8; bits = 16; }
                     else if (sub >= simd_sub::i32x4_shl && sub <= simd_sub::i32x4_shr_u) { vty = v4xi32_ty; lanes = 4; bits = 32; }
                     else { vty = v2xi64_ty; lanes = 2; bits = 64; }

                     auto* vec = load_v128(inst.simd.v_src1, vty);
                     auto* shift_scalar = load_vreg(inst.simd.offset); // shift amount vreg
                     // WASM: shift amount is mod lane_bits
                     auto* elem_ty = vty->getElementType();
                     auto* shift_trunc = convert_type(shift_scalar, elem_ty);
                     auto* mod_val = llvm::ConstantInt::get(elem_ty, bits);
                     auto* shift_mod = builder.CreateURem(shift_trunc, mod_val);
                     auto* shift_vec = builder.CreateVectorSplat(lanes, shift_mod);

                     llvm::Value* result;
                     bool is_shl = (sub == simd_sub::i8x16_shl || sub == simd_sub::i16x8_shl ||
                                    sub == simd_sub::i32x4_shl || sub == simd_sub::i64x2_shl);
                     bool is_shr_s = (sub == simd_sub::i8x16_shr_s || sub == simd_sub::i16x8_shr_s ||
                                      sub == simd_sub::i32x4_shr_s || sub == simd_sub::i64x2_shr_s);
                     if (is_shl) result = builder.CreateShl(vec, shift_vec);
                     else if (is_shr_s) result = builder.CreateAShr(vec, shift_vec);
                     else result = builder.CreateLShr(vec, shift_vec);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── Integer add/sub/mul ──
                  case simd_sub::i8x16_add: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateAdd(a, b), v128_ty)); break; }
                  case simd_sub::i16x8_add: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateAdd(a, b), v128_ty)); break; }
                  case simd_sub::i32x4_add: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateAdd(a, b), v128_ty)); break; }
                  case simd_sub::i64x2_add: { auto* a = load_v128(inst.simd.v_src1, v2xi64_ty); auto* b = load_v128(inst.simd.v_src2, v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateAdd(a, b)); break; }
                  case simd_sub::i8x16_sub: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSub(a, b), v128_ty)); break; }
                  case simd_sub::i16x8_sub: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSub(a, b), v128_ty)); break; }
                  case simd_sub::i32x4_sub: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSub(a, b), v128_ty)); break; }
                  case simd_sub::i64x2_sub: { auto* a = load_v128(inst.simd.v_src1, v2xi64_ty); auto* b = load_v128(inst.simd.v_src2, v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateSub(a, b)); break; }
                  case simd_sub::i16x8_mul: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(a, b), v128_ty)); break; }
                  case simd_sub::i32x4_mul: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(a, b), v128_ty)); break; }
                  case simd_sub::i64x2_mul: { auto* a = load_v128(inst.simd.v_src1, v2xi64_ty); auto* b = load_v128(inst.simd.v_src2, v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateMul(a, b)); break; }

                  // ── Saturating add/sub ──
                  case simd_sub::i8x16_add_sat_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sadd_sat, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i8x16_add_sat_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::uadd_sat, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i8x16_sub_sat_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ssub_sat, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i8x16_sub_sat_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::usub_sat, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_add_sat_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sadd_sat, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_add_sat_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::uadd_sat, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_sub_sat_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ssub_sat, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_sub_sat_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::usub_sat, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }

                  // ── Min/Max ──
                  case simd_sub::i8x16_min_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::smin, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i8x16_min_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::umin, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i8x16_max_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::smax, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i8x16_max_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::umax, {v16xi8_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_min_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::smin, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_min_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::umin, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_max_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::smax, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i16x8_max_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::umax, {v8xi16_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i32x4_min_s: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::smin, {v4xi32_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i32x4_min_u: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::umin, {v4xi32_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i32x4_max_s: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::smax, {v4xi32_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }
                  case simd_sub::i32x4_max_u: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::umax, {v4xi32_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b}), v128_ty)); break; }

                  // ── avgr_u ──
                  case simd_sub::i8x16_avgr_u: {
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty);
                     auto* wide_ty = llvm::FixedVectorType::get(i16_ty, 16);
                     auto* wa = builder.CreateZExt(a, wide_ty); auto* wb = builder.CreateZExt(b, wide_ty);
                     auto* sum = builder.CreateAdd(builder.CreateAdd(wa, wb), builder.CreateVectorSplat(16, builder.getInt16(1)));
                     auto* avg = builder.CreateLShr(sum, builder.CreateVectorSplat(16, builder.getInt16(1)));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateTrunc(avg, v16xi8_ty), v128_ty));
                     break;
                  }
                  case simd_sub::i16x8_avgr_u: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty);
                     auto* wide_ty = llvm::FixedVectorType::get(i32_ty, 8);
                     auto* wa = builder.CreateZExt(a, wide_ty); auto* wb = builder.CreateZExt(b, wide_ty);
                     auto* sum = builder.CreateAdd(builder.CreateAdd(wa, wb), builder.CreateVectorSplat(8, builder.getInt32(1)));
                     auto* avg = builder.CreateLShr(sum, builder.CreateVectorSplat(8, builder.getInt32(1)));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateTrunc(avg, v8xi16_ty), v128_ty));
                     break;
                  }

                  // ── Extend (widen) low/high ──
                  case simd_sub::i16x8_extend_low_i8x16_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSExt(half, v8xi16_ty), v128_ty)); break; }
                  case simd_sub::i16x8_extend_high_i8x16_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{8,9,10,11,12,13,14,15}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSExt(half, v8xi16_ty), v128_ty)); break; }
                  case simd_sub::i16x8_extend_low_i8x16_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateZExt(half, v8xi16_ty), v128_ty)); break; }
                  case simd_sub::i16x8_extend_high_i8x16_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{8,9,10,11,12,13,14,15}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateZExt(half, v8xi16_ty), v128_ty)); break; }
                  case simd_sub::i32x4_extend_low_i16x8_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSExt(half, v4xi32_ty), v128_ty)); break; }
                  case simd_sub::i32x4_extend_high_i16x8_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{4,5,6,7}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSExt(half, v4xi32_ty), v128_ty)); break; }
                  case simd_sub::i32x4_extend_low_i16x8_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateZExt(half, v4xi32_ty), v128_ty)); break; }
                  case simd_sub::i32x4_extend_high_i16x8_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{4,5,6,7}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateZExt(half, v4xi32_ty), v128_ty)); break; }
                  case simd_sub::i64x2_extend_low_i32x4_s: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1}); store_v128(inst.simd.v_dest, builder.CreateSExt(half, v2xi64_ty)); break; }
                  case simd_sub::i64x2_extend_high_i32x4_s: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{2,3}); store_v128(inst.simd.v_dest, builder.CreateSExt(half, v2xi64_ty)); break; }
                  case simd_sub::i64x2_extend_low_i32x4_u: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1}); store_v128(inst.simd.v_dest, builder.CreateZExt(half, v2xi64_ty)); break; }
                  case simd_sub::i64x2_extend_high_i32x4_u: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* half = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{2,3}); store_v128(inst.simd.v_dest, builder.CreateZExt(half, v2xi64_ty)); break; }

                  // ── Extended multiply ──
                  case simd_sub::i16x8_extmul_low_i8x16_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* al = builder.CreateSExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7}), v8xi16_ty); auto* bl = builder.CreateSExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7}), v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(al, bl), v128_ty)); break; }
                  case simd_sub::i16x8_extmul_high_i8x16_s: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* ah = builder.CreateSExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{8,9,10,11,12,13,14,15}), v8xi16_ty); auto* bh = builder.CreateSExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{8,9,10,11,12,13,14,15}), v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(ah, bh), v128_ty)); break; }
                  case simd_sub::i16x8_extmul_low_i8x16_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* al = builder.CreateZExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7}), v8xi16_ty); auto* bl = builder.CreateZExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{0,1,2,3,4,5,6,7}), v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(al, bl), v128_ty)); break; }
                  case simd_sub::i16x8_extmul_high_i8x16_u: { auto* a = load_v128(inst.simd.v_src1, v16xi8_ty); auto* b = load_v128(inst.simd.v_src2, v16xi8_ty); auto* ah = builder.CreateZExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{8,9,10,11,12,13,14,15}), v8xi16_ty); auto* bh = builder.CreateZExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{8,9,10,11,12,13,14,15}), v8xi16_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(ah, bh), v128_ty)); break; }
                  case simd_sub::i32x4_extmul_low_i16x8_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* al = builder.CreateSExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3}), v4xi32_ty); auto* bl = builder.CreateSExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{0,1,2,3}), v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(al, bl), v128_ty)); break; }
                  case simd_sub::i32x4_extmul_high_i16x8_s: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* ah = builder.CreateSExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{4,5,6,7}), v4xi32_ty); auto* bh = builder.CreateSExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{4,5,6,7}), v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(ah, bh), v128_ty)); break; }
                  case simd_sub::i32x4_extmul_low_i16x8_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* al = builder.CreateZExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1,2,3}), v4xi32_ty); auto* bl = builder.CreateZExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{0,1,2,3}), v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(al, bl), v128_ty)); break; }
                  case simd_sub::i32x4_extmul_high_i16x8_u: { auto* a = load_v128(inst.simd.v_src1, v8xi16_ty); auto* b = load_v128(inst.simd.v_src2, v8xi16_ty); auto* ah = builder.CreateZExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{4,5,6,7}), v4xi32_ty); auto* bh = builder.CreateZExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{4,5,6,7}), v4xi32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateMul(ah, bh), v128_ty)); break; }
                  case simd_sub::i64x2_extmul_low_i32x4_s: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* al = builder.CreateSExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1}), v2xi64_ty); auto* bl = builder.CreateSExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{0,1}), v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateMul(al, bl)); break; }
                  case simd_sub::i64x2_extmul_high_i32x4_s: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* ah = builder.CreateSExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{2,3}), v2xi64_ty); auto* bh = builder.CreateSExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{2,3}), v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateMul(ah, bh)); break; }
                  case simd_sub::i64x2_extmul_low_i32x4_u: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* al = builder.CreateZExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1}), v2xi64_ty); auto* bl = builder.CreateZExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{0,1}), v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateMul(al, bl)); break; }
                  case simd_sub::i64x2_extmul_high_i32x4_u: { auto* a = load_v128(inst.simd.v_src1, v4xi32_ty); auto* b = load_v128(inst.simd.v_src2, v4xi32_ty); auto* ah = builder.CreateZExt(builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{2,3}), v2xi64_ty); auto* bh = builder.CreateZExt(builder.CreateShuffleVector(b, b, llvm::ArrayRef<int>{2,3}), v2xi64_ty); store_v128(inst.simd.v_dest, builder.CreateMul(ah, bh)); break; }

                  // ── Extended add pairwise ──
                  case simd_sub::i16x8_extadd_pairwise_i8x16_s: {
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* wide = builder.CreateSExt(a, llvm::FixedVectorType::get(i16_ty, 16));
                     llvm::Value* result = llvm::Constant::getNullValue(v8xi16_ty);
                     for (int j = 0; j < 8; j++) {
                        auto* e0 = builder.CreateExtractElement(wide, builder.getInt32(j*2));
                        auto* e1 = builder.CreateExtractElement(wide, builder.getInt32(j*2+1));
                        result = builder.CreateInsertElement(result, builder.CreateAdd(e0, e1), builder.getInt32(j));
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i16x8_extadd_pairwise_i8x16_u: {
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* wide = builder.CreateZExt(a, llvm::FixedVectorType::get(i16_ty, 16));
                     llvm::Value* result = llvm::Constant::getNullValue(v8xi16_ty);
                     for (int j = 0; j < 8; j++) {
                        auto* e0 = builder.CreateExtractElement(wide, builder.getInt32(j*2));
                        auto* e1 = builder.CreateExtractElement(wide, builder.getInt32(j*2+1));
                        result = builder.CreateInsertElement(result, builder.CreateAdd(e0, e1), builder.getInt32(j));
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i32x4_extadd_pairwise_i16x8_s: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* wide = builder.CreateSExt(a, llvm::FixedVectorType::get(i32_ty, 8));
                     llvm::Value* result = llvm::Constant::getNullValue(v4xi32_ty);
                     for (int j = 0; j < 4; j++) {
                        auto* e0 = builder.CreateExtractElement(wide, builder.getInt32(j*2));
                        auto* e1 = builder.CreateExtractElement(wide, builder.getInt32(j*2+1));
                        result = builder.CreateInsertElement(result, builder.CreateAdd(e0, e1), builder.getInt32(j));
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i32x4_extadd_pairwise_i16x8_u: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* wide = builder.CreateZExt(a, llvm::FixedVectorType::get(i32_ty, 8));
                     llvm::Value* result = llvm::Constant::getNullValue(v4xi32_ty);
                     for (int j = 0; j < 4; j++) {
                        auto* e0 = builder.CreateExtractElement(wide, builder.getInt32(j*2));
                        auto* e1 = builder.CreateExtractElement(wide, builder.getInt32(j*2+1));
                        result = builder.CreateInsertElement(result, builder.CreateAdd(e0, e1), builder.getInt32(j));
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── q15mulr_sat_s ──
                  case simd_sub::i16x8_q15mulr_sat_s: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* b = load_v128(inst.simd.v_src2, v8xi16_ty);
                     auto* wide_ty = llvm::FixedVectorType::get(i32_ty, 8);
                     auto* wa = builder.CreateSExt(a, wide_ty);
                     auto* wb = builder.CreateSExt(b, wide_ty);
                     auto* prod = builder.CreateAdd(builder.CreateMul(wa, wb), builder.CreateVectorSplat(8, builder.getInt32(0x4000)));
                     auto* shifted = builder.CreateAShr(prod, builder.CreateVectorSplat(8, builder.getInt32(15)));
                     // Saturate to i16 range
                     auto* min_v = builder.CreateVectorSplat(8, builder.getInt32(-32768));
                     auto* max_v = builder.CreateVectorSplat(8, builder.getInt32(32767));
                     auto* clamped = builder.CreateSelect(builder.CreateICmpSLT(shifted, min_v), min_v,
                        builder.CreateSelect(builder.CreateICmpSGT(shifted, max_v), max_v, shifted));
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateTrunc(clamped, v8xi16_ty), v128_ty));
                     break;
                  }

                  // ── dot product ──
                  case simd_sub::i32x4_dot_i16x8_s: {
                     auto* a = load_v128(inst.simd.v_src1, v8xi16_ty);
                     auto* b = load_v128(inst.simd.v_src2, v8xi16_ty);
                     auto* wa = builder.CreateSExt(a, llvm::FixedVectorType::get(i32_ty, 8));
                     auto* wb = builder.CreateSExt(b, llvm::FixedVectorType::get(i32_ty, 8));
                     auto* prod = builder.CreateMul(wa, wb);
                     llvm::Value* result = llvm::Constant::getNullValue(v4xi32_ty);
                     for (int j = 0; j < 4; j++) {
                        auto* e0 = builder.CreateExtractElement(prod, builder.getInt32(j*2));
                        auto* e1 = builder.CreateExtractElement(prod, builder.getInt32(j*2+1));
                        result = builder.CreateInsertElement(result, builder.CreateAdd(e0, e1), builder.getInt32(j));
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  // ── Float arithmetic ──
                  // In softfloat mode, scalarize lane-wise through scalar sf_* helpers.
                  // That guarantees bit-identical results to the scalar softfloat backend.
                  case simd_sub::f32x4_abs: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_unop(builder, sf_f32_abs, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fabs, {v4xf32_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::f64x2_abs: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_unop(builder, sf_f64_abs, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fabs, {v2xf64_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::f32x4_neg: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f32x4_unop(builder, sf_f32_neg, a) : (llvm::Value*)builder.CreateFNeg(a);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::f64x2_neg: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f64x2_unop(builder, sf_f64_neg, a) : (llvm::Value*)builder.CreateFNeg(a);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::f32x4_sqrt: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_unop(builder, sf_f32_sqrt, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sqrt, {v4xf32_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_sqrt: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_unop(builder, sf_f64_sqrt, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sqrt, {v2xf64_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }

                  case simd_sub::f32x4_add: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f32x4_binop(builder, sf_f32_add, a, b) : (llvm::Value*)builder.CreateFAdd(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_add: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f64x2_binop(builder, sf_f64_add, a, b) : (llvm::Value*)builder.CreateFAdd(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_sub: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f32x4_binop(builder, sf_f32_sub, a, b) : (llvm::Value*)builder.CreateFSub(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_sub: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f64x2_binop(builder, sf_f64_sub, a, b) : (llvm::Value*)builder.CreateFSub(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_mul: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f32x4_binop(builder, sf_f32_mul, a, b) : (llvm::Value*)builder.CreateFMul(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_mul: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f64x2_binop(builder, sf_f64_mul, a, b) : (llvm::Value*)builder.CreateFMul(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_div: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f32x4_binop(builder, sf_f32_div, a, b) : (llvm::Value*)builder.CreateFDiv(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_div: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     llvm::Value* r = (opts.fp == fp_mode::softfloat) ? simd_sf_f64x2_binop(builder, sf_f64_div, a, b) : (llvm::Value*)builder.CreateFDiv(a, b);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }

                  case simd_sub::f32x4_min: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_binop(builder, sf_f32_min, a, b);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::minimum, {v4xf32_ty}); r = builder.CreateCall(fn, {a, b}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_min: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_binop(builder, sf_f64_min, a, b);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::minimum, {v2xf64_ty}); r = builder.CreateCall(fn, {a, b}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_max: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_binop(builder, sf_f32_max, a, b);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::maximum, {v4xf32_ty}); r = builder.CreateCall(fn, {a, b}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_max: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_binop(builder, sf_f64_max, a, b);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::maximum, {v2xf64_ty}); r = builder.CreateCall(fn, {a, b}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_pmin: { auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSelect(builder.CreateFCmpOLT(b, a), b, a), v128_ty)); break; }
                  case simd_sub::f64x2_pmin: { auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSelect(builder.CreateFCmpOLT(b, a), b, a), v128_ty)); break; }
                  case simd_sub::f32x4_pmax: { auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* b = load_v128(inst.simd.v_src2, v4xf32_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSelect(builder.CreateFCmpOLT(a, b), b, a), v128_ty)); break; }
                  case simd_sub::f64x2_pmax: { auto* a = load_v128(inst.simd.v_src1, v2xf64_ty); auto* b = load_v128(inst.simd.v_src2, v2xf64_ty); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateSelect(builder.CreateFCmpOLT(a, b), b, a), v128_ty)); break; }

                  // ── Float rounding ──
                  case simd_sub::f32x4_ceil: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_unop(builder, sf_f32_ceil, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ceil, {v4xf32_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_ceil: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_unop(builder, sf_f64_ceil, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::ceil, {v2xf64_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_floor: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_unop(builder, sf_f32_floor, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::floor, {v4xf32_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_floor: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_unop(builder, sf_f64_floor, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::floor, {v2xf64_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_trunc: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_unop(builder, sf_f32_trunc, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::trunc, {v4xf32_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_trunc: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_unop(builder, sf_f64_trunc, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::trunc, {v2xf64_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f32x4_nearest: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f32x4_unop(builder, sf_f32_nearest, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::nearbyint, {v4xf32_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v4xf32(builder, r), v128_ty)); break;
                  }
                  case simd_sub::f64x2_nearest: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) r = simd_sf_f64x2_unop(builder, sf_f64_nearest, a);
                     else { auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::nearbyint, {v2xf64_ty}); r = builder.CreateCall(fn, {a}); }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(maybe_canon_v2xf64(builder, r), v128_ty)); break;
                  }

                  // ── Conversions ──
                  case simd_sub::i32x4_trunc_sat_f32x4_s: { auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptosi_sat, {v4xi32_ty, v4xf32_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a}), v128_ty)); break; }
                  case simd_sub::i32x4_trunc_sat_f32x4_u: { auto* a = load_v128(inst.simd.v_src1, v4xf32_ty); auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptoui_sat, {v4xi32_ty, v4xf32_ty}); store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a}), v128_ty)); break; }
                  case simd_sub::f32x4_convert_i32x4_s: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) {
                        r = llvm::UndefValue::get(v4xf32_ty);
                        for (int j = 0; j < 4; ++j) {
                           auto* idx = builder.getInt32(j);
                           auto* ae = builder.CreateExtractElement(a, idx);
                           auto* fb = builder.CreateCall(sf_f32_convert_i32s, {ae});
                           r = builder.CreateInsertElement(r, fb, idx);
                        }
                     } else {
                        r = builder.CreateSIToFP(a, v4xf32_ty);
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::f32x4_convert_i32x4_u: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) {
                        r = llvm::UndefValue::get(v4xf32_ty);
                        for (int j = 0; j < 4; ++j) {
                           auto* idx = builder.getInt32(j);
                           auto* ae = builder.CreateExtractElement(a, idx);
                           auto* fb = builder.CreateCall(sf_f32_convert_i32u, {ae});
                           r = builder.CreateInsertElement(r, fb, idx);
                        }
                     } else {
                        r = builder.CreateUIToFP(a, v4xf32_ty);
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::i32x4_trunc_sat_f64x2_s_zero: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptosi_sat, {llvm::FixedVectorType::get(i32_ty, 2), v2xf64_ty});
                     auto* trunc2 = builder.CreateCall(fn, {a});
                     auto* zero2 = llvm::Constant::getNullValue(llvm::FixedVectorType::get(i32_ty, 2));
                     auto* result = builder.CreateShuffleVector(trunc2, zero2, llvm::ArrayRef<int>{0,1,2,3});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i32x4_trunc_sat_f64x2_u_zero: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fptoui_sat, {llvm::FixedVectorType::get(i32_ty, 2), v2xf64_ty});
                     auto* trunc2 = builder.CreateCall(fn, {a});
                     auto* zero2 = llvm::Constant::getNullValue(llvm::FixedVectorType::get(i32_ty, 2));
                     auto* result = builder.CreateShuffleVector(trunc2, zero2, llvm::ArrayRef<int>{0,1,2,3});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::f64x2_convert_low_i32x4_s: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) {
                        r = llvm::UndefValue::get(v2xf64_ty);
                        for (int j = 0; j < 2; ++j) {
                           auto* idx = builder.getInt32(j);
                           auto* ae = builder.CreateExtractElement(a, idx);
                           auto* fb = builder.CreateCall(sf_f64_convert_i32s, {ae});
                           r = builder.CreateInsertElement(r, fb, idx);
                        }
                     } else {
                        auto* low2 = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1});
                        r = builder.CreateSIToFP(low2, v2xf64_ty);
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::f64x2_convert_low_i32x4_u: {
                     auto* a = load_v128(inst.simd.v_src1, v4xi32_ty);
                     llvm::Value* r;
                     if (opts.fp == fp_mode::softfloat) {
                        r = llvm::UndefValue::get(v2xf64_ty);
                        for (int j = 0; j < 2; ++j) {
                           auto* idx = builder.getInt32(j);
                           auto* ae = builder.CreateExtractElement(a, idx);
                           auto* fb = builder.CreateCall(sf_f64_convert_i32u, {ae});
                           r = builder.CreateInsertElement(r, fb, idx);
                        }
                     } else {
                        auto* low2 = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1});
                        r = builder.CreateUIToFP(low2, v2xf64_ty);
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(r, v128_ty)); break;
                  }
                  case simd_sub::f32x4_demote_f64x2_zero: {
                     // NaN-payload-sensitive: route through the softfloat helper
                     // in any non-`fast` mode so results match the interpreter
                     // bit-for-bit. Hardware fptrunc canonicalizes NaN payloads
                     // to 0x7fc00000, which diverges from softfloat's f64_to_f32.
                     // When the softfloat helper is used, skip maybe_canon_v4xf32
                     // — the helper already produces the WASM-spec NaN payload
                     // and canonicalization would overwrite it (matches scalar
                     // f32_demote_f64 handling above).
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     llvm::Value* r;
                     bool used_sf = (opts.fp != fp_mode::fast);
                     if (used_sf) {
                        r = llvm::ConstantVector::getSplat(llvm::ElementCount::getFixed(4),
                              llvm::ConstantFP::get(f32_ty, 0.0));
                        for (int j = 0; j < 2; ++j) {
                           auto* idx = builder.getInt32(j);
                           auto* ae = builder.CreateExtractElement(a, idx);
                           auto* fb = builder.CreateCall(sf_f32_demote_f64, {ae});
                           r = builder.CreateInsertElement(r, fb, idx);
                        }
                     } else {
                        auto* narrow2 = builder.CreateFPTrunc(a, llvm::FixedVectorType::get(f32_ty, 2));
                        auto* zero2 = llvm::Constant::getNullValue(llvm::FixedVectorType::get(f32_ty, 2));
                        r = builder.CreateShuffleVector(narrow2, zero2, llvm::ArrayRef<int>{0,1,2,3});
                     }
                     llvm::Value* final_r = used_sf ? r : maybe_canon_v4xf32(builder, r);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(final_r, v128_ty)); break;
                  }
                  case simd_sub::f64x2_promote_low_f32x4: {
                     // NaN-payload-sensitive: same reasoning as f32x4_demote_f64x2_zero.
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     llvm::Value* r;
                     bool used_sf = (opts.fp != fp_mode::fast);
                     if (used_sf) {
                        r = llvm::UndefValue::get(v2xf64_ty);
                        for (int j = 0; j < 2; ++j) {
                           auto* idx = builder.getInt32(j);
                           auto* ae = builder.CreateExtractElement(a, idx);
                           auto* fb = builder.CreateCall(sf_f64_promote_f32, {ae});
                           r = builder.CreateInsertElement(r, fb, idx);
                        }
                     } else {
                        auto* low2 = builder.CreateShuffleVector(a, a, llvm::ArrayRef<int>{0,1});
                        r = builder.CreateFPExt(low2, v2xf64_ty);
                     }
                     llvm::Value* final_r = used_sf ? r : maybe_canon_v2xf64(builder, r);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(final_r, v128_ty)); break;
                  }

                  // ── Relaxed SIMD ──
                  case simd_sub::f32x4_relaxed_madd: {
                     // relaxed_madd: a * b + c. Third operand c is in addr.
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     auto* c = load_v128(static_cast<uint16_t>(inst.simd.addr), v4xf32_ty);
                     auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fma, {v4xf32_ty});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b, c}), v128_ty));
                     break;
                  }
                  case simd_sub::f32x4_relaxed_nmadd: {
                     auto* a = load_v128(inst.simd.v_src1, v4xf32_ty);
                     auto* b = load_v128(inst.simd.v_src2, v4xf32_ty);
                     auto* c = load_v128(static_cast<uint16_t>(inst.simd.addr), v4xf32_ty);
                     auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fma, {v4xf32_ty});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {builder.CreateFNeg(a), b, c}), v128_ty));
                     break;
                  }
                  case simd_sub::f64x2_relaxed_madd: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     auto* c = load_v128(static_cast<uint16_t>(inst.simd.addr), v2xf64_ty);
                     auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fma, {v2xf64_ty});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {a, b, c}), v128_ty));
                     break;
                  }
                  case simd_sub::f64x2_relaxed_nmadd: {
                     auto* a = load_v128(inst.simd.v_src1, v2xf64_ty);
                     auto* b = load_v128(inst.simd.v_src2, v2xf64_ty);
                     auto* c = load_v128(static_cast<uint16_t>(inst.simd.addr), v2xf64_ty);
                     auto* fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::fma, {v2xf64_ty});
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(builder.CreateCall(fn, {builder.CreateFNeg(a), b, c}), v128_ty));
                     break;
                  }
                  case simd_sub::i16x8_relaxed_dot_i8x16_i7x16_s: {
                     // Relaxed: multiply i8 lanes, add pairwise to i16
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* b = load_v128(inst.simd.v_src2, v16xi8_ty);
                     auto* wa = builder.CreateSExt(a, llvm::FixedVectorType::get(i16_ty, 16));
                     // b is unsigned (i7x16)
                     auto* wb = builder.CreateZExt(b, llvm::FixedVectorType::get(i16_ty, 16));
                     auto* prod = builder.CreateMul(wa, wb);
                     llvm::Value* result = llvm::Constant::getNullValue(v8xi16_ty);
                     for (int j = 0; j < 8; j++) {
                        auto* e0 = builder.CreateExtractElement(prod, builder.getInt32(j*2));
                        auto* e1 = builder.CreateExtractElement(prod, builder.getInt32(j*2+1));
                        auto* sum = builder.CreateAdd(e0, e1);
                        // Saturate to i16 range
                        auto* sat_fn = llvm::Intrinsic::getOrInsertDeclaration(llvm_mod.get(), llvm::Intrinsic::sadd_sat, {i16_ty});
                        (void)sat_fn;
                        result = builder.CreateInsertElement(result, sum, builder.getInt32(j));
                     }
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }
                  case simd_sub::i32x4_relaxed_dot_i8x16_i7x16_add_s: {
                     // Relaxed: multiply i8*u8 pairwise to i16, then add pairs to i32, then add accumulator (c)
                     auto* a = load_v128(inst.simd.v_src1, v16xi8_ty);
                     auto* b = load_v128(inst.simd.v_src2, v16xi8_ty);
                     auto* c = load_v128(static_cast<uint16_t>(inst.simd.addr), v4xi32_ty);
                     auto* wa = builder.CreateSExt(a, llvm::FixedVectorType::get(i32_ty, 16));
                     auto* wb = builder.CreateZExt(b, llvm::FixedVectorType::get(i32_ty, 16));
                     auto* prod = builder.CreateMul(wa, wb);
                     llvm::Value* result = llvm::Constant::getNullValue(v4xi32_ty);
                     for (int j = 0; j < 4; j++) {
                        auto* sum = builder.CreateAdd(
                           builder.CreateAdd(builder.CreateExtractElement(prod, builder.getInt32(j*4)),
                                             builder.CreateExtractElement(prod, builder.getInt32(j*4+1))),
                           builder.CreateAdd(builder.CreateExtractElement(prod, builder.getInt32(j*4+2)),
                                             builder.CreateExtractElement(prod, builder.getInt32(j*4+3))));
                        result = builder.CreateInsertElement(result, sum, builder.getInt32(j));
                     }
                     // Add accumulator
                     result = builder.CreateAdd(result, c);
                     store_v128(inst.simd.v_dest, builder.CreateBitCast(result, v128_ty));
                     break;
                  }

                  default:
                     // Unhandled SIMD sub-opcode — no-op
                     break;
                  } // end switch(sub)
                  break;
               } // end case ir_op::v128_op

               // ──── Atomic operations ────
               case ir_op::atomic_op: {
                  uint8_t asub = inst.simd.lane;
                  auto sub = static_cast<atomic_sub>(asub);
                  if (sub == atomic_sub::atomic_fence) break;
                  if (sub == atomic_sub::memory_atomic_notify) {
                     store_vreg(inst.dest, builder.getInt64(0));
                     break;
                  }
                  if (sub == atomic_sub::memory_atomic_wait32 || sub == atomic_sub::memory_atomic_wait64) {
                     store_vreg(inst.dest, builder.getInt64(1));
                     break;
                  }
                  // Loads
                  if (asub >= 0x10 && asub <= 0x16) {
                     auto* addr = load_vreg(inst.simd.addr);
                     auto* addr32 = builder.CreateTrunc(addr, builder.getInt32Ty());
                     auto* offset = builder.getInt32(inst.simd.offset);
                     auto* eff = builder.CreateAdd(addr32, offset);
                     auto* eff64 = builder.CreateZExt(eff, builder.getInt64Ty());
                     // Checked mode: watermark for atomic loads
                     {
                        uint32_t asz = 4;
                        switch (sub) {
                           case atomic_sub::i32_atomic_load8_u:
                           case atomic_sub::i64_atomic_load8_u: asz = 1; break;
                           case atomic_sub::i32_atomic_load16_u:
                           case atomic_sub::i64_atomic_load16_u: asz = 2; break;
                           case atomic_sub::i32_atomic_load: asz = 4; break;
                           case atomic_sub::i64_atomic_load32_u: asz = 4; break;
                           case atomic_sub::i64_atomic_load: asz = 8; break;
                           default: break;
                        }
                        emit_read_watermark_update(builder, eff64, asz, watermark_alloca);
                     }
                     auto* ptr = builder.CreateGEP(builder.getInt8Ty(), load_mem_ptr(), eff64);
                     llvm::Value* val = nullptr;
                     switch(sub) {
                     case atomic_sub::i32_atomic_load: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt32Ty()));
                        val = builder.CreateZExt(builder.CreateLoad(builder.getInt32Ty(), tp), builder.getInt64Ty());
                        break;
                     }
                     case atomic_sub::i64_atomic_load: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt64Ty()));
                        val = builder.CreateLoad(builder.getInt64Ty(), tp);
                        break;
                     }
                     case atomic_sub::i32_atomic_load8_u:
                     case atomic_sub::i64_atomic_load8_u:
                        val = builder.CreateZExt(builder.CreateLoad(builder.getInt8Ty(), ptr), builder.getInt64Ty());
                        break;
                     case atomic_sub::i32_atomic_load16_u:
                     case atomic_sub::i64_atomic_load16_u: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt16Ty()));
                        val = builder.CreateZExt(builder.CreateLoad(builder.getInt16Ty(), tp), builder.getInt64Ty());
                        break;
                     }
                     case atomic_sub::i64_atomic_load32_u: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt32Ty()));
                        val = builder.CreateZExt(builder.CreateLoad(builder.getInt32Ty(), tp), builder.getInt64Ty());
                        break;
                     }
                     default: val = builder.getInt64(0); break;
                     }
                     store_vreg(inst.dest, val);
                     break;
                  }
                  // Stores
                  if (asub >= 0x17 && asub <= 0x1D) {
                     auto* val = load_vreg(inst.simd.v_src1);
                     auto* addr = load_vreg(inst.simd.addr);
                     auto* addr32 = builder.CreateTrunc(addr, builder.getInt32Ty());
                     auto* offset = builder.getInt32(inst.simd.offset);
                     auto* eff = builder.CreateAdd(addr32, offset);
                     auto* eff64 = builder.CreateZExt(eff, builder.getInt64Ty());
                     // Checked mode: immediate bounds check for atomic stores
                     {
                        uint32_t asz = 4;
                        switch (sub) {
                           case atomic_sub::i32_atomic_store8:
                           case atomic_sub::i64_atomic_store8: asz = 1; break;
                           case atomic_sub::i32_atomic_store16:
                           case atomic_sub::i64_atomic_store16: asz = 2; break;
                           case atomic_sub::i32_atomic_store: asz = 4; break;
                           case atomic_sub::i64_atomic_store32: asz = 4; break;
                           case atomic_sub::i64_atomic_store: asz = 8; break;
                           default: break;
                        }
                        emit_write_bounds_check(builder, ctx_ptr, load_mem_ptr(), eff64, asz, mem_size_cache);
                     }
                     auto* ptr = builder.CreateGEP(builder.getInt8Ty(), load_mem_ptr(), eff64);
                     switch(sub) {
                     case atomic_sub::i32_atomic_store: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt32Ty()));
                        builder.CreateStore(builder.CreateTrunc(val, builder.getInt32Ty()), tp);
                        break;
                     }
                     case atomic_sub::i64_atomic_store: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt64Ty()));
                        builder.CreateStore(val, tp);
                        break;
                     }
                     case atomic_sub::i32_atomic_store8:
                     case atomic_sub::i64_atomic_store8:
                        builder.CreateStore(builder.CreateTrunc(val, builder.getInt8Ty()), ptr);
                        break;
                     case atomic_sub::i32_atomic_store16:
                     case atomic_sub::i64_atomic_store16: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt16Ty()));
                        builder.CreateStore(builder.CreateTrunc(val, builder.getInt16Ty()), tp);
                        break;
                     }
                     case atomic_sub::i64_atomic_store32: {
                        auto* tp = builder.CreateBitCast(ptr, llvm::PointerType::getUnqual(builder.getInt32Ty()));
                        builder.CreateStore(builder.CreateTrunc(val, builder.getInt32Ty()), tp);
                        break;
                     }
                     default: break;
                     }
                     break;
                  }
                  // RMW + cmpxchg: call __psizam_atomic_rmw
                  {
                     auto* rmw_fn_ty = llvm::FunctionType::get(builder.getInt64Ty(),
                        {builder.getPtrTy(), builder.getInt8Ty(), builder.getInt32Ty(),
                         builder.getInt32Ty(), builder.getInt64Ty(), builder.getInt64Ty()}, false);
                     auto rmw_fn = llvm_mod->getOrInsertFunction("__psizam_atomic_rmw", rmw_fn_ty);
                     bool is_cmpxchg = (asub >= 0x48);
                     llvm::Value* val1 = nullptr;
                     llvm::Value* val2 = nullptr;
                     if (is_cmpxchg) {
                        val1 = load_vreg(inst.simd.v_src1); // expected
                        val2 = load_vreg(inst.simd.v_src2); // replacement
                     } else {
                        val1 = load_vreg(inst.simd.v_src1);
                        val2 = builder.getInt64(0);
                     }
                     auto* addr = load_vreg(inst.simd.addr);
                     auto* addr32 = builder.CreateTrunc(addr, builder.getInt32Ty());
                     auto* result = builder.CreateCall(rmw_fn, {
                        ctx_ptr, builder.getInt8(asub), addr32,
                        builder.getInt32(inst.simd.offset), val1, val2});
                     store_vreg(inst.dest, result);
                  }
                  break;
               }

               // ──── Exception Handling ────

               case ir_op::eh_enter: {
                  // ri.imm = eh_data_index, ri.src1 = catch_count
                  uint32_t eh_idx = static_cast<uint32_t>(inst.ri.imm);
                  uint32_t catch_count = inst.ri.src1;
                  const auto& ehd = func.eh_data[eh_idx];

                  // Build catch_data array on stack: each entry = (kind << 32) | tag_index
                  auto* catch_data = builder.CreateAlloca(i64_ty,
                     builder.getInt32(catch_count));
                  for (uint32_t c = 0; c < catch_count; c++) {
                     uint64_t packed = (static_cast<uint64_t>(ehd.catches[c].kind) << 32)
                                     | ehd.catches[c].tag_index;
                     auto* slot = builder.CreateGEP(i64_ty, catch_data, builder.getInt32(c));
                     builder.CreateStore(builder.getInt64(packed), slot);
                  }

                  // Call __psizam_eh_enter(ctx, catch_count, catch_data) → jmpbuf ptr
                  auto* jmpbuf = builder.CreateCall(rt_eh_enter,
                     {ctx_ptr, builder.getInt32(catch_count), catch_data});
                  // Convert ptr to i64 for vreg storage
                  store_vreg(inst.dest, builder.CreatePtrToInt(jmpbuf, i64_ty));
                  break;
               }

               case ir_op::eh_setjmp: {
                  // rr.src1 = jmpbuf vreg (stored as i64, actually a pointer).
                  // Call __psizam_setjmp(jmpbuf). Returns 0 on normal, non-zero after longjmp.
                  auto* jmpbuf = load_vreg(inst.rr.src1);
                  auto* jmpbuf_ptr = builder.CreateIntToPtr(jmpbuf, ptr_ty);
                  auto* result = builder.CreateCall(rt_setjmp, {jmpbuf_ptr});
                  store_vreg(inst.dest, result);
                  break;
               }

               case ir_op::eh_leave: {
                  builder.CreateCall(rt_eh_leave, {ctx_ptr});
                  break;
               }

               case ir_op::eh_throw: {
                  // Payload values accumulated in call_args via preceding arg ops.
                  uint32_t tag_index = static_cast<uint32_t>(inst.ri.imm);
                  uint32_t payload_count = static_cast<uint32_t>(call_args.size());

                  llvm::Value* payload_ptr;
                  if (payload_count > 0) {
                     auto* payload = builder.CreateAlloca(i64_ty,
                        builder.getInt32(payload_count));
                     for (uint32_t p = 0; p < payload_count; p++) {
                        llvm::Value* v = call_args[p];
                        // Widen to i64
                        if (v->getType() == i32_ty)
                           v = builder.CreateZExt(v, i64_ty);
                        else if (v->getType() == f32_ty)
                           v = builder.CreateZExt(builder.CreateBitCast(v, i32_ty), i64_ty);
                        else if (v->getType() == f64_ty)
                           v = builder.CreateBitCast(v, i64_ty);
                        auto* slot = builder.CreateGEP(i64_ty, payload, builder.getInt32(p));
                        builder.CreateStore(v, slot);
                     }
                     payload_ptr = payload;
                  } else {
                     payload_ptr = llvm::Constant::getNullValue(ptr_ty);
                  }
                  call_args.clear();

                  builder.CreateCall(rt_eh_throw,
                     {ctx_ptr, builder.getInt32(tag_index),
                      payload_ptr, builder.getInt32(payload_count)});
                  builder.CreateUnreachable();
                  break;
               }

               case ir_op::eh_throw_ref: {
                  auto* exnref = load_vreg(inst.rr.src1);
                  if (!exnref) break;
                  // Truncate exnref to i32
                  auto* idx = builder.CreateTrunc(exnref, i32_ty);
                  builder.CreateCall(rt_eh_throw_ref, {ctx_ptr, idx});
                  builder.CreateUnreachable();
                  break;
               }

               case ir_op::eh_get_match: {
                  auto* result = builder.CreateCall(rt_eh_get_match, {ctx_ptr});
                  store_vreg(inst.dest, result);
                  break;
               }

               case ir_op::eh_get_payload: {
                  uint32_t idx = static_cast<uint32_t>(inst.ri.imm);
                  auto* result = builder.CreateCall(rt_eh_get_payload,
                     {ctx_ptr, builder.getInt32(idx)});
                  store_vreg(inst.dest, result);
                  break;
               }

               case ir_op::eh_get_exnref: {
                  auto* result = builder.CreateCall(rt_eh_get_exnref, {ctx_ptr});
                  store_vreg(inst.dest, result);
                  break;
               }

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
               emit_watermark_validate(builder, ctx_ptr, load_mem_ptr(), watermark_alloca, mem_size_cache);
               // Multi-value return slots are already populated by the
               // `multi_return_store` IR ops that ir_writer emits in emit_end
               // and emit_return (see ir_writer.hpp). Do NOT re-store them
               // here from load_vreg(i): the function-scope merge vregs v0/v1/v2
               // are never written for multi-value functions, so loading them
               // produces undef and overwrites the correct values.
               // Function exit block — return merge vreg v0 (direct LLVM return is
               // ignored by the caller for multi-value, which reads _multi_return[]).
               if (fn->getReturnType()->isVoidTy()) {
                  builder.CreateRetVoid();
               } else if (fn->getReturnType() == v128_ty &&
                          v128_slots.size() > 0 && v128_slots[0]) {
                  auto* ret_val = load_v128(0, v128_ty);
                  builder.CreateRet(ret_val);
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
         // Entry wrapper type: i64(ptr, ptr, ptr)
         auto* entry_ty = llvm::FunctionType::get(i64_ty, {ptr_ty, ptr_ty, ptr_ty}, false);

         // Only iterate functions we actually translated — avoids scanning
         // the entire module's function table (most entries are null under
         // lazy declaration).
         for (uint32_t i : translated_funcs) {
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

            // Track cumulative slot offset since v128 params occupy 2 slots
            uint32_t slot_offset = 0;
            for (uint32_t p = 0; p < ft.param_types.size(); p++) {
               llvm::Type* param_ty = wasm_type_to_llvm(ft.param_types[p]);
               if (param_ty == v128_ty) {
                  // v128: args_raw stores [high, low]. LLVM <2 x i64> is [elem0, elem1].
                  // We want elem0=low, elem1=high, so swap.
                  auto* lo_ptr = b.CreateGEP(i64_ty, arr_arg, b.getInt32(slot_offset + 1));
                  auto* hi_ptr = b.CreateGEP(i64_ty, arr_arg, b.getInt32(slot_offset));
                  auto* lo = b.CreateLoad(i64_ty, lo_ptr);
                  auto* hi = b.CreateLoad(i64_ty, hi_ptr);
                  llvm::Value* vec = llvm::UndefValue::get(v128_ty);
                  vec = b.CreateInsertElement(vec, lo, b.getInt32(0));
                  vec = b.CreateInsertElement(vec, hi, b.getInt32(1));
                  call_args.push_back(vec);
                  slot_offset += 2;
               } else {
                  auto* elem_ptr = b.CreateGEP(i64_ty, arr_arg, b.getInt32(slot_offset));
                  llvm::Value* raw = b.CreateLoad(i64_ty, elem_ptr);
                  if (param_ty == i32_ty) {
                     raw = b.CreateTrunc(raw, i32_ty);
                  } else if (param_ty == f32_ty) {
                     auto* as_i32 = b.CreateTrunc(raw, i32_ty);
                     raw = b.CreateBitCast(as_i32, f32_ty);
                  } else if (param_ty == f64_ty) {
                     raw = b.CreateBitCast(raw, f64_ty);
                  }
                  call_args.push_back(raw);
                  slot_offset += 1;
               }
            }

            auto* call_result = b.CreateCall(real_fn, call_args);

            // Convert return value to i64
            if (ft.return_count == 0) {
               b.CreateRet(llvm::ConstantInt::get(i64_ty, 0));
            } else {
               llvm::Value* ret = call_result;
               llvm::Type* ret_ty = wasm_type_to_llvm(ft.return_type);
               if (ret_ty == v128_ty) {
                  // Store full v128 result into args_raw buffer (caller reads it)
                  b.CreateStore(ret, arr_arg);
                  // Return low i64 for scalar result path
                  ret = b.CreateExtractElement(ret, b.getInt32(0));
               } else if (ret_ty == i32_ty) {
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

      // DEBUG: dump IR for specific function(s) pre-optimization.
      // PSIZAM_LLVM_DUMP_FUNC=N,M,... writes wasm_func_N.ll to /tmp
      // for each requested index. The dump is pre-optimization IR so we
      // can inspect what the translator produced before any LLVM pass.
      if (const char* want = std::getenv("PSIZAM_LLVM_DUMP_FUNC")) {
         std::string s(want);
         size_t pos = 0;
         while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos) comma = s.size();
            uint32_t idx = static_cast<uint32_t>(std::stoul(s.substr(pos, comma - pos)));
            pos = comma + 1;
            if (idx < _impl->wasm_funcs.size() && _impl->wasm_funcs[idx]) {
               auto* fn = _impl->wasm_funcs[idx];
               if (!fn->isDeclaration()) {
                  std::string path = "/tmp/wasm_func_" + std::to_string(idx) + "_pre.ll";
                  std::error_code ec;
                  llvm::raw_fd_ostream os(path, ec);
                  if (!ec) { fn->print(os); fprintf(stderr, "[dump] wrote %s\n", path.c_str()); }
               }
            }
         }
      }

      // Verify the module
      std::string err;
      llvm::raw_string_ostream err_stream(err);
      if (llvm::verifyModule(*_impl->llvm_mod, &err_stream)) {
         if (const char* dump = std::getenv("PSIZAM_LLVM_DUMP_ON_VERIFY_FAIL")) {
            (void)dump;
            std::string ir;
            llvm::raw_string_ostream os(ir);
            _impl->llvm_mod->print(os, nullptr);
            fprintf(stderr, "=== LLVM module dump (verify failed) ===\n%s\n=== end ===\n",
                    ir.c_str());
         }
         throw wasm_parse_exception("LLVM module verification failed: " + err);
      }

      // Run optimization passes.
      // Manual analysis registration instead of PassBuilder to avoid pulling in
      // all 65+ analyses from PassRegistry.def (IPO, Vectorize, Coroutines, etc.)
      if (_impl->opts.opt_level > 0) {
         llvm::LoopAnalysisManager lam;
         llvm::FunctionAnalysisManager fam;
         llvm::CGSCCAnalysisManager cgam;
         llvm::ModuleAnalysisManager mam;

         // Register only the analyses our passes actually need.
         // Function analyses (13 of 36+ in PassRegistry.def):
         fam.registerPass([&] { return llvm::DominatorTreeAnalysis(); });
         fam.registerPass([&] { return llvm::AssumptionAnalysis(); });
         fam.registerPass([&] { return llvm::TargetLibraryAnalysis(); });
         fam.registerPass([&] { return llvm::TargetIRAnalysis(); });
         fam.registerPass([&] { return llvm::LoopAnalysis(); });
         fam.registerPass([&] { return llvm::PostDominatorTreeAnalysis(); });
         fam.registerPass([&] { return llvm::OptimizationRemarkEmitterAnalysis(); });
         fam.registerPass([&] { return llvm::MemorySSAAnalysis(); });
         fam.registerPass([&] { return llvm::MemoryDependenceAnalysis(); });
         fam.registerPass([&] { return llvm::ScalarEvolutionAnalysis(); });
         fam.registerPass([&] { return llvm::BlockFrequencyAnalysis(); });
         fam.registerPass([&] { return llvm::LastRunTrackingAnalysis(); });
         fam.registerPass([&] { return llvm::PassInstrumentationAnalysis(); });
         // Alias analysis: BasicAA registered in FAM, then AAManager uses it
         fam.registerPass([&] { return llvm::BasicAA(); });
         fam.registerPass([&] {
            llvm::AAManager aa;
            aa.registerFunctionAnalysis<llvm::BasicAA>();
            return aa;
         });

         // Loop analyses (only what LICM needs):
         lam.registerPass([&] { return llvm::PassInstrumentationAnalysis(); });

         // Module analyses: just PassInstrumentation
         mam.registerPass([&] { return llvm::PassInstrumentationAnalysis(); });

         // CGSCC analyses: just PassInstrumentation
         cgam.registerPass([&] { return llvm::PassInstrumentationAnalysis(); });

         // Cross-register proxies (same as PassBuilder::crossRegisterProxies)
         mam.registerPass([&] { return llvm::FunctionAnalysisManagerModuleProxy(fam); });
         mam.registerPass([&] { return llvm::CGSCCAnalysisManagerModuleProxy(cgam); });
         cgam.registerPass([&] { return llvm::ModuleAnalysisManagerCGSCCProxy(mam); });
         fam.registerPass([&] { return llvm::CGSCCAnalysisManagerFunctionProxy(cgam); });
         fam.registerPass([&] { return llvm::ModuleAnalysisManagerFunctionProxy(mam); });
         fam.registerPass([&] { return llvm::LoopAnalysisManagerFunctionProxy(lam); });
         lam.registerPass([&] { return llvm::FunctionAnalysisManagerLoopProxy(fam); });

         // Custom WASM-tuned pipeline: only passes that matter for translated WASM IR.
         // ~10 passes vs ~60 in the generic pipeline — compiles ~45% faster with
         // identical or slightly better execution speed.
         llvm::ModulePassManager mpm;
         llvm::FunctionPassManager fpm;

         // DEBUG: skip Phase 2 passes to bisect the runtime crash.
         // PSIZAM_LLVM_SKIP_PHASE2=1 keeps Phase 1 (canonicalize) and
         // Phase 3 (cleanup) but skips Reassociate/GVN/LICM.
         bool skip_phase2 = std::getenv("PSIZAM_LLVM_SKIP_PHASE2") != nullptr;

         // Phase 1: Canonicalize — cheap, huge IR reduction.
         // mem2reg is critical: promotes alloca-per-register to SSA.
         // SROA breaks up the host_args alloca when it can.
         fpm.addPass(llvm::PromotePass());
         fpm.addPass(llvm::SROAPass(llvm::SROAOptions::ModifyCFG));
         fpm.addPass(llvm::EarlyCSEPass(/*UseMemorySSA=*/false));
         fpm.addPass(llvm::InstCombinePass());
         fpm.addPass(llvm::SimplifyCFGPass());

         if (!skip_phase2) {
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
         }

         // Phase 3: Cleanup — removes dead code exposed by earlier passes.
         fpm.addPass(llvm::ADCEPass());
         fpm.addPass(llvm::InstCombinePass());
         fpm.addPass(llvm::SimplifyCFGPass());

         mpm.addPass(llvm::createModuleToFunctionPassAdaptor(std::move(fpm)));
         mpm.run(*_impl->llvm_mod, mam);
      }

      // DEBUG: dump IR for specific function(s) post-optimization.
      // PSIZAM_LLVM_DUMP_FUNC_POST=N,M,... writes wasm_func_N_post.ll
      // after the pass pipeline runs. Compare with _pre.ll to see what
      // each optimization transformed.
      if (const char* want = std::getenv("PSIZAM_LLVM_DUMP_FUNC_POST")) {
         std::string s(want);
         size_t pos = 0;
         while (pos < s.size()) {
            size_t comma = s.find(',', pos);
            if (comma == std::string::npos) comma = s.size();
            uint32_t idx = static_cast<uint32_t>(std::stoul(s.substr(pos, comma - pos)));
            pos = comma + 1;
            if (idx < _impl->wasm_funcs.size() && _impl->wasm_funcs[idx]) {
               auto* fn = _impl->wasm_funcs[idx];
               if (!fn->isDeclaration()) {
                  std::string path = "/tmp/wasm_func_" + std::to_string(idx) + "_post.ll";
                  std::error_code ec;
                  llvm::raw_fd_ostream os(path, ec);
                  if (!ec) { fn->print(os); fprintf(stderr, "[dump] wrote %s\n", path.c_str()); }
               }
            }
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

} // namespace psizam::detail
