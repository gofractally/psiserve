#pragma once

// Pass 2 of the two-pass optimizing JIT (jit2) — AArch64 backend.
// Converts IR instructions to ARM64 machine code.
//
// Register conventions (AAPCS64):
//   X0, X1    = temporaries (spill loads, like rax/rcx on x86)
//   X2-X15    = caller-saved, available for register allocation (14 regs)
//   X16       = scratch for large immediates
//   X19       = context pointer (callee-saved)
//   X20       = linear memory base (callee-saved)
//   X21       = call depth counter (callee-saved)
//   X22-X28   = callee-saved, available for register allocation (7 regs)
//   X29       = frame pointer (FP)
//   X30       = link register (LR)
//   SP        = stack pointer (must be 16-byte aligned at calls)

#include <psizam/allocator.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/execution_context.hpp>
#include <psizam/jit_ir.hpp>
#include <psizam/jit_regalloc.hpp>
#include <psizam/llvm_runtime_helpers.hpp>
#include <psizam/softfloat.hpp>
#include <psizam/types.hpp>
#include <psizam/utils.hpp>
#include <psizam/signals.hpp>

#include <cassert>
#include <cstdint>
#include <cstring>

namespace psizam {

   class jit_codegen_a64 {
    public:
      // Register numbers
      static constexpr uint32_t X0  = 0,  X1  = 1,  X2  = 2,  X3  = 3;
      static constexpr uint32_t X4  = 4,  X5  = 5,  X6  = 6,  X7  = 7;
      static constexpr uint32_t X8  = 8,  X9  = 9,  X10 = 10, X11 = 11;
      static constexpr uint32_t X12 = 12, X13 = 13, X14 = 14, X15 = 15;
      static constexpr uint32_t X16 = 16, X17 = 17;
      static constexpr uint32_t X19 = 19, X20 = 20, X21 = 21, X22 = 22;
      static constexpr uint32_t X23 = 23, X24 = 24, X25 = 25;
      static constexpr uint32_t X29 = 29, X30 = 30;
      static constexpr uint32_t XZR = 31, SP = 31, FP = 29;

      // Condition codes
      static constexpr uint32_t COND_EQ = 0,  COND_NE = 1;
      static constexpr uint32_t COND_HS = 2,  COND_LO = 3;   // unsigned >= / <
      static constexpr uint32_t COND_HI = 8,  COND_LS = 9;   // unsigned > / <=
      static constexpr uint32_t COND_MI = 4,  COND_PL = 5;   // negative / positive
      static constexpr uint32_t COND_VS = 6,  COND_VC = 7;   // overflow / no overflow
      static constexpr uint32_t COND_GE = 10, COND_LT = 11;  // signed >= / <
      static constexpr uint32_t COND_GT = 12, COND_LE = 13;   // signed > / <=

      static constexpr uint32_t invert_condition(uint32_t cond) { return cond ^ 1; }
      static constexpr int32_t multi_return_offset = 24; // offset of _multi_return in frame_info_holder

      static constexpr bool use_softfloat =
#ifdef PSIZAM_SOFTFLOAT
         true;
#else
         false;
#endif

      jit_codegen_a64(growable_allocator& alloc, module& mod, growable_allocator& scratch_alloc,
                      bool enable_backtrace, bool stack_limit_is_bytes,
                      void* code_segment_base = nullptr)
         : _allocator(alloc), _scratch_alloc(scratch_alloc), _mod(mod),
           _enable_backtrace(enable_backtrace), _stack_limit_is_bytes(stack_limit_is_bytes) {
         init_relocations();
         _code_segment_base = code_segment_base ? code_segment_base : _allocator.start_code();
      }

      // Offset of _remaining_call_depth in unified frame_info_holder layout
      // (always at offset 16, after _bottom_frame and _top_frame pointers)
      static constexpr int32_t call_depth_offset() { return 16; }

      // ──────── Entry point and error handlers ────────

      void emit_entry_and_error_handlers() {
         // AAPCS64 entry point
         auto* buf = _allocator.alloc<unsigned char>(512);
         code = buf;
         emit_aapcs64_interface();

         // Error handlers
         buf = _allocator.alloc<unsigned char>(256);
         code = buf;
         fpe_handler = emit_error_handler(&on_fp_error);
         call_indirect_handler = emit_error_handler(&on_call_indirect_error);
         type_error_handler = emit_error_handler(&on_type_error);
         stack_overflow_handler = emit_error_handler(&on_stack_overflow);
         memory_handler = emit_error_handler(&on_memory_error);

         // Host function stubs
         const uint32_t num_imported = _mod.get_imported_functions_size();
         if (num_imported > 0) {
            const std::size_t host_functions_size = 256 * num_imported;
            buf = _allocator.alloc<unsigned char>(host_functions_size);
            code = buf;
            for (uint32_t i = 0; i < num_imported; ++i) {
               start_function(code, i);
               emit_host_call(i);
            }
         }
      }

      // ──────── Compile one function from IR to ARM64 ────────

      void compile_function(ir_function& func, function_body& body) {
         jit_scratch_allocator scratch(_scratch_alloc);

         _block_addrs = scratch.alloc<void*>(func.block_count);
         _block_fixups = scratch.alloc<block_fixup*>(func.block_count);
         _num_blocks = func.block_count;
         for (uint32_t i = 0; i < func.block_count; ++i) {
            _block_addrs[i] = nullptr;
            _block_fixups[i] = nullptr;
         }

         // Build vreg → physical register mapping
         _vreg_map = nullptr;
         _spill_map = nullptr;
         _num_vregs = 0;
         _num_spill_slots = 0;
         if (func.interval_count > 0 && func.intervals) {
            // ARM64 has no XMM registers. The regalloc GPR pass skips f32/f64
            // intervals and the XMM pass assigns them XMM slots, leaving
            // phys_reg = -1 and spill_slot = -1. Force-assign spill slots so
            // the codegen can materialize them via the spill map.
            for (uint32_t iv = 0; iv < func.interval_count; ++iv) {
               auto& interval = func.intervals[iv];
               if (interval.phys_reg < 0 && interval.spill_slot < 0
                   && (interval.type == types::f32 || interval.type == types::f64)) {
                  interval.spill_slot = static_cast<int16_t>(func.num_spill_slots++);
               }
            }
            _num_vregs = func.next_vreg;
            _vreg_map = scratch.alloc<int8_t>(_num_vregs);
            _spill_map = scratch.alloc<int16_t>(_num_vregs);
            for (uint32_t v = 0; v < _num_vregs; ++v) {
               _vreg_map[v] = -1;
               _spill_map[v] = -1;
            }
            for (uint32_t iv = 0; iv < func.interval_count; ++iv) {
               auto& interval = func.intervals[iv];
               if (interval.vreg < _num_vregs) {
                  _vreg_map[interval.vreg] = interval.phys_reg;
                  _spill_map[interval.vreg] = interval.spill_slot;
               }
            }
            _num_spill_slots = func.num_spill_slots;
            _use_regalloc = true;
         } else {
            _use_regalloc = false;
         }

         _func_def_inst = func.def_inst;
         _func_use_count = func.use_count;
         _func_insts = func.insts;
         _func_inst_count = func.inst_count;

         // Pre-allocate fixup pool before disarming scratch.
         _fixup_pool = scratch.alloc<block_fixup>(func.inst_count + 1);
         _fixup_pool_next = 0;
         _fixup_pool_size = func.inst_count + 1;

         // Disarm scratch so its destructor won't reclaim the code buffer.
         // When _scratch_alloc == _allocator, scratch data stays interleaved
         // with code — the waste is acceptable given deterministic limits.
         scratch.disarm();

         // Code buffer
         const std::size_t bytes_per_inst = func.has_simd ? 128 : 64;
         const std::size_t est_size = static_cast<std::size_t>(func.inst_count) * bytes_per_inst + 512;
         auto* buf = _allocator.alloc<unsigned char>(est_size);
         auto* code_start = buf;
         code = buf;

         start_function(code, func.func_index + _mod.get_imported_functions_size());

         emit_function_prologue(func);

         for (uint32_t i = 0; i < func.inst_count; ++i) {
            emit_ir_inst_reg(func, func.insts[i], i);
         }

         emit_function_epilogue(func);

         body.jit_code_offset = code_start - static_cast<unsigned char*>(_code_segment_base);
         body.jit_code_size = static_cast<uint32_t>(code - code_start);

         _block_addrs = nullptr;
         _block_fixups = nullptr;
         _num_blocks = 0;
         _if_fixup_top = 0;
         _in_br_table = false;
      }

      void finalize_code() {
         _allocator.end_code<true>(_code_segment_base);


         auto num_functions = _mod.get_functions_total();
         for (auto& elem : _mod.elements) {
            for (auto& entry : elem.elems) {
               void* addr = call_indirect_handler;
               if (entry.index < num_functions && entry.index < _num_relocs) {
                  if (_relocs[entry.index].address) {
                     addr = _relocs[entry.index].address;
                  }
               }
               std::size_t offset = static_cast<char*>(addr) - static_cast<char*>(_code_segment_base);
               entry.code_ptr = _mod.allocator._code_base + offset;
            }
         }
      }

    private:

      // ──────── Instruction emission ────────

      void emit32(uint32_t instr) {
         std::memcpy(code, &instr, 4);
         code += 4;
      }

      // ──────── Branch fixup ────────

      static void fix_branch(void* branch, void* target) {
         auto* branch_bytes = static_cast<uint8_t*>(branch);
         auto* target_bytes = static_cast<uint8_t*>(target);
         int64_t offset = (target_bytes - branch_bytes) / 4;

         uint32_t instr;
         std::memcpy(&instr, branch, 4);

         if ((instr & 0xFC000000) == 0x14000000 || (instr & 0xFC000000) == 0x94000000) {
            // B / BL: imm26
            PSIZAM_ASSERT(offset <= 0x1FFFFFF && offset >= -0x2000000, wasm_parse_exception, "branch out of range");
            instr = (instr & 0xFC000000) | (static_cast<uint32_t>(offset) & 0x3FFFFFF);
         } else if ((instr & 0xFF000010) == 0x54000000) {
            // B.cond: imm19
            PSIZAM_ASSERT(offset <= 0x3FFFF && offset >= -0x40000, wasm_parse_exception, "branch out of range");
            instr = (instr & 0xFF00001F) | ((static_cast<uint32_t>(offset) & 0x7FFFF) << 5);
         } else if ((instr & 0x7F000000) == 0x34000000 || (instr & 0x7F000000) == 0x35000000 ||
                    (instr & 0xFF000000) == 0xB4000000 || (instr & 0xFF000000) == 0xB5000000) {
            // CBZ/CBNZ (32/64-bit): imm19
            PSIZAM_ASSERT(offset <= 0x3FFFF && offset >= -0x40000, wasm_parse_exception, "branch out of range");
            instr = (instr & 0xFF00001F) | ((static_cast<uint32_t>(offset) & 0x7FFFF) << 5);
         } else {
            PSIZAM_ASSERT(false, wasm_parse_exception, "unknown branch instruction to fix");
         }

         std::memcpy(branch, &instr, 4);
      }

      // ──────── Immediate encoding helpers ────────

      void emit_mov_imm32(uint32_t rd, uint32_t value) {
         if (value == 0) {
            emit32(0x2A1F03E0 | rd); // MOV Wd, WZR
            return;
         }
         uint16_t lo = value & 0xFFFF;
         uint16_t hi = (value >> 16) & 0xFFFF;
         if (hi == 0) {
            emit32(0x52800000 | (static_cast<uint32_t>(lo) << 5) | rd); // MOVZ Wd, #lo
         } else if (lo == 0) {
            emit32(0x52A00000 | (static_cast<uint32_t>(hi) << 5) | rd); // MOVZ Wd, #hi, LSL #16
         } else if ((value & 0xFFFF0000) == 0xFFFF0000) {
            emit32(0x12800000 | (static_cast<uint32_t>(static_cast<uint16_t>(~lo)) << 5) | rd); // MOVN
         } else {
            emit32(0x52800000 | (static_cast<uint32_t>(lo) << 5) | rd);
            emit32(0x72A00000 | (static_cast<uint32_t>(hi) << 5) | rd); // MOVK Wd, #hi, LSL #16
         }
      }

      void emit_mov_imm64(uint32_t rd, uint64_t value) {
         if (value == 0) {
            emit32(0xAA1F03E0 | rd); // MOV Xd, XZR
            return;
         }
         if (value <= 0xFFFFFFFF) {
            emit_mov_imm32(rd, static_cast<uint32_t>(value));
            return;
         }
         uint16_t chunks[4] = {
            static_cast<uint16_t>(value),
            static_cast<uint16_t>(value >> 16),
            static_cast<uint16_t>(value >> 32),
            static_cast<uint16_t>(value >> 48)
         };
         bool first = true;
         for (int i = 0; i < 4; ++i) {
            if (chunks[i] != 0) {
               if (first) {
                  emit32(0xD2800000 | (static_cast<uint32_t>(i) << 21) | (static_cast<uint32_t>(chunks[i]) << 5) | rd);
                  first = false;
               } else {
                  emit32(0xF2800000 | (static_cast<uint32_t>(i) << 21) | (static_cast<uint32_t>(chunks[i]) << 5) | rd);
               }
            }
         }
      }

      // ADD Xd, Xn, #imm (unsigned)
      void emit_add_imm(uint32_t rd, uint32_t rn, uint32_t imm) {
         if (imm <= 4095) {
            emit32(0x91000000 | (imm << 10) | (rn << 5) | rd);
         } else if ((imm & 0xFFF) == 0 && (imm >> 12) <= 4095) {
            emit32(0x91400000 | ((imm >> 12) << 10) | (rn << 5) | rd);
         } else {
            emit_mov_imm64(X16, imm);
            emit32(0x8B100000 | (rn << 5) | rd); // ADD Xd, Xn, X16
         }
      }

      // SUB Xd, Xn, #imm
      void emit_sub_imm(uint32_t rd, uint32_t rn, uint32_t imm) {
         if (imm <= 4095) {
            emit32(0xD1000000 | (imm << 10) | (rn << 5) | rd);
         } else if ((imm & 0xFFF) == 0 && (imm >> 12) <= 4095) {
            emit32(0xD1400000 | ((imm >> 12) << 10) | (rn << 5) | rd);
         } else {
            emit_mov_imm64(X16, imm);
            emit32(0xCB100000 | (rn << 5) | rd); // SUB Xd, Xn, X16
         }
      }

      // ADD with signed immediate
      void emit_add_signed_imm(uint32_t rd, uint32_t rn, int32_t imm) {
         if (imm >= 0) emit_add_imm(rd, rn, static_cast<uint32_t>(imm));
         else emit_sub_imm(rd, rn, static_cast<uint32_t>(-static_cast<int64_t>(imm)));
      }

      // CMP Wn, #imm (32-bit)
      void emit_cmp_imm32(uint32_t rn, uint32_t value) {
         if (value <= 4095) {
            emit32(0x7100001F | (value << 10) | (rn << 5)); // SUBS WZR, Wn, #value
         } else {
            emit_mov_imm32(X16, value);
            emit32(0x6B10001F | (rn << 5)); // CMP Wn, W16
         }
      }

      // CMP Xn, #imm (64-bit)
      void emit_cmp_imm64(uint32_t rn, uint32_t value) {
         if (value <= 4095) {
            emit32(0xF100001F | (value << 10) | (rn << 5)); // SUBS XZR, Xn, #value
         } else {
            emit_mov_imm32(X16, value);
            emit32(0xEB10001F | (rn << 5)); // CMP Xn, X16
         }
      }

      // CMP Wn, Wm (32-bit register)
      void emit_cmp_reg32(uint32_t rn, uint32_t rm) {
         emit32(0x6B00001F | (rm << 16) | (rn << 5)); // SUBS WZR, Wn, Wm
      }

      // CMP Xn, Xm (64-bit register)
      void emit_cmp_reg64(uint32_t rn, uint32_t rm) {
         emit32(0xEB00001F | (rm << 16) | (rn << 5)); // SUBS XZR, Xn, Xm
      }

      // CSET Xd, cond
      void emit_cset(uint32_t rd, uint32_t cond) {
         uint32_t inv = invert_condition(cond);
         emit32(0x9A9F07E0 | (inv << 12) | rd); // CSINC Xd, XZR, XZR, inv(cond)
      }

      // MOV Xd, Xm
      void emit_mov_reg(uint32_t rd, uint32_t rm) {
         if (rd != rm)
            emit32(0xAA0003E0 | (rm << 16) | rd); // ORR Xd, XZR, Xm
      }

      // MOV Wd, Wm (32-bit, zero-extends upper 32 bits of Xd)
      // Must emit even when rd == rm — the write to Wd zeros the upper 32 bits.
      void emit_mov_reg32(uint32_t rd, uint32_t rm) {
         emit32(0x2A0003E0 | (rm << 16) | rd); // ORR Wd, WZR, Wm
      }

      // LDR Xt, [Xn, #offset] (signed offset, using X16 as scratch if needed)
      void emit_ldr_offset(uint32_t rt, uint32_t rn, int32_t offset) {
         if (offset >= -256 && offset < 256) {
            // LDUR Xt, [Xn, #offset]
            emit32(0xF8400000 | ((static_cast<uint32_t>(offset) & 0x1FF) << 12) | (rn << 5) | rt);
         } else if (offset >= 0 && (offset % 8) == 0 && (offset / 8) <= 4095) {
            // LDR Xt, [Xn, #offset] (unsigned scaled)
            emit32(0xF9400000 | ((offset / 8) << 10) | (rn << 5) | rt);
         } else {
            emit_mov_imm64(X16, static_cast<uint64_t>(static_cast<int64_t>(offset)));
            emit32(0xF8706800 | (X16 << 16) | (rn << 5) | rt); // LDR Xt, [Xn, X16]
         }
      }

      // STR Xt, [Xn, #offset]
      void emit_str_offset(uint32_t rt, uint32_t rn, int32_t offset) {
         if (offset >= -256 && offset < 256) {
            emit32(0xF8000000 | ((static_cast<uint32_t>(offset) & 0x1FF) << 12) | (rn << 5) | rt);
         } else if (offset >= 0 && (offset % 8) == 0 && (offset / 8) <= 4095) {
            emit32(0xF9000000 | ((offset / 8) << 10) | (rn << 5) | rt);
         } else {
            emit_mov_imm64(X16, static_cast<uint64_t>(static_cast<int64_t>(offset)));
            emit32(0xF8306800 | (X16 << 16) | (rn << 5) | rt); // STR Xt, [Xn, X16]
         }
      }

      // LDR Xt, [FP, #offset]
      void emit_ldr_fp(uint32_t rt, int32_t offset) { emit_ldr_offset(rt, FP, offset); }
      // STR Xt, [FP, #offset]
      void emit_str_fp(uint32_t rt, int32_t offset) { emit_str_offset(rt, FP, offset); }

      // Push/pop via SP (16-byte aligned)
      void emit_push(uint32_t rt) {
         emit32(0xF81F0FE0 | rt); // STR Xt, [SP, #-16]!
      }
      void emit_pop(uint32_t rt) {
         emit32(0xF84107E0 | rt); // LDR Xt, [SP], #16
      }

      // ──────── AAPCS64 interface ────────

      void emit_aapcs64_interface() {
         // Args: X0=context, X1=memory, X2=data, X3=fun, X4=stack, X5=count, X6=vector_result
         // Save callee-saved
         emit32(0xA9BF7BFD); // STP X29, X30, [SP, #-16]!
         emit32(0x910003FD); // MOV X29, SP
         emit32(0xA9BF53F3); // STP X19, X20, [SP, #-16]!
         emit32(0xA9BF5BF5); // STP X21, X22, [SP, #-16]!
         emit32(0xA9BF63F7); // STP X23, X24, [SP, #-16]!
         emit32(0xA9BF6BF9); // STP X25, X26, [SP, #-16]!

         // Save vector_result flag
         emit32(0xAA0603F6); // MOV X22, X6

         // Set up context and memory base
         emit32(0xAA0003F3); // MOV X19, X0
         emit32(0xAA0103F4); // MOV X20, X1

         // Optional stack switch
         emit32(0xB4000044); // CBZ X4, +8
         emit32(0x91000000 | (X4 << 5) | SP); // MOV SP, X4

         // Save SP before arg push
         emit32(0x910003F7); // MOV X23, SP

         // Push args from data array
         void* skip_push = code;
         emit32(0xB4000000 | X5); // CBZ X5, skip (patched)

         // Allocate stack: SP -= count * 16
         emit32(0xCB2573FF); // SUB SP, SP, X5, UXTX #4

         // Copy data in reverse order
         emit32(0x8B050C42); // ADD X2, X2, X5, LSL #3
         emit32(0x910003E9); // MOV X9, SP
         emit32(0xAA0503E8); // MOV X8, X5
         void* loop_top = code;
         emit32(0xF85F8C4A); // LDR X10, [X2, #-8]!
         emit32(0xF801052A); // STR X10, [X9], #16
         emit32(0xF1000508); // SUBS X8, X8, #1
         {
            int32_t off = static_cast<int32_t>((static_cast<uint8_t*>(loop_top) - code)) / 4;
            emit32(0x54000001 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5)); // B.NE loop
         }
         fix_branch(skip_push, code);

         // Load call depth
         // _remaining_call_depth is always at offset 16 in unified frame_info_holder
         emit32(0xB9401275); // LDR W21, [X19, #16]

         if (_enable_backtrace) {
            emit32(0xF9000673); // STR X19, [X19, #8]
         }

         // BLR X3 (call WASM function)
         emit32(0xD63F0060);

         // Restore SP
         emit32(0x910002FF); // MOV SP, X23

         if (_enable_backtrace) {
            emit32(0xF900067F); // STR XZR, [X19, #8]
         }

         // Restore callee-saved via FP
         emit32(0xD10103A0 | SP); // SUB SP, X29, #64

         emit32(0xA8C16BF9); // LDP X25, X26, [SP], #16
         emit32(0xA8C163F7); // LDP X23, X24, [SP], #16
         emit32(0xA8C15BF5); // LDP X21, X22, [SP], #16
         emit32(0xA8C153F3); // LDP X19, X20, [SP], #16
         emit32(0xA8C17BFD); // LDP X29, X30, [SP], #16
         emit32(0xD65F03C0); // RET
      }

      // ──────── Error handlers ────────

      void* emit_error_handler(void (*handler)()) {
         void* result = code;
         // Align SP
         emit32(0x910003E8); // MOV X8, SP
         emit32(0x927CED08); // AND X8, X8, #~0xF
         emit32(0x9100011F); // MOV SP, X8
         emit_mov_imm64(X8, reinterpret_cast<uint64_t>(handler));
         emit32(0xD63F0100); // BLR X8
         return result;
      }

      void emit_branch_to_handler(uint32_t cond, void* handler) {
         int64_t offset = (static_cast<uint8_t*>(handler) - code) / 4;
         if (offset >= -0x40000 && offset < 0x40000) {
            emit32(0x54000000 | ((static_cast<uint32_t>(offset) & 0x7FFFF) << 5) | cond);
         } else {
            // Invert condition, skip trampoline
            void* skip = code;
            emit32(0x54000000 | invert_condition(cond)); // B.inv skip
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(handler));
            emit32(0xD61F0100); // BR X8
            fix_branch(skip, code);
         }
      }

      // CBZ Xn, handler — branch to handler if Xn == 0
      void emit_branch_to_handler_cbz(uint32_t reg, void* handler) {
         int64_t offset = (static_cast<uint8_t*>(handler) - code) / 4;
         if (offset >= -0x40000 && offset < 0x40000) {
            // CBZ Xn, offset
            emit32(0xB4000000 | ((static_cast<uint32_t>(offset) & 0x7FFFF) << 5) | reg);
         } else {
            // CBNZ Xn, skip; B handler
            void* skip = code;
            emit32(0xB5000000 | reg); // CBNZ Xn, skip (placeholder)
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(handler));
            emit32(0xD61F0100); // BR X8
            fix_branch(skip, code);
         }
      }

      // ──────── Host call stubs ────────

      void emit_host_call(uint32_t funcnum) {
         // Save FP/LR
         emit32(0xA9BF7BFD); // STP X29, X30, [SP, #-16]!
         emit32(0x910003FD); // MOV X29, SP

         if (_enable_backtrace) {
            emit32(0xF9000273); // STR X19, [X19]
         }

         // Save X19/X20
         emit32(0xA9BF53F3); // STP X19, X20, [SP, #-16]!

         const auto& ft = _mod.get_function_type(funcnum);
         uint32_t num_params = ft.param_types.size();

         // Repack args from 16-byte stride to 8-byte stride buffer
         uint32_t buf_size = num_params > 0 ? ((num_params * 8 + 15) / 16) * 16 : 0;
         if (buf_size > 0) {
            emit_sub_imm(SP, SP, buf_size);
         }

         emit32(0x910003E1); // MOV X1, SP (buffer pointer)

         uint32_t extra = buf_size + 32; // buf + saved X19/X20 + saved FP/LR
         for (uint32_t i = 0; i < num_params; ++i) {
            uint32_t src_off = extra + i * 16;
            uint32_t dst_off = i * 8;
            // LDR X8, [SP, #src_off]
            if (src_off % 8 == 0 && src_off / 8 <= 4095) {
               emit32(0xF9400000 | ((src_off / 8) << 10) | (SP << 5) | X8);
            } else {
               emit_mov_imm32(X8, src_off);
               emit32(0xF8686BE8); // LDR X8, [SP, X8]
            }
            // STR X8, [X1, #dst_off]
            if (dst_off % 8 == 0 && dst_off / 8 <= 4095) {
               emit32(0xF9000000 | ((dst_off / 8) << 10) | (X1 << 5) | X8);
            } else {
               emit_mov_imm32(X16, dst_off);
               emit32(0xF8306828 | (X16 << 16)); // STR X8, [X1, X16]
            }
         }

         emit_mov_imm32(X2, funcnum);       // W2 = host function index
         emit32(0x2A1503E3);                // MOV W3, W21 (remaining call depth)
         emit32(0xAA1303E0);                // MOV X0, X19 (context)

         emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&call_host_function));
         emit32(0xD63F0100); // BLR X8

         if (buf_size > 0) {
            emit_add_imm(SP, SP, buf_size);
         }

         emit32(0xA8C153F3); // LDP X19, X20, [SP], #16

         if (_enable_backtrace) {
            emit32(0xF900027F); // STR XZR, [X19]
         }

         emit32(0xA8C17BFD); // LDP X29, X30, [SP], #16
         emit32(0xD65F03C0); // RET
      }

      // ──────── Call depth checking ────────

      void emit_call_depth_dec() {
         if (!_stack_limit_is_bytes) {
            // SUBS W21, W21, #1
            emit32(0x71000400 | (1 << 10) | (X21 << 5) | X21);
            emit_branch_to_handler(COND_EQ, stack_overflow_handler);
         }
      }

      void emit_call_depth_inc() {
         if (!_stack_limit_is_bytes) {
            // ADD W21, W21, #1
            emit32(0x11000400 | (1 << 10) | (X21 << 5) | X21);
         }
      }

      // ──────── Function prologue/epilogue ────────

      void emit_function_prologue(ir_function& func) {
         // STP X29, X30, [SP, #-16]!
         emit32(0xA9BF7BFD);
         // MOV X29, SP
         emit32(0x910003FD);

         // Count body local slots, accounting for v128 locals using 2 slots (16 bytes)
         uint32_t body_local_slots = 0;
         {
            const auto& locals = _mod.code[func.func_index].locals;
            for (uint32_t g = 0; g < locals.size(); ++g) {
               uint32_t slots_per = (locals[g].type == types::v128) ? 2 : 1;
               body_local_slots += locals[g].count * slots_per;
            }
         }
         _body_locals = body_local_slots;

         _callee_saved_used = func.callee_saved_used;
         _callee_saved_count = __builtin_popcount(_callee_saved_used);

         // Total frame slots: body locals + spill slots + callee-saved saves
         uint32_t total_slots = body_local_slots + _num_spill_slots + _callee_saved_count;
         // Round up to even for 16-byte alignment
         uint32_t aligned_slots = (total_slots + 1) & ~1u;

         _frame_size = static_cast<int32_t>(aligned_slots * 8);
         if (aligned_slots > 0) {
            emit_sub_imm(SP, SP, _frame_size);
            // Zero-initialize locals + spill slots
            uint32_t zero_slots = body_local_slots + _num_spill_slots;
            for (uint32_t i = 0; i < zero_slots; i += 2) {
               int32_t off = i * 8;
               if (i + 1 < zero_slots) {
                  // STP XZR, XZR, [SP, #off]
                  if (off % 8 == 0 && off / 8 <= 63) {
                     emit32(0xA9000000 | ((off / 8) << 15) | (XZR << 10) | (SP << 5) | XZR); // STP XZR, XZR, [SP, #off]
                  } else {
                     emit_str_offset(XZR, SP, off);
                     emit_str_offset(XZR, SP, off + 8);
                  }
               } else {
                  emit_str_offset(XZR, SP, off);
               }
            }
         }

         // Save callee-saved registers used by regalloc
         if (_use_regalloc && _callee_saved_used) {
            int32_t save_offset = (body_local_slots + _num_spill_slots) * 8;
            for (int i = 0; i < 7; ++i) {
               if (_callee_saved_used & (1 << i)) {
                  uint32_t reg = callee_saved_reg(i);
                  emit_str_offset(reg, SP, save_offset);
                  save_offset += 8;
               }
            }
         }
      }

      void emit_function_epilogue(ir_function& func) {
         if (func.type->return_count > 1) {
            // Multi-value return: values already stored to _multi_return by multi_return_store ops
         } else if (_use_regalloc) {
            if (func.type->return_count != 0 && func.vstack_top > 0) {
               if (func.type->return_type == types::v128) {
                  emit_ldr_offset(X0, SP, 0);
                  emit_ldr_offset(X1, SP, 16);
               } else {
                  uint32_t result_vreg = func.vstack[func.vstack_top - 1];
                  load_vreg_to(X0, result_vreg);
               }
            }
         } else {
            if (func.type->return_count != 0) {
               if (func.type->return_type == types::v128) {
                  emit_ldr_offset(X0, SP, 0);
                  emit_ldr_offset(X1, SP, 16);
               } else {
                  emit_pop(X0);
               }
            }
         }

         // Restore callee-saved registers (FP-relative — stable regardless of v128 stack)
         if (_use_regalloc && _callee_saved_used) {
            int32_t save_offset = static_cast<int32_t>((_body_locals + _num_spill_slots) * 8) - _frame_size;
            for (int i = 0; i < 7; ++i) {
               if (_callee_saved_used & (1 << i)) {
                  uint32_t reg = callee_saved_reg(i);
                  emit_ldr_offset(reg, FP, save_offset);
                  save_offset += 8;
               }
            }
         }

         // MOV SP, X29
         emit32(0x91000000 | (FP << 5) | SP);
         // LDP X29, X30, [SP], #16
         emit32(0xA8C17BFD);
         // RET
         emit32(0xD65F03C0);
      }

      // ──────── Register allocation helpers ────────

      static constexpr uint32_t phys_to_reg(int8_t pr) {
         constexpr uint32_t map[] = {
            2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,  // caller-saved (14)
            22, 23, 24, 25, 26, 27, 28                          // callee-saved (7)
         };
         return map[pr];
      }

      static constexpr uint32_t callee_saved_reg(int idx) {
         constexpr uint32_t regs[] = { 22, 23, 24, 25, 26, 27, 28 };
         return regs[idx];
      }

      int8_t get_phys(uint32_t vreg) const {
         if (!_vreg_map || vreg >= _num_vregs) return -1;
         return _vreg_map[vreg];
      }

      void load_vreg_to(uint32_t rd, uint32_t vreg) {
         if (vreg == ir_vreg_none) return;
         int8_t pr = get_phys(vreg);
         if (pr >= 0) {
            emit_mov_reg(rd, phys_to_reg(pr));
         } else if (_spill_map && vreg < _num_vregs && _spill_map[vreg] >= 0) {
            emit_ldr_offset(rd, FP, get_spill_offset(_spill_map[vreg]));
         } else {
            // vreg has no phys reg and no spill slot — should not happen
         }
      }

      void store_from_to_vreg(uint32_t rs, uint32_t vreg) {
         if (vreg == ir_vreg_none) return;
         int8_t pr = get_phys(vreg);
         if (pr >= 0) {
            emit_mov_reg(phys_to_reg(pr), rs);
         } else if (_spill_map && vreg < _num_vregs && _spill_map[vreg] >= 0) {
            emit_str_offset(rs, FP, get_spill_offset(_spill_map[vreg]));
         }
      }

      // Convenience: load into X0/X1 (temps)
      void load_vreg_x0(uint32_t vreg) { load_vreg_to(X0, vreg); }
      void load_vreg_x1(uint32_t vreg) { load_vreg_to(X1, vreg); }
      void store_x0_vreg(uint32_t vreg) { store_from_to_vreg(X0, vreg); }

      int32_t get_spill_offset(int16_t slot) const {
         // FP-relative: offset from frame pointer (negative)
         return static_cast<int32_t>((_body_locals + static_cast<uint32_t>(slot)) * 8) - _frame_size;
      }

      // Frame offset for locals: params above FP, locals below
      // Returns FP-relative offset for all locals (params above FP, body locals below)
      int32_t get_frame_offset(const ir_function& func, uint32_t local_idx) {
         const func_type* ft = func.type;
         if (local_idx < ft->param_types.size()) {
            // Params are above FP, pushed with 16-byte stride (ARM64 SP alignment)
            int32_t offset = 16; // skip saved FP/LR
            for (uint32_t i = ft->param_types.size(); i-- > 0; ) {
               if (i == local_idx) return offset;
               offset += (ft->param_types[i] == types::v128) ? 32 : 16;
            }
            return offset;
         } else {
            // Body locals are below FP in the frame (FP-relative, negative)
            uint32_t li = local_idx - ft->param_types.size();
            const auto& locals = _mod.code[func.func_index].locals;
            int32_t offset = 0;
            uint32_t count = 0;
            for (uint32_t g = 0; g < locals.size(); ++g) {
               uint8_t size = (locals[g].type == types::v128) ? 16 : 8;
               if (li < count + locals[g].count) {
                  offset += static_cast<int32_t>((li - count) * size);
                  return offset - _frame_size;
               }
               count += locals[g].count;
               offset += static_cast<int32_t>(locals[g].count * size);
            }
            return offset - _frame_size;
         }
      }

      // Load/store a local variable — always FP-relative
      void emit_local_load(const ir_function& func, uint32_t local_idx, uint32_t rd) {
         emit_ldr_offset(rd, FP, get_frame_offset(func, local_idx));
      }

      void emit_local_store(const ir_function& func, uint32_t local_idx, uint32_t rs) {
         emit_str_offset(rs, FP, get_frame_offset(func, local_idx));
      }

      // ──────── Global access ────────

      void emit_global_load(uint32_t gi, uint32_t rd) {
         auto offset = _mod.get_global_offset(gi);
         emit_ldr_offset(X16, X20, wasm_allocator::globals_end() - 8);
         emit_add_signed_imm(X16, X16, static_cast<int32_t>(offset));
         emit_ldr_offset(rd, X16, 0);
      }

      void emit_global_store(uint32_t gi, uint32_t rs) {
         auto offset = _mod.get_global_offset(gi);
         emit_ldr_offset(X16, X20, wasm_allocator::globals_end() - 8);
         emit_add_signed_imm(X16, X16, static_cast<int32_t>(offset));
         emit_str_offset(rs, X16, 0);
      }

      // ──────── Memory access helpers ────────

      // Compute native address: X0 = X20 + wasm_addr + offset
      // wasm_addr in rd, static offset added
      void emit_effective_addr(uint32_t rd, uint32_t addr_reg, uint32_t uoffset) {
         if (uoffset & 0x80000000u) {
            emit_mov_imm32(X16, uoffset);
            emit32(0x8B100000 | (addr_reg << 5) | rd); // ADD Xd, addr, X16
            emit32(0x8B140000 | (rd << 5) | rd);       // ADD Xd, Xd, X20
         } else if (uoffset > 0) {
            emit_add_imm(rd, addr_reg, uoffset);
            emit32(0x8B140000 | (rd << 5) | rd); // ADD Xd, Xd, X20
         } else {
            emit32(0x8B140000 | (addr_reg << 5) | rd); // ADD Xd, addr, X20
         }
      }

      // Memory load: various sizes and sign extensions
      void emit_mem_load(uint32_t rd, uint32_t addr_reg, uint32_t uoffset, uint32_t load_op) {
         emit_effective_addr(X16, addr_reg, uoffset);
         emit32(load_op | (X16 << 5) | rd);
      }

      void emit_mem_store(uint32_t rs, uint32_t addr_reg, uint32_t uoffset, uint32_t store_op) {
         emit_effective_addr(X16, addr_reg, uoffset);
         emit32(store_op | (X16 << 5) | rs);
      }

      // Load opcodes (all use [Xn] addressing, offset 0)
      static constexpr uint32_t LDR_W  = 0xB9400000; // LDR Wt, [Xn]
      static constexpr uint32_t LDR_X  = 0xF9400000; // LDR Xt, [Xn]
      static constexpr uint32_t LDRB   = 0x39400000; // LDRB Wt, [Xn]
      static constexpr uint32_t LDRH   = 0x79400000; // LDRH Wt, [Xn]
      static constexpr uint32_t LDRSB_W = 0x39C00000; // LDRSB Wt, [Xn]
      static constexpr uint32_t LDRSH_W = 0x79C00000; // LDRSH Wt, [Xn]
      static constexpr uint32_t LDRSB_X = 0x39800000; // LDRSB Xt, [Xn]
      static constexpr uint32_t LDRSH_X = 0x79800000; // LDRSH Xt, [Xn]
      static constexpr uint32_t LDRSW   = 0xB9800000; // LDRSW Xt, [Xn]

      // Store opcodes
      static constexpr uint32_t STR_W  = 0xB9000000; // STR Wt, [Xn]
      static constexpr uint32_t STR_X  = 0xF9000000; // STR Xt, [Xn]
      static constexpr uint32_t STRB   = 0x39000000; // STRB Wt, [Xn]
      static constexpr uint32_t STRH   = 0x79000000; // STRH Wt, [Xn]

      // ──────── Const-immediate helpers ────────

      bool try_get_const(uint32_t vreg, int64_t& out) {
         if (!_func_def_inst || vreg >= _num_vregs) return false;
         uint32_t def = _func_def_inst[vreg];
         if (def >= _func_inst_count) return false;
         auto& di = _func_insts[def];
         if (di.opcode != ir_op::const_i32 && di.opcode != ir_op::const_i64) return false;
         out = di.imm64;
         return true;
      }

      void kill_const_if_single_use(uint32_t vreg) {
         if (_func_use_count && vreg < _num_vregs && _func_use_count[vreg] == 1) {
            uint32_t def = _func_def_inst[vreg];
            if (def < _func_inst_count)
               _func_insts[def].flags |= IR_DEAD;
         }
      }

      // ──────── Block/branch tracking ────────

      struct block_fixup {
         void* branch;
         block_fixup* next;
      };

      void mark_block_start(uint32_t block_idx) {
         if (block_idx < _num_blocks) _block_addrs[block_idx] = code;
      }

      void mark_block_end(ir_function& func, uint32_t block_idx, bool is_if) {
         if (block_idx >= _num_blocks) return;
         // For loop blocks, don't overwrite the start address — backward
         // branches need it.  Forward fixups still resolve to code (the end).
         bool is_loop = func.blocks && func.blocks[block_idx].is_loop;
         if (!is_loop) _block_addrs[block_idx] = code;
         for (auto* f = _block_fixups[block_idx]; f; f = f->next) {
            fix_branch(f->branch, code);
         }
         _block_fixups[block_idx] = nullptr;
         if (is_if) pop_if_fixup_to(code);
      }

      void* emit_branch_placeholder() {
         void* branch = code;
         emit32(0x14000000); // B (patched later)
         return branch;
      }

      void* emit_cond_branch_placeholder(uint32_t cond) {
         void* branch = code;
         emit32(0x54000000 | cond); // B.cond (patched later)
         return branch;
      }

      void emit_branch_to_block(ir_function& func, uint32_t block_idx) {
         if (block_idx >= _num_blocks) return;
         // Loop blocks: backward branch to block_start (address already known).
         // Non-loop blocks: forward fixup to block_end (address set later).
         bool is_loop = func.blocks && func.blocks[block_idx].is_loop;
         if (is_loop && _block_addrs[block_idx] != nullptr) {
            void* branch = emit_branch_placeholder();
            fix_branch(branch, _block_addrs[block_idx]);
         } else {
            void* branch = emit_branch_placeholder();
            auto* fixup = _allocator.alloc<block_fixup>(1);
            fixup->branch = branch;
            fixup->next = _block_fixups[block_idx];
            _block_fixups[block_idx] = fixup;
         }
      }

      void emit_cond_branch_to_block(ir_function& func, uint32_t block_idx, uint32_t cond) {
         if (block_idx >= _num_blocks) return;
         bool is_loop = func.blocks && func.blocks[block_idx].is_loop;
         if (is_loop && _block_addrs[block_idx] != nullptr) {
            void* branch = emit_cond_branch_placeholder(cond);
            fix_branch(branch, _block_addrs[block_idx]);
         } else {
            void* branch = emit_cond_branch_placeholder(cond);
            auto* fixup = _allocator.alloc<block_fixup>(1);
            fixup->branch = branch;
            fixup->next = _block_fixups[block_idx];
            _block_fixups[block_idx] = fixup;
         }
      }

      // If fixup stack
      static constexpr uint32_t MAX_IF_DEPTH = 256;
      void* _if_fixups[MAX_IF_DEPTH];
      uint32_t _if_fixup_top = 0;

      void push_if_fixup(void* branch) {
         if (_if_fixup_top < MAX_IF_DEPTH) _if_fixups[_if_fixup_top++] = branch;
      }
      void pop_if_fixup_to(void* target) {
         if (_if_fixup_top > 0) {
            void* branch = _if_fixups[--_if_fixup_top];
            if (branch && target) fix_branch(branch, target);
         }
      }

      // ──────── Branch fusion ────────

      bool emit_fused_branch(ir_function& func, uint32_t idx, uint32_t cc) {
         auto& next = func.insts[idx + 1];
         if (next.opcode == ir_op::if_) {
            void* branch = emit_cond_branch_placeholder(invert_condition(cc));
            push_if_fixup(branch);
            return true;
         }
         if (next.opcode == ir_op::br_if) {
            emit_cond_branch_to_block(func, next.br.target, cc);
            return true;
         }
         return false;
      }

      // ──────── Emit call ────────

      void* emit_bl_placeholder() {
         void* branch = code;
         emit32(0x94000000); // BL (patched)
         return branch;
      }

      // ──────── SSE/NEON float helpers ────────

      // Move between GP and FP registers
      void emit_fmov_to_fp(uint32_t vd, uint32_t xn, bool is64) {
         if (is64) emit32(0x9E670000 | (xn << 5) | vd); // FMOV Dd, Xn
         else      emit32(0x1E270000 | (xn << 5) | vd); // FMOV Sd, Wn
      }
      void emit_fmov_from_fp(uint32_t xd, uint32_t vn, bool is64) {
         if (is64) emit32(0x9E660000 | (vn << 5) | xd); // FMOV Xd, Dn
         else      emit32(0x1E260000 | (vn << 5) | xd); // FMOV Wd, Sn
      }


      // ──────── v128 stack helpers (32-byte layout: low@SP+0, high@SP+16) ────────

      static constexpr uint32_t V0n = 0, V1n = 1, V2n = 2, V3n = 3;

      // Load v128 from stack at SP+offset into NEON register Vd
      void emit_v128_load_at(uint32_t vd, int32_t offset) {
         emit_ldr_offset(X0, SP, offset);        // low half
         emit_ldr_offset(X1, SP, offset + 16);   // high half
         emit32(0x9E670000 | (X0 << 5) | vd);    // FMOV Dd, X0
         emit32(0x4E181C00 | (X1 << 5) | vd);    // INS Vd.D[1], X1
      }

      // Store NEON register Vs to stack at SP+offset
      void emit_v128_store_at(uint32_t vs, int32_t offset) {
         emit32(0x4E083C00 | (vs << 5) | X0);    // UMOV X0, Vs.D[0]
         emit32(0x4E183C00 | (vs << 5) | X1);    // UMOV X1, Vs.D[1]
         emit_str_offset(X0, SP, offset);
         emit_str_offset(X1, SP, offset + 16);
      }

      // Load v128 from TOS (SP+0)
      void emit_v128_load_tos(uint32_t vd = 0) { emit_v128_load_at(vd, 0); }
      // Store to TOS (SP+0)
      void emit_v128_store_tos(uint32_t vs = 0) { emit_v128_store_at(vs, 0); }

      // Pop v128 from stack (32 bytes)
      void emit_v128_pop(uint32_t vd = 0) {
         emit_v128_load_tos(vd);
         emit_add_imm(SP, SP, 32);
      }

      // Push v128 to stack (32 bytes)
      void emit_v128_push(uint32_t vs = 0) {
         emit32(0x4E083C00 | (vs << 5) | X0);    // UMOV X0, Vs.D[0]
         emit32(0x4E183C00 | (vs << 5) | X1);    // UMOV X1, Vs.D[1]
         emit_push(X1);  // high first (deeper)
         emit_push(X0);  // low on top
      }

      // ──────── SIMD helper patterns ────────

      // Unary NEON: load v128 TOS, apply op V0→V0, store back
      void simd_neon_unop(uint32_t opcode) {
         emit_v128_load_tos(V0n);
         emit32(opcode | (V0n << 5) | V0n);
         emit_v128_store_tos(V0n);
      }

      // Binary NEON: pop TOS→V1, load NOS→V0, OP V0,V0,V1, store to NOS
      void simd_neon_binop(uint32_t opcode) {
         emit_v128_pop(V1n);
         emit_v128_load_tos(V0n);
         emit32(opcode | (V1n << 16) | (V0n << 5) | V0n);
         emit_v128_store_tos(V0n);
      }

      // Binary NEON (commutative): same as binop
      void simd_neon_binop_r(uint32_t opcode) { simd_neon_binop(opcode); }

      // Comparison NEON: pop TOS→V1(rhs), load NOS→V0(lhs), compare, store
      void simd_neon_cmp(uint32_t opcode, bool swap, bool invert) {
         emit_v128_pop(V1n);
         emit_v128_load_tos(V0n);
         if (swap)
            emit32(opcode | (V0n << 16) | (V1n << 5) | V0n);  // OP V0, V1, V0
         else
            emit32(opcode | (V1n << 16) | (V0n << 5) | V0n);  // OP V0, V0, V1
         if (invert) {
            emit32(0x6E205800 | (V0n << 5) | V0n);  // MVN V0.16B, V0.16B
         }
         emit_v128_store_tos(V0n);
      }

      // Softfloat unary: v128_t fn(v128_t)
      void simd_v128_unop_softfloat(v128_t (*fn)(v128_t)) {
         // Load v128 from TOS into X0(low), X1(high) — AAPCS64
         emit_ldr_offset(X0, SP, 0);
         emit_ldr_offset(X1, SP, 16);
         // X19-X28 are callee-saved, no need to save context/membase
         emit_mov_imm64(X8, reinterpret_cast<uint64_t>(fn));
         emit32(0xD63F0100);  // BLR X8
         // Result in X0(low), X1(high) — store back to TOS
         emit_str_offset(X0, SP, 0);
         emit_str_offset(X1, SP, 16);
      }

      // Softfloat binary: v128_t fn(v128_t, v128_t)
      void simd_v128_binop_softfloat(v128_t (*fn)(v128_t, v128_t)) {
         // TOS=arg2(32b), NOS=arg1(32b). AAPCS64: arg1 in X0:X1, arg2 in X2:X3
         emit_ldr_offset(X2, SP, 0);    // arg2.low
         emit_ldr_offset(X3, SP, 16);   // arg2.high
         emit_ldr_offset(X0, SP, 32);   // arg1.low
         emit_ldr_offset(X1, SP, 48);   // arg1.high
         emit_mov_imm64(X8, reinterpret_cast<uint64_t>(fn));
         emit32(0xD63F0100);  // BLR X8
         // Pop arg2 (32 bytes)
         emit_add_imm(SP, SP, 32);
         // Store result to NOS (now TOS)
         emit_str_offset(X0, SP, 0);
         emit_str_offset(X1, SP, 16);
      }

      // Softfloat ternary: v128_t fn(v128_t, v128_t, v128_t)
      void simd_v128_ternop_softfloat(v128_t (*fn)(v128_t, v128_t, v128_t)) {
         // Stack: arg1 (bottom, 32b), arg2 (middle, 32b), arg3 (top, 32b)
         // AAPCS64: arg1 in X0:X1, arg2 in X2:X3, arg3 in X4:X5
         emit_ldr_offset(X4, SP, 0);    // arg3.low
         emit_ldr_offset(X5, SP, 16);   // arg3.high
         emit_ldr_offset(X2, SP, 32);   // arg2.low
         emit_ldr_offset(X3, SP, 48);   // arg2.high
         emit_ldr_offset(X0, SP, 64);   // arg1.low
         emit_ldr_offset(X1, SP, 80);   // arg1.high
         emit_mov_imm64(X8, reinterpret_cast<uint64_t>(fn));
         emit32(0xD63F0100);  // BLR X8
         // Pop arg2 + arg3 (64 bytes)
         emit_add_imm(SP, SP, 64);
         // Store result to arg1 slot (now TOS)
         emit_str_offset(X0, SP, 0);
         emit_str_offset(X1, SP, 16);
      }

      // SIMD memory load: load addr from vreg, load from memory, push v128
      void simd_v128_load(uint32_t offset, uint32_t addr_vreg) {
         load_vreg_x0(addr_vreg);
         emit_effective_addr(X0, X0, offset);
         // LDR Q0, [X0]
         emit32(0x3DC00000 | (X0 << 5) | V0n);
         emit_v128_push(V0n);
      }

      // SIMD memory load with sign/zero extend (64→128)
      void simd_v128_load_extend(uint32_t offset, uint32_t addr_vreg, uint32_t extend_op) {
         load_vreg_x0(addr_vreg);
         emit_effective_addr(X0, X0, offset);
         // Load 64 bits into D0
         emit32(0xFD400000 | (X0 << 5) | V0n);  // LDR D0, [X0]
         // Extend: e.g., SSHLL V0.8H, V0.8B, #0 (sign extend bytes to halfwords)
         emit32(extend_op | (V0n << 5) | V0n);
         emit_v128_push(V0n);
      }

      // SIMD memory load splat
      void simd_v128_load_splat(uint32_t offset, uint32_t addr_vreg, uint32_t ld1r_op) {
         load_vreg_x0(addr_vreg);
         emit_effective_addr(X0, X0, offset);
         // LD1R {V0.xT}, [X0]
         emit32(ld1r_op | (X0 << 5) | V0n);
         emit_v128_push(V0n);
      }

      // SIMD memory load zero (32 or 64 bits, rest zeroed)
      void simd_v128_load_zero(uint32_t offset, uint32_t addr_vreg, bool is64) {
         load_vreg_x0(addr_vreg);
         emit_effective_addr(X0, X0, offset);
         // Zero the full register first
         emit32(0x6F00E400 | V0n);  // MOVI V0.2D, #0
         if (is64) {
            emit32(0xFD400000 | (X0 << 5) | V0n);  // LDR D0, [X0] (sets low 64, zeros high)
         } else {
            emit32(0xBD400000 | (X0 << 5) | V0n);  // LDR S0, [X0] (sets low 32, zeros rest)
         }
         emit_v128_push(V0n);
      }

      // SIMD memory store
      void simd_v128_store(uint32_t offset, uint32_t addr_vreg) {
         emit_v128_pop(V0n);
         load_vreg_x0(addr_vreg);
         emit_effective_addr(X0, X0, offset);
         // STR Q0, [X0]
         emit32(0x3D800000 | (X0 << 5) | V0n);
      }

      // SIMD load lane: pop v128, load addr from vreg, insert lane, push v128
      void simd_v128_load_lane(uint32_t offset, uint32_t addr_vreg, uint8_t lane, uint32_t ldr_op, uint32_t ins_op, uint32_t imm5) {
         emit_v128_pop(V0n);
         load_vreg_x0(addr_vreg);
         emit_effective_addr(X0, X0, offset);
         // Load scalar value
         emit32(ldr_op | (X0 << 5) | X0);
         // Insert into lane
         emit32(ins_op | (imm5 << 16) | (X0 << 5) | V0n);
         emit_v128_push(V0n);
      }

      // SIMD store lane: pop v128, load addr from vreg, extract lane, store
      void simd_v128_store_lane(uint32_t offset, uint32_t addr_vreg, uint8_t lane, uint32_t extract_op, uint32_t imm5, uint32_t str_op) {
         emit_v128_pop(V0n);
         load_vreg_x0(addr_vreg);
         emit_effective_addr(X0, X0, offset);
         // Extract lane to X1/W1
         emit32(extract_op | (imm5 << 16) | (V0n << 5) | X1);
         // Store
         emit32(str_op | (X0 << 5) | X1);
      }

      // SIMD extract lane: pop v128, extract scalar, store to dest vreg
      void simd_v128_extract_lane(uint32_t extract_op, uint32_t imm5, uint32_t dest_vreg) {
         emit_v128_pop(V0n);
         emit32(extract_op | (imm5 << 16) | (V0n << 5) | X0);
         store_x0_vreg(dest_vreg);
      }

      // SIMD replace lane: load scalar from vreg, pop v128, insert lane, push v128
      void simd_v128_replace_lane(uint32_t ins_op, uint32_t imm5, uint32_t scalar_vreg) {
         emit_v128_pop(V0n);  // pop v128 first (clobbers X0/X1)
         load_vreg_x0(scalar_vreg);  // then load scalar
         emit32(ins_op | (imm5 << 16) | (X0 << 5) | V0n);
         emit_v128_push(V0n);
      }

      // SIMD splat: load scalar from vreg, broadcast to v128, push v128
      void simd_v128_splat(uint32_t dup_op, uint32_t scalar_vreg) {
         load_vreg_x0(scalar_vreg);
         emit32(dup_op | (X0 << 5) | V0n);  // DUP V0.xT, X0/W0
         emit_v128_push(V0n);
      }

      // SIMD shift: pop i32 count, load v128 TOS, shift, store back
      void simd_neon_shift(uint32_t shift_op, uint32_t dup_size, uint8_t mask, uint32_t shift_vreg) {
         emit_v128_load_tos(V0n);  // load v128 first (clobbers X0/X1)
         load_vreg_x0(shift_vreg);  // then load shift amount
         emit_mov_imm32(X16, mask);
         emit32(0x0A100000 | (X0 << 5) | X0);  // AND W0, W0, W16
         // DUP the shift amount to a vector
         emit32(dup_size | (X0 << 5) | V1n);  // DUP V1.xT, W0
         // Apply shift
         emit32(shift_op | (V1n << 16) | (V0n << 5) | V0n);
         emit_v128_store_tos(V0n);
      }

      // SIMD right shift (need to negate shift amount for SSHL/USHL)
      void simd_neon_shift_right(uint32_t shift_op, uint32_t dup_size, uint8_t mask, uint32_t shift_vreg) {
         emit_v128_load_tos(V0n);  // load v128 first (clobbers X0/X1)
         load_vreg_x0(shift_vreg);  // then load shift amount
         emit_mov_imm32(X16, mask);
         emit32(0x0A100000 | (X0 << 5) | X0);  // AND W0, W0, W16
         // Negate for right shift: SSHL/USHL use negative for right
         emit32(0x4B0003E0 | X0);  // NEG W0, W0
         emit32(dup_size | (X0 << 5) | V1n);
         emit32(shift_op | (V1n << 16) | (V0n << 5) | V0n);
         emit_v128_store_tos(V0n);
      }


      // ──────── Main IR instruction emission (register mode) ────────

      void emit_ir_inst_reg(ir_function& func, const ir_inst& inst, uint32_t idx) {
         if (inst.flags & IR_DEAD) return;

         switch (inst.opcode) {
         case ir_op::nop:
         case ir_op::block:
         case ir_op::loop:
            break;
         case ir_op::drop:
            if (inst.type == types::v128) {
               emit_add_imm(SP, SP, 32);  // pop v128 (32 bytes)
            }
            break;

         // arg: push a vreg value to the stack (for upcoming call)
         // For v128 vregs (no phys reg or spill), data is already on native stack — no-op
         case ir_op::arg: {
            uint32_t src = inst.rr.src1;
            int8_t pr = get_phys(src);
            if (pr >= 0 || (_spill_map && src < _num_vregs && _spill_map[src] >= 0)) {
               load_vreg_x0(src);
               emit_push(X0);
            }
            break;
         }

         case ir_op::block_start:
            mark_block_start(inst.dest);
            break;
         case ir_op::block_end:
            mark_block_end(func, inst.dest, inst.flags & 1);
            break;

         // ── Constants ──
         case ir_op::const_i32: {
            uint32_t val = static_cast<uint32_t>(inst.imm64);
            int8_t pr = get_phys(inst.dest);
            if (pr >= 0) emit_mov_imm32(phys_to_reg(pr), val);
            else { emit_mov_imm32(X0, val); store_x0_vreg(inst.dest); }
            break;
         }
         case ir_op::const_i64: {
            uint64_t val = static_cast<uint64_t>(inst.imm64);
            int8_t pr = get_phys(inst.dest);
            if (pr >= 0) emit_mov_imm64(phys_to_reg(pr), val);
            else { emit_mov_imm64(X0, val); store_x0_vreg(inst.dest); }
            break;
         }
         case ir_op::const_f32: {
            uint32_t bits;
            memcpy(&bits, &inst.immf32, 4);
            int8_t pr = get_phys(inst.dest);
            if (pr >= 0) emit_mov_imm32(phys_to_reg(pr), bits);
            else { emit_mov_imm32(X0, bits); store_x0_vreg(inst.dest); }
            break;
         }
         case ir_op::const_f64: {
            uint64_t bits;
            memcpy(&bits, &inst.immf64, 8);
            int8_t pr = get_phys(inst.dest);
            if (pr >= 0) emit_mov_imm64(phys_to_reg(pr), bits);
            else { emit_mov_imm64(X0, bits); store_x0_vreg(inst.dest); }
            break;
         }

         case ir_op::const_v128: {
            uint64_t low, high;
            memcpy(&low, &inst.immv128, 8);
            memcpy(&high, reinterpret_cast<const char*>(&inst.immv128) + 8, 8);
            emit_mov_imm64(X0, high);
            emit_push(X0);  // high first (deeper)
            emit_mov_imm64(X0, low);
            emit_push(X0);  // low on top
            break;
         }

         case ir_op::v128_op:
            emit_simd_op(inst);
            break;

         // ── Mov ──
         case ir_op::mov: {
            int8_t pr_d = get_phys(inst.dest);
            int8_t pr_s = get_phys(inst.rr.src1);
            if (pr_d >= 0 && pr_s >= 0) emit_mov_reg(phys_to_reg(pr_d), phys_to_reg(pr_s));
            else { load_vreg_x0(inst.rr.src1); store_x0_vreg(inst.dest); }
            break;
         }

         // ── Integer binary ops (3-operand ARM64) ──
         case ir_op::i32_add: emit_binop(func, inst, 0x0B000000, true); break;  // ADD Wd
         case ir_op::i32_sub: emit_binop(func, inst, 0x4B000000, true); break;  // SUB Wd
         case ir_op::i32_mul: emit_binop3(inst, 0x1B007C00, true); break;       // MUL Wd (MADD Wd,Wn,Wm,WZR)
         case ir_op::i32_and: emit_binop(func, inst, 0x0A000000, true); break;  // AND Wd
         case ir_op::i32_or:  emit_binop(func, inst, 0x2A000000, true); break;  // ORR Wd
         case ir_op::i32_xor: emit_binop(func, inst, 0x4A000000, true); break;  // EOR Wd
         case ir_op::i64_add: emit_binop(func, inst, 0x8B000000, false); break;
         case ir_op::i64_sub: emit_binop(func, inst, 0xCB000000, false); break;
         case ir_op::i64_mul: emit_binop3(inst, 0x9B007C00, false); break;      // MUL Xd (MADD Xd,Xn,Xm,XZR)
         case ir_op::i64_and: emit_binop(func, inst, 0x8A000000, false); break;
         case ir_op::i64_or:  emit_binop(func, inst, 0xAA000000, false); break;
         case ir_op::i64_xor: emit_binop(func, inst, 0xCA000000, false); break;

         // ── Shifts ──
         case ir_op::i32_shl:   emit_binop_simple(inst, 0x1AC02000, true); break;  // LSLV Wd
         case ir_op::i32_shr_u: emit_binop_simple(inst, 0x1AC02400, true); break;  // LSRV Wd
         case ir_op::i32_shr_s: emit_binop_simple(inst, 0x1AC02800, true); break;  // ASRV Wd
         case ir_op::i32_rotl: emit_i32_rotl(inst); break;
         case ir_op::i32_rotr:  emit_binop_simple(inst, 0x1AC02C00, true); break;  // RORV Wd
         case ir_op::i64_shl:   emit_binop_simple(inst, 0x9AC02000, false); break;
         case ir_op::i64_shr_u: emit_binop_simple(inst, 0x9AC02400, false); break;
         case ir_op::i64_shr_s: emit_binop_simple(inst, 0x9AC02800, false); break;
         case ir_op::i64_rotl: emit_i64_rotl(inst); break;
         case ir_op::i64_rotr:  emit_binop_simple(inst, 0x9AC02C00, false); break;

         // ── Division (with trap checks for div-by-zero and overflow) ──
         case ir_op::i32_div_s: emit_i32_div_s(inst); break;
         case ir_op::i32_div_u: emit_i32_div_u(inst); break;
         case ir_op::i64_div_s: emit_i64_div_s(inst); break;
         case ir_op::i64_div_u: emit_i64_div_u(inst); break;

         // ── Remainder (with trap check for div-by-zero) ──
         case ir_op::i32_rem_s: emit_i32_rem_s(inst); break;
         case ir_op::i32_rem_u: emit_i32_rem_u(inst); break;
         case ir_op::i64_rem_s: emit_i64_rem_s(inst); break;
         case ir_op::i64_rem_u: emit_i64_rem_u(inst); break;

         // ── Unary integer ──
         case ir_op::i32_clz:
            load_vreg_x0(inst.rr.src1);
            emit32(0x5AC01000); // CLZ W0, W0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i32_ctz:
            load_vreg_x0(inst.rr.src1);
            emit32(0x5AC00000); // RBIT W0, W0
            emit32(0x5AC01000); // CLZ W0, W0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i32_popcnt:
            load_vreg_x0(inst.rr.src1);
            // FMOV S0, W0; CNT V0.8B, V0.8B; ADDV B0, V0.8B; UMOV W0, V0.B[0]
            emit32(0x1E270000); // FMOV S0, W0
            emit32(0x0E205800); // CNT V0.8B, V0.8B
            emit32(0x0E31B800); // ADDV B0, V0.8B
            emit32(0x0E013C00); // UMOV W0, V0.B[0]
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_clz:
            load_vreg_x0(inst.rr.src1);
            emit32(0xDAC01000); // CLZ X0, X0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_ctz:
            load_vreg_x0(inst.rr.src1);
            emit32(0xDAC00000); // RBIT X0, X0
            emit32(0xDAC01000); // CLZ X0, X0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_popcnt:
            load_vreg_x0(inst.rr.src1);
            emit32(0x9E670000); // FMOV D0, X0
            emit32(0x0E205800); // CNT V0.8B, V0.8B
            emit32(0x0E31B800); // ADDV B0, V0.8B
            emit32(0x0E013C00); // UMOV W0, V0.B[0]
            store_x0_vreg(inst.dest);
            break;

         // ── Comparisons ──
         case ir_op::i32_eqz: emit_eqz(func, inst, idx, true); break;
         case ir_op::i64_eqz: emit_eqz(func, inst, idx, false); break;
         case ir_op::i32_eq:  emit_cmp(func, inst, idx, COND_EQ, true); break;
         case ir_op::i32_ne:  emit_cmp(func, inst, idx, COND_NE, true); break;
         case ir_op::i32_lt_s: emit_cmp(func, inst, idx, COND_LT, true); break;
         case ir_op::i32_lt_u: emit_cmp(func, inst, idx, COND_LO, true); break;
         case ir_op::i32_gt_s: emit_cmp(func, inst, idx, COND_GT, true); break;
         case ir_op::i32_gt_u: emit_cmp(func, inst, idx, COND_HI, true); break;
         case ir_op::i32_le_s: emit_cmp(func, inst, idx, COND_LE, true); break;
         case ir_op::i32_le_u: emit_cmp(func, inst, idx, COND_LS, true); break;
         case ir_op::i32_ge_s: emit_cmp(func, inst, idx, COND_GE, true); break;
         case ir_op::i32_ge_u: emit_cmp(func, inst, idx, COND_HS, true); break;
         case ir_op::i64_eq:  emit_cmp(func, inst, idx, COND_EQ, false); break;
         case ir_op::i64_ne:  emit_cmp(func, inst, idx, COND_NE, false); break;
         case ir_op::i64_lt_s: emit_cmp(func, inst, idx, COND_LT, false); break;
         case ir_op::i64_lt_u: emit_cmp(func, inst, idx, COND_LO, false); break;
         case ir_op::i64_gt_s: emit_cmp(func, inst, idx, COND_GT, false); break;
         case ir_op::i64_gt_u: emit_cmp(func, inst, idx, COND_HI, false); break;
         case ir_op::i64_le_s: emit_cmp(func, inst, idx, COND_LE, false); break;
         case ir_op::i64_le_u: emit_cmp(func, inst, idx, COND_LS, false); break;
         case ir_op::i64_ge_s: emit_cmp(func, inst, idx, COND_GE, false); break;
         case ir_op::i64_ge_u: emit_cmp(func, inst, idx, COND_HS, false); break;

         // ── Select ──
         case ir_op::select: {
            load_vreg_x0(inst.sel.cond);
            load_vreg_to(X1, inst.sel.val1);
            load_vreg_to(X16, inst.sel.val2);
            emit_cmp_imm32(X0, 0);
            // CSEL X0, X1, X16, NE
            emit32(0x9A900020 | (COND_NE << 12) | (X16 << 16) | (X1 << 5) | X0);
            store_x0_vreg(inst.dest);
            break;
         }

         // ── Conversions ──
         case ir_op::i32_wrap_i64:
            load_vreg_x0(inst.rr.src1);
            emit_mov_reg32(X0, X0); // MOV W0, W0 (zero-extend)
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_extend_s_i32:
            load_vreg_x0(inst.rr.src1);
            emit32(0x93407C00); // SXTW X0, W0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_extend_u_i32:
            load_vreg_x0(inst.rr.src1);
            emit_mov_reg32(X0, X0); // zero-extend by moving 32-bit
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i32_extend8_s:
            load_vreg_x0(inst.rr.src1);
            emit32(0x13001C00); // SXTB W0, W0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i32_extend16_s:
            load_vreg_x0(inst.rr.src1);
            emit32(0x13003C00); // SXTH W0, W0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_extend8_s:
            load_vreg_x0(inst.rr.src1);
            emit32(0x93401C00); // SXTB X0, W0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_extend16_s:
            load_vreg_x0(inst.rr.src1);
            emit32(0x93403C00); // SXTH X0, W0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::i64_extend32_s:
            load_vreg_x0(inst.rr.src1);
            emit32(0x93407C00); // SXTW X0, W0
            store_x0_vreg(inst.dest);
            break;

         // ── Reinterpret (no-op, same bit pattern) ──
         case ir_op::i32_reinterpret_f32:
         case ir_op::i64_reinterpret_f64:
         case ir_op::f32_reinterpret_i32:
         case ir_op::f64_reinterpret_i64:
            if (inst.rr.src1 != ir_vreg_none && inst.dest != ir_vreg_none) {
               load_vreg_x0(inst.rr.src1);
               store_x0_vreg(inst.dest);
            }
            break;

         // ── Control flow ──
         case ir_op::if_: {
            load_vreg_x0(inst.br.src1);
            emit_cmp_imm32(X0, 0);
            void* branch = emit_cond_branch_placeholder(COND_EQ);
            push_if_fixup(branch);
            break;
         }
         case ir_op::else_: {
            uint32_t target_block = inst.br.target;
            if (target_block < _num_blocks) {
               void* jmp = emit_branch_placeholder();
               auto* fixup = _allocator.alloc<block_fixup>(1);
               fixup->branch = jmp;
               fixup->next = _block_fixups[target_block];
               _block_fixups[target_block] = fixup;
            }
            pop_if_fixup_to(code);
            break;
         }
         case ir_op::br: {
            if (_in_br_table) {
               bool is_default = (_br_table_case >= _br_table_size);
               if (is_default) {
                  emit_pop(X0); // discard index
                  emit_branch_to_block(func, inst.br.target);
                  _in_br_table = false;
               } else {
                  // CMP W [SP], #case — use X17 for index since emit_cmp_imm32 clobbers X16
                  emit_ldr_offset(X17, SP, 0);
                  emit_cmp_imm32(X17, _br_table_case);
                  void* skip = emit_cond_branch_placeholder(COND_NE);
                  emit_pop(X0); // pop index
                  emit_branch_to_block(func, inst.br.target);
                  fix_branch(skip, code);
                  _br_table_case++;
               }
            } else {
               emit_branch_to_block(func, inst.br.target);
            }
            break;
         }
         case ir_op::br_if: {
            load_vreg_x0(inst.br.src1);
            emit_cmp_imm32(X0, 0);
            emit_cond_branch_to_block(func, inst.br.target, COND_NE);
            break;
         }
         case ir_op::br_table: {
            load_vreg_x0(inst.rr.src1);
            emit_push(X0);
            _br_table_case = 0;
            _br_table_size = inst.dest;
            _in_br_table = true;
            break;
         }
         case ir_op::unreachable:
            emit_error_handler(&on_unreachable);
            break;

         // ── Multi-value return store ──
         case ir_op::multi_return_store: {
            load_vreg_x0(inst.ri.src1);
            int32_t offset = multi_return_offset + inst.ri.imm;
            // STR X0, [X19, #offset]
            emit_str_offset(X0, X19, offset);
            break;
         }

         // ── Multi-value call return load ──
         case ir_op::multi_return_load: {
            int32_t offset = multi_return_offset + inst.ri.imm;
            emit_ldr_offset(X0, X19, offset);
            store_x0_vreg(inst.dest);
            break;
         }

         // ── Return ──
         case ir_op::return_: {
            if (inst.rr.src1 != ir_vreg_none) {
               if (inst.type == types::v128) {
                  // v128 return: load from native stack into X0:X1 (AAPCS64 16-byte return)
                  emit_ldr_offset(X0, SP, 0);   // low qword
                  emit_ldr_offset(X1, SP, 16);  // high qword
               } else {
                  load_vreg_x0(inst.rr.src1);
               }
            }
            // Restore callee-saved (FP-relative — stable regardless of v128 stack)
            if (_callee_saved_used) {
               int32_t save_offset = static_cast<int32_t>((_body_locals + _num_spill_slots) * 8) - _frame_size;
               for (int i = 0; i < 7; ++i) {
                  if (_callee_saved_used & (1 << i)) {
                     emit_ldr_offset(callee_saved_reg(i), FP, save_offset);
                     save_offset += 8;
                  }
               }
            }
            emit32(0x91000000 | (FP << 5) | SP); // MOV SP, X29
            emit32(0xA8C17BFD); // LDP X29, X30, [SP], #16
            emit32(0xD65F03C0); // RET
            break;
         }

         // ── Calls ──
         case ir_op::call: {
            uint32_t funcnum = inst.call.index;
            const func_type& ft = _mod.get_function_type(funcnum);
            emit_call_depth_dec();
            void* branch = emit_bl_placeholder();
            register_call(branch, funcnum);
            // Pop params
            uint32_t arg_bytes = 0;
            for (uint32_t p = 0; p < ft.param_types.size(); ++p)
               arg_bytes += (ft.param_types[p] == types::v128) ? 32 : 16;
            if (arg_bytes > 0) emit_add_imm(SP, SP, arg_bytes);
            emit_call_depth_inc();
            if (ft.return_count > 1) {
               // Multi-value: separate multi_return_load instructions handle the loads
            } else if (ft.return_count > 0) {
               if (ft.return_type == types::v128) {
                  emit_push(X1);
                  emit_push(X0);
               } else if (inst.dest != ir_vreg_none) {
                  store_x0_vreg(inst.dest);
               }
            }
            break;
         }
         case ir_op::call_indirect: {
            // Unpack: lower 16 bits = function type index, upper 16 = table index
            uint32_t fti = inst.call.index & 0xFFFF;
            uint32_t table_idx = inst.call.index >> 16;
            const func_type& ft = _mod.types[fti];
            // Pop element index
            emit_pop(X0);

            if (table_idx != 0) {
               // Non-zero table: use runtime helper for bounds/type check
               emit32(0xAA0003E3); // MOV X3, X0 (elem_idx)
               emit_mov_reg(X0, X19);           // X0 = ctx
               emit_mov_imm32(X1, fti);         // W1 = type_idx
               emit_mov_imm32(X2, table_idx);   // W2 = table_idx
               emit_push(X19); emit_push(X20);
               emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&__psizam_resolve_indirect));
               emit32(0xD63F0100); // BLR X8
               emit_pop(X20); emit_pop(X19);
               // X0 = code_ptr (null on error)
               // CBZ X0 → call_indirect_handler
               emit_branch_to_handler_cbz(X0, call_indirect_handler);
               emit32(0xAA0003E8); // MOV X8, X0
               emit_call_depth_dec();
               emit32(0xD63F0100); // BLR X8
            } else {
               // Bounds check
               uint32_t table_size = _mod.tables[0].limits.initial;
               emit_cmp_imm32(X0, table_size);
               emit_branch_to_handler(COND_HS, call_indirect_handler);
               // Table entry: index * 16
               emit32(0xD37CEC00); // LSL X0, X0, #4
               if (_mod.indirect_table(0)) {
                  int32_t toff = wasm_allocator::table_offset();
                  if (toff >= 0) {
                     emit_ldr_offset(X8, X20, toff);
                  } else {
                     emit_mov_imm64(X16, static_cast<uint64_t>(static_cast<int64_t>(toff)));
                     emit32(0x8B100000 | (X20 << 5) | X8); // ADD X8, X20, X16
                     emit32(0xF9400108); // LDR X8, [X8]
                  }
                  emit32(0x8B000000 | (X0 << 16) | (X8 << 5) | X0); // ADD X0, X8, X0
               } else {
                  // table_offset is negative: X0 = X20 + X0 + table_offset
                  int32_t toff = wasm_allocator::table_offset();
                  emit32(0x8B000000 | (X0 << 16) | (X20 << 5) | X0); // ADD X0, X20, X0
                  if (toff < 0) {
                     emit_sub_imm(X0, X0, static_cast<uint32_t>(-toff));
                  } else {
                     emit_add_imm(X0, X0, static_cast<uint32_t>(toff));
                  }
               }
               // Type check
               emit32(0xB9400008 | (X0 << 5)); // LDR W8, [X0]
               emit_cmp_imm32(X8, fti);
               emit_branch_to_handler(COND_NE, type_error_handler);
               // Load function pointer
               emit_ldr_offset(X8, X0, 8);
               emit_call_depth_dec();
               emit32(0xD63F0100); // BLR X8
            }
            // Pop params
            uint32_t arg_bytes = 0;
            for (uint32_t p = 0; p < ft.param_types.size(); ++p)
               arg_bytes += (ft.param_types[p] == types::v128) ? 32 : 16;
            if (arg_bytes > 0) emit_add_imm(SP, SP, arg_bytes);
            emit_call_depth_inc();
            if (ft.return_count > 1) {
               // Multi-value: separate multi_return_load instructions handle the loads
            } else if (ft.return_count > 0) {
               if (ft.return_type == types::v128) {
                  emit_push(X1);
                  emit_push(X0);
               } else if (inst.dest != ir_vreg_none) {
                  store_x0_vreg(inst.dest);
               }
            }
            break;
         }

         // ── Local access ──
         case ir_op::local_get: {
            if (inst.type == types::v128) {
               int32_t offset = get_frame_offset(func, inst.local.index);
               bool is_param = inst.local.index < func.type->param_types.size();
               int32_t high_stride = is_param ? 16 : 8;  // params: 16-byte stride, body locals: contiguous
               emit_ldr_offset(X0, FP, offset);                // low qword
               emit_ldr_offset(X1, FP, offset + high_stride);  // high qword
               emit_push(X1);  // high first (deeper)
               emit_push(X0);  // low on top
            } else {
               int8_t pr = get_phys(inst.dest);
               uint32_t rd = (pr >= 0) ? phys_to_reg(pr) : X0;
               emit_local_load(func, inst.local.index, rd);
               if (pr < 0) store_x0_vreg(inst.dest);
            }
            break;
         }
         case ir_op::local_set: {
            if (inst.type == types::v128) {
               int32_t offset = get_frame_offset(func, inst.local.index);
               bool is_param = inst.local.index < func.type->param_types.size();
               int32_t high_stride = is_param ? 16 : 8;
               emit_ldr_offset(X0, SP, 0);    // low qword (TOS)
               emit_ldr_offset(X1, SP, 16);   // high qword (TOS + 16)
               emit_add_imm(SP, SP, 32);      // pop v128 (32 bytes)
               emit_str_offset(X0, FP, offset);                // store low
               emit_str_offset(X1, FP, offset + high_stride);  // store high
            } else {
               int8_t pr = get_phys(inst.local.src1);
               uint32_t rs = (pr >= 0) ? phys_to_reg(pr) : X0;
               if (pr < 0) load_vreg_x0(inst.local.src1);
               emit_local_store(func, inst.local.index, rs);
            }
            break;
         }
         case ir_op::local_tee: {
            if (inst.type == types::v128) {
               int32_t offset = get_frame_offset(func, inst.local.index);
               bool is_param = inst.local.index < func.type->param_types.size();
               int32_t high_stride = is_param ? 16 : 8;
               emit_ldr_offset(X0, SP, 0);    // low qword (TOS, don't pop)
               emit_ldr_offset(X1, SP, 16);   // high qword
               emit_str_offset(X0, FP, offset);
               emit_str_offset(X1, FP, offset + high_stride);
            } else {
               load_vreg_x0(inst.local.src1);
               emit_local_store(func, inst.local.index, X0);
            }
            break;
         }

         // ── Global access ──
         case ir_op::global_get: {
            if (inst.type == types::v128) {
               auto offset = _mod.get_global_offset(inst.local.index);
               emit_ldr_offset(X16, X20, wasm_allocator::globals_end() - 8);
               emit_add_signed_imm(X16, X16, static_cast<int32_t>(offset));
               emit_ldr_offset(X0, X16, 0);   // low qword
               emit_ldr_offset(X1, X16, 8);   // high qword
               emit_push(X1);  // high first (deeper)
               emit_push(X0);  // low on top
            } else {
               emit_global_load(inst.local.index, X0);
               store_x0_vreg(inst.dest);
            }
            break;
         }
         case ir_op::global_set: {
            if (inst.type == types::v128) {
               emit_ldr_offset(X0, SP, 0);    // low qword (TOS)
               emit_ldr_offset(X1, SP, 16);   // high qword
               emit_add_imm(SP, SP, 32);      // pop v128
               auto offset = _mod.get_global_offset(inst.local.index);
               emit_ldr_offset(X16, X20, wasm_allocator::globals_end() - 8);
               emit_add_signed_imm(X16, X16, static_cast<int32_t>(offset));
               emit_str_offset(X0, X16, 0);   // store low
               emit_str_offset(X1, X16, 8);   // store high
            } else {
               load_vreg_x0(inst.local.src1);
               emit_global_store(inst.local.index, X0);
            }
            break;
         }

         // ── Memory loads ──
         case ir_op::i32_load:     emit_load_op(inst, LDR_W); break;
         case ir_op::i64_load:     emit_load_op(inst, LDR_X); break;
         case ir_op::f32_load:     emit_load_op(inst, LDR_W); break;
         case ir_op::f64_load:     emit_load_op(inst, LDR_X); break;
         case ir_op::i32_load8_u:  emit_load_op(inst, LDRB); break;
         case ir_op::i32_load16_u: emit_load_op(inst, LDRH); break;
         case ir_op::i32_load8_s:  emit_load_op(inst, LDRSB_W); break;
         case ir_op::i32_load16_s: emit_load_op(inst, LDRSH_W); break;
         case ir_op::i64_load8_u:  emit_load_op(inst, LDRB); break;
         case ir_op::i64_load16_u: emit_load_op(inst, LDRH); break;
         case ir_op::i64_load32_u: emit_load_op(inst, LDR_W); break;
         case ir_op::i64_load8_s:  emit_load_op(inst, LDRSB_X); break;
         case ir_op::i64_load16_s: emit_load_op(inst, LDRSH_X); break;
         case ir_op::i64_load32_s: emit_load_op(inst, LDRSW); break;

         // ── Memory stores ──
         case ir_op::i32_store:   emit_store_op(inst, STR_W); break;
         case ir_op::i64_store:   emit_store_op(inst, STR_X); break;
         case ir_op::f32_store:   emit_store_op(inst, STR_W); break;
         case ir_op::f64_store:   emit_store_op(inst, STR_X); break;
         case ir_op::i32_store8:  emit_store_op(inst, STRB); break;
         case ir_op::i32_store16: emit_store_op(inst, STRH); break;
         case ir_op::i64_store8:  emit_store_op(inst, STRB); break;
         case ir_op::i64_store16: emit_store_op(inst, STRH); break;
         case ir_op::i64_store32: emit_store_op(inst, STR_W); break;

         // ── Memory management ──
         case ir_op::memory_size:
            emit_push(X19); emit_push(X20);
            emit_mov_reg(X0, X19);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&current_memory));
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            store_x0_vreg(inst.dest);
            break;
         case ir_op::memory_grow:
            load_vreg_x0(inst.rr.src1);
            emit_push(X19); emit_push(X20);
            emit_mov_reg(X1, X0); // pages
            emit_mov_reg(X0, X19); // context
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&grow_memory));
            emit32(0xD63F0100);
            emit_pop(X20); emit_pop(X19);
            store_x0_vreg(inst.dest);
            break;

         // ── Float unary ops ──
         case ir_op::f32_abs:
            load_vreg_x0(inst.rr.src1);
            emit32(0x12007800); // AND W0, W0, #0x7FFFFFFF
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f32_neg:
            load_vreg_x0(inst.rr.src1);
            emit32(0x52B00008); // MOVZ W8, #0x8000, LSL #16
            emit32(0x4A080000); // EOR W0, W0, W8
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f64_abs:
            load_vreg_x0(inst.rr.src1);
            emit32(0x9240F800); // AND X0, X0, #0x7FFFFFFFFFFFFFFF
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f64_neg:
            load_vreg_x0(inst.rr.src1);
            emit_mov_imm64(X8, 0x8000000000000000ULL);
            emit32(0xCA080000); // EOR X0, X0, X8
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f32_sqrt:
            load_vreg_x0(inst.rr.src1);
            emit32(0x1E270000); // FMOV S0, W0
            emit32(0x1E21C000); // FSQRT S0, S0
            emit32(0x1E260000); // FMOV W0, S0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f64_sqrt:
            load_vreg_x0(inst.rr.src1);
            emit32(0x9E670000); // FMOV D0, X0
            emit32(0x1E61C000); // FSQRT D0, D0
            emit32(0x9E660000); // FMOV X0, D0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f32_ceil:   emit_f32_round(inst, 0x1E24C000); break; // FRINTP S0, S0
         case ir_op::f32_floor:  emit_f32_round(inst, 0x1E254000); break; // FRINTM S0, S0
         case ir_op::f32_trunc:  emit_f32_round(inst, 0x1E25C000); break; // FRINTZ S0, S0
         case ir_op::f32_nearest:emit_f32_round(inst, 0x1E244000); break; // FRINTN S0, S0 (ties to even)
         case ir_op::f64_ceil:   emit_f64_round(inst, 0x1E64C000); break; // FRINTP D0, D0
         case ir_op::f64_floor:  emit_f64_round(inst, 0x1E654000); break; // FRINTM D0, D0
         case ir_op::f64_trunc:  emit_f64_round(inst, 0x1E65C000); break; // FRINTZ D0, D0
         case ir_op::f64_nearest:emit_f64_round(inst, 0x1E644000); break; // FRINTN D0, D0 (ties to even)

         // ── Float binary ops ──
         case ir_op::f32_add: emit_f32_binop(inst, 0x1E202800); break; // FADD S0, S0, S1
         case ir_op::f32_sub: emit_f32_binop(inst, 0x1E203800); break;
         case ir_op::f32_mul: emit_f32_binop(inst, 0x1E200800); break;
         case ir_op::f32_div: emit_f32_binop(inst, 0x1E201800); break;
         case ir_op::f32_min: emit_f32_min(inst); break;
         case ir_op::f32_max: emit_f32_max(inst); break;
         case ir_op::f64_add: emit_f64_binop(inst, 0x1E602800); break;
         case ir_op::f64_sub: emit_f64_binop(inst, 0x1E603800); break;
         case ir_op::f64_mul: emit_f64_binop(inst, 0x1E600800); break;
         case ir_op::f64_div: emit_f64_binop(inst, 0x1E601800); break;
         case ir_op::f64_min: emit_f64_min(inst); break;
         case ir_op::f64_max: emit_f64_max(inst); break;

         case ir_op::f32_copysign:
            load_vreg_x0(inst.rr.src1); // magnitude
            load_vreg_x1(inst.rr.src2); // sign
            emit32(0x12007800); // AND W0, W0, #0x7FFFFFFF
            emit32(0x12010021); // AND W1, W1, #0x80000000
            emit32(0x2A010000); // ORR W0, W0, W1
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f64_copysign:
            load_vreg_x0(inst.rr.src1);
            load_vreg_x1(inst.rr.src2);
            emit32(0x9240F800); // AND X0, X0, #0x7FFFFFFFFFFFFFFF
            emit_mov_imm64(X8, 0x8000000000000000ULL);
            emit32(0x8A080021); // AND X1, X1, X8
            emit32(0xAA010000); // ORR X0, X0, X1
            store_x0_vreg(inst.dest);
            break;

         // ── Float comparisons ──
         case ir_op::f32_eq: emit_f32_cmp(func, inst, idx, COND_EQ); break;
         case ir_op::f32_ne: emit_f32_cmp(func, inst, idx, COND_NE); break;
         case ir_op::f32_lt: emit_f32_cmp(func, inst, idx, COND_LO); break; // MI for ordered, but LO handles unordered
         case ir_op::f32_gt: emit_f32_cmp(func, inst, idx, COND_GT); break;
         case ir_op::f32_le: emit_f32_cmp(func, inst, idx, COND_LS); break;
         case ir_op::f32_ge: emit_f32_cmp(func, inst, idx, COND_GE); break;
         case ir_op::f64_eq: emit_f64_cmp(func, inst, idx, COND_EQ); break;
         case ir_op::f64_ne: emit_f64_cmp(func, inst, idx, COND_NE); break;
         case ir_op::f64_lt: emit_f64_cmp(func, inst, idx, COND_LO); break;
         case ir_op::f64_gt: emit_f64_cmp(func, inst, idx, COND_GT); break;
         case ir_op::f64_le: emit_f64_cmp(func, inst, idx, COND_LS); break;
         case ir_op::f64_ge: emit_f64_cmp(func, inst, idx, COND_GE); break;

         // ── Float-to-int conversions (trapping: overflow/NaN → exception) ──
         case ir_op::i32_trunc_s_f32:     emit_fcvt_trap(inst, 0x1E380000, true, true); break;   // FCVTZS W0, S0
         case ir_op::i32_trunc_u_f32:     emit_fcvt_trap(inst, 0x1E390000, true, true); break;   // FCVTZU W0, S0
         case ir_op::i32_trunc_s_f64:     emit_fcvt_trap(inst, 0x1E780000, true, false); break;  // FCVTZS W0, D0
         case ir_op::i32_trunc_u_f64:     emit_fcvt_trap(inst, 0x1E790000, true, false); break;
         case ir_op::i64_trunc_s_f32:     emit_fcvt_trap(inst, 0x9E380000, true, true); break;   // FCVTZS X0, S0
         case ir_op::i64_trunc_u_f32:     emit_fcvt_trap(inst, 0x9E390000, true, true); break;
         case ir_op::i64_trunc_s_f64:     emit_fcvt_trap(inst, 0x9E780000, true, false); break;
         case ir_op::i64_trunc_u_f64:     emit_fcvt_trap(inst, 0x9E790000, true, false); break;
         case ir_op::i32_trunc_sat_f32_s: emit_fcvt(inst, 0x1E380000, true, true); break;
         case ir_op::i32_trunc_sat_f32_u: emit_fcvt(inst, 0x1E390000, true, true); break;
         case ir_op::i32_trunc_sat_f64_s: emit_fcvt(inst, 0x1E780000, true, false); break;
         case ir_op::i32_trunc_sat_f64_u: emit_fcvt(inst, 0x1E790000, true, false); break;
         case ir_op::i64_trunc_sat_f32_s: emit_fcvt(inst, 0x9E380000, true, true); break;
         case ir_op::i64_trunc_sat_f32_u: emit_fcvt(inst, 0x9E390000, true, true); break;
         case ir_op::i64_trunc_sat_f64_s: emit_fcvt(inst, 0x9E780000, true, false); break;
         case ir_op::i64_trunc_sat_f64_u: emit_fcvt(inst, 0x9E790000, true, false); break;

         // ── Int-to-float conversions ──
         case ir_op::f32_convert_s_i32:   emit_icvt(inst, 0x1E220000, true, true); break;   // SCVTF S0, W0
         case ir_op::f32_convert_u_i32:   emit_icvt(inst, 0x1E230000, true, true); break;
         case ir_op::f32_convert_s_i64:   emit_icvt(inst, 0x9E220000, false, true); break;
         case ir_op::f32_convert_u_i64:   emit_icvt(inst, 0x9E230000, false, true); break;
         case ir_op::f64_convert_s_i32:   emit_icvt(inst, 0x1E620000, true, false); break;
         case ir_op::f64_convert_u_i32:   emit_icvt(inst, 0x1E630000, true, false); break;
         case ir_op::f64_convert_s_i64:   emit_icvt(inst, 0x9E620000, false, false); break;
         case ir_op::f64_convert_u_i64:   emit_icvt(inst, 0x9E630000, false, false); break;

         // ── Float-float conversions ──
         case ir_op::f32_demote_f64:
            load_vreg_x0(inst.rr.src1);
            emit32(0x9E670000); // FMOV D0, X0
            emit32(0x1E624000); // FCVT S0, D0
            emit32(0x1E260000); // FMOV W0, S0
            store_x0_vreg(inst.dest);
            break;
         case ir_op::f64_promote_f32:
            load_vreg_x0(inst.rr.src1);
            emit32(0x1E270000); // FMOV S0, W0
            emit32(0x1E22C000); // FCVT D0, S0
            emit32(0x9E660000); // FMOV X0, D0
            store_x0_vreg(inst.dest);
            break;

         // ── Bulk memory ──
         case ir_op::memory_fill: {
            // Stack: [dest, value, count]
            emit_pop(X3); // count
            emit_pop(X2); // value
            emit_pop(X1); // dest
            emit_mov_reg(X0, X19); // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&memory_fill_checked));
            emit32(0xD63F0100);
            emit_pop(X20); emit_pop(X19);
            break;
         }
         case ir_op::memory_copy: {
            // Stack: [dest, src, count]
            emit_pop(X3); // count
            emit_pop(X2); // src
            emit_pop(X1); // dest
            emit_mov_reg(X0, X19); // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&memory_copy_checked));
            emit32(0xD63F0100);
            emit_pop(X20); emit_pop(X19);
            break;
         }

         case ir_op::memory_init:
         case ir_op::table_init: {
            // Stack: [dest, src, count] from arg pushes
            emit_pop(X4);  // count
            emit_pop(X3);  // src
            emit_pop(X2);  // dest
            emit_mov_imm32(X1, static_cast<uint32_t>(inst.ri.imm)); // seg_idx
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            auto fn = (inst.opcode == ir_op::memory_init)
               ? reinterpret_cast<uint64_t>(&memory_init_impl)
               : reinterpret_cast<uint64_t>(&table_init_impl);
            emit_mov_imm64(X8, fn);
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            break;
         }
         case ir_op::data_drop:
         case ir_op::elem_drop: {
            emit_mov_imm32(X1, static_cast<uint32_t>(inst.ri.imm)); // seg_idx
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            auto fn = (inst.opcode == ir_op::data_drop)
               ? reinterpret_cast<uint64_t>(&data_drop_impl)
               : reinterpret_cast<uint64_t>(&elem_drop_impl);
            emit_mov_imm64(X8, fn);
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            break;
         }
         case ir_op::table_copy: {
            // Stack: [dest, src, count] from arg pushes
            emit_pop(X3);  // count
            emit_pop(X2);  // src
            emit_pop(X1);  // dest
            emit_mov_imm32(X4, static_cast<uint32_t>(inst.ri.imm)); // packed table indices
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&table_copy_impl));
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            break;
         }

         case ir_op::table_get: {
            emit_pop(X2);  // elem_idx → arg3
            emit_mov_imm32(X1, static_cast<uint32_t>(inst.ri.imm)); // table_idx → arg2
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&__psizam_table_get));
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            emit_push(X0); // push result
            break;
         }
         case ir_op::table_set: {
            emit_pop(X3);  // val → arg4
            emit_pop(X2);  // elem_idx → arg3
            emit_mov_imm32(X1, static_cast<uint32_t>(inst.ri.imm)); // table_idx → arg2
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&__psizam_table_set));
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            break;
         }
         case ir_op::table_grow: {
            // Stack: [init_val, delta]
            emit_pop(X2);  // delta → arg3
            emit_pop(X3);  // init_val → arg4
            emit_mov_imm32(X1, static_cast<uint32_t>(inst.ri.imm)); // table_idx → arg2
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&__psizam_table_grow));
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            emit_push(X0); // push result
            break;
         }
         case ir_op::table_size: {
            emit_mov_imm32(X1, static_cast<uint32_t>(inst.ri.imm)); // table_idx → arg2
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&__psizam_table_size));
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            emit_push(X0); // push result
            break;
         }
         case ir_op::table_fill: {
            // Stack: [i, val, n]
            emit_pop(X4);  // n → arg5
            emit_pop(X3);  // val → arg4
            emit_pop(X2);  // i → arg3
            emit_mov_imm32(X1, static_cast<uint32_t>(inst.ri.imm)); // table_idx → arg2
            emit_mov_reg(X0, X19);  // ctx
            emit_push(X19); emit_push(X20);
            emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&__psizam_table_fill));
            emit32(0xD63F0100); // BLR X8
            emit_pop(X20); emit_pop(X19);
            break;
         }

         case ir_op::atomic_op: {
            uint8_t asub = inst.simd.lane;
            auto sub = static_cast<atomic_sub>(asub);
            // Fence: no-op
            if (sub == atomic_sub::atomic_fence) break;
            // Notify: push 0
            if (sub == atomic_sub::memory_atomic_notify) {
               emit_mov_imm32(X0, 0);
               emit_push(X0);
               break;
            }
            // Wait: push 1
            if (sub == atomic_sub::memory_atomic_wait32 || sub == atomic_sub::memory_atomic_wait64) {
               emit_mov_imm32(X0, 1);
               emit_push(X0);
               break;
            }
            // Loads: addr from stack, load from memory
            if (asub >= 0x10 && asub <= 0x16) {
               emit_pop(X0); // addr
               emit32(0x8B000000 | (X0 << 16) | (X20 << 5) | X0); // ADD X0, X20, X0 (membase+addr)
               if (inst.simd.offset) emit_add_imm(X0, X0, inst.simd.offset);
               switch(sub) {
               case atomic_sub::i32_atomic_load:    emit32(0xB9400000); break; // LDR W0, [X0]
               case atomic_sub::i64_atomic_load:    emit32(0xF9400000); break; // LDR X0, [X0]
               case atomic_sub::i32_atomic_load8_u: emit32(0x39400000); break; // LDRB W0, [X0]
               case atomic_sub::i32_atomic_load16_u:emit32(0x79400000); break; // LDRH W0, [X0]
               case atomic_sub::i64_atomic_load8_u: emit32(0x39400000); break; // LDRB W0, [X0]
               case atomic_sub::i64_atomic_load16_u:emit32(0x79400000); break; // LDRH W0, [X0]
               case atomic_sub::i64_atomic_load32_u:emit32(0xB9400000); break; // LDR W0, [X0]
               default: break;
               }
               emit_push(X0);
               break;
            }
            // Stores: pop value and addr, store
            if (asub >= 0x17 && asub <= 0x1D) {
               emit_pop(X1); // value
               emit_pop(X0); // addr
               emit32(0x8B000000 | (X0 << 16) | (X20 << 5) | X0); // ADD X0, X20, X0 (membase+addr)
               if (inst.simd.offset) emit_add_imm(X0, X0, inst.simd.offset);
               switch(sub) {
               case atomic_sub::i32_atomic_store:   emit32(0xB9000001); break; // STR W1, [X0]
               case atomic_sub::i64_atomic_store:   emit32(0xF9000001); break; // STR X1, [X0]
               case atomic_sub::i32_atomic_store8:  emit32(0x39000001); break; // STRB W1, [X0]
               case atomic_sub::i32_atomic_store16: emit32(0x79000001); break; // STRH W1, [X0]
               case atomic_sub::i64_atomic_store8:  emit32(0x39000001); break; // STRB W1, [X0]
               case atomic_sub::i64_atomic_store16: emit32(0x79000001); break; // STRH W1, [X0]
               case atomic_sub::i64_atomic_store32: emit32(0xB9000001); break; // STR W1, [X0]
               default: break;
               }
               break;
            }
            // RMW + cmpxchg: call __psizam_atomic_rmw(ctx, sub, addr, offset, val1, val2)
            {
               bool is_cmpxchg = (asub >= 0x48);
               if (is_cmpxchg) {
                  emit_pop(X5); // replacement
                  emit_pop(X4); // expected
               } else {
                  emit_pop(X4); // value
                  emit_mov_imm64(X5, 0);
               }
               emit_pop(X2); // addr
               emit_push(X19); emit_push(X20);
               emit_mov_reg(X0, X19); // ctx
               emit_mov_imm32(X1, asub); // sub
               emit_mov_imm32(X3, inst.simd.offset); // offset
               emit_mov_imm64(X8, reinterpret_cast<uint64_t>(&__psizam_atomic_rmw));
               emit32(0xD63F0100); // BLR X8
               emit_pop(X20); emit_pop(X19);
               emit_push(X0); // push old value
            }
            break;
         }

         default:
            break;
         }
      }

      void emit_simd_op(const ir_inst& inst) {
         auto sub = static_cast<simd_sub>(inst.dest);
         switch (sub) {

         // ── Memory operations ──
         case simd_sub::v128_load: simd_v128_load(inst.simd.offset, inst.simd.addr); break;
         case simd_sub::v128_load8x8_s:  simd_v128_load_extend(inst.simd.offset, inst.simd.addr, 0x0F08A400); break;
         case simd_sub::v128_load8x8_u:  simd_v128_load_extend(inst.simd.offset, inst.simd.addr, 0x2F08A400); break;
         case simd_sub::v128_load16x4_s: simd_v128_load_extend(inst.simd.offset, inst.simd.addr, 0x0F10A400); break;
         case simd_sub::v128_load16x4_u: simd_v128_load_extend(inst.simd.offset, inst.simd.addr, 0x2F10A400); break;
         case simd_sub::v128_load32x2_s: simd_v128_load_extend(inst.simd.offset, inst.simd.addr, 0x0F20A400); break;
         case simd_sub::v128_load32x2_u: simd_v128_load_extend(inst.simd.offset, inst.simd.addr, 0x2F20A400); break;
         case simd_sub::v128_load8_splat:  simd_v128_load_splat(inst.simd.offset, inst.simd.addr, 0x4D40C000); break;
         case simd_sub::v128_load16_splat: simd_v128_load_splat(inst.simd.offset, inst.simd.addr, 0x4D40C400); break;
         case simd_sub::v128_load32_splat: simd_v128_load_splat(inst.simd.offset, inst.simd.addr, 0x4D40C800); break;
         case simd_sub::v128_load64_splat: simd_v128_load_splat(inst.simd.offset, inst.simd.addr, 0x4D40CC00); break;
         case simd_sub::v128_load32_zero: simd_v128_load_zero(inst.simd.offset, inst.simd.addr, false); break;
         case simd_sub::v128_load64_zero: simd_v128_load_zero(inst.simd.offset, inst.simd.addr, true); break;
         case simd_sub::v128_store: simd_v128_store(inst.simd.offset, inst.simd.addr); break;

         // Lane loads: load scalar from memory, insert into v128 lane
         case simd_sub::v128_load8_lane:
            simd_v128_load_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane, LDRB, 0x4E001C00,
               static_cast<uint32_t>((inst.simd.lane << 1) | 1)); break;
         case simd_sub::v128_load16_lane:
            simd_v128_load_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane, LDRH, 0x4E001C00,
               static_cast<uint32_t>((inst.simd.lane << 2) | 2)); break;
         case simd_sub::v128_load32_lane:
            simd_v128_load_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane, LDR_W, 0x4E001C00,
               static_cast<uint32_t>((inst.simd.lane << 3) | 4)); break;
         case simd_sub::v128_load64_lane:
            simd_v128_load_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane, LDR_X, 0x4E001C00,
               static_cast<uint32_t>((inst.simd.lane << 4) | 8)); break;

         // Lane stores: extract lane from v128, store scalar to memory
         case simd_sub::v128_store8_lane:
            simd_v128_store_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane,
               0x0E003C00, static_cast<uint32_t>((inst.simd.lane << 1) | 1), STRB); break;
         case simd_sub::v128_store16_lane:
            simd_v128_store_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane,
               0x0E003C00, static_cast<uint32_t>((inst.simd.lane << 2) | 2), STRH); break;
         case simd_sub::v128_store32_lane:
            simd_v128_store_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane,
               0x0E003C00, static_cast<uint32_t>((inst.simd.lane << 3) | 4), STR_W); break;
         case simd_sub::v128_store64_lane:
            simd_v128_store_lane(inst.simd.offset, inst.simd.addr, inst.simd.lane,
               0x4E083C00, static_cast<uint32_t>((inst.simd.lane << 4) | 8), STR_X); break;

         // ── Shuffle ──
         case simd_sub::i8x16_shuffle: {
            const uint8_t* lanes = reinterpret_cast<const uint8_t*>(&inst.immv128);
            emit_v128_pop(V1n);      // second operand (indices 16-31)
            emit_v128_load_tos(V0n); // first operand (indices 0-15)
            // Build the lane index vector in V2
            uint64_t idx_low, idx_high;
            memcpy(&idx_low, lanes, 8);
            memcpy(&idx_high, lanes + 8, 8);
            emit_mov_imm64(X0, idx_low);
            emit_mov_imm64(X1, idx_high);
            emit32(0x9E670000 | (X0 << 5) | V2n);     // FMOV D2, X0
            emit32(0x4E181C00 | (X1 << 5) | V2n);     // INS V2.D[1], X1
            // TBL V0.16B, {V0.16B, V1.16B}, V2.16B
            emit32(0x4E002000 | (V2n << 16) | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }

         case simd_sub::i8x16_swizzle: {
            emit_v128_pop(V1n);      // indices
            emit_v128_load_tos(V0n); // table
            // TBL V0.16B, {V0.16B}, V1.16B
            emit32(0x4E000000 | (V1n << 16) | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }

         // ── Extract lane ──
         case simd_sub::i8x16_extract_lane_s: {
            uint32_t imm5 = (inst.simd.lane << 1) | 1;
            emit_v128_pop(V0n);
            emit32(0x0E002C00 | (imm5 << 16) | (V0n << 5) | X0);  // SMOV W0, V0.B[lane]
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i8x16_extract_lane_u:
            simd_v128_extract_lane(0x0E003C00, (inst.simd.lane << 1) | 1, inst.simd.addr); break;
         case simd_sub::i16x8_extract_lane_s: {
            uint32_t imm5 = (inst.simd.lane << 2) | 2;
            emit_v128_pop(V0n);
            emit32(0x0E002C00 | (imm5 << 16) | (V0n << 5) | X0);  // SMOV W0, V0.H[lane]
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i16x8_extract_lane_u:
            simd_v128_extract_lane(0x0E003C00, (inst.simd.lane << 2) | 2, inst.simd.addr); break;
         case simd_sub::i32x4_extract_lane:
            simd_v128_extract_lane(0x0E003C00, (inst.simd.lane << 3) | 4, inst.simd.addr); break;
         case simd_sub::i64x2_extract_lane:
            simd_v128_extract_lane(0x4E083C00, (inst.simd.lane << 4) | 8, inst.simd.addr); break;
         case simd_sub::f32x4_extract_lane:
            simd_v128_extract_lane(0x0E003C00, (inst.simd.lane << 3) | 4, inst.simd.addr); break;
         case simd_sub::f64x2_extract_lane:
            simd_v128_extract_lane(0x4E083C00, (inst.simd.lane << 4) | 8, inst.simd.addr); break;

         // ── Replace lane ──
         case simd_sub::i8x16_replace_lane:
            simd_v128_replace_lane(0x4E001C00, (inst.simd.lane << 1) | 1, inst.simd.offset); break;
         case simd_sub::i16x8_replace_lane:
            simd_v128_replace_lane(0x4E001C00, (inst.simd.lane << 2) | 2, inst.simd.offset); break;
         case simd_sub::i32x4_replace_lane:
            simd_v128_replace_lane(0x4E001C00, (inst.simd.lane << 3) | 4, inst.simd.offset); break;
         case simd_sub::i64x2_replace_lane:
            simd_v128_replace_lane(0x4E001C00, (inst.simd.lane << 4) | 8, inst.simd.offset); break;
         case simd_sub::f32x4_replace_lane:
            simd_v128_replace_lane(0x4E001C00, (inst.simd.lane << 3) | 4, inst.simd.offset); break;
         case simd_sub::f64x2_replace_lane:
            simd_v128_replace_lane(0x4E001C00, (inst.simd.lane << 4) | 8, inst.simd.offset); break;

         // ── Splat ──
         case simd_sub::i8x16_splat: simd_v128_splat(0x4E010C00, inst.simd.addr); break;  // DUP V0.16B, W0
         case simd_sub::i16x8_splat: simd_v128_splat(0x4E020C00, inst.simd.addr); break;  // DUP V0.8H, W0
         case simd_sub::i32x4_splat: simd_v128_splat(0x4E040C00, inst.simd.addr); break;  // DUP V0.4S, W0
         case simd_sub::i64x2_splat: simd_v128_splat(0x4E080C00, inst.simd.addr); break;  // DUP V0.2D, X0
         case simd_sub::f32x4_splat: simd_v128_splat(0x4E040C00, inst.simd.addr); break;  // DUP V0.4S, W0 (bits)
         case simd_sub::f64x2_splat: simd_v128_splat(0x4E080C00, inst.simd.addr); break;  // DUP V0.2D, X0 (bits)

         // ── i8x16 comparisons ──
         case simd_sub::i8x16_eq:   simd_neon_cmp(0x6E208C00, false, false); break; // CMEQ .16B
         case simd_sub::i8x16_ne:   simd_neon_cmp(0x6E208C00, false, true); break;  // CMEQ + NOT
         case simd_sub::i8x16_lt_s: simd_neon_cmp(0x4E203400, true, false); break;  // CMGT swap
         case simd_sub::i8x16_lt_u: simd_neon_cmp(0x6E203400, true, false); break;  // CMHI swap
         case simd_sub::i8x16_gt_s: simd_neon_cmp(0x4E203400, false, false); break; // CMGT
         case simd_sub::i8x16_gt_u: simd_neon_cmp(0x6E203400, false, false); break; // CMHI
         case simd_sub::i8x16_le_s: simd_neon_cmp(0x4E203400, false, true); break;  // CMGT + NOT
         case simd_sub::i8x16_le_u: simd_neon_cmp(0x6E203400, false, true); break;  // CMHI + NOT
         case simd_sub::i8x16_ge_s: simd_neon_cmp(0x4E203400, true, true); break;   // CMGT swap + NOT = CMLE
         case simd_sub::i8x16_ge_u: simd_neon_cmp(0x6E203400, true, true); break;   // CMHI swap + NOT = CMHS

         // ── i16x8 comparisons ──
         case simd_sub::i16x8_eq:   simd_neon_cmp(0x6E608C00, false, false); break;
         case simd_sub::i16x8_ne:   simd_neon_cmp(0x6E608C00, false, true); break;
         case simd_sub::i16x8_lt_s: simd_neon_cmp(0x4E603400, true, false); break;
         case simd_sub::i16x8_lt_u: simd_neon_cmp(0x6E603400, true, false); break;
         case simd_sub::i16x8_gt_s: simd_neon_cmp(0x4E603400, false, false); break;
         case simd_sub::i16x8_gt_u: simd_neon_cmp(0x6E603400, false, false); break;
         case simd_sub::i16x8_le_s: simd_neon_cmp(0x4E603400, false, true); break;
         case simd_sub::i16x8_le_u: simd_neon_cmp(0x6E603400, false, true); break;
         case simd_sub::i16x8_ge_s: simd_neon_cmp(0x4E603400, true, true); break;
         case simd_sub::i16x8_ge_u: simd_neon_cmp(0x6E603400, true, true); break;

         // ── i32x4 comparisons ──
         case simd_sub::i32x4_eq:   simd_neon_cmp(0x6EA08C00, false, false); break;
         case simd_sub::i32x4_ne:   simd_neon_cmp(0x6EA08C00, false, true); break;
         case simd_sub::i32x4_lt_s: simd_neon_cmp(0x4EA03400, true, false); break;
         case simd_sub::i32x4_lt_u: simd_neon_cmp(0x6EA03400, true, false); break;
         case simd_sub::i32x4_gt_s: simd_neon_cmp(0x4EA03400, false, false); break;
         case simd_sub::i32x4_gt_u: simd_neon_cmp(0x6EA03400, false, false); break;
         case simd_sub::i32x4_le_s: simd_neon_cmp(0x4EA03400, false, true); break;
         case simd_sub::i32x4_le_u: simd_neon_cmp(0x6EA03400, false, true); break;
         case simd_sub::i32x4_ge_s: simd_neon_cmp(0x4EA03400, true, true); break;
         case simd_sub::i32x4_ge_u: simd_neon_cmp(0x6EA03400, true, true); break;

         // ── i64x2 comparisons ──
         case simd_sub::i64x2_eq:   simd_neon_cmp(0x6EE08C00, false, false); break;
         case simd_sub::i64x2_ne:   simd_neon_cmp(0x6EE08C00, false, true); break;
         case simd_sub::i64x2_lt_s: simd_neon_cmp(0x4EE03400, true, false); break;
         case simd_sub::i64x2_gt_s: simd_neon_cmp(0x4EE03400, false, false); break;
         case simd_sub::i64x2_le_s: simd_neon_cmp(0x4EE03400, false, true); break;
         case simd_sub::i64x2_ge_s: simd_neon_cmp(0x4EE03400, true, true); break;

         // ── f32x4 comparisons (softfloat) ──
         case simd_sub::f32x4_eq: simd_v128_binop_softfloat(&_psizam_f32x4_eq); break;
         case simd_sub::f32x4_ne: simd_v128_binop_softfloat(&_psizam_f32x4_ne); break;
         case simd_sub::f32x4_lt: simd_v128_binop_softfloat(&_psizam_f32x4_lt); break;
         case simd_sub::f32x4_gt: simd_v128_binop_softfloat(&_psizam_f32x4_gt); break;
         case simd_sub::f32x4_le: simd_v128_binop_softfloat(&_psizam_f32x4_le); break;
         case simd_sub::f32x4_ge: simd_v128_binop_softfloat(&_psizam_f32x4_ge); break;

         // ── f64x2 comparisons (softfloat) ──
         case simd_sub::f64x2_eq: simd_v128_binop_softfloat(&_psizam_f64x2_eq); break;
         case simd_sub::f64x2_ne: simd_v128_binop_softfloat(&_psizam_f64x2_ne); break;
         case simd_sub::f64x2_lt: simd_v128_binop_softfloat(&_psizam_f64x2_lt); break;
         case simd_sub::f64x2_gt: simd_v128_binop_softfloat(&_psizam_f64x2_gt); break;
         case simd_sub::f64x2_le: simd_v128_binop_softfloat(&_psizam_f64x2_le); break;
         case simd_sub::f64x2_ge: simd_v128_binop_softfloat(&_psizam_f64x2_ge); break;

         // ── Logical ──
         case simd_sub::v128_not: {
            emit_v128_load_tos(V0n);
            emit32(0x6E205800 | (V0n << 5) | V0n);  // MVN V0.16B, V0.16B
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::v128_and:    simd_neon_binop(0x4E201C00); break;  // AND
         case simd_sub::v128_andnot: {
            // andnot(a, b) = a & ~b. NEON BIC = Vd & ~Vm
            emit_v128_pop(V1n);       // b
            emit_v128_load_tos(V0n);  // a
            emit32(0x4E611C00 | (V1n << 16) | (V0n << 5) | V0n);  // BIC V0, V0, V1
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::v128_or:     simd_neon_binop(0x4EA01C00); break;  // ORR
         case simd_sub::v128_xor:    simd_neon_binop(0x6E201C00); break;  // EOR

         case simd_sub::v128_bitselect: {
            // Stack: val1 (bottom), val2 (middle), mask (top)
            emit_v128_pop(V2n);              // mask
            emit_v128_pop(V1n);              // val2
            emit_v128_load_tos(V0n);         // val1
            // BSL V2, V0, V1 → result = (V0 & V2) | (V1 & ~V2)
            emit32(0x6E601C00 | (V1n << 16) | (V0n << 5) | V2n);  // BSL V2, V0, V1
            // Result is in V2
            emit32(0x4EA01C00 | (V2n << 16) | (V2n << 5) | V0n);  // ORR V0, V2, V2 (move)
            emit_v128_store_tos(V0n);
            break;
         }

         case simd_sub::v128_any_true: {
            emit_v128_pop(V0n);
            // UMAXV B0, V0.16B — reduce max across all bytes
            emit32(0x6E30A800 | (V0n << 5) | V0n);
            // UMOV W0, V0.B[0]
            emit32(0x0E013C00 | (V0n << 5) | X0);
            // CMP W0, #0; CSET W0, NE
            emit_cmp_imm32(X0, 0);
            emit_cset(X0, COND_NE);
            store_x0_vreg(inst.simd.addr);
            break;
         }

         // ── i8x16 arithmetic ──
         case simd_sub::i8x16_abs: simd_neon_unop(0x4E20B800); break;
         case simd_sub::i8x16_neg: simd_neon_unop(0x6E20B800); break;
         case simd_sub::i8x16_popcnt: simd_neon_unop(0x4E205800); break;
         case simd_sub::i8x16_all_true: {
            emit_v128_pop(V0n);
            emit32(0x6E31A800 | (V0n << 5) | V0n);  // UMINV B0, V0.16B
            emit32(0x0E013C00 | (V0n << 5) | X0);   // UMOV W0, V0.B[0]
            emit_cmp_imm32(X0, 0);
            emit_cset(X0, COND_NE);
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i8x16_bitmask: {
            emit_v128_pop(V0n);
            // CMLT V1.16B, V0.16B, #0 → each byte becomes 0xFF (sign set) or 0x00
            emit32(0x4E20A800 | (V0n << 5) | V1n);
            // Position mask: each byte gets its bit position (0x01,0x02,...,0x80) repeated
            emit_mov_imm64(X0, 0x8040201008040201ULL);
            emit32(0x9E670000 | (X0 << 5) | V2n);   // FMOV D2, X0
            emit32(0x4E181C00 | (X0 << 5) | V2n);   // INS V2.D[1], X0
            // AND V1, V1, V2 — mask sign bits into bit positions
            emit32(0x4E201C00 | (V2n << 16) | (V1n << 5) | V1n);
            // UADDLP chain to sum each 8-byte group into its doubleword
            emit32(0x6E202800 | (V1n << 5) | V1n);  // UADDLP V1.8H, V1.16B
            emit32(0x6E602800 | (V1n << 5) | V1n);  // UADDLP V1.4S, V1.8H
            emit32(0x6EA02800 | (V1n << 5) | V1n);  // UADDLP V1.2D, V1.4S
            // V1.D[0] = 8-bit bitmask for lanes 0-7, V1.D[1] = 8-bit bitmask for lanes 8-15
            // Combine: result = low | (high << 8)
            emit32(0x4E083C00 | (V1n << 5) | X0);   // UMOV X0, V1.D[0]
            emit32(0x4E183C00 | (V1n << 5) | X1);   // UMOV X1, V1.D[1]
            emit32(0xAA012000 | X0);                 // ORR X0, X0, X1, LSL #8
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i8x16_narrow_i16x8_s: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            // SQXTN V0.8B, V0.8H (narrow low)
            emit32(0x0E214800 | (V0n << 5) | V0n);
            // SQXTN2 V0.16B, V1.8H (narrow high)
            emit32(0x4E214800 | (V1n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i8x16_narrow_i16x8_u: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x2E212800 | (V0n << 5) | V0n);  // SQXTUN V0.8B, V0.8H
            emit32(0x6E212800 | (V1n << 5) | V0n);  // SQXTUN2 V0.16B, V1.8H
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i8x16_shl:   simd_neon_shift(0x4E204400, 0x4E010C00, 7, inst.simd.offset); break;
         case simd_sub::i8x16_shr_s: simd_neon_shift_right(0x4E204400, 0x4E010C00, 7, inst.simd.offset); break;
         case simd_sub::i8x16_shr_u: simd_neon_shift_right(0x6E204400, 0x4E010C00, 7, inst.simd.offset); break;
         case simd_sub::i8x16_add:       simd_neon_binop(0x4E208400); break;
         case simd_sub::i8x16_add_sat_s: simd_neon_binop(0x4E200C00); break;  // SQADD
         case simd_sub::i8x16_add_sat_u: simd_neon_binop(0x6E200C00); break;  // UQADD
         case simd_sub::i8x16_sub:       simd_neon_binop(0x6E208400); break;
         case simd_sub::i8x16_sub_sat_s: simd_neon_binop(0x4E202C00); break;  // SQSUB
         case simd_sub::i8x16_sub_sat_u: simd_neon_binop(0x6E202C00); break;  // UQSUB
         case simd_sub::i8x16_min_s: simd_neon_binop(0x4E206C00); break;  // SMIN
         case simd_sub::i8x16_min_u: simd_neon_binop(0x6E206C00); break;  // UMIN
         case simd_sub::i8x16_max_s: simd_neon_binop(0x4E206400); break;  // SMAX
         case simd_sub::i8x16_max_u: simd_neon_binop(0x6E206400); break;  // UMAX
         case simd_sub::i8x16_avgr_u: simd_neon_binop(0x6E201400); break; // URHADD

         // ── i16x8 arithmetic ──
         case simd_sub::i16x8_extadd_pairwise_i8x16_s: {
            emit_v128_load_tos(V0n);
            emit32(0x4E202800 | (V0n << 5) | V0n);  // SADDLP V0.8H, V0.16B
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_extadd_pairwise_i8x16_u: {
            emit_v128_load_tos(V0n);
            emit32(0x6E202800 | (V0n << 5) | V0n);  // UADDLP V0.8H, V0.16B
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_abs: simd_neon_unop(0x4E60B800); break;
         case simd_sub::i16x8_neg: simd_neon_unop(0x6E60B800); break;
         case simd_sub::i16x8_q15mulr_sat_s: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x6E60B400 | (V1n << 16) | (V0n << 5) | V0n);  // SQRDMULH V0.8H, V0.8H, V1.8H
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_all_true: {
            emit_v128_pop(V0n);
            emit32(0x6E71A800 | (V0n << 5) | V0n);  // UMINV H0, V0.8H
            emit32(0x0E023C00 | (V0n << 5) | X0);   // UMOV W0, V0.H[0]
            emit_cmp_imm32(X0, 0);
            emit_cset(X0, COND_NE);
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i16x8_bitmask: {
            emit_v128_pop(V0n);
            // CMLT V1.8H, V0.8H, #0 → 0xFFFF per negative lane, else 0
            emit32(0x4E60A800 | (V0n << 5) | V1n);
            // XTN V1.8B, V1.8H → narrow to bytes (0xFF or 0x00)
            emit32(0x0E212800 | (V1n << 5) | V1n);
            // Collect bits: multiply by powers of 2 and add
            emit_mov_imm64(X0, 0x8040201008040201ULL);
            emit32(0x9E670000 | (X0 << 5) | V2n);
            emit32(0x4E201C00 | (V2n << 16) | (V1n << 5) | V1n);  // AND
            emit32(0x6E202800 | (V1n << 5) | V1n);  // UADDLP V1.8H, V1.16B
            emit32(0x6E602800 | (V1n << 5) | V1n);  // UADDLP V1.4S, V1.8H
            emit32(0x6EA02800 | (V1n << 5) | V1n);  // UADDLP V1.2D, V1.4S
            emit32(0x9E660000 | (V1n << 5) | X0);   // FMOV X0, D1
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i16x8_narrow_i32x4_s: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x0E614800 | (V0n << 5) | V0n);  // SQXTN V0.4H, V0.4S
            emit32(0x4E614800 | (V1n << 5) | V0n);  // SQXTN2 V0.8H, V1.4S
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_narrow_i32x4_u: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x2E612800 | (V0n << 5) | V0n);  // SQXTUN V0.4H, V0.4S
            emit32(0x6E612800 | (V1n << 5) | V0n);  // SQXTUN2 V0.8H, V1.4S
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_extend_low_i8x16_s:  simd_neon_unop(0x0F08A400); break;  // SSHLL V0.8H, V0.8B, #0
         case simd_sub::i16x8_extend_high_i8x16_s: {
            emit_v128_load_tos(V0n);
            // EXT V0.16B, V0.16B, V0.16B, #8 (shift right 8 bytes)
            emit32(0x6E004000 | (8 << 11) | (V0n << 16) | (V0n << 5) | V0n);
            emit32(0x0F08A400 | (V0n << 5) | V0n);  // SSHLL V0.8H, V0.8B, #0
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_extend_low_i8x16_u:  simd_neon_unop(0x2F08A400); break;  // USHLL
         case simd_sub::i16x8_extend_high_i8x16_u: {
            emit_v128_load_tos(V0n);
            emit32(0x6E004000 | (8 << 11) | (V0n << 16) | (V0n << 5) | V0n);
            emit32(0x2F08A400 | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_shl:   simd_neon_shift(0x4E604400, 0x4E020C00, 0x0f, inst.simd.offset); break;
         case simd_sub::i16x8_shr_s: simd_neon_shift_right(0x4E604400, 0x4E020C00, 0x0f, inst.simd.offset); break;
         case simd_sub::i16x8_shr_u: simd_neon_shift_right(0x6E604400, 0x4E020C00, 0x0f, inst.simd.offset); break;
         case simd_sub::i16x8_add:       simd_neon_binop(0x4E608400); break;
         case simd_sub::i16x8_add_sat_s: simd_neon_binop(0x4E600C00); break;
         case simd_sub::i16x8_add_sat_u: simd_neon_binop(0x6E600C00); break;
         case simd_sub::i16x8_sub:       simd_neon_binop(0x6E608400); break;
         case simd_sub::i16x8_sub_sat_s: simd_neon_binop(0x4E602C00); break;
         case simd_sub::i16x8_sub_sat_u: simd_neon_binop(0x6E602C00); break;
         case simd_sub::i16x8_mul:       simd_neon_binop(0x4E609C00); break;
         case simd_sub::i16x8_min_s: simd_neon_binop(0x4E606C00); break;
         case simd_sub::i16x8_min_u: simd_neon_binop(0x6E606C00); break;
         case simd_sub::i16x8_max_s: simd_neon_binop(0x4E606400); break;
         case simd_sub::i16x8_max_u: simd_neon_binop(0x6E606400); break;
         case simd_sub::i16x8_avgr_u: simd_neon_binop(0x6E601400); break;
         case simd_sub::i16x8_extmul_low_i8x16_s: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x0E20C000 | (V1n << 16) | (V0n << 5) | V0n);  // SMULL V0.8H, V0.8B, V1.8B
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_extmul_high_i8x16_s: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x4E20C000 | (V1n << 16) | (V0n << 5) | V0n);  // SMULL2 V0.8H, V0.16B, V1.16B
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_extmul_low_i8x16_u: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x2E20C000 | (V1n << 16) | (V0n << 5) | V0n);  // UMULL V0.8H, V0.8B, V1.8B
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i16x8_extmul_high_i8x16_u: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x6E20C000 | (V1n << 16) | (V0n << 5) | V0n);  // UMULL2 V0.8H, V0.16B, V1.16B
            emit_v128_store_tos(V0n);
            break;
         }

         // ── i32x4 arithmetic ──
         case simd_sub::i32x4_extadd_pairwise_i16x8_s: {
            emit_v128_load_tos(V0n);
            emit32(0x4E602800 | (V0n << 5) | V0n);  // SADDLP V0.4S, V0.8H
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i32x4_extadd_pairwise_i16x8_u: {
            emit_v128_load_tos(V0n);
            emit32(0x6E602800 | (V0n << 5) | V0n);  // UADDLP V0.4S, V0.8H
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i32x4_abs: simd_neon_unop(0x4EA0B800); break;
         case simd_sub::i32x4_neg: simd_neon_unop(0x6EA0B800); break;
         case simd_sub::i32x4_all_true: {
            emit_v128_pop(V0n);
            emit32(0x6EB1A800 | (V0n << 5) | V0n);  // UMINV S0, V0.4S
            emit32(0x0E043C00 | (V0n << 5) | X0);   // UMOV W0, V0.S[0]
            emit_cmp_imm32(X0, 0);
            emit_cset(X0, COND_NE);
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i32x4_bitmask: {
            emit_v128_pop(V0n);
            // CMLT V0.4S, V0.4S, #0 → 0xFFFFFFFF per negative lane, else 0
            emit32(0x4EA0A800 | (V0n << 5) | V0n);
            // XTN V1.4H, V0.4S → 0xFFFF per negative lane
            emit32(0x0E612800 | (V0n << 5) | V1n);
            // XTN V1.8B, V1.8H → 0xFF per negative lane
            emit32(0x0E212800 | (V1n << 5) | V1n);
            // Now V1 has 4 bytes that are 0 or 1
            emit_mov_imm32(X0, 0x08040201);
            emit32(0x9E670000 | (X0 << 5) | V2n);   // FMOV D2, X0
            emit32(0x4E201C00 | (V2n << 16) | (V1n << 5) | V1n);  // AND V1, V1, V2
            emit32(0x4E31B800 | (V1n << 5) | V1n);  // ADDV B1, V1.16B
            emit32(0x0E013C00 | (V1n << 5) | X0);   // UMOV W0, V1.B[0]
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i32x4_extend_low_i16x8_s: simd_neon_unop(0x0F10A400); break;   // SSHLL V0.4S, V0.4H, #0
         case simd_sub::i32x4_extend_high_i16x8_s: {
            emit_v128_load_tos(V0n);
            emit32(0x6E004000 | (8 << 11) | (V0n << 16) | (V0n << 5) | V0n);  // EXT #8
            emit32(0x0F10A400 | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i32x4_extend_low_i16x8_u: simd_neon_unop(0x2F10A400); break;
         case simd_sub::i32x4_extend_high_i16x8_u: {
            emit_v128_load_tos(V0n);
            emit32(0x6E004000 | (8 << 11) | (V0n << 16) | (V0n << 5) | V0n);
            emit32(0x2F10A400 | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i32x4_shl:   simd_neon_shift(0x4EA04400, 0x4E040C00, 0x1f, inst.simd.offset); break;
         case simd_sub::i32x4_shr_s: simd_neon_shift_right(0x4EA04400, 0x4E040C00, 0x1f, inst.simd.offset); break;
         case simd_sub::i32x4_shr_u: simd_neon_shift_right(0x6EA04400, 0x4E040C00, 0x1f, inst.simd.offset); break;
         case simd_sub::i32x4_add: simd_neon_binop(0x4EA08400); break;
         case simd_sub::i32x4_sub: simd_neon_binop(0x6EA08400); break;
         case simd_sub::i32x4_mul: simd_neon_binop(0x4EA09C00); break;
         case simd_sub::i32x4_min_s: simd_neon_binop(0x4EA06C00); break;
         case simd_sub::i32x4_min_u: simd_neon_binop(0x6EA06C00); break;
         case simd_sub::i32x4_max_s: simd_neon_binop(0x4EA06400); break;
         case simd_sub::i32x4_max_u: simd_neon_binop(0x6EA06400); break;
         case simd_sub::i32x4_dot_i16x8_s: {
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            // SMULL V2.4S, V0.4H, V1.4H (low halves)
            emit32(0x0E60C000 | (V1n << 16) | (V0n << 5) | V2n);
            // SMULL2 V3.4S, V0.8H, V1.8H (high halves)
            emit32(0x4E60C000 | (V1n << 16) | (V0n << 5) | V3n);
            // ADDP V0.4S, V2.4S, V3.4S
            emit32(0x4EA0BC00 | (V3n << 16) | (V2n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i32x4_extmul_low_i16x8_s: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x0E60C000 | (V1n << 16) | (V0n << 5) | V0n);  // SMULL V0.4S, V0.4H, V1.4H
            emit_v128_store_tos(V0n); break;
         }
         case simd_sub::i32x4_extmul_high_i16x8_s: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x4E60C000 | (V1n << 16) | (V0n << 5) | V0n);  // SMULL2
            emit_v128_store_tos(V0n); break;
         }
         case simd_sub::i32x4_extmul_low_i16x8_u: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x2E60C000 | (V1n << 16) | (V0n << 5) | V0n);  // UMULL
            emit_v128_store_tos(V0n); break;
         }
         case simd_sub::i32x4_extmul_high_i16x8_u: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x6E60C000 | (V1n << 16) | (V0n << 5) | V0n);  // UMULL2
            emit_v128_store_tos(V0n); break;
         }

         // ── i64x2 arithmetic ──
         case simd_sub::i64x2_abs: {
            emit_v128_load_tos(V0n);
            // CMLT V1.2D, V0.2D, #0 (sign mask)
            emit32(0x4EE0A800 | (V0n << 5) | V1n);
            // EOR V0, V0, V1 (negate if negative)
            emit32(0x6E201C00 | (V1n << 16) | (V0n << 5) | V0n);
            // SUB V0.2D, V0.2D, V1.2D
            emit32(0x6EE08400 | (V1n << 16) | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i64x2_neg: {
            emit_v128_load_tos(V0n);
            emit32(0x6F00E400 | V1n);  // MOVI V1.2D, #0
            emit32(0x6EE08400 | (V0n << 16) | (V1n << 5) | V0n);  // SUB V0.2D, V1.2D, V0.2D
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i64x2_all_true: {
            emit_v128_pop(V0n);
            // Check both lanes non-zero
            emit32(0x4E083C00 | (V0n << 5) | X0);   // UMOV X0, V0.D[0]
            emit32(0x4E183C00 | (V0n << 5) | X1);   // UMOV X1, V0.D[1]
            // ORN instead: both must be nonzero
            emit_cmp_imm64(X0, 0);
            emit_cset(X0, COND_NE);
            emit_cmp_imm64(X1, 0);
            emit_cset(X1, COND_NE);
            emit32(0x0A010000);  // AND W0, W0, W1
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i64x2_bitmask: {
            emit_v128_pop(V0n);
            // USHR V0.2D, V0.2D, #63
            emit32(0x6F410400 | (V0n << 5) | V0n);
            // Extract lanes
            emit32(0x4E083C00 | (V0n << 5) | X0);   // UMOV X0, V0.D[0]
            emit32(0x4E183C00 | (V0n << 5) | X1);   // UMOV X1, V0.D[1]
            // Combine: X0 = bit0, X1 = bit1
            emit32(0xAA010400 | X0);  // ORR X0, X0, X1, LSL #1
            store_x0_vreg(inst.simd.addr);
            break;
         }
         case simd_sub::i64x2_extend_low_i32x4_s: simd_neon_unop(0x0F20A400); break;   // SSHLL
         case simd_sub::i64x2_extend_high_i32x4_s: {
            emit_v128_load_tos(V0n);
            emit32(0x6E004000 | (8 << 11) | (V0n << 16) | (V0n << 5) | V0n);
            emit32(0x0F20A400 | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i64x2_extend_low_i32x4_u: simd_neon_unop(0x2F20A400); break;
         case simd_sub::i64x2_extend_high_i32x4_u: {
            emit_v128_load_tos(V0n);
            emit32(0x6E004000 | (8 << 11) | (V0n << 16) | (V0n << 5) | V0n);
            emit32(0x2F20A400 | (V0n << 5) | V0n);
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i64x2_shl:   simd_neon_shift(0x4EE04400, 0x4E080C00, 0x3f, inst.simd.offset); break;
         case simd_sub::i64x2_shr_s: simd_neon_shift_right(0x4EE04400, 0x4E080C00, 0x3f, inst.simd.offset); break;
         case simd_sub::i64x2_shr_u: simd_neon_shift_right(0x6EE04400, 0x4E080C00, 0x3f, inst.simd.offset); break;
         case simd_sub::i64x2_add: simd_neon_binop(0x4EE08400); break;
         case simd_sub::i64x2_sub: simd_neon_binop(0x6EE08400); break;
         case simd_sub::i64x2_mul: {
            // No native 64x2 multiply on NEON. Use scalar multiply.
            emit_v128_pop(V1n);
            emit_v128_load_tos(V0n);
            emit32(0x4E083C00 | (V0n << 5) | X0);   // UMOV X0, V0.D[0]
            emit32(0x4E183C00 | (V0n << 5) | X1);   // UMOV X1, V0.D[1]
            emit32(0x4E083C00 | (V1n << 5) | X8);   // UMOV X8, V1.D[0]  (use X8 as temp)
            emit32(0x4E183C00 | (V1n << 5) | X16);  // UMOV X16, V1.D[1]
            emit32(0x9B087C00);  // MUL X0, X0, X8
            emit32(0x9B107C21);  // MUL X1, X1, X16
            emit32(0x9E670000 | (X0 << 5) | V0n);   // FMOV D0, X0
            emit32(0x4E181C00 | (X1 << 5) | V0n);   // INS V0.D[1], X1
            emit_v128_store_tos(V0n);
            break;
         }
         case simd_sub::i64x2_extmul_low_i32x4_s: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x0EA0C000 | (V1n << 16) | (V0n << 5) | V0n);  // SMULL V0.2D, V0.2S, V1.2S
            emit_v128_store_tos(V0n); break;
         }
         case simd_sub::i64x2_extmul_high_i32x4_s: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x4EA0C000 | (V1n << 16) | (V0n << 5) | V0n);  // SMULL2
            emit_v128_store_tos(V0n); break;
         }
         case simd_sub::i64x2_extmul_low_i32x4_u: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x2EA0C000 | (V1n << 16) | (V0n << 5) | V0n);  // UMULL
            emit_v128_store_tos(V0n); break;
         }
         case simd_sub::i64x2_extmul_high_i32x4_u: {
            emit_v128_pop(V1n); emit_v128_load_tos(V0n);
            emit32(0x6EA0C000 | (V1n << 16) | (V0n << 5) | V0n);  // UMULL2
            emit_v128_store_tos(V0n); break;
         }

         // ── f32x4 arithmetic (softfloat) ──
         case simd_sub::f32x4_ceil:    simd_v128_unop_softfloat(&_psizam_f32x4_ceil); break;
         case simd_sub::f32x4_floor:   simd_v128_unop_softfloat(&_psizam_f32x4_floor); break;
         case simd_sub::f32x4_trunc:   simd_v128_unop_softfloat(&_psizam_f32x4_trunc); break;
         case simd_sub::f32x4_nearest: simd_v128_unop_softfloat(&_psizam_f32x4_nearest); break;
         case simd_sub::f32x4_abs:     simd_v128_unop_softfloat(&_psizam_f32x4_abs); break;
         case simd_sub::f32x4_neg:     simd_v128_unop_softfloat(&_psizam_f32x4_neg); break;
         case simd_sub::f32x4_sqrt:    simd_v128_unop_softfloat(&_psizam_f32x4_sqrt); break;
         case simd_sub::f32x4_add:     simd_v128_binop_softfloat(&_psizam_f32x4_add); break;
         case simd_sub::f32x4_sub:     simd_v128_binop_softfloat(&_psizam_f32x4_sub); break;
         case simd_sub::f32x4_mul:     simd_v128_binop_softfloat(&_psizam_f32x4_mul); break;
         case simd_sub::f32x4_div:     simd_v128_binop_softfloat(&_psizam_f32x4_div); break;
         case simd_sub::f32x4_min:     simd_v128_binop_softfloat(&_psizam_f32x4_min); break;
         case simd_sub::f32x4_max:     simd_v128_binop_softfloat(&_psizam_f32x4_max); break;
         case simd_sub::f32x4_pmin:    simd_v128_binop_softfloat(&_psizam_f32x4_pmin); break;
         case simd_sub::f32x4_pmax:    simd_v128_binop_softfloat(&_psizam_f32x4_pmax); break;

         // ── f64x2 arithmetic (softfloat) ──
         case simd_sub::f64x2_ceil:    simd_v128_unop_softfloat(&_psizam_f64x2_ceil); break;
         case simd_sub::f64x2_floor:   simd_v128_unop_softfloat(&_psizam_f64x2_floor); break;
         case simd_sub::f64x2_trunc:   simd_v128_unop_softfloat(&_psizam_f64x2_trunc); break;
         case simd_sub::f64x2_nearest: simd_v128_unop_softfloat(&_psizam_f64x2_nearest); break;
         case simd_sub::f64x2_abs:     simd_v128_unop_softfloat(&_psizam_f64x2_abs); break;
         case simd_sub::f64x2_neg:     simd_v128_unop_softfloat(&_psizam_f64x2_neg); break;
         case simd_sub::f64x2_sqrt:    simd_v128_unop_softfloat(&_psizam_f64x2_sqrt); break;
         case simd_sub::f64x2_add:     simd_v128_binop_softfloat(&_psizam_f64x2_add); break;
         case simd_sub::f64x2_sub:     simd_v128_binop_softfloat(&_psizam_f64x2_sub); break;
         case simd_sub::f64x2_mul:     simd_v128_binop_softfloat(&_psizam_f64x2_mul); break;
         case simd_sub::f64x2_div:     simd_v128_binop_softfloat(&_psizam_f64x2_div); break;
         case simd_sub::f64x2_min:     simd_v128_binop_softfloat(&_psizam_f64x2_min); break;
         case simd_sub::f64x2_max:     simd_v128_binop_softfloat(&_psizam_f64x2_max); break;
         case simd_sub::f64x2_pmin:    simd_v128_binop_softfloat(&_psizam_f64x2_pmin); break;
         case simd_sub::f64x2_pmax:    simd_v128_binop_softfloat(&_psizam_f64x2_pmax); break;

         // ── Conversions (softfloat) ──
         case simd_sub::i32x4_trunc_sat_f32x4_s:     simd_v128_unop_softfloat(&_psizam_i32x4_trunc_sat_f32x4_s); break;
         case simd_sub::i32x4_trunc_sat_f32x4_u:     simd_v128_unop_softfloat(&_psizam_i32x4_trunc_sat_f32x4_u); break;
         case simd_sub::f32x4_convert_i32x4_s:       simd_v128_unop_softfloat(&_psizam_f32x4_convert_i32x4_s); break;
         case simd_sub::f32x4_convert_i32x4_u:       simd_v128_unop_softfloat(&_psizam_f32x4_convert_i32x4_u); break;
         case simd_sub::i32x4_trunc_sat_f64x2_s_zero: simd_v128_unop_softfloat(&_psizam_i32x4_trunc_sat_f64x2_s_zero); break;
         case simd_sub::i32x4_trunc_sat_f64x2_u_zero: simd_v128_unop_softfloat(&_psizam_i32x4_trunc_sat_f64x2_u_zero); break;
         case simd_sub::f64x2_convert_low_i32x4_s:   simd_v128_unop_softfloat(&_psizam_f64x2_convert_low_i32x4_s); break;
         case simd_sub::f64x2_convert_low_i32x4_u:   simd_v128_unop_softfloat(&_psizam_f64x2_convert_low_i32x4_u); break;
         case simd_sub::f32x4_demote_f64x2_zero:     simd_v128_unop_softfloat(&_psizam_f32x4_demote_f64x2_zero); break;
         case simd_sub::f64x2_promote_low_f32x4:     simd_v128_unop_softfloat(&_psizam_f64x2_promote_low_f32x4); break;

         // ── Relaxed SIMD ──
         case simd_sub::f32x4_relaxed_madd:  simd_v128_ternop_softfloat(&_psizam_f32x4_relaxed_madd); break;
         case simd_sub::f32x4_relaxed_nmadd: simd_v128_ternop_softfloat(&_psizam_f32x4_relaxed_nmadd); break;
         case simd_sub::f64x2_relaxed_madd:  simd_v128_ternop_softfloat(&_psizam_f64x2_relaxed_madd); break;
         case simd_sub::f64x2_relaxed_nmadd: simd_v128_ternop_softfloat(&_psizam_f64x2_relaxed_nmadd); break;
         case simd_sub::i16x8_relaxed_dot_i8x16_i7x16_s: simd_v128_binop_softfloat(&_psizam_i16x8_relaxed_dot_i8x16_i7x16_s); break;
         case simd_sub::i32x4_relaxed_dot_i8x16_i7x16_add_s: simd_v128_ternop_softfloat(&_psizam_i32x4_relaxed_dot_i8x16_i7x16_add_s); break;

         default:
            break;
         }
      }


      // ──────── Binary op helpers ────────

      // 2-operand binary: OP Xd, Xn, Xm (with const-imm folding for add/sub)
      void emit_binop(ir_function& func, const ir_inst& inst, uint32_t opcode, bool is32) {
         // Try const-immediate for add/sub
         bool is_add = (opcode == 0x0B000000 || opcode == 0x8B000000);
         bool is_sub = (opcode == 0x4B000000 || opcode == 0xCB000000);
         if (is_add || is_sub) {
            int64_t cval;
            if (try_get_const(inst.rr.src2, cval)) {
               uint32_t uval = static_cast<uint32_t>(cval & (is32 ? 0xFFFFFFFF : cval));
               if (uval <= 4095) {
                  load_vreg_x0(inst.rr.src1);
                  if (is_add) {
                     if (is32) emit32(0x11000000 | (uval << 10) | (X0 << 5) | X0);
                     else      emit32(0x91000000 | (uval << 10) | (X0 << 5) | X0);
                  } else {
                     if (is32) emit32(0x51000000 | (uval << 10) | (X0 << 5) | X0);
                     else      emit32(0xD1000000 | (uval << 10) | (X0 << 5) | X0);
                  }
                  kill_const_if_single_use(inst.rr.src2);
                  store_x0_vreg(inst.dest);
                  return;
               }
            }
         }
         // General case
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(opcode | (X1 << 16) | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
      }

      // 3-operand (MUL = MADD Xd, Xn, Xm, XZR)
      void emit_binop3(const ir_inst& inst, uint32_t opcode, bool is32) {
         (void)is32;
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(opcode | (X1 << 16) | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
      }

      // Simple 2-register binary: OP Xd, Xn, Xm
      void emit_binop_simple(const ir_inst& inst, uint32_t opcode, bool is32) {
         (void)is32;
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(opcode | (X1 << 16) | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
      }

      // ── Division with trap checks ──
      // ARM64 SDIV/UDIV don't trap on div-by-zero (they return 0).
      // WASM requires trapping on div-by-zero and signed overflow (INT_MIN/-1).

      void emit_i32_div_s(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ W1, fpe_handler (div by zero)
         emit32(0x34000000 | X1); // CBZ W1, +0 (patched)
         void* cbz = code - 4;
         // Check signed overflow: INT_MIN / -1
         // CMN W1, #1 (is divisor -1?)
         emit32(0x3100043F); // ADDS WZR, W1, #1
         void* not_minus1 = code;
         emit32(0x54000001); // B.NE not_minus1
         // CMN W0, W0 -> sets V if W0 == INT_MIN (0x80000000)
         emit32(0x2B00001F | (X0 << 5) | (X0 << 16));
         emit_branch_to_handler(COND_VS, fpe_handler);
         fix_branch(not_minus1, code);
         // SDIV W0, W0, W1
         emit32(0x1AC00C00 | (X1 << 16) | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
         // Patch CBZ to branch to fpe_handler
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0x34000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      void emit_i32_div_u(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ W1, fpe_handler (div by zero)
         emit32(0x34000000 | X1);
         void* cbz = code - 4;
         // UDIV W0, W0, W1
         emit32(0x1AC00800 | (X1 << 16) | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0x34000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      void emit_i64_div_s(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ X1, fpe_handler (div by zero)
         emit32(0xB4000000 | X1);
         void* cbz = code - 4;
         // Check signed overflow: INT64_MIN / -1
         // CMN X1, #1
         emit32(0xB100043F);
         void* not_minus1 = code;
         emit32(0x54000001); // B.NE
         // ADDS XZR, X0, X0 -> sets V if X0 == INT64_MIN
         emit32(0xAB00001F | (X0 << 5) | (X0 << 16));
         emit_branch_to_handler(COND_VS, fpe_handler);
         fix_branch(not_minus1, code);
         // SDIV X0, X0, X1
         emit32(0x9AC00C00 | (X1 << 16) | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0xB4000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      void emit_i64_div_u(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ X1, fpe_handler (div by zero)
         emit32(0xB4000000 | X1);
         void* cbz = code - 4;
         // UDIV X0, X0, X1
         emit32(0x9AC00800 | (X1 << 16) | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0xB4000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      void emit_i32_rem_s(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ W1, fpe_handler
         emit32(0x34000000 | X1);
         void* cbz = code - 4;
         // Check -1 case (result is 0)
         // CMN W1, #1
         emit32(0x3100043F);
         void* not_minus1 = code;
         emit32(0x54000001); // B.NE
         // MOV W0, #0
         emit32(0x52800000);
         void* done = code;
         emit32(0x14000000); // B done
         fix_branch(not_minus1, code);
         // SDIV W2, W0, W1
         emit32(0x1AC00C02 | (X1 << 16) | (X0 << 5));
         // MSUB W0, W2, W1, W0
         emit32(0x1B018040 | (X1 << 16) | (X0 << 10));
         fix_branch(done, code);
         store_x0_vreg(inst.dest);
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0x34000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      void emit_i32_rem_u(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ W1, fpe_handler
         emit32(0x34000000 | X1);
         void* cbz = code - 4;
         // UDIV W2, W0, W1
         emit32(0x1AC00802 | (X1 << 16) | (X0 << 5));
         // MSUB W0, W2, W1, W0
         emit32(0x1B018040 | (X1 << 16) | (X0 << 10));
         store_x0_vreg(inst.dest);
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0x34000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      void emit_i64_rem_s(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ X1, fpe_handler
         emit32(0xB4000000 | X1);
         void* cbz = code - 4;
         // Check -1 case (result is 0)
         emit32(0xB100043F); // CMN X1, #1
         void* not_minus1 = code;
         emit32(0x54000001); // B.NE
         emit32(0xD2800000); // MOV X0, #0
         void* done = code;
         emit32(0x14000000); // B done
         fix_branch(not_minus1, code);
         // SDIV X2, X0, X1
         emit32(0x9AC00C02 | (X1 << 16) | (X0 << 5));
         // MSUB X0, X2, X1, X0
         emit32(0x9B018040 | (X1 << 16) | (X0 << 10));
         fix_branch(done, code);
         store_x0_vreg(inst.dest);
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0xB4000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      void emit_i64_rem_u(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // CBZ X1, fpe_handler
         emit32(0xB4000000 | X1);
         void* cbz = code - 4;
         // UDIV X2, X0, X1
         emit32(0x9AC00802 | (X1 << 16) | (X0 << 5));
         // MSUB X0, X2, X1, X0
         emit32(0x9B018040 | (X1 << 16) | (X0 << 10));
         store_x0_vreg(inst.dest);
         { int64_t off = (static_cast<uint8_t*>(fpe_handler) - static_cast<uint8_t*>(cbz)) / 4;
           uint32_t patched = 0xB4000000 | X1 | ((static_cast<uint32_t>(off) & 0x7FFFF) << 5);
           std::memcpy(cbz, &patched, 4); }
      }

      // Rotate left = rotate right by (32/64 - amount)
      void emit_i32_rotl(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         // NEG W1, W1 (SUB W1, WZR, W1)
         emit32(0x4B0103E1);
         // RORV W0, W0, W1
         emit32(0x1AC12C00);
         store_x0_vreg(inst.dest);
      }
      void emit_i64_rotl(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0xCB0103E1); // NEG X1, X1
         emit32(0x9AC12C00); // RORV X0, X0, X1
         store_x0_vreg(inst.dest);
      }

      // ── Comparison helpers ──

      void emit_eqz(ir_function& func, const ir_inst& inst, uint32_t idx, bool is32) {
         int8_t pr = get_phys(inst.rr.src1);
         uint32_t rn;
         if (pr >= 0) { rn = phys_to_reg(pr); }
         else { load_vreg_x0(inst.rr.src1); rn = X0; }

         if (is32) emit_cmp_imm32(rn, 0);
         else      emit_cmp_imm64(rn, 0);

         if ((inst.flags & IR_FUSE_NEXT) && emit_fused_branch(func, idx, COND_EQ)) return;
         emit_cset(X0, COND_EQ);
         store_x0_vreg(inst.dest);
      }

      void emit_cmp(ir_function& func, const ir_inst& inst, uint32_t idx, uint32_t cond, bool is32) {
         // Try const-immediate
         int64_t cval;
         if (try_get_const(inst.rr.src2, cval)) {
            uint32_t uval = static_cast<uint32_t>(is32 ? (cval & 0xFFFFFFFF) : cval);
            if (uval <= 4095) {
               load_vreg_x0(inst.rr.src1);
               if (is32) emit_cmp_imm32(X0, uval);
               else      emit_cmp_imm64(X0, uval);
               kill_const_if_single_use(inst.rr.src2);
               if ((inst.flags & IR_FUSE_NEXT) && emit_fused_branch(func, idx, cond)) return;
               emit_cset(X0, cond);
               store_x0_vreg(inst.dest);
               return;
            }
         }
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         if (is32) emit_cmp_reg32(X0, X1);
         else      emit_cmp_reg64(X0, X1);
         if ((inst.flags & IR_FUSE_NEXT) && emit_fused_branch(func, idx, cond)) return;
         emit_cset(X0, cond);
         store_x0_vreg(inst.dest);
      }

      // ── Memory load/store helpers ──

      void emit_load_op(const ir_inst& inst, uint32_t load_op) {
         uint32_t uoffset = static_cast<uint32_t>(inst.ri.imm);
         load_vreg_x0(inst.ri.src1);
         emit_effective_addr(X0, X0, uoffset);
         emit32(load_op | (X0 << 5) | X0);
         store_x0_vreg(inst.dest);
      }

      void emit_store_op(const ir_inst& inst, uint32_t store_op) {
         uint32_t uoffset = static_cast<uint32_t>(inst.ri.imm);
         load_vreg_x0(inst.dest);  // value
         load_vreg_x1(inst.ri.src1); // addr
         emit_effective_addr(X1, X1, uoffset);
         emit32(store_op | (X1 << 5) | X0);
      }

      // ── Float helpers ──

      // NaN canonicalization on FP reg: if S0 is NaN, replace with canonical NaN
      void emit_f32_canonicalize_nan() {
         emit32(0x1E202000);  // FCMP S0, S0 — unordered if NaN
         void* not_nan = emit_cond_branch_placeholder(COND_VC); // B.VC (ordered → not NaN)
         emit_mov_imm32(X0, 0x7FC00000u);
         emit32(0x1E270000);  // FMOV S0, W0
         fix_branch(not_nan, code);
      }
      void emit_f64_canonicalize_nan() {
         emit32(0x1E602000);  // FCMP D0, D0 — unordered if NaN
         void* not_nan = emit_cond_branch_placeholder(COND_VC); // B.VC
         emit_mov_imm64(X0, 0x7FF8000000000000ull);
         emit32(0x9E670000);  // FMOV D0, X0
         fix_branch(not_nan, code);
      }
      // NaN canonicalization on GPR: if W0 (as f32 bits) is NaN, replace with canonical
      void emit_f32_canonicalize_nan_gpr() {
         // Check if exponent=0xFF and mantissa!=0 (NaN)
         // Use: FMOV S0, W0; FCMP S0, S0 to leverage HW NaN detection
         emit32(0x1E270000);  // FMOV S0, W0
         emit32(0x1E202000);  // FCMP S0, S0
         void* not_nan = emit_cond_branch_placeholder(COND_VC);
         emit_mov_imm32(X0, 0x7FC00000u);
         fix_branch(not_nan, code);
      }
      void emit_f64_canonicalize_nan_gpr() {
         emit32(0x9E670000);  // FMOV D0, X0
         emit32(0x1E602000);  // FCMP D0, D0
         void* not_nan = emit_cond_branch_placeholder(COND_VC);
         emit_mov_imm64(X0, 0x7FF8000000000000ull);
         fix_branch(not_nan, code);
      }

      void emit_f32_round(const ir_inst& inst, uint32_t round_op) {
         load_vreg_x0(inst.rr.src1);
         emit32(0x1E270000); // FMOV S0, W0
         emit32(round_op);   // FRINTx S0, S0
         emit32(0x1E260000); // FMOV W0, S0
         store_x0_vreg(inst.dest);
      }
      void emit_f64_round(const ir_inst& inst, uint32_t round_op) {
         load_vreg_x0(inst.rr.src1);
         emit32(0x9E670000); // FMOV D0, X0
         emit32(round_op);
         emit32(0x9E660000); // FMOV X0, D0
         store_x0_vreg(inst.dest);
      }

      void emit_f32_binop(const ir_inst& inst, uint32_t op) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x1E270000);             // FMOV S0, W0
         emit32(0x1E270000 | (X1 << 5) | 1); // FMOV S1, W1
         emit32(op | (1 << 16));         // OP S0, S0, S1
         emit32(0x1E260000);             // FMOV W0, S0
         store_x0_vreg(inst.dest);
      }
      void emit_f64_binop(const ir_inst& inst, uint32_t op) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x9E670000);             // FMOV D0, X0
         emit32(0x9E670000 | (X1 << 5) | 1); // FMOV D1, X1
         emit32(op | (1 << 16));         // OP D0, D0, D1
         emit32(0x9E660000);             // FMOV X0, D0
         store_x0_vreg(inst.dest);
      }

      // IEEE 754 min/max with proper NaN + signed-zero handling
      // Uses FMIN/FMAX (NaN-propagating) + canonical NaN fixup
      void emit_f32_min(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x1E270000);                   // FMOV S0, W0 (lhs)
         emit32(0x1E270000 | (X1 << 5) | 1);  // FMOV S1, W1 (rhs)
         emit32(0x1E215800);                   // FMIN S0, S0, S1
         emit_f32_canonicalize_nan();
         emit32(0x1E260000);                   // FMOV W0, S0
         store_x0_vreg(inst.dest);
      }
      void emit_f32_max(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x1E270000);                   // FMOV S0, W0
         emit32(0x1E270000 | (X1 << 5) | 1);  // FMOV S1, W1
         emit32(0x1E214800);                   // FMAX S0, S0, S1
         emit_f32_canonicalize_nan();
         emit32(0x1E260000);                   // FMOV W0, S0
         store_x0_vreg(inst.dest);
      }
      void emit_f64_min(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x9E670000);                   // FMOV D0, X0
         emit32(0x9E670000 | (X1 << 5) | 1);  // FMOV D1, X1
         emit32(0x1E615800);                   // FMIN D0, D0, D1
         emit_f64_canonicalize_nan();
         emit32(0x9E660000);                   // FMOV X0, D0
         store_x0_vreg(inst.dest);
      }
      void emit_f64_max(const ir_inst& inst) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x9E670000);                   // FMOV D0, X0
         emit32(0x9E670000 | (X1 << 5) | 1);  // FMOV D1, X1
         emit32(0x1E614800);                   // FMAX D0, D0, D1
         emit_f64_canonicalize_nan();
         emit32(0x9E660000);                   // FMOV X0, D0
         store_x0_vreg(inst.dest);
      }

      void emit_f32_cmp(ir_function& func, const ir_inst& inst, uint32_t idx, uint32_t cond) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x1E270000);             // FMOV S0, W0
         emit32(0x1E270000 | (X1 << 5) | 1); // FMOV S1, W1
         emit32(0x1E212000);             // FCMP S0, S1
         if ((inst.flags & IR_FUSE_NEXT) && emit_fused_branch(func, idx, cond)) return;
         emit_cset(X0, cond);
         store_x0_vreg(inst.dest);
      }
      void emit_f64_cmp(ir_function& func, const ir_inst& inst, uint32_t idx, uint32_t cond) {
         load_vreg_x0(inst.rr.src1);
         load_vreg_x1(inst.rr.src2);
         emit32(0x9E670000);             // FMOV D0, X0
         emit32(0x9E670000 | (X1 << 5) | 1); // FMOV D1, X1
         emit32(0x1E612000);             // FCMP D0, D1
         if ((inst.flags & IR_FUSE_NEXT) && emit_fused_branch(func, idx, cond)) return;
         emit_cset(X0, cond);
         store_x0_vreg(inst.dest);
      }

      // ── Float/int conversion helpers ──

      void emit_fcvt(const ir_inst& inst, uint32_t cvt_op, bool src_is32, bool fp_is32) {
         load_vreg_x0(inst.rr.src1);
         if (fp_is32) emit32(0x1E270000); // FMOV S0, W0
         else         emit32(0x9E670000); // FMOV D0, X0
         emit32(cvt_op); // FCVTZx Wd/Xd, S0/D0
         store_x0_vreg(inst.dest);
      }

      // Trapping float-to-int: clear FPSR, convert, check IOC flag → trap on overflow/NaN
      void emit_fcvt_trap(const ir_inst& inst, uint32_t cvt_op, bool src_is32, bool fp_is32) {
         load_vreg_x0(inst.rr.src1);
         if (fp_is32) emit32(0x1E270000); // FMOV S0, W0
         else         emit32(0x9E670000); // FMOV D0, X0
         // Clear FPSR
         emit32(0xD2800008); // MOVZ X8, #0
         emit32(0xD51B4428); // MSR FPSR, X8
         emit32(cvt_op);     // FCVTZx Wd/Xd, S0/D0
         // Check FPSR IOC (Invalid Operation) bit
         emit32(0xD53B4428); // MRS X8, FPSR
         emit32(0x7200011F); // TST W8, #1
         emit_branch_to_handler(COND_NE, fpe_handler);
         store_x0_vreg(inst.dest);
      }

      void emit_icvt(const ir_inst& inst, uint32_t cvt_op, bool src_is32, bool fp_is32) {
         load_vreg_x0(inst.rr.src1);
         emit32(cvt_op); // xCVTF S0/D0, W0/X0
         if (fp_is32) emit32(0x1E260000); // FMOV W0, S0
         else         emit32(0x9E660000); // FMOV X0, D0
         store_x0_vreg(inst.dest);
      }

      // ──────── Function relocation ────────

      struct call_fixup {
         void* branch;
         call_fixup* next;
      };

      struct func_reloc {
         void* address = nullptr;
         call_fixup* pending = nullptr;
      };

      void init_relocations() {
         uint32_t total = _mod.get_functions_total();
         _relocs = _allocator.alloc<func_reloc>(total);
         _num_relocs = total;
         for (uint32_t i = 0; i < total; ++i) _relocs[i] = func_reloc{};
      }

      void register_call(void* branch_addr, uint32_t funcnum) {
         if (funcnum >= _num_relocs) return;
         auto& r = _relocs[funcnum];
         if (r.address) {
            fix_branch(branch_addr, r.address);
         } else {
            auto* fixup = _allocator.alloc<call_fixup>(1);
            fixup->branch = branch_addr;
            fixup->next = r.pending;
            r.pending = fixup;
         }
      }

      void start_function(void* func_start, uint32_t funcnum) {
         if (funcnum >= _num_relocs) return;
         auto& r = _relocs[funcnum];
         for (auto* f = r.pending; f; f = f->next) {
            fix_branch(f->branch, func_start);
         }
         r.address = func_start;
         r.pending = nullptr;
      }

      // ──────── Static callbacks ────────

      static native_value call_host_function(void* ctx, native_value* stack, uint32_t idx, uint32_t remaining_stack) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         native_value result;
         psizam::longjmp_on_exception([&]() {
            auto saved = context->_remaining_call_depth;
            context->_remaining_call_depth = remaining_stack;
            scope_guard g{[&](){ context->_remaining_call_depth = saved; }};
            result = context->call_host_function(stack, idx);
         });
         return result;
      }

      static int32_t current_memory(void* ctx) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         return context->current_linear_memory();
      }
      static int32_t grow_memory(void* ctx, int32_t pages) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         return context->grow_linear_memory(pages);
      }
      static void on_memory_error() { signal_throw<wasm_memory_exception>("wasm memory out-of-bounds"); }
      static void on_unreachable() { psizam::signal_throw<wasm_interpreter_exception>("unreachable"); }
      static void on_fp_error() { psizam::signal_throw<wasm_interpreter_exception>("floating point error"); }
      static void on_call_indirect_error() { psizam::signal_throw<wasm_interpreter_exception>("call_indirect out of range"); }
      static void on_type_error() { psizam::signal_throw<wasm_interpreter_exception>("call_indirect incorrect function type"); }
      static void on_stack_overflow() { psizam::signal_throw<wasm_interpreter_exception>("stack overflow"); }

      // Bulk memory/table runtime helpers
      static void memory_init_impl(void* ctx, uint32_t seg_idx, uint32_t dest, uint32_t src, uint32_t count) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         psizam::longjmp_on_exception([&]() {
            context->init_linear_memory(seg_idx, dest, src, count);
         });
      }
      static void data_drop_impl(void* ctx, uint32_t seg_idx) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         psizam::longjmp_on_exception([&]() {
            context->drop_data(seg_idx);
         });
      }
      static void table_init_impl(void* ctx, uint32_t packed_idx, uint32_t dest, uint32_t src, uint32_t count) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         uint32_t seg_idx = packed_idx & 0xFFFF;
         uint32_t table_idx = packed_idx >> 16;
         psizam::longjmp_on_exception([&]() {
            context->init_table(seg_idx, dest, src, count, table_idx);
         });
      }
      static void elem_drop_impl(void* ctx, uint32_t seg_idx) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         psizam::longjmp_on_exception([&]() {
            context->drop_elem(seg_idx);
         });
      }
      static void table_copy_impl(void* ctx, uint32_t dest, uint32_t src, uint32_t count, uint32_t packed_tables = 0) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         uint32_t dst_table = packed_tables & 0xFFFF;
         uint32_t src_table = packed_tables >> 16;
         psizam::longjmp_on_exception([&]() {
            auto* s = context->get_table_ptr(src, count, src_table);
            auto* d = context->get_table_ptr(dest, count, dst_table);
            if (count > 0)
               std::memmove(d, s, count * sizeof(table_entry));
         });
      }
      static void memory_copy_checked(void* ctx, uint32_t dest, uint32_t src, uint32_t count) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         psizam::longjmp_on_exception([&]() {
            char* mem = context->linear_memory();
            uint32_t mem_size = static_cast<uint32_t>(context->current_linear_memory()) * 65536u;
            if (uint64_t(dest) + count > mem_size || uint64_t(src) + count > mem_size)
               psizam::signal_throw<wasm_memory_exception>("out of bounds memory access");
            if (count > 0)
               std::memmove(mem + dest, mem + src, count);
         });
      }
      static void memory_fill_checked(void* ctx, uint32_t dest, uint32_t val, uint32_t count) {
         auto* context = static_cast<jit_execution_context<false>*>(ctx);
         psizam::longjmp_on_exception([&]() {
            char* mem = context->linear_memory();
            uint32_t mem_size = static_cast<uint32_t>(context->current_linear_memory()) * 65536u;
            if (uint64_t(dest) + count > mem_size)
               psizam::signal_throw<wasm_memory_exception>("out of bounds memory access");
            if (count > 0)
               std::memset(mem + dest, static_cast<uint8_t>(val), count);
         });
      }

      // ──────── State ────────

      unsigned char* code = nullptr;
      growable_allocator& _allocator;
      growable_allocator& _scratch_alloc;
      module& _mod;
      bool _enable_backtrace;
      bool _stack_limit_is_bytes;
      void* _code_segment_base = nullptr;
      void* fpe_handler = nullptr;
      void* call_indirect_handler = nullptr;
      void* type_error_handler = nullptr;
      void* stack_overflow_handler = nullptr;
      void* memory_handler = nullptr;
      func_reloc* _relocs = nullptr;
      uint32_t _num_relocs = 0;
      void** _block_addrs = nullptr;
      block_fixup** _block_fixups = nullptr;
      uint32_t _num_blocks = 0;
      bool _in_br_table = false;
      uint32_t _br_table_case = 0;
      uint32_t _br_table_size = 0;
      int8_t* _vreg_map = nullptr;
      int16_t* _spill_map = nullptr;
      uint32_t _num_vregs = 0;
      uint32_t _num_spill_slots = 0;
      uint32_t _body_locals = 0;
      int32_t _frame_size = 0;
      bool _use_regalloc = false;
      uint32_t _callee_saved_used = 0;
      uint32_t _callee_saved_count = 0;
      uint32_t* _func_def_inst = nullptr;
      uint16_t* _func_use_count = nullptr;
      ir_inst*  _func_insts = nullptr;
      uint32_t  _func_inst_count = 0;
      block_fixup* _fixup_pool = nullptr;
      uint32_t _fixup_pool_next = 0;
      uint32_t _fixup_pool_size = 0;

      // Stub — a64 codegen does not yet track absolute address relocations for .pzam.
      // TODO: Add emit_reloc() calls throughout the a64 codegen to enable cross-compilation output.
      relocation_recorder _reloc_recorder_stub;
    public:
      const relocation_recorder& relocations() const { return _reloc_recorder_stub; }
   };

} // namespace psizam
