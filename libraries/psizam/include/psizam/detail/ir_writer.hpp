#pragma once

// Two-pass optimizing JIT (jit2) IR builder.
// Converts WASM stack machine operations to virtual-register IR.
//
// Per-function compilation: as the parser completes each function body,
// finalize() immediately runs optimize → regalloc → native codegen,
// then resets the IR allocator.  This bounds peak memory to module
// metadata + one function's IR + accumulated native code, which is
// critical for wasm32-hosted compilation (4GB address space).

#include <psizam/allocator.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/detail/jit_codegen.hpp>
#include <psizam/detail/jit_codegen_a64.hpp>
#include <psizam/detail/jit_ir.hpp>
#include <psizam/detail/jit_reloc.hpp>
#include <psizam/detail/jit_optimize.hpp>
#include <psizam/detail/jit_regalloc.hpp>
#include <psizam/types.hpp>

#include <cstdint>
#include <cstring>
#include <exception>
#include <optional>

#if !defined(__wasi__)
#include <pthread.h>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <functional>
#endif

namespace psizam::detail {

   /// Result from compilation — captures relocations before codegen is destroyed.
   /// For LLVM AOT, also captures the code blob and function offsets.
   struct pzam_compile_result {
      std::vector<code_relocation> relocs;
      std::vector<uint8_t>         code_blob;          // filled by LLVM AOT path
      std::vector<std::pair<uint32_t, uint32_t>> function_offsets; // entry wrapper (offset, size)
      std::vector<std::pair<uint32_t, uint32_t>> body_offsets;     // function body (offset, size) — LLVM only
      std::string                  target_triple;       // set by caller for LLVM AOT
      std::string                  error;               // non-empty on failure (e.g. LLVM AOT)
      bool                         softfloat = false;   // use softfloat wrappers (runtime option)
      bool                         backtrace = false;   // enable async backtrace frame tracking
      int                          opt_level = 2;       // LLVM optimization level (0-3)
   };

#if !defined(__wasi__)
   // Reactor-pattern compilation thread pool.
   // Workers are pre-spawned once and persist for the process lifetime.
   // Two-phase batch execution:
   //   Phase 1 (compile): workers claim function indices via atomic counter
   //   Phase 2 (merge):   workers copy their code + patch calls in parallel
   class compile_reactor {
    public:
      explicit compile_reactor(uint32_t num_threads, size_t stack_size = 8 * 1024 * 1024)
         : _num_workers(num_threads) {
         for (uint32_t i = 0; i < num_threads; ++i) {
            // Use pthread with 8MB stack (matching main thread) for LLVM's deep recursion.
            // Default std::thread gets 512KB on macOS which overflows on large WASM modules.
            pthread_t tid;
            pthread_attr_t attr;
            pthread_attr_init(&attr);
            pthread_attr_setstacksize(&attr, stack_size);
            auto* ctx = new std::pair<compile_reactor*, uint32_t>(this, i);
            pthread_create(&tid, &attr, [](void* arg) -> void* {
               auto* p = static_cast<std::pair<compile_reactor*, uint32_t>*>(arg);
               p->first->worker_loop(p->second);
               delete p;
               return nullptr;
            }, ctx);
            pthread_attr_destroy(&attr);
            _pthreads.push_back(tid);
         }
      }
      ~compile_reactor() {
         _phase.store(PHASE_SHUTDOWN, std::memory_order_release);
         _phase_cv.notify_all();
         for (auto tid : _pthreads) pthread_join(tid, nullptr);
      }
      compile_reactor(const compile_reactor&) = delete;
      compile_reactor& operator=(const compile_reactor&) = delete;

      // Execute a two-phase batch:
      //   Phase 1: workers call work_fn(worker_id, item_idx) for items claimed via atomic counter
      //   between_fn: main thread runs between phases (prefix sums, buffer allocation)
      //   Phase 2: workers call merge_fn(worker_id)
      void run_batch(
         uint32_t num_items,
         std::function<void(uint32_t, uint32_t)> work_fn,
         std::function<void()> between_fn,
         std::function<void(uint32_t)> merge_fn)
      {
         _work_fn = std::move(work_fn);
         _merge_fn = std::move(merge_fn);
         _num_items = num_items;
         _next_item.store(0, std::memory_order_relaxed);

         // Phase 1: compile
         _workers_done.store(0, std::memory_order_relaxed);
         _phase.store(PHASE_COMPILE, std::memory_order_release);
         _phase_cv.notify_all();
         {
            std::unique_lock<std::mutex> lk(_mu);
            _done_cv.wait(lk, [this] { return _workers_done.load(std::memory_order_acquire) >= _num_workers; });
         }

         // Between phases (main thread)
         between_fn();

         // Phase 2: merge
         _workers_done.store(0, std::memory_order_relaxed);
         _phase.store(PHASE_MERGE, std::memory_order_release);
         _phase_cv.notify_all();
         {
            std::unique_lock<std::mutex> lk(_mu);
            _done_cv.wait(lk, [this] { return _workers_done.load(std::memory_order_acquire) >= _num_workers; });
         }

         // Park workers
         _phase.store(PHASE_IDLE, std::memory_order_release);
         _phase_cv.notify_all();
      }

      uint32_t num_workers() const { return _num_workers; }

    private:
      static constexpr uint32_t PHASE_IDLE     = 0;
      static constexpr uint32_t PHASE_COMPILE  = 1;
      static constexpr uint32_t PHASE_MERGE    = 2;
      static constexpr uint32_t PHASE_SHUTDOWN  = 3;

      void worker_loop(uint32_t id) {
         uint32_t last_phase = PHASE_IDLE;
         while (true) {
            // Wait for phase change
            uint32_t phase;
            {
               std::unique_lock<std::mutex> lk(_mu);
               _phase_cv.wait(lk, [&] {
                  phase = _phase.load(std::memory_order_acquire);
                  return phase != last_phase;
               });
            }
            last_phase = phase;

            if (phase == PHASE_SHUTDOWN) return;
            if (phase == PHASE_IDLE) continue;

            if (phase == PHASE_COMPILE) {
               // Claim and process items via atomic counter
               while (true) {
                  uint32_t idx = _next_item.fetch_add(1, std::memory_order_relaxed);
                  if (idx >= _num_items) break;
                  _work_fn(id, idx);
               }
            } else if (phase == PHASE_MERGE) {
               _merge_fn(id);
            }

            // Signal done
            if (_workers_done.fetch_add(1, std::memory_order_acq_rel) + 1 >= _num_workers) {
               std::lock_guard<std::mutex> lk(_mu);
               _done_cv.notify_one();
            }
         }
      }

      std::vector<pthread_t> _pthreads;
      uint32_t _num_workers;

      std::function<void(uint32_t, uint32_t)> _work_fn;
      std::function<void(uint32_t)> _merge_fn;
      uint32_t _num_items = 0;

      std::atomic<uint32_t> _next_item{0};
      std::atomic<uint32_t> _workers_done{0};
      std::atomic<uint32_t> _phase{PHASE_IDLE};

      std::mutex _mu;
      std::condition_variable _phase_cv, _done_cv;
   };

   // Get persistent reactor (created once, lives for process lifetime).
   inline compile_reactor& get_compile_reactor(uint32_t num_threads) {
      static compile_reactor instance(num_threads);
      return instance;
   }
#endif

   template<typename CodeGen>
   class ir_writer_impl {
      using codegen_t = CodeGen;
    public:
      // Subclass access to IR data for alternative codegen pipelines (e.g., LLVM)
      ir_function* get_functions() const { return _functions; }
      uint32_t     get_num_functions() const { return _num_functions; }
      module&      get_ir_module() const { return _mod; }
      // Parser hook: called before emit_block/loop/if/try_table/return to
      // supply the actual WASM result types. Lets us size vstack slots for
      // v128 (2 slots per value) instead of assuming 1 slot per result.
      void set_pending_result_types(const uint8_t* types, uint32_t count) {
         _pending_result_types = types;
         _pending_result_types_count = count;
      }
      growable_allocator& get_ir_allocator() { return _ir_alloc; }
      jit_scratch_allocator& get_ir_scratch() { return _scratch; }

      // Phase 4 gas-metering handles. The parser hooks these via SFINAE:
      // after emit_prologue it captures prologue_gas_handle(), after
      // emit_loop it captures last_loop_gas_handle(), and at end-of-scope
      // it calls patch_gas_imm_add_extra(handle, extras) to widen the
      // per-scope heavy-op sum. For jit2 there's no byte patching —
      // codegen is deferred, so the handle points directly at the IR
      // field (prologue_gas_extra / loop_gas_extra) that codegen reads.
      void* prologue_gas_handle() const {
         return _func ? static_cast<void*>(&_func->prologue_gas_extra) : nullptr;
      }
      void* last_loop_gas_handle() const { return _last_loop_gas_ptr; }
      void patch_gas_imm_add_extra(void* handle, int64_t extra) {
         if (!handle || extra == 0) return;
         *static_cast<int64_t*>(handle) += extra;
      }
      // Branch/label types — dummy values since IR tracks control flow directly.
      // The parser stores and passes these between emit_if/emit_else/emit_end/emit_br
      // but never interprets them. fix_branch is a no-op.
      using branch_t = uint32_t;
      using label_t  = uint32_t;
      using base_writer_t = ir_writer_impl;
      using parse_callback_t = std::function<void(uint32_t, ir_writer_impl&, uint64_t&)>;

      ir_writer_impl(growable_allocator& alloc, std::size_t source_bytes, module& mod,
                bool enable_backtrace = false, bool stack_limit_is_bytes = false,
                uint32_t compile_threads = 0)
         : _allocator(alloc), _source_bytes(source_bytes), _mod(mod),
           _scratch(_ir_alloc), _compile_threads(compile_threads),
           _enable_backtrace(enable_backtrace), _stack_limit_is_bytes(stack_limit_is_bytes) {
         // IR allocator — holds per-function IR data, reset after each function.
         // In serial mode, a single allocator is used and reset after each function.
         // In parallel mode, each pipeline slot gets its own allocator so parsing
         // overlaps with optimization of the previous function.
         size_t max_func_size = 0;
         for (uint32_t i = 0; i < mod.code.size(); ++i) {
            if (mod.code[i].size > max_func_size)
               max_func_size = mod.code[i].size;
         }
#ifdef PSIZAM_COMPILE_ONLY
         {
            size_t ir_size;
            if (compile_threads > 1) {
               // Parallel mode: _functions array + one function's IR (parsed on main
               // thread before being compiled on workers, then reset per-function).
               ir_size = mod.code.size() * sizeof(ir_function) + max_func_size * 250 + 1024 * 1024;
            } else {
               ir_size = max_func_size * 250 + 1024 * 1024;
            }
            if (ir_size < 16 * 1024 * 1024) ir_size = 16 * 1024 * 1024;
            ir_size = growable_allocator::align_to_page(ir_size);
            _ir_alloc.use_fixed_memory(ir_size);
         }
#else
         _ir_alloc.use_default_memory();
#endif
         _max_func_bytes = max_func_size;
         _num_functions = mod.code.size();
         _functions = _scratch.alloc<ir_function>(_num_functions);
         for (uint32_t i = 0; i < _num_functions; ++i) {
            _functions[i] = ir_function{};
         }
         _post_array_offset = _ir_alloc._offset;
      }

      // Worker constructor: lightweight instance for parallel parse threads.
      // Shares _functions array and module with the parent writer.
      // Has its own _func/_unreachable (member vars) and IR allocator.
      // Destructor is a no-op (_skip_codegen = true).
      ir_writer_impl(module& mod, ir_function* functions, uint32_t num_functions,
                     growable_allocator& worker_ir_alloc,
                     bool enable_backtrace, bool stack_limit_is_bytes)
         : _allocator(worker_ir_alloc), _source_bytes(0), _mod(mod),
           _scratch(worker_ir_alloc), _compile_threads(0),
           _enable_backtrace(enable_backtrace), _stack_limit_is_bytes(stack_limit_is_bytes) {
         _functions = functions;
         _num_functions = num_functions;
         _skip_codegen = true;
      }

      ~ir_writer_impl() {
         // If destructor runs during stack unwinding (parsing threw an exception),
         // skip compilation — the codegen may be in an inconsistent state.
#ifdef __EXCEPTIONS
         if (std::uncaught_exceptions() > 0) {
            return;
         }
#endif

         // If a subclass handles codegen (e.g., LLVM), skip native codegen finalization.
         if (_skip_codegen) {
            return;
         }

#if !defined(__wasi__)
         if (_compile_threads > 1) {
            parallel_parse_compile_all();
            _allocator.reset();
            return;
         }
#endif

         // Finalize the code segment (all functions already compiled in finalize()).
         if (_codegen) {
            _codegen->finalize_code();
#if defined(PSIZAM_JIT_SIGNAL_DIAGNOSTICS) && !defined(PSIZAM_COMPILE_ONLY)
            // Build function range table for crash diagnostics
            {
               uint32_t num_imported = _mod.get_imported_functions_size();
               auto* ranges = new jit_func_range[_num_functions];
               for (uint32_t i = 0; i < _num_functions; ++i) {
                  auto& body = _mod.code[i];
                  ranges[i] = { static_cast<uint32_t>(body.jit_code_offset),
                                body.jit_code_size, i + num_imported };
               }
               jit_func_ranges = ranges;
               jit_func_range_count = _num_functions;
            }
#endif
            // Capture relocations before codegen is destroyed
            if (_compile_result) {
               const auto& entries = _codegen->relocations().entries();
               _compile_result->relocs.assign(entries.begin(), entries.end());
            }
            _codegen.reset();
         }
         // Reset the parsing allocator — native code has been copied to the
         // JIT segment by end_code<true>(). This allows the module allocator
         // to be reused and satisfies the assert in backend::construct().
         _allocator.reset();
      }

      static constexpr uint32_t get_depth_for_type(uint8_t type) {
         if (type == types::v128) return 2;
         return type == types::pseudo ? 0 : 1;
      }

      // ──── Branch hinting ────

      void set_wasm_pc(const uint8_t* pc) { _wasm_pc = pc; }

      uint8_t lookup_branch_hint() const {
         if (!_wasm_pc || !_func) return 0xFF;
         auto& fb = _mod.code[_func->func_index];
         if (fb.branch_hints.empty() || !fb.body_start) return 0xFF;
         uint32_t offset = static_cast<uint32_t>(_wasm_pc - fb.body_start);
         // Binary search for the offset in sorted hints
         for (const auto& h : fb.branch_hints) {
            if (h.offset == offset) return h.value;
            if (h.offset > offset) break;
         }
         return 0xFF;
      }

      // ──── Prologue / epilogue ────

      void emit_prologue(const func_type& ft, const std::vector<local_entry>& locals, uint32_t funcnum) {
         // Initialize IR for this function using the actual function body size
         _func = &_functions[funcnum];
         _func->init(_scratch, _mod.code[funcnum].size);
         _func->func_index = funcnum;
         _func->type = &ft;
         _func->num_params = ft.param_types.size();
         _last_loop_gas_ptr = nullptr;

         // Count total locals (params + body locals)
         uint32_t total = ft.param_types.size();
         for (const auto& local : locals) {
            total += local.count;
         }
         _func->num_locals = total;
         _unreachable = false;

         // Push function-level control entry
         ir_control_entry entry{};
         entry.block_idx = _func->new_block();
         entry.stack_depth = 0;
         entry.result_type = ft.return_count > 0 ? static_cast<uint8_t>(ft.return_type) : types::pseudo;
         entry.is_loop = 0;
         entry.is_function = 1;
         entry.entered_unreachable = 0;
         entry.result_count = ft.return_count;
         std::memset(entry.result_types, 0, sizeof(entry.result_types));
         std::memset(entry.merge_vregs, 0xFF, sizeof(entry.merge_vregs));
         // Allocate merge vregs so br-to-function-body can pass return values
         if (ft.return_count > 1) {
            for (uint32_t i = 0; i < ft.return_count && i < 16; ++i) {
               uint8_t rti = (i < ft.return_types.size()) ? static_cast<uint8_t>(ft.return_types[i]) : types::i64;
               entry.merge_vregs[i] = _func->alloc_vreg(rti);
               entry.result_types[i] = rti;
            }
            entry.merge_vreg = entry.merge_vregs[0];
         } else if (entry.result_type != types::pseudo) {
            entry.merge_vreg = _func->alloc_vreg(entry.result_type);
            entry.merge_vregs[0] = entry.merge_vreg;
            entry.result_types[0] = entry.result_type;
         } else {
            entry.merge_vreg = ir_vreg_none;
         }
         _func->ctrl_push(entry);
         _func->start_block(entry.block_idx, compute_v128_result_bytes(entry));
      }

      void emit_epilogue(const func_type& /*ft*/, const std::vector<local_entry>& /*locals*/, uint32_t /*funcnum*/) {
         // Nothing to do — IR is complete after parsing
      }

      void ensure_codegen() {
         if (!_codegen) {
#ifdef PSIZAM_COMPILE_ONLY
            // Scratch must hold per-function transient data (vreg maps, block fixups, etc.)
            // Scale with largest function: ~20 bytes per WASM byte for scratch data.
            size_t scratch_size = std::max<size_t>(32 * 1024 * 1024,
               growable_allocator::align_to_page(_max_func_bytes * 20 + 4 * 1024 * 1024));
            _codegen_scratch.use_fixed_memory(scratch_size);
            _opt_scratch_alloc.use_fixed_memory(scratch_size);
#else
            _codegen_scratch.use_default_memory();
            _opt_scratch_alloc.use_default_memory();
#endif
            _codegen.emplace(_allocator, _mod, _codegen_scratch, _enable_backtrace, _stack_limit_is_bytes);
            // Apply caller-requested fp_mode BEFORE emitting entry/handlers.
            // Default in the codegen is fp_mode::softfloat, which bakes raw
            // (unrelocated) pointers to _psizam_f32_* / _psizam_f64_* helpers
            // into every FP op — fine for in-process JIT, fatal for AOT where
            // those addresses are stale at load time. AOT callers (pzam-compile)
            // pass softfloat=false via pzam_compile_result so we emit native FP
            // instructions instead.
            if (_compile_result) {
               _codegen->set_fp_mode(_compile_result->softfloat
                                     ? fp_mode::softfloat
                                     : fp_mode::fast);
            }
            _codegen->emit_entry_and_error_handlers();
         }
      }

      void finalize(function_body& body) {
         if (!_skip_codegen && _func) {
#if !defined(__wasi__)
            if (_compile_threads > 1) {
               // Parallel mode: leave IR in place, compile later in destructor
               _func = nullptr;
               return;
            }
#endif
            // Serial mode: compile immediately
            ensure_codegen();
            {
               jit_scratch_allocator opt_scratch(_opt_scratch_alloc);
               jit_optimizer::optimize(*_func, opt_scratch);
               jit_regalloc::compute_live_intervals(*_func, opt_scratch);
               jit_regalloc::allocate_registers(*_func);
               _codegen->compile_function(*_func, body);
            }
            _ir_alloc._offset = _post_array_offset;
         }
         _func = nullptr;
      }

      // Parser calls these for instruction map — return dummy values since
      // null_debug_info ignores them and addresses are set during Pass 2.
      const void* get_addr() const { return nullptr; }
      const void* get_base_addr() const { return nullptr; }
      void set_stack_usage(std::uint64_t /*u*/) { }

      // ──── Control flow ────
      void emit_unreachable() {
         if (!_unreachable) {
            ir_emit_nullary(ir_op::unreachable, types::pseudo);
         }
         _unreachable = true;
      }

      void emit_nop() { }

      label_t emit_end() {
         uint32_t block_idx = UINT32_MAX;
         if (_func->ctrl_stack_top > 0) {
            auto entry = _func->ctrl_pop();
            block_idx = entry.block_idx;
            if (entry.result_count > 1) {
               if (entry.is_function) {
                  // Function scope multi-value: emit multi_return_store IR ops
                  // so regalloc sees the stores (epilogue can't reference vregs safely)
                  if (!_unreachable && _func->vstack_depth() > entry.stack_depth) {
                     uint32_t first_src = ir_vreg_none;
                     uint32_t byte_offs[16] = {};
                     compute_multi_return_offsets(entry.result_types, entry.result_count, byte_offs);
                     for (uint32_t i = entry.result_count; i > 0; --i) {
                        uint8_t rt = entry.result_types[i - 1];
                        if (rt == types::v128) _func->vpop();  // ghost slot
                        uint32_t src = _func->vpop();
                        if (i == 1) first_src = src;
                        ir_inst store{};
                        store.opcode = ir_op::multi_return_store;
                        store.type = rt;
                        store.flags = IR_SIDE_EFFECT;
                        store.dest = ir_vreg_none;
                        store.ri.src1 = src;
                        store.ri.imm = static_cast<int32_t>(byte_offs[i - 1]);
                        _func->emit(store);
                     }
                     // Also mov the first return value to merge_vregs[0] so the
                     // LLVM-level return (which uses merge_vregs[0] as the alloca
                     // for the scalar return) sees a defined value. x86_64/aarch64
                     // codegens use _multi_return[] and ignore this mov.
                     if (first_src != ir_vreg_none &&
                         entry.merge_vregs[0] != ir_vreg_none &&
                         first_src != entry.merge_vregs[0]) {
                        ir_inst mov{};
                        mov.opcode = ir_op::mov;
                        mov.type = entry.result_types[0];
                        mov.flags = IR_NONE;
                        mov.dest = entry.merge_vregs[0];
                        mov.rr.src1 = first_src;
                        mov.rr.src2 = ir_vreg_none;
                        _func->emit(mov);
                     }
                  }
                  _func->vstack_resize(entry.stack_depth);
               } else {
                  // Non-function multi-value block end: mov N results to merge vregs, push them
                  if (!_unreachable && _func->vstack_depth() > entry.stack_depth) {
                     for (int i = static_cast<int>(entry.result_count) - 1; i >= 0; --i) {
                        uint8_t rt = entry.result_types[i];
                        if (rt == types::v128) _func->vpop();  // ghost slot
                        uint32_t src = _func->vpop();
                        if (src != entry.merge_vregs[i]) {
                           ir_inst mov{};
                           mov.opcode = ir_op::mov;
                           mov.type = rt;
                           mov.flags = IR_NONE;
                           mov.dest = entry.merge_vregs[i];
                           mov.rr.src1 = src;
                           mov.rr.src2 = ir_vreg_none;
                           _func->emit(mov);
                        }
                     }
                  }
                  _func->vstack_resize(entry.stack_depth);
                  for (uint32_t i = 0; i < entry.result_count; ++i) {
                     _func->vpush(entry.merge_vregs[i]);
                     if (entry.result_types[i] == types::v128) {
                        uint32_t ghost = _func->alloc_vreg(types::v128);
                        _func->vpush(ghost);
                     }
                  }
               }
            } else if (entry.merge_vreg != ir_vreg_none) {
               // Single-value: existing path
               if (!_unreachable && entry.result_type != types::pseudo &&
                   _func->vstack_depth() > entry.stack_depth) {
                  if (entry.result_type == types::v128) _func->vpop(); // extra v128 slot
                  uint32_t else_result = _func->vpop();
                  ir_inst mov{};
                  mov.opcode = ir_op::mov;
                  mov.type = entry.result_type;
                  mov.flags = IR_NONE;
                  mov.dest = entry.merge_vreg;
                  mov.rr.src1 = else_result;
                  mov.rr.src2 = ir_vreg_none;
                  _func->emit(mov);
               }
               _func->vstack_resize(entry.stack_depth);
               _func->vpush(entry.merge_vreg);
               if (entry.result_type == types::v128) {
                  uint32_t extra = _func->alloc_vreg(types::v128);
                  _func->vpush(extra);
               }
            } else if (_unreachable) {
               _func->vstack_resize(entry.stack_depth);
               if (entry.result_count > 1) {
                  for (uint32_t i = 0; i < entry.result_count; ++i) {
                     uint8_t rt = entry.result_types[i];
                     uint32_t dest = _func->alloc_vreg(rt);
                     _func->vpush(dest);
                     if (rt == types::v128) {
                        uint32_t ghost = _func->alloc_vreg(types::v128);
                        _func->vpush(ghost);
                     }
                  }
               } else if (entry.result_type != types::pseudo) {
                  uint32_t dest = _func->alloc_vreg(entry.result_type);
                  _func->vpush(dest);
                  if (entry.result_type == types::v128) {
                     uint32_t extra = _func->alloc_vreg(types::v128);
                     _func->vpush(extra);
                  }
               }
            }

            _func->end_block(entry.block_idx, compute_v128_result_bytes(entry));
            _unreachable = entry.entered_unreachable ? true : false;
         }
         return block_idx;
      }

      branch_t emit_return(uint32_t dc, uint8_t rt, uint32_t result_count = 0) {
         if (!_unreachable) {
            if (result_count > 1) {
               // Multi-value return: emit N store instructions to _multi_return buffer
               // We use a special sequence: store each value to the context buffer,
               // then emit a return with result_count encoded in dest field
               // Result types come from the pending side channel set by the parser;
               // if absent, treat each result as i64 (legacy behavior — v128 would
               // be mis-sized but this only happens when the parser didn't set them).
               uint8_t rt_buf[16];
               const uint8_t* pt = _pending_result_types;
               uint32_t pc = _pending_result_types_count;
               _pending_result_types = nullptr;
               _pending_result_types_count = 0;
               for (uint32_t i = 0; i < result_count && i < 16; ++i)
                  rt_buf[i] = (pt && i < pc) ? pt[i] : types::i64;
               uint32_t byte_offs[16] = {};
               compute_multi_return_offsets(rt_buf, result_count, byte_offs);
               uint32_t first_src = ir_vreg_none;
               for (uint32_t i = result_count; i > 0; --i) {
                  uint8_t rti = rt_buf[i - 1];
                  if (rti == types::v128) _func->vpop(); // ghost slot
                  uint32_t src = _func->vpop();
                  if (i == 1) first_src = src;
                  ir_inst store{};
                  store.opcode = ir_op::multi_return_store;
                  store.type = rti;
                  store.flags = IR_SIDE_EFFECT;
                  store.dest = ir_vreg_none;
                  store.ri.src1 = src;
                  store.ri.imm = static_cast<int32_t>(byte_offs[i - 1]);
                  _func->emit(store);
               }
               // Also mov first value to the function's merge_vregs[0] so jit_llvm's
               // scalar-return path reads a defined value (other backends use
               // _multi_return[] and ignore this mov).
               if (_func->ctrl_stack_top > 0 && first_src != ir_vreg_none) {
                  auto& fn_entry = _func->ctrl_stack[0];
                  if (fn_entry.is_function &&
                      fn_entry.merge_vregs[0] != ir_vreg_none &&
                      first_src != fn_entry.merge_vregs[0]) {
                     ir_inst mov{};
                     mov.opcode = ir_op::mov;
                     mov.type = rt_buf[0];
                     mov.flags = IR_NONE;
                     mov.dest = fn_entry.merge_vregs[0];
                     mov.rr.src1 = first_src;
                     mov.rr.src2 = ir_vreg_none;
                     _func->emit(mov);
                  }
               }
               ir_inst inst{};
               inst.opcode = ir_op::return_;
               inst.type = rt;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = result_count; // encode result_count for codegen
               inst.rr.src1 = ir_vreg_none; // values already stored
               inst.rr.src2 = ir_vreg_none;
               _func->emit(inst);
            } else {
               ir_inst inst{};
               inst.opcode = ir_op::return_;
               inst.type = rt;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = ir_vreg_none;
               if (rt != types::pseudo && _func->vstack_depth() > 0) {
                  if (rt == types::v128) _func->vpop(); // extra v128 slot
                  inst.rr.src1 = _func->vpop();
               } else {
                  inst.rr.src1 = ir_vreg_none;
               }
               inst.rr.src2 = ir_vreg_none;
               _func->emit(inst);
            }
         }
         _unreachable = true;
         return UINT32_MAX; // return doesn't need branch fixup
      }

      // Pulls the types previously set via set_pending_result_types into entry
      // and clears the side channel. Falls back to types::i64 if unset.
      void consume_pending_result_types(ir_control_entry& entry, uint32_t result_count) {
         const uint8_t* pt = _pending_result_types;
         uint32_t pc = _pending_result_types_count;
         _pending_result_types = nullptr;
         _pending_result_types_count = 0;
         for (uint32_t i = 0; i < result_count && i < 16; ++i) {
            entry.result_types[i] = (pt && i < pc) ? pt[i] : types::i64;
         }
      }

      // Number of vstack slots for a given value type (v128 = 2, others = 1).
      static uint32_t slots_for_type(uint8_t t) {
         return (t == types::v128) ? 2u : 1u;
      }

      // Byte size occupied in ctx->_multi_return[] for a given result type.
      // v128 packs as 16 bytes (low then high), others as one 8-byte slot.
      static uint32_t multi_return_bytes_for_type(uint8_t t) {
         return (t == types::v128) ? 16u : 8u;
      }

      static uint32_t compute_v128_result_bytes(const ir_control_entry& entry) {
         uint32_t bytes = 0;
         if (entry.result_count > 1) {
            for (uint32_t i = 0; i < entry.result_count; ++i)
               if (entry.result_types[i] == types::v128) bytes += 32;
         } else if (entry.result_type == types::v128) {
            bytes = 32;
         }
         return bytes;
      }

      // Fill byte_offs[0..count-1] with the byte offset of each result in
      // ctx->_multi_return[], respecting v128 = 16 bytes, scalars = 8 bytes.
      static void compute_multi_return_offsets(const uint8_t* result_types,
                                               uint32_t count,
                                               uint32_t* byte_offs) {
         uint32_t accum = 0;
         for (uint32_t i = 0; i < count; ++i) {
            byte_offs[i] = accum;
            accum += multi_return_bytes_for_type(result_types[i]);
         }
      }

      void emit_block(uint8_t result_type = types::pseudo, uint32_t result_count = 0, uint32_t param_count = 0) {
         ir_control_entry entry{};
         entry.block_idx = _func->new_block();
         entry.param_count = static_cast<uint8_t>(param_count);
         std::memset(entry.param_vregs, 0xFF, sizeof(entry.param_vregs));
         if (!_unreachable && param_count <= _func->vstack_depth()) {
            entry.stack_depth = _func->vstack_depth() - param_count;
            // Save param vregs so emit_else can re-push them for the else branch
            for (uint32_t i = 0; i < param_count && i < 16; ++i) {
               entry.param_vregs[i] = _func->vstack[entry.stack_depth + i];
            }
         } else {
            entry.stack_depth = _func->vstack_depth();
         }
         entry.result_type = result_type;
         entry.is_loop = 0;
         entry.is_function = 0;
         entry.entered_unreachable = _unreachable ? 1 : 0;
         entry.result_count = static_cast<uint8_t>(result_count);
         std::memset(entry.result_types, 0, sizeof(entry.result_types));
         std::memset(entry.merge_vregs, 0xFF, sizeof(entry.merge_vregs)); // ir_vreg_none
         if (result_count > 1) {
            // Multi-value: one merge vreg per result; use real types so emit_end
            // can pop the extra vstack slot reserved for v128 ghosts.
            consume_pending_result_types(entry, result_count);
            for (uint32_t i = 0; i < result_count && i < 16; ++i) {
               entry.merge_vregs[i] = _func->alloc_vreg(entry.result_types[i]);
            }
            entry.merge_vreg = entry.merge_vregs[0];
         } else if (result_type != types::pseudo) {
            // Single-value: backwards-compatible path
            entry.merge_vreg = _func->alloc_vreg(result_type);
            entry.merge_vregs[0] = entry.merge_vreg;
            entry.result_types[0] = result_type;
         } else {
            entry.merge_vreg = ir_vreg_none;
            // Clear any stale side-channel data so it doesn't leak to the next call.
            _pending_result_types = nullptr;
            _pending_result_types_count = 0;
         }
         _func->ctrl_push(entry);
      }

      label_t emit_loop(uint8_t result_type = types::pseudo, uint32_t result_count = 0, uint32_t param_count = 0) {
         ir_control_entry entry{};
         entry.block_idx = _func->new_block();
         _func->blocks[entry.block_idx].is_loop = 1;
         _last_loop_gas_ptr = &_func->blocks[entry.block_idx].loop_gas_extra;
         entry.param_count = static_cast<uint8_t>(param_count);
         std::memset(entry.param_vregs, 0xFF, sizeof(entry.param_vregs));
         if (!_unreachable && param_count <= _func->vstack_depth()) {
            entry.stack_depth = _func->vstack_depth() - param_count;
            for (uint32_t i = 0; i < param_count && i < 16; ++i) {
               entry.param_vregs[i] = _func->vstack[entry.stack_depth + i];
            }
         } else {
            entry.stack_depth = _func->vstack_depth();
         }
         entry.result_type = result_type;
         entry.is_loop = 1;
         entry.is_function = 0;
         entry.entered_unreachable = _unreachable ? 1 : 0;
         entry.result_count = static_cast<uint8_t>(result_count);
         std::memset(entry.result_types, 0, sizeof(entry.result_types));
         std::memset(entry.merge_vregs, 0xFF, sizeof(entry.merge_vregs));
         // Loops use param_vregs for back-branches (br to loop = re-entry), but
         // end-of-loop fall-through still produces result values that the
         // enclosing scope consumes. Allocate merge_vregs for multi-value loops
         // so emit_end's generic pop/mov/push path works; without them,
         // emit_end pushes ir_vreg_none and corrupts the outer stack (LLVM
         // return then loads undef from v0).
         if (result_count > 1) {
            consume_pending_result_types(entry, result_count);
            for (uint32_t i = 0; i < result_count && i < 16; ++i) {
               entry.merge_vregs[i] = _func->alloc_vreg(entry.result_types[i]);
            }
            entry.merge_vreg = entry.merge_vregs[0];
         } else {
            entry.merge_vreg = ir_vreg_none;
            _pending_result_types = nullptr;
            _pending_result_types_count = 0;
         }
         _func->ctrl_push(entry);
         if (!_unreachable) {
            _func->start_block(entry.block_idx, compute_v128_result_bytes(entry));
         }
         return entry.block_idx;
      }

      branch_t emit_if(uint8_t result_type = types::pseudo, uint32_t result_count = 0, uint32_t param_count = 0) {
         ir_control_entry entry{};
         entry.block_idx = _func->new_block();
         entry.result_type = result_type;
         entry.is_loop = 0;
         entry.is_function = 0;
         entry.entered_unreachable = _unreachable ? 1 : 0;
         entry.result_count = static_cast<uint8_t>(result_count);
         entry.param_count = static_cast<uint8_t>(param_count);
         std::memset(entry.result_types, 0, sizeof(entry.result_types));
         std::memset(entry.merge_vregs, 0xFF, sizeof(entry.merge_vregs));
         std::memset(entry.param_vregs, 0xFF, sizeof(entry.param_vregs));
         if (result_count > 1) {
            consume_pending_result_types(entry, result_count);
            for (uint32_t i = 0; i < result_count && i < 16; ++i) {
               entry.merge_vregs[i] = _func->alloc_vreg(entry.result_types[i]);
            }
            entry.merge_vreg = entry.merge_vregs[0];
         } else if (result_type != types::pseudo) {
            entry.merge_vreg = _func->alloc_vreg(result_type);
            entry.merge_vregs[0] = entry.merge_vreg;
            entry.result_types[0] = result_type;
         } else {
            entry.merge_vreg = ir_vreg_none;
            _pending_result_types = nullptr;
            _pending_result_types_count = 0;
         }
         uint32_t inst_idx = UINT32_MAX;
         if (!_unreachable) {
            // Only mark as if-block when reachable — this ensures the codegen's
            // block_end will call pop_if_fixup_to only when a matching if_ fixup
            // was actually pushed.  Unreachable ifs don't emit if_ instructions
            // or push fixups, so marking their blocks as is_if would cause
            // pop_if_fixup_to to steal fixups belonging to outer reachable ifs.
            _func->blocks[entry.block_idx].is_if = 1;
            uint32_t cond = _func->vpop();
            if (param_count <= _func->vstack_depth()) {
               entry.stack_depth = _func->vstack_depth() - param_count;
               // Save param vregs so emit_else can re-push them for the else branch
               for (uint32_t i = 0; i < param_count && i < 16; ++i) {
                  entry.param_vregs[i] = _func->vstack[entry.stack_depth + i];
               }
            } else {
               // Invalid module: not enough values on stack for block params.
               // Parser validation will catch this; avoid crashing here.
               entry.stack_depth = _func->vstack_depth();
            }
            ir_inst inst{};
            inst.opcode = ir_op::if_;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            uint8_t hint = lookup_branch_hint();
            if (hint == 0) inst.flags |= IR_BRANCH_UNLIKELY;
            else if (hint == 1) inst.flags |= IR_BRANCH_LIKELY;
            inst.dest = ir_vreg_none;
            inst.br.src1 = cond;
            inst.br.target = entry.block_idx;  // default target (patched by fix_branch)
            inst_idx = _func->current_inst_index();
            _func->emit(inst);
         } else {
            // Unreachable: vstack may not have param_count entries
            entry.stack_depth = _func->vstack_depth();
         }
         _func->ctrl_push(entry);
         return inst_idx;
      }

      branch_t emit_else(branch_t if_inst_idx) {
         uint32_t else_inst_idx = UINT32_MAX;
         bool was_entered_unreachable = false;
         if (_func->ctrl_stack_top > 0) {
            auto& entry = _func->ctrl_back();
            was_entered_unreachable = entry.entered_unreachable;

            // If the if was entered in unreachable code, no if_ instruction was
            // emitted and no fixup was pushed.  Skip else_ emission entirely —
            // emitting it would cause the codegen's pop_if_fixup_to to steal a
            // fixup belonging to an outer reachable if.
            if (!was_entered_unreachable) {
               // If the block has a result type and the then-branch produced a value,
               // emit a mov to the merge vreg so both branches write the same destination.
               if (!_unreachable && _func->vstack_depth() > entry.stack_depth) {
                  if (entry.result_count > 1) {
                     // Multi-value: pop N results (skipping v128 ghost slots)
                     // and mov to merge vregs with correct per-result type.
                     // Must mirror emit_end's non-function path — otherwise the
                     // then-branch writes the wrong values to merge_vregs and
                     // the else-branch (which does pop ghosts correctly) then
                     // overwrites only its own branch, leaving the joined
                     // control flow with swapped/undefined v128 results when
                     // the then-branch is taken.
                     for (int i = static_cast<int>(entry.result_count) - 1; i >= 0; --i) {
                        uint8_t rt = entry.result_types[i];
                        if (rt == types::v128) _func->vpop();  // ghost slot
                        uint32_t src = _func->vpop();
                        if (src != entry.merge_vregs[i]) {
                           ir_inst mov{};
                           mov.opcode = ir_op::mov;
                           mov.type = rt;
                           mov.flags = IR_NONE;
                           mov.dest = entry.merge_vregs[i];
                           mov.rr.src1 = src;
                           mov.rr.src2 = ir_vreg_none;
                           _func->emit(mov);
                        }
                     }
                  } else if (entry.merge_vreg != ir_vreg_none) {
                     if (entry.result_type == types::v128) _func->vpop(); // extra v128 slot
                     uint32_t then_result = _func->vpop();
                     ir_inst mov{};
                     mov.opcode = ir_op::mov;
                     mov.type = entry.result_type;
                     mov.flags = IR_NONE;
                     mov.dest = entry.merge_vreg;
                     mov.rr.src1 = then_result;
                     mov.rr.src2 = ir_vreg_none;
                     _func->emit(mov);
                  }
               }

               // Emit else_ instruction: then-block jumps to block end
               ir_inst inst{};
               inst.opcode = ir_op::else_;
               inst.type = types::pseudo;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = ir_vreg_none;
               inst.br.target = UINT32_MAX; // patched by fix_branch at end
               inst.br.src1 = ir_vreg_none;
               else_inst_idx = _func->current_inst_index();
               _func->emit(inst);
               // Patch the if_ instruction to branch HERE (else start)
               // We need a new block for the else, mark it with current inst position
               if (if_inst_idx < _func->inst_count) {
                  // if_ should branch to the else-block, not the end-block
                  // Create a new block for the else entry point
                  uint32_t else_block = _func->new_block();
                  _func->start_block(else_block);
                  _func->insts[if_inst_idx].br.target = else_block;
               }
               // Clear is_if: the else handles the if_fixup pop, so the
               // block_end emitted by emit_end shouldn't pop again.
               if (entry.block_idx < _func->block_count) {
                  _func->blocks[entry.block_idx].is_if = 0;
               }
            }
            _func->vstack_resize(entry.stack_depth);
            // Re-push parameter vregs so the else branch has them available
            for (uint32_t i = 0; i < entry.param_count && i < 16; ++i) {
               _func->vpush(entry.param_vregs[i]);
            }
         }
         // Only become reachable if the if was entered in reachable code.
         // If the if was in dead code, the else body is also unreachable.
         _unreachable = was_entered_unreachable;
         return else_inst_idx;
      }

      branch_t emit_br(uint32_t dc, uint8_t rt, uint32_t label = UINT32_MAX, uint32_t result_count = 0) {
         uint32_t inst_idx = UINT32_MAX;
         if (!_unreachable) {
            // Branch target handling: emit movs/stores depending on target type
            if (label != UINT32_MAX) {
               uint32_t target_ctrl = _func->ctrl_stack_top - 1 - label;
               if (target_ctrl < _func->ctrl_stack_top) {
                  auto& target_entry = _func->ctrl_stack[target_ctrl];
                  if (target_entry.is_loop && target_entry.param_count > 0) {
                     // Loop target: mov values to loop's param vregs
                     uint32_t n = std::min<uint32_t>(result_count, target_entry.param_count);
                     if (n > _func->vstack_top) n = _func->vstack_top;
                     uint32_t base_depth = _func->vstack_top - n;
                     for (uint32_t i = 0; i < n; ++i) {
                        uint32_t src = _func->vstack[base_depth + i];
                        if (src != target_entry.param_vregs[i]) {
                           ir_inst mov{};
                           mov.opcode = ir_op::mov;
                           mov.type = types::i64;
                           mov.flags = IR_NONE;
                           mov.dest = target_entry.param_vregs[i];
                           mov.rr.src1 = src;
                           mov.rr.src2 = ir_vreg_none;
                           _func->emit(mov);
                        }
                     }
                  } else if (!target_entry.is_loop && result_count > 1 && target_entry.result_count > 1) {
                     // Non-loop multi-value target: walk through target's result
                     // types so v128 (2 vstack slots) is picked up correctly.
                     uint32_t n = std::min<uint32_t>(result_count, target_entry.result_count);
                     uint32_t total = 0;
                     for (uint32_t i = 0; i < n; ++i)
                        total += slots_for_type(target_entry.result_types[i]);
                     if (total > _func->vstack_top) total = _func->vstack_top;
                     uint32_t base_depth = _func->vstack_top - total;
                     if (target_entry.is_function) {
                        // Function body target: emit multi_return_store.
                        // Emit in REVERSE order so aarch64 / x86_64-stack LIFO
                        // v128 pops from the native stack match byte_offs[].
                        uint32_t first_src = ir_vreg_none;
                        uint32_t byte_offs[16] = {};
                        uint32_t src_vregs[16] = {};
                        compute_multi_return_offsets(target_entry.result_types, n, byte_offs);
                        {
                           uint32_t off = 0;
                           for (uint32_t i = 0; i < n; ++i) {
                              src_vregs[i] = _func->vstack[base_depth + off];
                              off += slots_for_type(target_entry.result_types[i]);
                           }
                        }
                        first_src = src_vregs[0];
                        for (uint32_t i = n; i > 0; --i) {
                           uint8_t rti = target_entry.result_types[i - 1];
                           ir_inst store{};
                           store.opcode = ir_op::multi_return_store;
                           store.type = rti;
                           store.flags = IR_SIDE_EFFECT;
                           store.dest = ir_vreg_none;
                           store.ri.src1 = src_vregs[i - 1];
                           store.ri.imm = static_cast<int32_t>(byte_offs[i - 1]);
                           _func->emit(store);
                        }
                        // Also mov first value to merge_vregs[0] for jit_llvm scalar return
                        if (first_src != ir_vreg_none && target_entry.merge_vregs[0] != ir_vreg_none &&
                            first_src != target_entry.merge_vregs[0]) {
                           ir_inst mov{};
                           mov.opcode = ir_op::mov;
                           mov.type = target_entry.result_types[0];
                           mov.flags = IR_NONE;
                           mov.dest = target_entry.merge_vregs[0];
                           mov.rr.src1 = first_src;
                           mov.rr.src2 = ir_vreg_none;
                           _func->emit(mov);
                        }
                     } else {
                        uint32_t off = 0;
                        for (uint32_t i = 0; i < n; ++i) {
                           uint8_t rti = target_entry.result_types[i];
                           uint32_t src = _func->vstack[base_depth + off];
                           if (src != target_entry.merge_vregs[i]) {
                              ir_inst mov{};
                              mov.opcode = ir_op::mov;
                              mov.type = rti;
                              mov.flags = IR_NONE;
                              mov.dest = target_entry.merge_vregs[i];
                              mov.rr.src1 = src;
                              mov.rr.src2 = ir_vreg_none;
                              _func->emit(mov);
                           }
                           off += slots_for_type(rti);
                        }
                     }
                  } else if (!target_entry.is_loop && rt != types::pseudo &&
                             target_entry.merge_vreg != ir_vreg_none && _func->vstack_depth() > 0) {
                     // Single-value non-loop target
                     uint32_t src = (rt == types::v128 && _func->vstack_top >= 2)
                                    ? _func->vstack[_func->vstack_top - 2]
                                    : _func->vstack_back();
                     if (src != target_entry.merge_vreg) {
                        ir_inst mov{};
                        mov.opcode = ir_op::mov;
                        mov.type = rt;
                        mov.flags = IR_NONE;
                        mov.dest = target_entry.merge_vreg;
                        mov.rr.src1 = src;
                        mov.rr.src2 = ir_vreg_none;
                        _func->emit(mov);
                     }
                  }
               }
            }
            ir_inst inst{};
            inst.opcode = ir_op::br;
            inst.type = rt;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dc; // depth change for multipop
            inst.br.target = UINT32_MAX; // patched by fix_branch
            if (rt != types::pseudo && _func->vstack_depth() > 0) {
               inst.br.src1 = (rt == types::v128 && _func->vstack_top >= 2)
                              ? _func->vstack[_func->vstack_top - 2]
                              : _func->vstack_back();
            } else {
               inst.br.src1 = ir_vreg_none;
            }
            inst_idx = _func->current_inst_index();
            _func->emit(inst);
         }
         _unreachable = true;
         return inst_idx;
      }

      branch_t emit_br_if(uint32_t dc, uint8_t rt, uint32_t label = UINT32_MAX, uint32_t result_count = 0, uint32_t eh_leave_count = 0) {
         uint32_t inst_idx = UINT32_MAX;
         if (!_unreachable) {
            uint32_t cond = _func->vpop();
            if (label != UINT32_MAX) {
               uint32_t target_ctrl = _func->ctrl_stack_top - 1 - label;
               if (target_ctrl < _func->ctrl_stack_top) {
                  auto& target_entry = _func->ctrl_stack[target_ctrl];
                  if (target_entry.is_loop && target_entry.param_count > 0) {
                     // Loop target: mov values to loop's param vregs
                     uint32_t n = std::min<uint32_t>(result_count, target_entry.param_count);
                     if (n > _func->vstack_top) n = _func->vstack_top;
                     uint32_t base_depth = _func->vstack_top - n;
                     for (uint32_t i = 0; i < n; ++i) {
                        uint32_t src = _func->vstack[base_depth + i];
                        if (src != target_entry.param_vregs[i]) {
                           ir_inst mov{};
                           mov.opcode = ir_op::mov;
                           mov.type = types::i64;
                           mov.flags = IR_NONE;
                           mov.dest = target_entry.param_vregs[i];
                           mov.rr.src1 = src;
                           mov.rr.src2 = ir_vreg_none;
                           _func->emit(mov);
                        }
                     }
                  } else if (!target_entry.is_loop && result_count > 1 && target_entry.result_count > 1) {
                     // Non-loop multi-value target: walk through target's result
                     // types so v128 (2 vstack slots) is picked up correctly.
                     uint32_t n = std::min<uint32_t>(result_count, target_entry.result_count);
                     uint32_t total = 0;
                     for (uint32_t i = 0; i < n; ++i)
                        total += slots_for_type(target_entry.result_types[i]);
                     if (total > _func->vstack_top) total = _func->vstack_top;
                     uint32_t base_depth = _func->vstack_top - total;
                     if (target_entry.is_function) {
                        // Function body target: emit multi_return_store.
                        // Emit in REVERSE order so aarch64 / x86_64-stack LIFO
                        // v128 pops from the native stack match byte_offs[].
                        uint32_t first_src = ir_vreg_none;
                        uint32_t byte_offs[16] = {};
                        uint32_t src_vregs[16] = {};
                        compute_multi_return_offsets(target_entry.result_types, n, byte_offs);
                        {
                           uint32_t off = 0;
                           for (uint32_t i = 0; i < n; ++i) {
                              src_vregs[i] = _func->vstack[base_depth + off];
                              off += slots_for_type(target_entry.result_types[i]);
                           }
                        }
                        first_src = src_vregs[0];
                        for (uint32_t i = n; i > 0; --i) {
                           uint8_t rti = target_entry.result_types[i - 1];
                           ir_inst store{};
                           store.opcode = ir_op::multi_return_store;
                           store.type = rti;
                           store.flags = IR_SIDE_EFFECT;
                           store.dest = ir_vreg_none;
                           store.ri.src1 = src_vregs[i - 1];
                           store.ri.imm = static_cast<int32_t>(byte_offs[i - 1]);
                           _func->emit(store);
                        }
                        // Also mov first value to merge_vregs[0] for jit_llvm scalar return
                        if (first_src != ir_vreg_none && target_entry.merge_vregs[0] != ir_vreg_none &&
                            first_src != target_entry.merge_vregs[0]) {
                           ir_inst mov{};
                           mov.opcode = ir_op::mov;
                           mov.type = target_entry.result_types[0];
                           mov.flags = IR_NONE;
                           mov.dest = target_entry.merge_vregs[0];
                           mov.rr.src1 = first_src;
                           mov.rr.src2 = ir_vreg_none;
                           _func->emit(mov);
                        }
                     } else {
                        uint32_t off = 0;
                        for (uint32_t i = 0; i < n; ++i) {
                           uint8_t rti = target_entry.result_types[i];
                           uint32_t src = _func->vstack[base_depth + off];
                           if (src != target_entry.merge_vregs[i]) {
                              ir_inst mov{};
                              mov.opcode = ir_op::mov;
                              mov.type = rti;
                              mov.flags = IR_NONE;
                              mov.dest = target_entry.merge_vregs[i];
                              mov.rr.src1 = src;
                              mov.rr.src2 = ir_vreg_none;
                              _func->emit(mov);
                           }
                           off += slots_for_type(rti);
                        }
                     }
                  } else if (!target_entry.is_loop && rt != types::pseudo &&
                             target_entry.merge_vreg != ir_vreg_none && _func->vstack_depth() > 0) {
                     // Single-value non-loop target
                     uint32_t src = (rt == types::v128 && _func->vstack_top >= 2)
                                    ? _func->vstack[_func->vstack_top - 2]
                                    : _func->vstack_back();
                     if (src != target_entry.merge_vreg) {
                        ir_inst mov{};
                        mov.opcode = ir_op::mov;
                        mov.type = rt;
                        mov.flags = IR_NONE;
                        mov.dest = target_entry.merge_vreg;
                        mov.rr.src1 = src;
                        mov.rr.src2 = ir_vreg_none;
                        _func->emit(mov);
                     }
                  }
               }
            }
            ir_inst inst{};
            inst.opcode = ir_op::br_if;
            inst.type = rt;
            inst.flags = IR_SIDE_EFFECT;
            { uint8_t hint = lookup_branch_hint();
              if (hint == 0) inst.flags |= IR_BRANCH_UNLIKELY;
              else if (hint == 1) inst.flags |= IR_BRANCH_LIKELY;
            }
            inst.dest = dc | (eh_leave_count << 16); // low 16 = depth change, high 16 = eh_leave_count
            inst.br.target = UINT32_MAX; // patched by fix_branch
            inst.br.src1 = cond;
            inst_idx = _func->current_inst_index();
            _func->emit(inst);
         }
         return inst_idx;
      }

      struct br_table_parser {
         ir_writer_impl* _writer;
         br_table_parser(ir_writer_impl* w) : _writer(w) {}
         branch_t emit_case(uint32_t dc, uint8_t rt, uint32_t label = UINT32_MAX, uint32_t result_count = 0, uint32_t eh_leave_count = 0) {
            // Emit merge movs if target has merge vregs
            if (label != UINT32_MAX) {
               auto* func = _writer->_func;
               uint32_t target_ctrl = func->ctrl_stack_top - 1 - label;
               if (target_ctrl < func->ctrl_stack_top) {
                  auto& target_entry = func->ctrl_stack[target_ctrl];
                  if (target_entry.is_loop && target_entry.param_count > 0) {
                     // Loop target: mov values to loop's param vregs
                     uint32_t n = std::min<uint32_t>(result_count, target_entry.param_count);
                     if (func->vstack_top >= n) {
                        uint32_t base_depth = func->vstack_top - n;
                        for (uint32_t i = 0; i < n; ++i) {
                           uint32_t src = func->vstack[base_depth + i];
                           if (src != target_entry.param_vregs[i]) {
                              ir_inst mov{};
                              mov.opcode = ir_op::mov;
                              mov.type = types::i64;
                              mov.flags = IR_NONE;
                              mov.dest = target_entry.param_vregs[i];
                              mov.rr.src1 = src;
                              mov.rr.src2 = ir_vreg_none;
                              func->emit(mov);
                           }
                        }
                     }
                  } else if (!target_entry.is_loop && result_count > 1 && target_entry.result_count > 1) {
                     // Non-loop multi-value target: walk via slots_for_type so
                     // v128 (2 vstack slots) is counted correctly.
                     uint32_t n = std::min<uint32_t>(result_count, target_entry.result_count);
                     uint32_t total = 0;
                     for (uint32_t i = 0; i < n; ++i)
                        total += ir_writer_impl::slots_for_type(target_entry.result_types[i]);
                     if (func->vstack_top >= total) {
                        uint32_t base_depth = func->vstack_top - total;
                        if (target_entry.is_function) {
                           // Function body target: emit multi_return_store.
                           // Emit in REVERSE order so aarch64 / x86_64-stack LIFO
                           // v128 pops from the native stack match byte_offs[].
                           uint32_t first_src = ir_vreg_none;
                           uint32_t byte_offs[16] = {};
                           uint32_t src_vregs[16] = {};
                           ir_writer_impl::compute_multi_return_offsets(target_entry.result_types, n, byte_offs);
                           {
                              uint32_t off = 0;
                              for (uint32_t i = 0; i < n; ++i) {
                                 src_vregs[i] = func->vstack[base_depth + off];
                                 off += ir_writer_impl::slots_for_type(target_entry.result_types[i]);
                              }
                           }
                           first_src = src_vregs[0];
                           for (uint32_t i = n; i > 0; --i) {
                              uint8_t rti = target_entry.result_types[i - 1];
                              ir_inst store{};
                              store.opcode = ir_op::multi_return_store;
                              store.type = rti;
                              store.flags = IR_SIDE_EFFECT;
                              store.dest = ir_vreg_none;
                              store.ri.src1 = src_vregs[i - 1];
                              store.ri.imm = static_cast<int32_t>(byte_offs[i - 1]);
                              func->emit(store);
                           }
                           // Also mov first value to merge_vregs[0] for jit_llvm scalar return
                           if (first_src != ir_vreg_none &&
                               target_entry.merge_vregs[0] != ir_vreg_none &&
                               first_src != target_entry.merge_vregs[0]) {
                              ir_inst mov{};
                              mov.opcode = ir_op::mov;
                              mov.type = target_entry.result_types[0];
                              mov.flags = IR_NONE;
                              mov.dest = target_entry.merge_vregs[0];
                              mov.rr.src1 = first_src;
                              mov.rr.src2 = ir_vreg_none;
                              func->emit(mov);
                           }
                        } else {
                           // Non-function block target: mov to merge vregs
                           uint32_t off = 0;
                           for (uint32_t i = 0; i < n; ++i) {
                              uint8_t rti = target_entry.result_types[i];
                              uint32_t src = func->vstack[base_depth + off];
                              if (src != target_entry.merge_vregs[i]) {
                                 ir_inst mov{};
                                 mov.opcode = ir_op::mov;
                                 mov.type = rti;
                                 mov.flags = IR_NONE;
                                 mov.dest = target_entry.merge_vregs[i];
                                 mov.rr.src1 = src;
                                 mov.rr.src2 = ir_vreg_none;
                                 func->emit(mov);
                              }
                              off += ir_writer_impl::slots_for_type(rti);
                           }
                        }
                     }
                  } else if (rt != types::pseudo && target_entry.merge_vreg != ir_vreg_none &&
                             !target_entry.is_loop && func->vstack_depth() > 0) {
                     uint32_t src = (rt == types::v128 && func->vstack_top >= 2)
                                    ? func->vstack[func->vstack_top - 2]
                                    : func->vstack_back();
                     if (src != target_entry.merge_vreg) {
                        ir_inst mov{};
                        mov.opcode = ir_op::mov;
                        mov.type = rt;
                        mov.flags = IR_NONE;
                        mov.dest = target_entry.merge_vreg;
                        mov.rr.src1 = src;
                        mov.rr.src2 = ir_vreg_none;
                        func->emit(mov);
                     }
                  }
               }
            }
            ir_inst inst{};
            inst.opcode = ir_op::br;
            inst.type = rt;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dc | (eh_leave_count << 16); // low 16 = depth change, high 16 = eh_leave_count
            inst.br.target = UINT32_MAX; // patched by fix_branch
            inst.br.src1 = ir_vreg_none;
            uint32_t inst_idx = _writer->_func->current_inst_index();
            _writer->_func->emit(inst);
            return inst_idx;
         }
         branch_t emit_default(uint32_t dc, uint8_t rt, uint32_t label = UINT32_MAX, uint32_t result_count = 0, uint32_t eh_leave_count = 0) {
            return emit_case(dc, rt, label, result_count, eh_leave_count);
         }
      };
      br_table_parser emit_br_table(uint32_t table_size) {
         if (!_unreachable) {
            uint32_t idx = _func->vpop();
            ir_inst inst{};
            inst.opcode = ir_op::br_table;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = table_size;  // store case count for jit_codegen
            inst.rr.src1 = idx;
            inst.rr.src2 = ir_vreg_none;
            _func->emit(inst);
         }
         _unreachable = true;
         return br_table_parser(this);
      }

      // ──── Calls ────
      void emit_call(const func_type& ft, uint32_t funcnum) {
         if (!_unreachable) {
            uint32_t nparams = ft.param_types.size();
            // Emit arg instructions for each parameter (needed by register mode
            // to push vreg values to the native stack before the call)
            // Pop params in reverse to get them in correct stack order
            uint32_t* param_vregs = _scratch.alloc<uint32_t>(nparams);
            for (uint32_t i = 0; i < nparams; ++i) {
               uint32_t pi = nparams - 1 - i;
               if (ft.param_types[pi] == types::v128) _func->vpop(); // extra v128 slot
               param_vregs[pi] = _func->vpop();
            }
            for (uint32_t i = 0; i < nparams; ++i) {
               ir_inst arg{};
               arg.opcode = ir_op::arg;
               arg.type = ft.param_types[i];
               arg.flags = IR_NONE;
               arg.dest = ir_vreg_none;
               arg.rr.src1 = param_vregs[i];
               arg.rr.src2 = ir_vreg_none;
               _func->emit(arg);
            }
            if (ft.return_count > 0) {
               uint32_t dest = _func->alloc_vreg(ft.return_type);
               ir_inst inst{};
               inst.opcode = ir_op::call;
               inst.type = ft.return_type;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = dest;
               inst.call.index = funcnum;
               inst.call.src1 = ir_vreg_none;
               _func->emit(inst);
               if (ft.return_count > 1) {
                  // Multi-value: first return is in dest (from RAX/X0).
                  // Store it to _multi_return[0] for consistency, then load all N from buffer.
                  // Actually: jit_codegen stores the first value to _multi_return[0] as well
                  // when return_count > 1. So we load all N values from the buffer.
                  // But wait -- the callee uses multi_return_store for all values,
                  // so we just need to load all N from _multi_return.
                  // The call instruction's dest vreg holds the return value in RAX which
                  // for multi-value is unused (callee stores everything to _multi_return).
                  // Load all N return values from _multi_return buffer.
                  uint32_t byte_offs[16] = {};
                  compute_multi_return_offsets(ft.return_types.data(), ft.return_count, byte_offs);
                  for (uint32_t i = 0; i < ft.return_count; ++i) {
                     uint32_t load_dest = _func->alloc_vreg(ft.return_types[i]);
                     ir_inst load{};
                     load.opcode = ir_op::multi_return_load;
                     load.type = ft.return_types[i];
                     load.flags = IR_SIDE_EFFECT;
                     load.dest = load_dest;
                     load.ri.src1 = ir_vreg_none;
                     load.ri.imm = static_cast<int32_t>(byte_offs[i]);
                     _func->emit(load);
                     _func->vpush(load_dest);
                     // v128 occupies 2 vstack slots (real + ghost) for consumer ops.
                     if (ft.return_types[i] == types::v128) {
                        uint32_t ghost = _func->alloc_vreg(types::v128);
                        _func->vpush(ghost);
                     }
                  }
               } else {
                  _func->vpush(dest);
                  if (ft.return_type == types::v128) {
                     uint32_t dest2 = _func->alloc_vreg(ft.return_type);
                     _func->vpush(dest2);
                  }
               }
            } else {
               ir_inst inst{};
               inst.opcode = ir_op::call;
               inst.type = types::pseudo;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = ir_vreg_none;
               inst.call.index = funcnum;
               inst.call.src1 = ir_vreg_none;
               _func->emit(inst);
            }
         }
      }

      void emit_table_get(uint32_t table_idx) {
         if (!_unreachable) {
            uint32_t idx_vreg = _func->vpop();
            ir_emit_arg(idx_vreg);
            uint32_t dest = _func->alloc_vreg(types::i32);
            ir_inst inst{};
            inst.opcode = ir_op::table_get;
            inst.type = types::i32;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.ri.imm = static_cast<int32_t>(table_idx);
            _func->emit(inst);
            _func->vpush(dest);
         }
      }
      void emit_ref_null(uint8_t /*type*/) {
         // ref.null → push UINT32_MAX (null sentinel)
         emit_i32_const(UINT32_MAX);
      }
      void emit_ref_is_null() {
         // ref.is_null → compare top of stack with UINT32_MAX
         if (!_unreachable) {
            // Push UINT32_MAX, then compare equal
            emit_i32_const(UINT32_MAX);
            ir_binop(ir_op::i32_eq, types::i32);
         }
      }
      void emit_ref_func(uint32_t idx) {
         // ref.func → push function index
         emit_i32_const(idx);
      }
      void emit_table_set(uint32_t table_idx) {
         if (!_unreachable) {
            uint32_t val_vreg = _func->vpop();
            uint32_t idx_vreg = _func->vpop();
            ir_emit_arg(idx_vreg);
            ir_emit_arg(val_vreg);
            ir_inst inst{};
            inst.opcode = ir_op::table_set;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.ri.imm = static_cast<int32_t>(table_idx);
            _func->emit(inst);
         }
      }
      void emit_table_grow(uint32_t table_idx) {
         if (!_unreachable) {
            uint32_t delta_vreg = _func->vpop();
            uint32_t init_vreg = _func->vpop();
            ir_emit_arg(init_vreg);
            ir_emit_arg(delta_vreg);
            uint32_t dest = _func->alloc_vreg(types::i32);
            ir_inst inst{};
            inst.opcode = ir_op::table_grow;
            inst.type = types::i32;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.ri.imm = static_cast<int32_t>(table_idx);
            _func->emit(inst);
            _func->vpush(dest);
         }
      }
      void emit_table_size(uint32_t table_idx) {
         if (!_unreachable) {
            uint32_t dest = _func->alloc_vreg(types::i32);
            ir_inst inst{};
            inst.opcode = ir_op::table_size;
            inst.type = types::i32;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.ri.imm = static_cast<int32_t>(table_idx);
            _func->emit(inst);
            _func->vpush(dest);
         }
      }
      void emit_table_fill(uint32_t table_idx) {
         ir_bulk_mem3(); // pops n, val, i
         if (!_unreachable) {
            ir_inst inst{};
            inst.opcode = ir_op::table_fill;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.ri.imm = static_cast<int32_t>(table_idx);
            _func->emit(inst);
         }
      }

      void emit_call_indirect(const func_type& ft, uint32_t fti, uint32_t table_idx = 0) {
         if (!_unreachable) {
            uint32_t elem_idx_vreg = _func->vpop();
            uint32_t nparams = ft.param_types.size();
            // Emit arg instructions for each parameter
            uint32_t* param_vregs = _scratch.alloc<uint32_t>(nparams);
            for (uint32_t i = 0; i < nparams; ++i) {
               uint32_t pi = nparams - 1 - i;
               if (ft.param_types[pi] == types::v128) _func->vpop(); // extra v128 slot
               param_vregs[pi] = _func->vpop();
            }
            for (uint32_t i = 0; i < nparams; ++i) {
               ir_inst arg{};
               arg.opcode = ir_op::arg;
               arg.type = ft.param_types[i];
               arg.flags = IR_NONE;
               arg.dest = ir_vreg_none;
               arg.rr.src1 = param_vregs[i];
               arg.rr.src2 = ir_vreg_none;
               _func->emit(arg);
            }
            // Emit arg for element index (must be on top of stack for call_indirect)
            {
               ir_inst arg{};
               arg.opcode = ir_op::arg;
               arg.type = types::pseudo;
               arg.flags = IR_NONE;
               arg.dest = ir_vreg_none;
               arg.rr.src1 = elem_idx_vreg;
               arg.rr.src2 = ir_vreg_none;
               _func->emit(arg);
            }
            // Pack table_idx into upper 16 bits of call.index
            uint32_t packed_fti = fti | (table_idx << 16);
            if (ft.return_count > 0) {
               uint32_t dest = _func->alloc_vreg(ft.return_type);
               ir_inst inst{};
               inst.opcode = ir_op::call_indirect;
               inst.type = ft.return_type;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = dest;
               inst.call.index = packed_fti;
               inst.call.src1 = elem_idx_vreg;
               _func->emit(inst);
               if (ft.return_count > 1) {
                  // Multi-value: load all N values from _multi_return buffer
                  uint32_t byte_offs[16] = {};
                  compute_multi_return_offsets(ft.return_types.data(), ft.return_count, byte_offs);
                  for (uint32_t i = 0; i < ft.return_count; ++i) {
                     uint32_t load_dest = _func->alloc_vreg(ft.return_types[i]);
                     ir_inst load{};
                     load.opcode = ir_op::multi_return_load;
                     load.type = ft.return_types[i];
                     load.flags = IR_SIDE_EFFECT;
                     load.dest = load_dest;
                     load.ri.src1 = ir_vreg_none;
                     load.ri.imm = static_cast<int32_t>(byte_offs[i]);
                     _func->emit(load);
                     _func->vpush(load_dest);
                     // v128 occupies 2 vstack slots (real + ghost) for consumer ops.
                     if (ft.return_types[i] == types::v128) {
                        uint32_t ghost = _func->alloc_vreg(types::v128);
                        _func->vpush(ghost);
                     }
                  }
               } else {
                  _func->vpush(dest);
                  if (ft.return_type == types::v128) {
                     uint32_t dest2 = _func->alloc_vreg(ft.return_type);
                     _func->vpush(dest2);
                  }
               }
            } else {
               ir_inst inst{};
               inst.opcode = ir_op::call_indirect;
               inst.type = types::pseudo;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = ir_vreg_none;
               inst.call.index = packed_fti;
               inst.call.src1 = elem_idx_vreg;
               _func->emit(inst);
            }
         }
      }

      // ──── Tail calls ────
      void emit_tail_call(const func_type& ft, uint32_t funcnum) {
         if (!_unreachable) {
            uint32_t nparams = ft.param_types.size();
            uint32_t* param_vregs = _scratch.alloc<uint32_t>(nparams);
            for (uint32_t i = 0; i < nparams; ++i) {
               uint32_t pi = nparams - 1 - i;
               if (ft.param_types[pi] == types::v128) _func->vpop();
               param_vregs[pi] = _func->vpop();
            }
            for (uint32_t i = 0; i < nparams; ++i) {
               ir_inst arg{};
               arg.opcode = ir_op::arg;
               arg.type = ft.param_types[i];
               arg.flags = IR_NONE;
               arg.dest = ir_vreg_none;
               arg.rr.src1 = param_vregs[i];
               arg.rr.src2 = ir_vreg_none;
               _func->emit(arg);
            }
            ir_inst inst{};
            inst.opcode = ir_op::tail_call;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.call.index = funcnum;
            inst.call.src1 = ir_vreg_none;
            _func->emit(inst);
         }
         _unreachable = true;
      }

      void emit_tail_call_indirect(const func_type& ft, uint32_t fti, uint32_t table_idx = 0) {
         if (!_unreachable) {
            uint32_t elem_idx_vreg = _func->vpop();
            uint32_t nparams = ft.param_types.size();
            uint32_t* param_vregs = _scratch.alloc<uint32_t>(nparams);
            for (uint32_t i = 0; i < nparams; ++i) {
               uint32_t pi = nparams - 1 - i;
               if (ft.param_types[pi] == types::v128) _func->vpop();
               param_vregs[pi] = _func->vpop();
            }
            for (uint32_t i = 0; i < nparams; ++i) {
               ir_inst arg{};
               arg.opcode = ir_op::arg;
               arg.type = ft.param_types[i];
               arg.flags = IR_NONE;
               arg.dest = ir_vreg_none;
               arg.rr.src1 = param_vregs[i];
               arg.rr.src2 = ir_vreg_none;
               _func->emit(arg);
            }
            {
               ir_inst arg{};
               arg.opcode = ir_op::arg;
               arg.type = types::pseudo;
               arg.flags = IR_NONE;
               arg.dest = ir_vreg_none;
               arg.rr.src1 = elem_idx_vreg;
               arg.rr.src2 = ir_vreg_none;
               _func->emit(arg);
            }
            uint32_t packed_fti = fti | (table_idx << 16);
            ir_inst inst{};
            inst.opcode = ir_op::tail_call_indirect;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.call.index = packed_fti;
            inst.call.src1 = elem_idx_vreg;
            _func->emit(inst);
         }
         _unreachable = true;
      }

      // ──── Parametric ────
      void emit_drop(uint8_t type) {
         if (!_unreachable) {
            _func->vpop();
            if (type == types::v128) {
               uint32_t low_vreg = _func->vpop(); // low vreg of v128
               // Emit a drop IR so codegen can pop 16 bytes from x86 stack if needed
               ir_inst inst{};
               inst.opcode = ir_op::drop;
               inst.type = types::v128;
               inst.flags = IR_NONE;
               inst.dest = ir_vreg_none;
               inst.rr.src1 = low_vreg; // track which vreg is dropped
               inst.rr.src2 = ir_vreg_none;
               _func->emit(inst);
            }
         }
      }

      void emit_select(uint8_t type) {
         if (!_unreachable) {
            uint32_t cond = _func->vpop();
            if (type == types::v128) _func->vpop(); // extra v128 slot for val2
            uint32_t val2 = _func->vpop();
            if (type == types::v128) _func->vpop(); // extra v128 slot for val1
            uint32_t val1 = _func->vpop();
            uint32_t dest = _func->alloc_vreg(type);
            ir_inst inst{};
            inst.opcode = ir_op::select;
            inst.type = type;
            inst.dest = dest;
            inst.sel.val1 = val1;
            inst.sel.val2 = val2;
            inst.sel.cond = cond;
            _func->emit(inst);
            _func->vpush(dest);
            if (type == types::v128) {
               uint32_t dest2 = _func->alloc_vreg(type);
               _func->vpush(dest2);
            }
         }
      }

      // ──── Local / global ────
      void emit_get_local(uint32_t li, uint8_t ty) {
         if (!_unreachable) {
            uint32_t dest = _func->alloc_vreg(ty);
            ir_inst inst{};
            inst.opcode = ir_op::local_get;
            inst.type = ty;
            inst.dest = dest;
            inst.local.index = li;
            inst.local.src1 = ir_vreg_none;
            _func->emit(inst);
            _func->vpush(dest);
            if (ty == types::v128) {
               uint32_t dest2 = _func->alloc_vreg(ty);
               _func->vpush(dest2);
            }
         }
      }

      void emit_set_local(uint32_t li, uint8_t ty) {
         if (!_unreachable) {
            if (ty == types::v128) _func->vpop(); // extra v128 slot
            uint32_t src = _func->vpop();
            ir_inst inst{};
            inst.opcode = ir_op::local_set;
            inst.type = ty;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.local.index = li;
            inst.local.src1 = src;
            _func->emit(inst);
         }
      }

      void emit_tee_local(uint32_t li, uint8_t ty) {
         if (!_unreachable) {
            // For v128, peek the low slot (2nd from top since v128 uses 2 slots)
            uint32_t src = (ty == types::v128 && _func->vstack_depth() >= 2)
                           ? _func->vstack[_func->vstack_top - 2]
                           : _func->vstack_back();
            ir_inst inst{};
            inst.opcode = ir_op::local_tee;
            inst.type = ty;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.local.index = li;
            inst.local.src1 = src;
            _func->emit(inst);
         }
      }

      void emit_get_global(uint32_t gi) {
         if (!_unreachable) {
            uint8_t gtype = _mod.globals.at(gi).type.content_type;
            uint32_t dest = _func->alloc_vreg(gtype);
            ir_inst inst{};
            inst.opcode = ir_op::global_get;
            inst.type = gtype;
            inst.flags = IR_SIDE_EFFECT; // must re-read from memory; global may be modified by global_set
            inst.dest = dest;
            inst.local.index = gi;
            inst.local.src1 = ir_vreg_none;
            _func->emit(inst);
            _func->vpush(dest);
            if (gtype == types::v128) {
               _func->has_simd = true;
               uint32_t dest2 = _func->alloc_vreg(gtype);
               _func->vpush(dest2);
            }
         }
      }

      void emit_set_global(uint32_t gi) {
         if (!_unreachable) {
            uint8_t gtype = _mod.globals.at(gi).type.content_type;
            if (gtype == types::v128) _func->vpop(); // extra v128 slot
            uint32_t src = _func->vpop();
            ir_inst inst{};
            inst.opcode = ir_op::global_set;
            inst.type = gtype;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.local.index = gi;
            inst.local.src1 = src;
            _func->emit(inst);
         }
      }

      // ──── Memory loads ────
      // Parser passes (alignment, offset) — we only need offset for IR
      void emit_i32_load(uint32_t /*align*/, uint32_t offset)    { ir_load(ir_op::i32_load, types::i32, offset); }
      void emit_i64_load(uint32_t /*align*/, uint32_t offset)    { ir_load(ir_op::i64_load, types::i64, offset); }
      void emit_f32_load(uint32_t /*align*/, uint32_t offset)    { ir_load(ir_op::f32_load, types::f32, offset); }
      void emit_f64_load(uint32_t /*align*/, uint32_t offset)    { ir_load(ir_op::f64_load, types::f64, offset); }
      void emit_i32_load8_s(uint32_t /*a*/, uint32_t o) { ir_load(ir_op::i32_load8_s, types::i32, o); }
      void emit_i32_load16_s(uint32_t /*a*/, uint32_t o){ ir_load(ir_op::i32_load16_s, types::i32, o); }
      void emit_i32_load8_u(uint32_t /*a*/, uint32_t o) { ir_load(ir_op::i32_load8_u, types::i32, o); }
      void emit_i32_load16_u(uint32_t /*a*/, uint32_t o){ ir_load(ir_op::i32_load16_u, types::i32, o); }
      void emit_i64_load8_s(uint32_t /*a*/, uint32_t o) { ir_load(ir_op::i64_load8_s, types::i64, o); }
      void emit_i64_load16_s(uint32_t /*a*/, uint32_t o){ ir_load(ir_op::i64_load16_s, types::i64, o); }
      void emit_i64_load32_s(uint32_t /*a*/, uint32_t o){ ir_load(ir_op::i64_load32_s, types::i64, o); }
      void emit_i64_load8_u(uint32_t /*a*/, uint32_t o) { ir_load(ir_op::i64_load8_u, types::i64, o); }
      void emit_i64_load16_u(uint32_t /*a*/, uint32_t o){ ir_load(ir_op::i64_load16_u, types::i64, o); }
      void emit_i64_load32_u(uint32_t /*a*/, uint32_t o){ ir_load(ir_op::i64_load32_u, types::i64, o); }

      // ──── Memory stores ────
      void emit_i32_store(uint32_t /*a*/, uint32_t o)   { ir_store(ir_op::i32_store, types::i32, o); }
      void emit_i64_store(uint32_t /*a*/, uint32_t o)   { ir_store(ir_op::i64_store, types::i64, o); }
      void emit_f32_store(uint32_t /*a*/, uint32_t o)   { ir_store(ir_op::f32_store, types::f32, o); }
      void emit_f64_store(uint32_t /*a*/, uint32_t o)   { ir_store(ir_op::f64_store, types::f64, o); }
      void emit_i32_store8(uint32_t /*a*/, uint32_t o)  { ir_store(ir_op::i32_store8, types::i32, o); }
      void emit_i32_store16(uint32_t /*a*/, uint32_t o) { ir_store(ir_op::i32_store16, types::i32, o); }
      void emit_i64_store8(uint32_t /*a*/, uint32_t o)  { ir_store(ir_op::i64_store8, types::i64, o); }
      void emit_i64_store16(uint32_t /*a*/, uint32_t o) { ir_store(ir_op::i64_store16, types::i64, o); }
      void emit_i64_store32(uint32_t /*a*/, uint32_t o) { ir_store(ir_op::i64_store32, types::i64, o); }

      // ──── Memory management ────
      bool is_memory64() const { return !_mod.memories.empty() && _mod.memories[0].is_memory64; }

      void emit_current_memory() {
         if (!_unreachable) {
            uint8_t rtype = is_memory64() ? types::i64 : types::i32;
            uint32_t dest = _func->alloc_vreg(rtype);
            ir_inst inst{};
            inst.opcode = ir_op::memory_size;
            inst.type = rtype;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.rr.src1 = ir_vreg_none;
            inst.rr.src2 = ir_vreg_none;
            _func->emit(inst);
            _func->vpush(dest);
         }
      }
      void emit_grow_memory() {
         if (!_unreachable) {
            uint32_t src = _func->vpop();
            uint8_t rtype = is_memory64() ? types::i64 : types::i32;
            uint32_t dest = _func->alloc_vreg(rtype);
            ir_inst inst{};
            inst.opcode = ir_op::memory_grow;
            inst.type = rtype;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.rr.src1 = src;
            inst.rr.src2 = ir_vreg_none;
            _func->emit(inst);
            _func->vpush(dest);
         }
      }

      // ──── Constants ────
      void emit_i32_const(uint32_t v) {
         if (!_unreachable) {
            uint32_t dest = _func->alloc_vreg(types::i32);
            ir_inst inst{};
            inst.opcode = ir_op::const_i32;
            inst.type = types::i32;
            inst.dest = dest;
            inst.imm64 = static_cast<int64_t>(static_cast<int32_t>(v));
            _func->emit(inst);
            _func->vpush(dest);
         }
      }

      void emit_i64_const(uint64_t v) {
         if (!_unreachable) {
            uint32_t dest = _func->alloc_vreg(types::i64);
            ir_inst inst{};
            inst.opcode = ir_op::const_i64;
            inst.type = types::i64;
            inst.dest = dest;
            inst.imm64 = static_cast<int64_t>(v);
            _func->emit(inst);
            _func->vpush(dest);
         }
      }

      void emit_f32_const(float v) {
         if (!_unreachable) {
            uint32_t dest = _func->alloc_vreg(types::f32);
            ir_inst inst{};
            inst.opcode = ir_op::const_f32;
            inst.type = types::f32;
            inst.dest = dest;
            std::memcpy(&inst.immf32, &v, 4);
            _func->emit(inst);
            _func->vpush(dest);
         }
      }

      void emit_f64_const(double v) {
         if (!_unreachable) {
            uint32_t dest = _func->alloc_vreg(types::f64);
            ir_inst inst{};
            inst.opcode = ir_op::const_f64;
            inst.type = types::f64;
            inst.dest = dest;
            std::memcpy(&inst.immf64, &v, 8);
            _func->emit(inst);
            _func->vpush(dest);
         }
      }

      // ──── Comparisons ────
      void emit_i32_eqz()  { ir_unop(ir_op::i32_eqz, types::i32); }
      void emit_i32_eq()   { ir_binop(ir_op::i32_eq, types::i32); }
      void emit_i32_ne()   { ir_binop(ir_op::i32_ne, types::i32); }
      void emit_i32_lt_s() { ir_binop(ir_op::i32_lt_s, types::i32); }
      void emit_i32_lt_u() { ir_binop(ir_op::i32_lt_u, types::i32); }
      void emit_i32_gt_s() { ir_binop(ir_op::i32_gt_s, types::i32); }
      void emit_i32_gt_u() { ir_binop(ir_op::i32_gt_u, types::i32); }
      void emit_i32_le_s() { ir_binop(ir_op::i32_le_s, types::i32); }
      void emit_i32_le_u() { ir_binop(ir_op::i32_le_u, types::i32); }
      void emit_i32_ge_s() { ir_binop(ir_op::i32_ge_s, types::i32); }
      void emit_i32_ge_u() { ir_binop(ir_op::i32_ge_u, types::i32); }
      void emit_i64_eqz()  { ir_unop(ir_op::i64_eqz, types::i32); }
      void emit_i64_eq()   { ir_binop(ir_op::i64_eq, types::i32); }
      void emit_i64_ne()   { ir_binop(ir_op::i64_ne, types::i32); }
      void emit_i64_lt_s() { ir_binop(ir_op::i64_lt_s, types::i32); }
      void emit_i64_lt_u() { ir_binop(ir_op::i64_lt_u, types::i32); }
      void emit_i64_gt_s() { ir_binop(ir_op::i64_gt_s, types::i32); }
      void emit_i64_gt_u() { ir_binop(ir_op::i64_gt_u, types::i32); }
      void emit_i64_le_s() { ir_binop(ir_op::i64_le_s, types::i32); }
      void emit_i64_le_u() { ir_binop(ir_op::i64_le_u, types::i32); }
      void emit_i64_ge_s() { ir_binop(ir_op::i64_ge_s, types::i32); }
      void emit_i64_ge_u() { ir_binop(ir_op::i64_ge_u, types::i32); }
      void emit_f32_eq()   { ir_binop(ir_op::f32_eq, types::i32); }
      void emit_f32_ne()   { ir_binop(ir_op::f32_ne, types::i32); }
      void emit_f32_lt()   { ir_binop(ir_op::f32_lt, types::i32); }
      void emit_f32_gt()   { ir_binop(ir_op::f32_gt, types::i32); }
      void emit_f32_le()   { ir_binop(ir_op::f32_le, types::i32); }
      void emit_f32_ge()   { ir_binop(ir_op::f32_ge, types::i32); }
      void emit_f64_eq()   { ir_binop(ir_op::f64_eq, types::i32); }
      void emit_f64_ne()   { ir_binop(ir_op::f64_ne, types::i32); }
      void emit_f64_lt()   { ir_binop(ir_op::f64_lt, types::i32); }
      void emit_f64_gt()   { ir_binop(ir_op::f64_gt, types::i32); }
      void emit_f64_le()   { ir_binop(ir_op::f64_le, types::i32); }
      void emit_f64_ge()   { ir_binop(ir_op::f64_ge, types::i32); }

      // ──── Integer arithmetic ────
      void emit_i32_clz()    { ir_unop(ir_op::i32_clz, types::i32); }
      void emit_i32_ctz()    { ir_unop(ir_op::i32_ctz, types::i32); }
      void emit_i32_popcnt() { ir_unop(ir_op::i32_popcnt, types::i32); }
      void emit_i32_add()    { ir_binop(ir_op::i32_add, types::i32); }
      void emit_i32_sub()    { ir_binop(ir_op::i32_sub, types::i32); }
      void emit_i32_mul()    { ir_binop(ir_op::i32_mul, types::i32); }
      void emit_i32_div_s()  { ir_binop(ir_op::i32_div_s, types::i32); }
      void emit_i32_div_u()  { ir_binop(ir_op::i32_div_u, types::i32); }
      void emit_i32_rem_s()  { ir_binop(ir_op::i32_rem_s, types::i32); }
      void emit_i32_rem_u()  { ir_binop(ir_op::i32_rem_u, types::i32); }
      void emit_i32_and()    { ir_binop(ir_op::i32_and, types::i32); }
      void emit_i32_or()     { ir_binop(ir_op::i32_or, types::i32); }
      void emit_i32_xor()    { ir_binop(ir_op::i32_xor, types::i32); }
      void emit_i32_shl()    { ir_binop(ir_op::i32_shl, types::i32); }
      void emit_i32_shr_s()  { ir_binop(ir_op::i32_shr_s, types::i32); }
      void emit_i32_shr_u()  { ir_binop(ir_op::i32_shr_u, types::i32); }
      void emit_i32_rotl()   { ir_binop(ir_op::i32_rotl, types::i32); }
      void emit_i32_rotr()   { ir_binop(ir_op::i32_rotr, types::i32); }
      void emit_i64_clz()    { ir_unop(ir_op::i64_clz, types::i64); }
      void emit_i64_ctz()    { ir_unop(ir_op::i64_ctz, types::i64); }
      void emit_i64_popcnt() { ir_unop(ir_op::i64_popcnt, types::i64); }
      void emit_i64_add()    { ir_binop(ir_op::i64_add, types::i64); }
      void emit_i64_sub()    { ir_binop(ir_op::i64_sub, types::i64); }
      void emit_i64_mul()    { ir_binop(ir_op::i64_mul, types::i64); }
      void emit_i64_div_s()  { ir_binop(ir_op::i64_div_s, types::i64); }
      void emit_i64_div_u()  { ir_binop(ir_op::i64_div_u, types::i64); }
      void emit_i64_rem_s()  { ir_binop(ir_op::i64_rem_s, types::i64); }
      void emit_i64_rem_u()  { ir_binop(ir_op::i64_rem_u, types::i64); }
      void emit_i64_and()    { ir_binop(ir_op::i64_and, types::i64); }
      void emit_i64_or()     { ir_binop(ir_op::i64_or, types::i64); }
      void emit_i64_xor()    { ir_binop(ir_op::i64_xor, types::i64); }
      void emit_i64_shl()    { ir_binop(ir_op::i64_shl, types::i64); }
      void emit_i64_shr_s()  { ir_binop(ir_op::i64_shr_s, types::i64); }
      void emit_i64_shr_u()  { ir_binop(ir_op::i64_shr_u, types::i64); }
      void emit_i64_rotl()   { ir_binop(ir_op::i64_rotl, types::i64); }
      void emit_i64_rotr()   { ir_binop(ir_op::i64_rotr, types::i64); }

      // ──── Float arithmetic ────
      void emit_f32_abs()      { ir_unop(ir_op::f32_abs, types::f32); }
      void emit_f32_neg()      { ir_unop(ir_op::f32_neg, types::f32); }
      void emit_f32_ceil()     { ir_unop(ir_op::f32_ceil, types::f32); }
      void emit_f32_floor()    { ir_unop(ir_op::f32_floor, types::f32); }
      void emit_f32_trunc()    { ir_unop(ir_op::f32_trunc, types::f32); }
      void emit_f32_nearest()  { ir_unop(ir_op::f32_nearest, types::f32); }
      void emit_f32_sqrt()     { ir_unop(ir_op::f32_sqrt, types::f32); }
      void emit_f32_add()      { ir_binop(ir_op::f32_add, types::f32); }
      void emit_f32_sub()      { ir_binop(ir_op::f32_sub, types::f32); }
      void emit_f32_mul()      { ir_binop(ir_op::f32_mul, types::f32); }
      void emit_f32_div()      { ir_binop(ir_op::f32_div, types::f32); }
      void emit_f32_min()      { ir_binop(ir_op::f32_min, types::f32); }
      void emit_f32_max()      { ir_binop(ir_op::f32_max, types::f32); }
      void emit_f32_copysign() { ir_binop(ir_op::f32_copysign, types::f32); }
      void emit_f64_abs()      { ir_unop(ir_op::f64_abs, types::f64); }
      void emit_f64_neg()      { ir_unop(ir_op::f64_neg, types::f64); }
      void emit_f64_ceil()     { ir_unop(ir_op::f64_ceil, types::f64); }
      void emit_f64_floor()    { ir_unop(ir_op::f64_floor, types::f64); }
      void emit_f64_trunc()    { ir_unop(ir_op::f64_trunc, types::f64); }
      void emit_f64_nearest()  { ir_unop(ir_op::f64_nearest, types::f64); }
      void emit_f64_sqrt()     { ir_unop(ir_op::f64_sqrt, types::f64); }
      void emit_f64_add()      { ir_binop(ir_op::f64_add, types::f64); }
      void emit_f64_sub()      { ir_binop(ir_op::f64_sub, types::f64); }
      void emit_f64_mul()      { ir_binop(ir_op::f64_mul, types::f64); }
      void emit_f64_div()      { ir_binop(ir_op::f64_div, types::f64); }
      void emit_f64_min()      { ir_binop(ir_op::f64_min, types::f64); }
      void emit_f64_max()      { ir_binop(ir_op::f64_max, types::f64); }
      void emit_f64_copysign() { ir_binop(ir_op::f64_copysign, types::f64); }

      // ──── Conversions ────
      void emit_i32_wrap_i64()       { ir_unop(ir_op::i32_wrap_i64, types::i32); }
      void emit_i32_trunc_s_f32()    { ir_unop(ir_op::i32_trunc_s_f32, types::i32); }
      void emit_i32_trunc_u_f32()    { ir_unop(ir_op::i32_trunc_u_f32, types::i32); }
      void emit_i32_trunc_s_f64()    { ir_unop(ir_op::i32_trunc_s_f64, types::i32); }
      void emit_i32_trunc_u_f64()    { ir_unop(ir_op::i32_trunc_u_f64, types::i32); }
      void emit_i64_extend_s_i32()   { ir_unop(ir_op::i64_extend_s_i32, types::i64); }
      void emit_i64_extend_u_i32()   { ir_unop(ir_op::i64_extend_u_i32, types::i64); }
      void emit_i64_trunc_s_f32()    { ir_unop(ir_op::i64_trunc_s_f32, types::i64); }
      void emit_i64_trunc_u_f32()    { ir_unop(ir_op::i64_trunc_u_f32, types::i64); }
      void emit_i64_trunc_s_f64()    { ir_unop(ir_op::i64_trunc_s_f64, types::i64); }
      void emit_i64_trunc_u_f64()    { ir_unop(ir_op::i64_trunc_u_f64, types::i64); }
      void emit_f32_convert_s_i32()  { ir_unop(ir_op::f32_convert_s_i32, types::f32); }
      void emit_f32_convert_u_i32()  { ir_unop(ir_op::f32_convert_u_i32, types::f32); }
      void emit_f32_convert_s_i64()  { ir_unop(ir_op::f32_convert_s_i64, types::f32); }
      void emit_f32_convert_u_i64()  { ir_unop(ir_op::f32_convert_u_i64, types::f32); }
      void emit_f32_demote_f64()     { ir_unop(ir_op::f32_demote_f64, types::f32); }
      void emit_f64_convert_s_i32()  { ir_unop(ir_op::f64_convert_s_i32, types::f64); }
      void emit_f64_convert_u_i32()  { ir_unop(ir_op::f64_convert_u_i32, types::f64); }
      void emit_f64_convert_s_i64()  { ir_unop(ir_op::f64_convert_s_i64, types::f64); }
      void emit_f64_convert_u_i64()  { ir_unop(ir_op::f64_convert_u_i64, types::f64); }
      void emit_f64_promote_f32()    { ir_unop(ir_op::f64_promote_f32, types::f64); }
      void emit_i32_reinterpret_f32(){ ir_unop(ir_op::i32_reinterpret_f32, types::i32); }
      void emit_i64_reinterpret_f64(){ ir_unop(ir_op::i64_reinterpret_f64, types::i64); }
      void emit_f32_reinterpret_i32(){ ir_unop(ir_op::f32_reinterpret_i32, types::f32); }
      void emit_f64_reinterpret_i64(){ ir_unop(ir_op::f64_reinterpret_i64, types::f64); }
      void emit_i32_trunc_sat_f32_s(){ ir_unop(ir_op::i32_trunc_sat_f32_s, types::i32); }
      void emit_i32_trunc_sat_f32_u(){ ir_unop(ir_op::i32_trunc_sat_f32_u, types::i32); }
      void emit_i32_trunc_sat_f64_s(){ ir_unop(ir_op::i32_trunc_sat_f64_s, types::i32); }
      void emit_i32_trunc_sat_f64_u(){ ir_unop(ir_op::i32_trunc_sat_f64_u, types::i32); }
      void emit_i64_trunc_sat_f32_s(){ ir_unop(ir_op::i64_trunc_sat_f32_s, types::i64); }
      void emit_i64_trunc_sat_f32_u(){ ir_unop(ir_op::i64_trunc_sat_f32_u, types::i64); }
      void emit_i64_trunc_sat_f64_s(){ ir_unop(ir_op::i64_trunc_sat_f64_s, types::i64); }
      void emit_i64_trunc_sat_f64_u(){ ir_unop(ir_op::i64_trunc_sat_f64_u, types::i64); }
      void emit_i32_extend8_s()      { ir_unop(ir_op::i32_extend8_s, types::i32); }
      void emit_i32_extend16_s()     { ir_unop(ir_op::i32_extend16_s, types::i32); }
      void emit_i64_extend8_s()      { ir_unop(ir_op::i64_extend8_s, types::i64); }
      void emit_i64_extend16_s()     { ir_unop(ir_op::i64_extend16_s, types::i64); }
      void emit_i64_extend32_s()     { ir_unop(ir_op::i64_extend32_s, types::i64); }

      // ──── SIMD (vstack tracking + v128_op IR emission) ────
      void emit_v128_load(uint32_t /*align*/, uint32_t offset)  { ir_simd_load(simd_sub::v128_load, offset); }
      void emit_v128_load8x8_s(uint32_t /*align*/, uint32_t offset)  { ir_simd_load(simd_sub::v128_load8x8_s, offset); }
      void emit_v128_load8x8_u(uint32_t /*align*/, uint32_t offset)  { ir_simd_load(simd_sub::v128_load8x8_u, offset); }
      void emit_v128_load16x4_s(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load16x4_s, offset); }
      void emit_v128_load16x4_u(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load16x4_u, offset); }
      void emit_v128_load32x2_s(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load32x2_s, offset); }
      void emit_v128_load32x2_u(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load32x2_u, offset); }
      void emit_v128_load8_splat(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load8_splat, offset); }
      void emit_v128_load16_splat(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load16_splat, offset); }
      void emit_v128_load32_splat(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load32_splat, offset); }
      void emit_v128_load64_splat(uint32_t /*align*/, uint32_t offset) { ir_simd_load(simd_sub::v128_load64_splat, offset); }
      void emit_v128_load32_zero(uint32_t /*align*/, uint32_t offset)  { ir_simd_load(simd_sub::v128_load32_zero, offset); }
      void emit_v128_load64_zero(uint32_t /*align*/, uint32_t offset)  { ir_simd_load(simd_sub::v128_load64_zero, offset); }
      void emit_v128_store(uint32_t /*align*/, uint32_t offset) { ir_simd_store(simd_sub::v128_store, offset); }
      void emit_v128_load8_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane)  { ir_simd_load_lane(simd_sub::v128_load8_lane, offset, lane); }
      void emit_v128_load16_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane) { ir_simd_load_lane(simd_sub::v128_load16_lane, offset, lane); }
      void emit_v128_load32_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane) { ir_simd_load_lane(simd_sub::v128_load32_lane, offset, lane); }
      void emit_v128_load64_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane) { ir_simd_load_lane(simd_sub::v128_load64_lane, offset, lane); }
      void emit_v128_store8_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane)  { ir_simd_store_lane(simd_sub::v128_store8_lane, offset, lane); }
      void emit_v128_store16_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane) { ir_simd_store_lane(simd_sub::v128_store16_lane, offset, lane); }
      void emit_v128_store32_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane) { ir_simd_store_lane(simd_sub::v128_store32_lane, offset, lane); }
      void emit_v128_store64_lane(uint32_t /*align*/, uint32_t offset, uint8_t lane) { ir_simd_store_lane(simd_sub::v128_store64_lane, offset, lane); }

      void emit_v128_const(v128_t v) {
         if (!_unreachable) {
            uint32_t dest = _func->alloc_vreg(types::v128);
            uint32_t dest2 = _func->alloc_vreg(types::v128);
            ir_inst inst{};
            inst.opcode = ir_op::const_v128;
            inst.type = types::v128;
            inst.flags = IR_NONE;
            inst.dest = dest; // v128 dest vreg for XMM register allocation
            inst.immv128 = v;
            _func->emit(inst);
            _func->vpush(dest);
            _func->vpush(dest2);
         }
      }
      void emit_i8x16_shuffle(const uint8_t* l) {
         if (!_unreachable) {
            // Pop two v128 operands (4 vregs): src2 (TOS), then src1
            _func->vpop(); uint32_t s2 = _func->vpop();
            _func->vpop(); uint32_t s1 = _func->vpop();
            // Emit arg instructions to push v128 sources from XMM/spill to x86 stack.
            // shuffle's immv128 overlaps the simd struct, so we can't store vregs there.
            // Push src1 first (NOS), then src2 (TOS) — matching stack-based layout.
            auto emit_v128_arg = [&](uint32_t vreg) {
               ir_inst arg{};
               arg.opcode = ir_op::arg;
               arg.type = types::v128;
               arg.flags = IR_NONE;
               arg.rr.src1 = vreg;
               arg.dest = ir_vreg_none;
               _func->emit(arg);
            };
            emit_v128_arg(s1);
            emit_v128_arg(s2);
            // Emit v128_op with shuffle sub-opcode, lanes stored in immv128
            uint32_t d1 = _func->alloc_vreg(types::v128);
            uint32_t d2 = _func->alloc_vreg(types::v128);
            ir_inst inst{};
            inst.opcode = ir_op::v128_op;
            inst.type = types::v128;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = static_cast<uint32_t>(simd_sub::i8x16_shuffle);
            std::memcpy(&inst.immv128, l, 16);
            _func->emit(inst);
            // Emit a nop with dest=d1 so regalloc creates a v128 interval for the
            // shuffle result (the shuffle instruction itself can't store v_dest
            // because immv128 overlaps the simd struct).
            {
               ir_inst marker{};
               marker.opcode = ir_op::nop;
               marker.type = types::v128;
               marker.flags = IR_NONE;
               marker.dest = d1;
               _func->emit(marker);
            }
            _func->vpush(d1); _func->vpush(d2);
         }
      }

// SIMD ops: track vstack (2 slots per v128) AND emit v128_op IR instructions.
// The sub-opcode is derived from the function name suffix matching simd_sub enum.
#define SIMD_UNOP(name)  void name() { ir_simd_unop(name##_sub); }
#define SIMD_BINOP(name) void name() { ir_simd_binop(name##_sub); }
#define SIMD_SHIFT(name) void name() { ir_simd_shift(name##_sub); }
#define SIMD_EXTRACT(name) void name(uint8_t a) { ir_simd_extract(name##_sub, a); }
#define SIMD_REPLACE(name) void name(uint8_t a) { ir_simd_replace(name##_sub, a); }
#define SIMD_SPLAT(name)   void name() { ir_simd_splat(name##_sub); }
#define SIMD_RELOP(name)   void name() { ir_simd_binop(name##_sub); }

// Map emit_xxx function names to simd_sub::xxx enum values.
// The _sub suffix is appended by the macro to form the enum name.
// We define bridging constants to handle the emit_ prefix stripping.
      static constexpr auto emit_i8x16_extract_lane_s_sub = simd_sub::i8x16_extract_lane_s;
      static constexpr auto emit_i8x16_extract_lane_u_sub = simd_sub::i8x16_extract_lane_u;
      static constexpr auto emit_i8x16_replace_lane_sub = simd_sub::i8x16_replace_lane;
      static constexpr auto emit_i16x8_extract_lane_s_sub = simd_sub::i16x8_extract_lane_s;
      static constexpr auto emit_i16x8_extract_lane_u_sub = simd_sub::i16x8_extract_lane_u;
      static constexpr auto emit_i16x8_replace_lane_sub = simd_sub::i16x8_replace_lane;
      static constexpr auto emit_i32x4_extract_lane_sub = simd_sub::i32x4_extract_lane;
      static constexpr auto emit_i32x4_replace_lane_sub = simd_sub::i32x4_replace_lane;
      static constexpr auto emit_i64x2_extract_lane_sub = simd_sub::i64x2_extract_lane;
      static constexpr auto emit_i64x2_replace_lane_sub = simd_sub::i64x2_replace_lane;
      static constexpr auto emit_f32x4_extract_lane_sub = simd_sub::f32x4_extract_lane;
      static constexpr auto emit_f32x4_replace_lane_sub = simd_sub::f32x4_replace_lane;
      static constexpr auto emit_f64x2_extract_lane_sub = simd_sub::f64x2_extract_lane;
      static constexpr auto emit_f64x2_replace_lane_sub = simd_sub::f64x2_replace_lane;
      static constexpr auto emit_i8x16_swizzle_sub = simd_sub::i8x16_swizzle;
      static constexpr auto emit_i8x16_splat_sub = simd_sub::i8x16_splat;
      static constexpr auto emit_i16x8_splat_sub = simd_sub::i16x8_splat;
      static constexpr auto emit_i32x4_splat_sub = simd_sub::i32x4_splat;
      static constexpr auto emit_i64x2_splat_sub = simd_sub::i64x2_splat;
      static constexpr auto emit_f32x4_splat_sub = simd_sub::f32x4_splat;
      static constexpr auto emit_f64x2_splat_sub = simd_sub::f64x2_splat;
      static constexpr auto emit_i8x16_eq_sub = simd_sub::i8x16_eq;
      static constexpr auto emit_i8x16_ne_sub = simd_sub::i8x16_ne;
      static constexpr auto emit_i8x16_lt_s_sub = simd_sub::i8x16_lt_s;
      static constexpr auto emit_i8x16_lt_u_sub = simd_sub::i8x16_lt_u;
      static constexpr auto emit_i8x16_gt_s_sub = simd_sub::i8x16_gt_s;
      static constexpr auto emit_i8x16_gt_u_sub = simd_sub::i8x16_gt_u;
      static constexpr auto emit_i8x16_le_s_sub = simd_sub::i8x16_le_s;
      static constexpr auto emit_i8x16_le_u_sub = simd_sub::i8x16_le_u;
      static constexpr auto emit_i8x16_ge_s_sub = simd_sub::i8x16_ge_s;
      static constexpr auto emit_i8x16_ge_u_sub = simd_sub::i8x16_ge_u;
      static constexpr auto emit_i16x8_eq_sub = simd_sub::i16x8_eq;
      static constexpr auto emit_i16x8_ne_sub = simd_sub::i16x8_ne;
      static constexpr auto emit_i16x8_lt_s_sub = simd_sub::i16x8_lt_s;
      static constexpr auto emit_i16x8_lt_u_sub = simd_sub::i16x8_lt_u;
      static constexpr auto emit_i16x8_gt_s_sub = simd_sub::i16x8_gt_s;
      static constexpr auto emit_i16x8_gt_u_sub = simd_sub::i16x8_gt_u;
      static constexpr auto emit_i16x8_le_s_sub = simd_sub::i16x8_le_s;
      static constexpr auto emit_i16x8_le_u_sub = simd_sub::i16x8_le_u;
      static constexpr auto emit_i16x8_ge_s_sub = simd_sub::i16x8_ge_s;
      static constexpr auto emit_i16x8_ge_u_sub = simd_sub::i16x8_ge_u;
      static constexpr auto emit_i32x4_eq_sub = simd_sub::i32x4_eq;
      static constexpr auto emit_i32x4_ne_sub = simd_sub::i32x4_ne;
      static constexpr auto emit_i32x4_lt_s_sub = simd_sub::i32x4_lt_s;
      static constexpr auto emit_i32x4_lt_u_sub = simd_sub::i32x4_lt_u;
      static constexpr auto emit_i32x4_gt_s_sub = simd_sub::i32x4_gt_s;
      static constexpr auto emit_i32x4_gt_u_sub = simd_sub::i32x4_gt_u;
      static constexpr auto emit_i32x4_le_s_sub = simd_sub::i32x4_le_s;
      static constexpr auto emit_i32x4_le_u_sub = simd_sub::i32x4_le_u;
      static constexpr auto emit_i32x4_ge_s_sub = simd_sub::i32x4_ge_s;
      static constexpr auto emit_i32x4_ge_u_sub = simd_sub::i32x4_ge_u;
      static constexpr auto emit_i64x2_eq_sub = simd_sub::i64x2_eq;
      static constexpr auto emit_i64x2_ne_sub = simd_sub::i64x2_ne;
      static constexpr auto emit_i64x2_lt_s_sub = simd_sub::i64x2_lt_s;
      static constexpr auto emit_i64x2_gt_s_sub = simd_sub::i64x2_gt_s;
      static constexpr auto emit_i64x2_le_s_sub = simd_sub::i64x2_le_s;
      static constexpr auto emit_i64x2_ge_s_sub = simd_sub::i64x2_ge_s;
      static constexpr auto emit_f32x4_eq_sub = simd_sub::f32x4_eq;
      static constexpr auto emit_f32x4_ne_sub = simd_sub::f32x4_ne;
      static constexpr auto emit_f32x4_lt_sub = simd_sub::f32x4_lt;
      static constexpr auto emit_f32x4_gt_sub = simd_sub::f32x4_gt;
      static constexpr auto emit_f32x4_le_sub = simd_sub::f32x4_le;
      static constexpr auto emit_f32x4_ge_sub = simd_sub::f32x4_ge;
      static constexpr auto emit_f64x2_eq_sub = simd_sub::f64x2_eq;
      static constexpr auto emit_f64x2_ne_sub = simd_sub::f64x2_ne;
      static constexpr auto emit_f64x2_lt_sub = simd_sub::f64x2_lt;
      static constexpr auto emit_f64x2_gt_sub = simd_sub::f64x2_gt;
      static constexpr auto emit_f64x2_le_sub = simd_sub::f64x2_le;
      static constexpr auto emit_f64x2_ge_sub = simd_sub::f64x2_ge;
      static constexpr auto emit_v128_not_sub = simd_sub::v128_not;
      static constexpr auto emit_v128_and_sub = simd_sub::v128_and;
      static constexpr auto emit_v128_andnot_sub = simd_sub::v128_andnot;
      static constexpr auto emit_v128_or_sub = simd_sub::v128_or;
      static constexpr auto emit_v128_xor_sub = simd_sub::v128_xor;
      static constexpr auto emit_i8x16_abs_sub = simd_sub::i8x16_abs;
      static constexpr auto emit_i8x16_neg_sub = simd_sub::i8x16_neg;
      static constexpr auto emit_i8x16_popcnt_sub = simd_sub::i8x16_popcnt;
      static constexpr auto emit_i8x16_narrow_i16x8_s_sub = simd_sub::i8x16_narrow_i16x8_s;
      static constexpr auto emit_i8x16_narrow_i16x8_u_sub = simd_sub::i8x16_narrow_i16x8_u;
      static constexpr auto emit_i8x16_shl_sub = simd_sub::i8x16_shl;
      static constexpr auto emit_i8x16_shr_s_sub = simd_sub::i8x16_shr_s;
      static constexpr auto emit_i8x16_shr_u_sub = simd_sub::i8x16_shr_u;
      static constexpr auto emit_i8x16_add_sub = simd_sub::i8x16_add;
      static constexpr auto emit_i8x16_add_sat_s_sub = simd_sub::i8x16_add_sat_s;
      static constexpr auto emit_i8x16_add_sat_u_sub = simd_sub::i8x16_add_sat_u;
      static constexpr auto emit_i8x16_sub_sub = simd_sub::i8x16_sub;
      static constexpr auto emit_i8x16_sub_sat_s_sub = simd_sub::i8x16_sub_sat_s;
      static constexpr auto emit_i8x16_sub_sat_u_sub = simd_sub::i8x16_sub_sat_u;
      static constexpr auto emit_i8x16_min_s_sub = simd_sub::i8x16_min_s;
      static constexpr auto emit_i8x16_min_u_sub = simd_sub::i8x16_min_u;
      static constexpr auto emit_i8x16_max_s_sub = simd_sub::i8x16_max_s;
      static constexpr auto emit_i8x16_max_u_sub = simd_sub::i8x16_max_u;
      static constexpr auto emit_i8x16_avgr_u_sub = simd_sub::i8x16_avgr_u;
      static constexpr auto emit_i16x8_extadd_pairwise_i8x16_s_sub = simd_sub::i16x8_extadd_pairwise_i8x16_s;
      static constexpr auto emit_i16x8_extadd_pairwise_i8x16_u_sub = simd_sub::i16x8_extadd_pairwise_i8x16_u;
      static constexpr auto emit_i16x8_abs_sub = simd_sub::i16x8_abs;
      static constexpr auto emit_i16x8_neg_sub = simd_sub::i16x8_neg;
      static constexpr auto emit_i16x8_q15mulr_sat_s_sub = simd_sub::i16x8_q15mulr_sat_s;
      static constexpr auto emit_i16x8_narrow_i32x4_s_sub = simd_sub::i16x8_narrow_i32x4_s;
      static constexpr auto emit_i16x8_narrow_i32x4_u_sub = simd_sub::i16x8_narrow_i32x4_u;
      static constexpr auto emit_i16x8_extend_low_i8x16_s_sub = simd_sub::i16x8_extend_low_i8x16_s;
      static constexpr auto emit_i16x8_extend_high_i8x16_s_sub = simd_sub::i16x8_extend_high_i8x16_s;
      static constexpr auto emit_i16x8_extend_low_i8x16_u_sub = simd_sub::i16x8_extend_low_i8x16_u;
      static constexpr auto emit_i16x8_extend_high_i8x16_u_sub = simd_sub::i16x8_extend_high_i8x16_u;
      static constexpr auto emit_i16x8_shl_sub = simd_sub::i16x8_shl;
      static constexpr auto emit_i16x8_shr_s_sub = simd_sub::i16x8_shr_s;
      static constexpr auto emit_i16x8_shr_u_sub = simd_sub::i16x8_shr_u;
      static constexpr auto emit_i16x8_add_sub = simd_sub::i16x8_add;
      static constexpr auto emit_i16x8_add_sat_s_sub = simd_sub::i16x8_add_sat_s;
      static constexpr auto emit_i16x8_add_sat_u_sub = simd_sub::i16x8_add_sat_u;
      static constexpr auto emit_i16x8_sub_sub = simd_sub::i16x8_sub;
      static constexpr auto emit_i16x8_sub_sat_s_sub = simd_sub::i16x8_sub_sat_s;
      static constexpr auto emit_i16x8_sub_sat_u_sub = simd_sub::i16x8_sub_sat_u;
      static constexpr auto emit_i16x8_mul_sub = simd_sub::i16x8_mul;
      static constexpr auto emit_i16x8_min_s_sub = simd_sub::i16x8_min_s;
      static constexpr auto emit_i16x8_min_u_sub = simd_sub::i16x8_min_u;
      static constexpr auto emit_i16x8_max_s_sub = simd_sub::i16x8_max_s;
      static constexpr auto emit_i16x8_max_u_sub = simd_sub::i16x8_max_u;
      static constexpr auto emit_i16x8_avgr_u_sub = simd_sub::i16x8_avgr_u;
      static constexpr auto emit_i16x8_extmul_low_i8x16_s_sub = simd_sub::i16x8_extmul_low_i8x16_s;
      static constexpr auto emit_i16x8_extmul_high_i8x16_s_sub = simd_sub::i16x8_extmul_high_i8x16_s;
      static constexpr auto emit_i16x8_extmul_low_i8x16_u_sub = simd_sub::i16x8_extmul_low_i8x16_u;
      static constexpr auto emit_i16x8_extmul_high_i8x16_u_sub = simd_sub::i16x8_extmul_high_i8x16_u;
      static constexpr auto emit_i32x4_extadd_pairwise_i16x8_s_sub = simd_sub::i32x4_extadd_pairwise_i16x8_s;
      static constexpr auto emit_i32x4_extadd_pairwise_i16x8_u_sub = simd_sub::i32x4_extadd_pairwise_i16x8_u;
      static constexpr auto emit_i32x4_abs_sub = simd_sub::i32x4_abs;
      static constexpr auto emit_i32x4_neg_sub = simd_sub::i32x4_neg;
      static constexpr auto emit_i32x4_extend_low_i16x8_s_sub = simd_sub::i32x4_extend_low_i16x8_s;
      static constexpr auto emit_i32x4_extend_high_i16x8_s_sub = simd_sub::i32x4_extend_high_i16x8_s;
      static constexpr auto emit_i32x4_extend_low_i16x8_u_sub = simd_sub::i32x4_extend_low_i16x8_u;
      static constexpr auto emit_i32x4_extend_high_i16x8_u_sub = simd_sub::i32x4_extend_high_i16x8_u;
      static constexpr auto emit_i32x4_shl_sub = simd_sub::i32x4_shl;
      static constexpr auto emit_i32x4_shr_s_sub = simd_sub::i32x4_shr_s;
      static constexpr auto emit_i32x4_shr_u_sub = simd_sub::i32x4_shr_u;
      static constexpr auto emit_i32x4_add_sub = simd_sub::i32x4_add;
      static constexpr auto emit_i32x4_sub_sub = simd_sub::i32x4_sub;
      static constexpr auto emit_i32x4_mul_sub = simd_sub::i32x4_mul;
      static constexpr auto emit_i32x4_min_s_sub = simd_sub::i32x4_min_s;
      static constexpr auto emit_i32x4_min_u_sub = simd_sub::i32x4_min_u;
      static constexpr auto emit_i32x4_max_s_sub = simd_sub::i32x4_max_s;
      static constexpr auto emit_i32x4_max_u_sub = simd_sub::i32x4_max_u;
      static constexpr auto emit_i32x4_dot_i16x8_s_sub = simd_sub::i32x4_dot_i16x8_s;
      static constexpr auto emit_i32x4_extmul_low_i16x8_s_sub = simd_sub::i32x4_extmul_low_i16x8_s;
      static constexpr auto emit_i32x4_extmul_high_i16x8_s_sub = simd_sub::i32x4_extmul_high_i16x8_s;
      static constexpr auto emit_i32x4_extmul_low_i16x8_u_sub = simd_sub::i32x4_extmul_low_i16x8_u;
      static constexpr auto emit_i32x4_extmul_high_i16x8_u_sub = simd_sub::i32x4_extmul_high_i16x8_u;
      static constexpr auto emit_i64x2_abs_sub = simd_sub::i64x2_abs;
      static constexpr auto emit_i64x2_neg_sub = simd_sub::i64x2_neg;
      static constexpr auto emit_i64x2_extend_low_i32x4_s_sub = simd_sub::i64x2_extend_low_i32x4_s;
      static constexpr auto emit_i64x2_extend_high_i32x4_s_sub = simd_sub::i64x2_extend_high_i32x4_s;
      static constexpr auto emit_i64x2_extend_low_i32x4_u_sub = simd_sub::i64x2_extend_low_i32x4_u;
      static constexpr auto emit_i64x2_extend_high_i32x4_u_sub = simd_sub::i64x2_extend_high_i32x4_u;
      static constexpr auto emit_i64x2_shl_sub = simd_sub::i64x2_shl;
      static constexpr auto emit_i64x2_shr_s_sub = simd_sub::i64x2_shr_s;
      static constexpr auto emit_i64x2_shr_u_sub = simd_sub::i64x2_shr_u;
      static constexpr auto emit_i64x2_add_sub = simd_sub::i64x2_add;
      static constexpr auto emit_i64x2_sub_sub = simd_sub::i64x2_sub;
      static constexpr auto emit_i64x2_mul_sub = simd_sub::i64x2_mul;
      static constexpr auto emit_i64x2_extmul_low_i32x4_s_sub = simd_sub::i64x2_extmul_low_i32x4_s;
      static constexpr auto emit_i64x2_extmul_high_i32x4_s_sub = simd_sub::i64x2_extmul_high_i32x4_s;
      static constexpr auto emit_i64x2_extmul_low_i32x4_u_sub = simd_sub::i64x2_extmul_low_i32x4_u;
      static constexpr auto emit_i64x2_extmul_high_i32x4_u_sub = simd_sub::i64x2_extmul_high_i32x4_u;
      static constexpr auto emit_f32x4_ceil_sub = simd_sub::f32x4_ceil;
      static constexpr auto emit_f32x4_floor_sub = simd_sub::f32x4_floor;
      static constexpr auto emit_f32x4_trunc_sub = simd_sub::f32x4_trunc;
      static constexpr auto emit_f32x4_nearest_sub = simd_sub::f32x4_nearest;
      static constexpr auto emit_f32x4_abs_sub = simd_sub::f32x4_abs;
      static constexpr auto emit_f32x4_neg_sub = simd_sub::f32x4_neg;
      static constexpr auto emit_f32x4_sqrt_sub = simd_sub::f32x4_sqrt;
      static constexpr auto emit_f32x4_add_sub = simd_sub::f32x4_add;
      static constexpr auto emit_f32x4_sub_sub = simd_sub::f32x4_sub;
      static constexpr auto emit_f32x4_mul_sub = simd_sub::f32x4_mul;
      static constexpr auto emit_f32x4_div_sub = simd_sub::f32x4_div;
      static constexpr auto emit_f32x4_min_sub = simd_sub::f32x4_min;
      static constexpr auto emit_f32x4_max_sub = simd_sub::f32x4_max;
      static constexpr auto emit_f32x4_pmin_sub = simd_sub::f32x4_pmin;
      static constexpr auto emit_f32x4_pmax_sub = simd_sub::f32x4_pmax;
      static constexpr auto emit_f64x2_ceil_sub = simd_sub::f64x2_ceil;
      static constexpr auto emit_f64x2_floor_sub = simd_sub::f64x2_floor;
      static constexpr auto emit_f64x2_trunc_sub = simd_sub::f64x2_trunc;
      static constexpr auto emit_f64x2_nearest_sub = simd_sub::f64x2_nearest;
      static constexpr auto emit_f64x2_abs_sub = simd_sub::f64x2_abs;
      static constexpr auto emit_f64x2_neg_sub = simd_sub::f64x2_neg;
      static constexpr auto emit_f64x2_sqrt_sub = simd_sub::f64x2_sqrt;
      static constexpr auto emit_f64x2_add_sub = simd_sub::f64x2_add;
      static constexpr auto emit_f64x2_sub_sub = simd_sub::f64x2_sub;
      static constexpr auto emit_f64x2_mul_sub = simd_sub::f64x2_mul;
      static constexpr auto emit_f64x2_div_sub = simd_sub::f64x2_div;
      static constexpr auto emit_f64x2_min_sub = simd_sub::f64x2_min;
      static constexpr auto emit_f64x2_max_sub = simd_sub::f64x2_max;
      static constexpr auto emit_f64x2_pmin_sub = simd_sub::f64x2_pmin;
      static constexpr auto emit_f64x2_pmax_sub = simd_sub::f64x2_pmax;
      static constexpr auto emit_i32x4_trunc_sat_f32x4_s_sub = simd_sub::i32x4_trunc_sat_f32x4_s;
      static constexpr auto emit_i32x4_trunc_sat_f32x4_u_sub = simd_sub::i32x4_trunc_sat_f32x4_u;
      static constexpr auto emit_f32x4_convert_i32x4_s_sub = simd_sub::f32x4_convert_i32x4_s;
      static constexpr auto emit_f32x4_convert_i32x4_u_sub = simd_sub::f32x4_convert_i32x4_u;
      static constexpr auto emit_i32x4_trunc_sat_f64x2_s_zero_sub = simd_sub::i32x4_trunc_sat_f64x2_s_zero;
      static constexpr auto emit_i32x4_trunc_sat_f64x2_u_zero_sub = simd_sub::i32x4_trunc_sat_f64x2_u_zero;
      static constexpr auto emit_f64x2_convert_low_i32x4_s_sub = simd_sub::f64x2_convert_low_i32x4_s;
      static constexpr auto emit_f64x2_convert_low_i32x4_u_sub = simd_sub::f64x2_convert_low_i32x4_u;
      static constexpr auto emit_f32x4_demote_f64x2_zero_sub = simd_sub::f32x4_demote_f64x2_zero;
      static constexpr auto emit_f64x2_promote_low_f32x4_sub = simd_sub::f64x2_promote_low_f32x4;
      // Relaxed SIMD
      static constexpr auto emit_f32x4_relaxed_madd_sub = simd_sub::f32x4_relaxed_madd;
      static constexpr auto emit_f32x4_relaxed_nmadd_sub = simd_sub::f32x4_relaxed_nmadd;
      static constexpr auto emit_f64x2_relaxed_madd_sub = simd_sub::f64x2_relaxed_madd;
      static constexpr auto emit_f64x2_relaxed_nmadd_sub = simd_sub::f64x2_relaxed_nmadd;
      static constexpr auto emit_i16x8_relaxed_dot_i8x16_i7x16_s_sub = simd_sub::i16x8_relaxed_dot_i8x16_i7x16_s;
      static constexpr auto emit_i32x4_relaxed_dot_i8x16_i7x16_add_s_sub = simd_sub::i32x4_relaxed_dot_i8x16_i7x16_add_s;

      SIMD_EXTRACT(emit_i8x16_extract_lane_s) SIMD_EXTRACT(emit_i8x16_extract_lane_u) SIMD_REPLACE(emit_i8x16_replace_lane)
      SIMD_EXTRACT(emit_i16x8_extract_lane_s) SIMD_EXTRACT(emit_i16x8_extract_lane_u) SIMD_REPLACE(emit_i16x8_replace_lane)
      SIMD_EXTRACT(emit_i32x4_extract_lane) SIMD_REPLACE(emit_i32x4_replace_lane)
      SIMD_EXTRACT(emit_i64x2_extract_lane) SIMD_REPLACE(emit_i64x2_replace_lane)
      SIMD_EXTRACT(emit_f32x4_extract_lane) SIMD_REPLACE(emit_f32x4_replace_lane)
      SIMD_EXTRACT(emit_f64x2_extract_lane) SIMD_REPLACE(emit_f64x2_replace_lane)

      SIMD_BINOP(emit_i8x16_swizzle)
      SIMD_SPLAT(emit_i8x16_splat) SIMD_SPLAT(emit_i16x8_splat)
      SIMD_SPLAT(emit_i32x4_splat) SIMD_SPLAT(emit_i64x2_splat)
      SIMD_SPLAT(emit_f32x4_splat) SIMD_SPLAT(emit_f64x2_splat)

      SIMD_RELOP(emit_i8x16_eq) SIMD_RELOP(emit_i8x16_ne) SIMD_RELOP(emit_i8x16_lt_s) SIMD_RELOP(emit_i8x16_lt_u)
      SIMD_RELOP(emit_i8x16_gt_s) SIMD_RELOP(emit_i8x16_gt_u) SIMD_RELOP(emit_i8x16_le_s) SIMD_RELOP(emit_i8x16_le_u)
      SIMD_RELOP(emit_i8x16_ge_s) SIMD_RELOP(emit_i8x16_ge_u)
      SIMD_RELOP(emit_i16x8_eq) SIMD_RELOP(emit_i16x8_ne) SIMD_RELOP(emit_i16x8_lt_s) SIMD_RELOP(emit_i16x8_lt_u)
      SIMD_RELOP(emit_i16x8_gt_s) SIMD_RELOP(emit_i16x8_gt_u) SIMD_RELOP(emit_i16x8_le_s) SIMD_RELOP(emit_i16x8_le_u)
      SIMD_RELOP(emit_i16x8_ge_s) SIMD_RELOP(emit_i16x8_ge_u)
      SIMD_RELOP(emit_i32x4_eq) SIMD_RELOP(emit_i32x4_ne) SIMD_RELOP(emit_i32x4_lt_s) SIMD_RELOP(emit_i32x4_lt_u)
      SIMD_RELOP(emit_i32x4_gt_s) SIMD_RELOP(emit_i32x4_gt_u) SIMD_RELOP(emit_i32x4_le_s) SIMD_RELOP(emit_i32x4_le_u)
      SIMD_RELOP(emit_i32x4_ge_s) SIMD_RELOP(emit_i32x4_ge_u)
      SIMD_RELOP(emit_i64x2_eq) SIMD_RELOP(emit_i64x2_ne) SIMD_RELOP(emit_i64x2_lt_s) SIMD_RELOP(emit_i64x2_gt_s)
      SIMD_RELOP(emit_i64x2_le_s) SIMD_RELOP(emit_i64x2_ge_s)
      SIMD_RELOP(emit_f32x4_eq) SIMD_RELOP(emit_f32x4_ne) SIMD_RELOP(emit_f32x4_lt) SIMD_RELOP(emit_f32x4_gt)
      SIMD_RELOP(emit_f32x4_le) SIMD_RELOP(emit_f32x4_ge)
      SIMD_RELOP(emit_f64x2_eq) SIMD_RELOP(emit_f64x2_ne) SIMD_RELOP(emit_f64x2_lt) SIMD_RELOP(emit_f64x2_gt)
      SIMD_RELOP(emit_f64x2_le) SIMD_RELOP(emit_f64x2_ge)
      SIMD_UNOP(emit_v128_not) SIMD_BINOP(emit_v128_and) SIMD_BINOP(emit_v128_andnot) SIMD_BINOP(emit_v128_or)
      SIMD_BINOP(emit_v128_xor)
      void emit_simd_ternop(simd_sub sub) {
         if (!_unreachable) {
            _func->vpop(); uint32_t c_vreg = _func->vpop(); // third input
            _func->vpop(); uint32_t b_vreg = _func->vpop(); // second input
            _func->vpop(); uint32_t a_vreg = _func->vpop(); // first input
            uint32_t d1 = _func->alloc_vreg(types::v128);
            uint32_t d2 = _func->alloc_vreg(types::v128);
            ir_inst inst{};
            inst.opcode = ir_op::v128_op;
            inst.type = types::v128;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = static_cast<uint32_t>(sub);
            inst.simd.v_src1 = static_cast<uint16_t>(a_vreg);
            inst.simd.v_src2 = static_cast<uint16_t>(b_vreg);
            inst.simd.v_dest = static_cast<uint16_t>(d1);
            inst.simd.addr = c_vreg;
            _func->emit(inst);
            _func->vpush(d1); _func->vpush(d2);
         }
      }
      void emit_v128_bitselect() {
         if (!_unreachable) {
            _func->vpop(); uint32_t mask = _func->vpop(); // mask (low vreg)
            _func->vpop(); uint32_t val2 = _func->vpop(); // val2 (low vreg)
            _func->vpop(); uint32_t val1 = _func->vpop(); // val1 (low vreg)
            uint32_t d1 = _func->alloc_vreg(types::v128);
            uint32_t d2 = _func->alloc_vreg(types::v128);
            ir_inst inst{};
            inst.opcode = ir_op::v128_op;
            inst.type = types::v128;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = static_cast<uint32_t>(simd_sub::v128_bitselect);
            inst.simd.v_src1 = static_cast<uint16_t>(val1);
            inst.simd.v_src2 = static_cast<uint16_t>(val2);
            inst.simd.v_dest = static_cast<uint16_t>(d1);
            // Store mask vreg in addr field (unused for bitselect's memory addressing)
            inst.simd.addr = mask;
            _func->emit(inst);
            _func->vpush(d1); _func->vpush(d2);
         }
      }
      void emit_v128_any_true() {
         if (!_unreachable) {
            _func->has_simd = true;
            _func->vpop(); uint32_t s1 = _func->vpop();  // v128 (low vreg)
            uint32_t d = _func->alloc_vreg(types::i32);
            // Store result vreg in addr field so register path can pop scalar to vreg
            ir_simd_emit_with_offset(simd_sub::v128_any_true, 0, d, static_cast<uint16_t>(s1));
            _func->vpush(d);
         }
      }
      SIMD_UNOP(emit_i8x16_abs) SIMD_UNOP(emit_i8x16_neg) SIMD_UNOP(emit_i8x16_popcnt)
      void emit_i8x16_all_true() { ir_simd_test(simd_sub::i8x16_all_true); }
      void emit_i8x16_bitmask() { ir_simd_test(simd_sub::i8x16_bitmask); }
      SIMD_BINOP(emit_i8x16_narrow_i16x8_s) SIMD_BINOP(emit_i8x16_narrow_i16x8_u)
      SIMD_SHIFT(emit_i8x16_shl) SIMD_SHIFT(emit_i8x16_shr_s) SIMD_SHIFT(emit_i8x16_shr_u)
      SIMD_BINOP(emit_i8x16_add) SIMD_BINOP(emit_i8x16_add_sat_s) SIMD_BINOP(emit_i8x16_add_sat_u)
      SIMD_BINOP(emit_i8x16_sub) SIMD_BINOP(emit_i8x16_sub_sat_s) SIMD_BINOP(emit_i8x16_sub_sat_u)
      SIMD_BINOP(emit_i8x16_min_s) SIMD_BINOP(emit_i8x16_min_u) SIMD_BINOP(emit_i8x16_max_s)
      SIMD_BINOP(emit_i8x16_max_u) SIMD_BINOP(emit_i8x16_avgr_u)
      SIMD_UNOP(emit_i16x8_extadd_pairwise_i8x16_s) SIMD_UNOP(emit_i16x8_extadd_pairwise_i8x16_u)
      SIMD_UNOP(emit_i16x8_abs) SIMD_UNOP(emit_i16x8_neg) SIMD_BINOP(emit_i16x8_q15mulr_sat_s)
      void emit_i16x8_all_true() { ir_simd_test(simd_sub::i16x8_all_true); }
      void emit_i16x8_bitmask() { ir_simd_test(simd_sub::i16x8_bitmask); }
      SIMD_BINOP(emit_i16x8_narrow_i32x4_s) SIMD_BINOP(emit_i16x8_narrow_i32x4_u)
      SIMD_UNOP(emit_i16x8_extend_low_i8x16_s) SIMD_UNOP(emit_i16x8_extend_high_i8x16_s)
      SIMD_UNOP(emit_i16x8_extend_low_i8x16_u) SIMD_UNOP(emit_i16x8_extend_high_i8x16_u)
      SIMD_SHIFT(emit_i16x8_shl) SIMD_SHIFT(emit_i16x8_shr_s) SIMD_SHIFT(emit_i16x8_shr_u)
      SIMD_BINOP(emit_i16x8_add) SIMD_BINOP(emit_i16x8_add_sat_s) SIMD_BINOP(emit_i16x8_add_sat_u)
      SIMD_BINOP(emit_i16x8_sub) SIMD_BINOP(emit_i16x8_sub_sat_s) SIMD_BINOP(emit_i16x8_sub_sat_u)
      SIMD_BINOP(emit_i16x8_mul) SIMD_BINOP(emit_i16x8_min_s) SIMD_BINOP(emit_i16x8_min_u)
      SIMD_BINOP(emit_i16x8_max_s) SIMD_BINOP(emit_i16x8_max_u) SIMD_BINOP(emit_i16x8_avgr_u)
      SIMD_BINOP(emit_i16x8_extmul_low_i8x16_s) SIMD_BINOP(emit_i16x8_extmul_high_i8x16_s)
      SIMD_BINOP(emit_i16x8_extmul_low_i8x16_u) SIMD_BINOP(emit_i16x8_extmul_high_i8x16_u)
      SIMD_UNOP(emit_i32x4_extadd_pairwise_i16x8_s) SIMD_UNOP(emit_i32x4_extadd_pairwise_i16x8_u)
      SIMD_UNOP(emit_i32x4_abs) SIMD_UNOP(emit_i32x4_neg)
      void emit_i32x4_all_true() { ir_simd_test(simd_sub::i32x4_all_true); }
      void emit_i32x4_bitmask() { ir_simd_test(simd_sub::i32x4_bitmask); }
      SIMD_UNOP(emit_i32x4_extend_low_i16x8_s) SIMD_UNOP(emit_i32x4_extend_high_i16x8_s)
      SIMD_UNOP(emit_i32x4_extend_low_i16x8_u) SIMD_UNOP(emit_i32x4_extend_high_i16x8_u)
      SIMD_SHIFT(emit_i32x4_shl) SIMD_SHIFT(emit_i32x4_shr_s) SIMD_SHIFT(emit_i32x4_shr_u)
      SIMD_BINOP(emit_i32x4_add) SIMD_BINOP(emit_i32x4_sub) SIMD_BINOP(emit_i32x4_mul)
      SIMD_BINOP(emit_i32x4_min_s) SIMD_BINOP(emit_i32x4_min_u) SIMD_BINOP(emit_i32x4_max_s) SIMD_BINOP(emit_i32x4_max_u)
      SIMD_BINOP(emit_i32x4_dot_i16x8_s)
      SIMD_BINOP(emit_i32x4_extmul_low_i16x8_s) SIMD_BINOP(emit_i32x4_extmul_high_i16x8_s)
      SIMD_BINOP(emit_i32x4_extmul_low_i16x8_u) SIMD_BINOP(emit_i32x4_extmul_high_i16x8_u)
      SIMD_UNOP(emit_i64x2_abs) SIMD_UNOP(emit_i64x2_neg)
      void emit_i64x2_all_true() { ir_simd_test(simd_sub::i64x2_all_true); }
      void emit_i64x2_bitmask() { ir_simd_test(simd_sub::i64x2_bitmask); }
      SIMD_UNOP(emit_i64x2_extend_low_i32x4_s) SIMD_UNOP(emit_i64x2_extend_high_i32x4_s)
      SIMD_UNOP(emit_i64x2_extend_low_i32x4_u) SIMD_UNOP(emit_i64x2_extend_high_i32x4_u)
      SIMD_SHIFT(emit_i64x2_shl) SIMD_SHIFT(emit_i64x2_shr_s) SIMD_SHIFT(emit_i64x2_shr_u)
      SIMD_BINOP(emit_i64x2_add) SIMD_BINOP(emit_i64x2_sub) SIMD_BINOP(emit_i64x2_mul)
      SIMD_BINOP(emit_i64x2_extmul_low_i32x4_s) SIMD_BINOP(emit_i64x2_extmul_high_i32x4_s)
      SIMD_BINOP(emit_i64x2_extmul_low_i32x4_u) SIMD_BINOP(emit_i64x2_extmul_high_i32x4_u)
      SIMD_UNOP(emit_f32x4_ceil) SIMD_UNOP(emit_f32x4_floor) SIMD_UNOP(emit_f32x4_trunc) SIMD_UNOP(emit_f32x4_nearest)
      SIMD_UNOP(emit_f32x4_abs) SIMD_UNOP(emit_f32x4_neg) SIMD_UNOP(emit_f32x4_sqrt)
      SIMD_BINOP(emit_f32x4_add) SIMD_BINOP(emit_f32x4_sub) SIMD_BINOP(emit_f32x4_mul) SIMD_BINOP(emit_f32x4_div)
      SIMD_BINOP(emit_f32x4_min) SIMD_BINOP(emit_f32x4_max) SIMD_BINOP(emit_f32x4_pmin) SIMD_BINOP(emit_f32x4_pmax)
      SIMD_UNOP(emit_f64x2_ceil) SIMD_UNOP(emit_f64x2_floor) SIMD_UNOP(emit_f64x2_trunc) SIMD_UNOP(emit_f64x2_nearest)
      SIMD_UNOP(emit_f64x2_abs) SIMD_UNOP(emit_f64x2_neg) SIMD_UNOP(emit_f64x2_sqrt)
      SIMD_BINOP(emit_f64x2_add) SIMD_BINOP(emit_f64x2_sub) SIMD_BINOP(emit_f64x2_mul) SIMD_BINOP(emit_f64x2_div)
      SIMD_BINOP(emit_f64x2_min) SIMD_BINOP(emit_f64x2_max) SIMD_BINOP(emit_f64x2_pmin) SIMD_BINOP(emit_f64x2_pmax)
      SIMD_UNOP(emit_i32x4_trunc_sat_f32x4_s) SIMD_UNOP(emit_i32x4_trunc_sat_f32x4_u)
      SIMD_UNOP(emit_f32x4_convert_i32x4_s) SIMD_UNOP(emit_f32x4_convert_i32x4_u)
      SIMD_UNOP(emit_i32x4_trunc_sat_f64x2_s_zero) SIMD_UNOP(emit_i32x4_trunc_sat_f64x2_u_zero)
      SIMD_UNOP(emit_f64x2_convert_low_i32x4_s) SIMD_UNOP(emit_f64x2_convert_low_i32x4_u)
      SIMD_UNOP(emit_f32x4_demote_f64x2_zero) SIMD_UNOP(emit_f64x2_promote_low_f32x4)

      // Relaxed SIMD binary
      SIMD_BINOP(emit_i16x8_relaxed_dot_i8x16_i7x16_s)

      // Relaxed SIMD ternary (FMA, dot+add)
      void emit_f32x4_relaxed_madd() { emit_simd_ternop(simd_sub::f32x4_relaxed_madd); }
      void emit_f32x4_relaxed_nmadd() { emit_simd_ternop(simd_sub::f32x4_relaxed_nmadd); }
      void emit_f64x2_relaxed_madd() { emit_simd_ternop(simd_sub::f64x2_relaxed_madd); }
      void emit_f64x2_relaxed_nmadd() { emit_simd_ternop(simd_sub::f64x2_relaxed_nmadd); }
      void emit_i32x4_relaxed_dot_i8x16_i7x16_add_s() { emit_simd_ternop(simd_sub::i32x4_relaxed_dot_i8x16_i7x16_add_s); }

#undef SIMD_UNOP
#undef SIMD_BINOP
#undef SIMD_SHIFT
#undef SIMD_EXTRACT
#undef SIMD_REPLACE
#undef SIMD_SPLAT
#undef SIMD_RELOP

      // ──── Bulk memory ────
      void emit_memory_init(std::uint32_t s) {
         ir_bulk_mem3();
         if (!_unreachable) {
            ir_inst inst{}; inst.opcode = ir_op::memory_init;
            inst.flags = IR_SIDE_EFFECT; inst.dest = ir_vreg_none;
            inst.ri.imm = static_cast<int32_t>(s); // data segment index
            _func->emit(inst);
         }
      }
      void emit_data_drop(std::uint32_t s) {
         if (!_unreachable) {
            ir_inst inst{}; inst.opcode = ir_op::data_drop;
            inst.flags = IR_SIDE_EFFECT; inst.dest = ir_vreg_none;
            inst.ri.imm = static_cast<int32_t>(s); // data segment index
            _func->emit(inst);
         }
      }
      void emit_memory_copy() {
         ir_bulk_mem3(); // vpop the 3 args from vstack
         if (!_unreachable) {
            ir_inst inst{}; inst.opcode = ir_op::memory_copy;
            inst.flags = IR_SIDE_EFFECT; inst.dest = ir_vreg_none;
            _func->emit(inst);
         }
      }
      void emit_memory_fill() {
         ir_bulk_mem3();
         if (!_unreachable) {
            ir_inst inst{}; inst.opcode = ir_op::memory_fill;
            inst.flags = IR_SIDE_EFFECT; inst.dest = ir_vreg_none;
            _func->emit(inst);
         }
      }
      void emit_table_init(std::uint32_t elem_idx, std::uint32_t table_idx = 0) {
         ir_bulk_mem3();
         if (!_unreachable) {
            ir_inst inst{}; inst.opcode = ir_op::table_init;
            inst.flags = IR_SIDE_EFFECT; inst.dest = ir_vreg_none;
            inst.ri.imm = static_cast<int32_t>(elem_idx | (table_idx << 16));
            _func->emit(inst);
         }
      }
      void emit_elem_drop(std::uint32_t s) {
         if (!_unreachable) {
            ir_inst inst{}; inst.opcode = ir_op::elem_drop;
            inst.flags = IR_SIDE_EFFECT; inst.dest = ir_vreg_none;
            inst.ri.imm = static_cast<int32_t>(s); // element segment index
            _func->emit(inst);
         }
      }
      void emit_table_copy(std::uint32_t dst_table = 0, std::uint32_t src_table = 0) {
         ir_bulk_mem3();
         if (!_unreachable) {
            ir_inst inst{}; inst.opcode = ir_op::table_copy;
            inst.flags = IR_SIDE_EFFECT; inst.dest = ir_vreg_none;
            inst.ri.imm = static_cast<int32_t>(dst_table | (src_table << 16));
            _func->emit(inst);
         }
      }

      // ──── Atomic operations ────
      void emit_atomic_op(atomic_sub sub, uint32_t align, uint32_t offset) {
         if (_unreachable) return;
         uint8_t asub = static_cast<uint8_t>(sub);

         if (sub == atomic_sub::atomic_fence) {
            // No-op in single-threaded mode, but still emit for correctness
            ir_inst inst{};
            inst.opcode = ir_op::atomic_op;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.ri.imm = asub;
            _func->emit(inst);
            return;
         }

         if (sub == atomic_sub::memory_atomic_notify) {
            _func->vpop(); // count
            _func->vpop(); // addr
            uint32_t dest = _func->alloc_vreg(types::i32);
            ir_inst inst{};
            inst.opcode = ir_op::atomic_op;
            inst.type = types::i32;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.ri.imm = asub;
            _func->emit(inst);
            _func->vpush(dest);
            return;
         }

         if (sub == atomic_sub::memory_atomic_wait32 || sub == atomic_sub::memory_atomic_wait64) {
            _func->vpop(); // timeout
            _func->vpop(); // expected
            _func->vpop(); // addr
            uint32_t dest = _func->alloc_vreg(types::i32);
            ir_inst inst{};
            inst.opcode = ir_op::atomic_op;
            inst.type = types::i32;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.ri.imm = asub;
            _func->emit(inst);
            _func->vpush(dest);
            return;
         }

         // Atomic loads: pop addr, push result
         if (asub >= 0x10 && asub <= 0x16) {
            uint32_t addr = _func->vpop();
            bool is_i64 = (asub >= 0x14);
            uint8_t rtype = is_i64 ? types::i64 : types::i32;
            uint32_t dest = _func->alloc_vreg(rtype);
            ir_inst inst{};
            inst.opcode = ir_op::atomic_op;
            inst.type = rtype;
            inst.flags = IR_NONE;
            inst.dest = dest;
            inst.simd.offset = offset;
            inst.simd.addr = addr;
            inst.simd.lane = asub;
            _func->emit(inst);
            _func->vpush(dest);
            return;
         }

         // Atomic stores: pop value, pop addr
         if (asub >= 0x17 && asub <= 0x1D) {
            uint32_t val = _func->vpop();
            uint32_t addr = _func->vpop();
            ir_inst inst{};
            inst.opcode = ir_op::atomic_op;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = ir_vreg_none;
            inst.simd.offset = offset;
            inst.simd.addr = addr;
            inst.simd.lane = asub;
            inst.simd.v_src1 = static_cast<uint16_t>(val);
            _func->emit(inst);
            return;
         }

         // Atomic RMW: pop value, pop addr, push old
         if (asub >= 0x1E && asub <= 0x47) {
            uint32_t val = _func->vpop();
            uint32_t addr = _func->vpop();
            // Determine result type
            uint8_t in_group = (asub - 0x1E) % 7;
            bool is_i64 = (in_group == 1 || in_group >= 4);
            uint8_t rtype = is_i64 ? types::i64 : types::i32;
            uint32_t dest = _func->alloc_vreg(rtype);
            ir_inst inst{};
            inst.opcode = ir_op::atomic_op;
            inst.type = rtype;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.simd.offset = offset;
            inst.simd.addr = addr;
            inst.simd.lane = asub;
            inst.simd.v_src1 = static_cast<uint16_t>(val);
            _func->emit(inst);
            _func->vpush(dest);
            return;
         }

         // Atomic cmpxchg: pop replacement, pop expected, pop addr, push old
         if (asub >= 0x48 && asub <= 0x4E) {
            uint32_t replacement = _func->vpop();
            uint32_t expected = _func->vpop();
            uint32_t addr = _func->vpop();
            bool is_i64 = (asub == 0x49 || asub >= 0x4C);
            uint8_t rtype = is_i64 ? types::i64 : types::i32;
            uint32_t dest = _func->alloc_vreg(rtype);
            ir_inst inst{};
            inst.opcode = ir_op::atomic_op;
            inst.type = rtype;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = dest;
            inst.simd.offset = offset;
            inst.simd.addr = addr;
            inst.simd.lane = asub;
            inst.simd.v_src1 = static_cast<uint16_t>(expected);
            inst.simd.v_src2 = static_cast<uint16_t>(replacement);
            _func->emit(inst);
            _func->vpush(dest);
            return;
         }
      }

      // ──── Exception handling ────
      void emit_try(uint8_t result_type = types::pseudo, uint32_t result_count = 0) {
         emit_block(result_type, result_count);
      }
      branch_t emit_catch(uint32_t /*tag_index*/) {
         ir_inst inst{};
         inst.opcode = ir_op::br;
         inst.type = types::pseudo;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = 0; // no depth_change, no eh_leave_count
         inst.br.target = UINT32_MAX;
         uint32_t inst_idx = _func->current_inst_index();
         _func->emit(inst);
         uint32_t blk = _func->new_block();
         _func->start_block(blk);
         return inst_idx;
      }
      branch_t emit_catch_all() {
         return emit_catch(UINT32_MAX);
      }
      void emit_throw(uint32_t tag_index) {
         if (_unreachable) return;
         // Pop payload values from vstack and emit as arg ops
         const func_type& tag_ft = _mod.types[_mod.tags[tag_index].type_index];
         uint32_t nparams = static_cast<uint32_t>(tag_ft.param_types.size());
         uint32_t* param_vregs = _scratch.alloc<uint32_t>(nparams);
         for (uint32_t i = 0; i < nparams; ++i) {
            uint32_t pi = nparams - 1 - i;
            param_vregs[pi] = _func->vpop();
         }
         for (uint32_t i = 0; i < nparams; ++i) {
            ir_inst arg{};
            arg.opcode = ir_op::arg;
            arg.type = types::pseudo;
            arg.flags = IR_NONE;
            arg.dest = ir_vreg_none;
            arg.rr.src1 = param_vregs[i];
            arg.rr.src2 = ir_vreg_none;
            _func->emit(arg);
         }
         ir_inst inst{};
         inst.opcode = ir_op::eh_throw;
         inst.type = types::pseudo;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = ir_vreg_none;
         inst.ri.imm = static_cast<int32_t>(tag_index);
         inst.ri.src1 = ir_vreg_none;
         _func->emit(inst);
         _unreachable = true;
      }
      void emit_rethrow(uint32_t, uint8_t, uint32_t, uint32_t = UINT32_MAX) {
         ir_inst inst{};
         inst.opcode = ir_op::unreachable;
         inst.flags = IR_SIDE_EFFECT;
         _func->emit(inst);
         _unreachable = true;
      }
      void emit_delegate(uint32_t, uint8_t, uint32_t, uint32_t = UINT32_MAX) {
         // Structural no-op
      }
      std::vector<branch_t> emit_try_table(uint8_t result_type, uint32_t result_count, const std::vector<catch_clause>& clauses, uint32_t param_count = 0) {
         if (_unreachable) {
            emit_block(result_type, result_count, param_count);
            return std::vector<branch_t>(clauses.size(), UINT32_MAX);
         }

         uint32_t num_catches = static_cast<uint32_t>(clauses.size());

         // ── 1. Store catch clause data in EH side table ──
         uint32_t eh_idx = _func->add_eh_data(num_catches, clauses.data());

         // ── 2. Allocate the try body control entry (need merge vregs for dispatch) ──
         ir_control_entry try_entry{};
         try_entry.block_idx = _func->new_block(); // try body block
         try_entry.param_count = static_cast<uint8_t>(param_count);
         std::memset(try_entry.param_vregs, 0xFF, sizeof(try_entry.param_vregs));
         if (param_count <= _func->vstack_depth()) {
            try_entry.stack_depth = _func->vstack_depth() - param_count;
            for (uint32_t i = 0; i < param_count && i < 16; ++i) {
               try_entry.param_vregs[i] = _func->vstack[try_entry.stack_depth + i];
            }
         } else {
            try_entry.stack_depth = _func->vstack_depth();
         }
         try_entry.result_type = result_type;
         try_entry.is_loop = 0;
         try_entry.is_function = 0;
         try_entry.is_try_table = 1;
         try_entry.entered_unreachable = 0;
         try_entry.result_count = static_cast<uint8_t>(result_count);
         std::memset(try_entry.result_types, 0, sizeof(try_entry.result_types));
         std::memset(try_entry.merge_vregs, 0xFF, sizeof(try_entry.merge_vregs));
         if (result_count > 1) {
            consume_pending_result_types(try_entry, result_count);
            for (uint32_t i = 0; i < result_count && i < 16; ++i) {
               try_entry.merge_vregs[i] = _func->alloc_vreg(try_entry.result_types[i]);
            }
            try_entry.merge_vreg = try_entry.merge_vregs[0];
         } else if (result_type != types::pseudo) {
            try_entry.merge_vreg = _func->alloc_vreg(result_type);
            try_entry.merge_vregs[0] = try_entry.merge_vreg;
            try_entry.result_types[0] = result_type;
         } else {
            try_entry.merge_vreg = ir_vreg_none;
            _pending_result_types = nullptr;
            _pending_result_types_count = 0;
         }

         // ── 3. Emit eh_enter: pushes try frame, returns jmpbuf pointer ──
         uint32_t jmpbuf_vreg = _func->alloc_vreg(types::i64);
         {
            ir_inst inst{};
            inst.opcode = ir_op::eh_enter;
            inst.type = types::i64;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = jmpbuf_vreg;
            inst.ri.imm = static_cast<int32_t>(eh_idx);
            inst.ri.src1 = num_catches;
            _func->emit(inst);
         }

         // ── 4. Emit eh_setjmp: returns 0 (normal) or non-zero (longjmp) ──
         uint32_t sjresult = _func->alloc_vreg(types::i32);
         {
            ir_inst inst{};
            inst.opcode = ir_op::eh_setjmp;
            inst.type = types::i32;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = sjresult;
            inst.rr.src1 = jmpbuf_vreg;
            inst.rr.src2 = ir_vreg_none;
            _func->emit(inst);
         }

         // ── 5. Invert setjmp result: is_normal = (sjresult == 0) ──
         // Branch to try body on normal flow; fall through to dispatch on exception.
         uint32_t is_normal = _func->alloc_vreg(types::i32);
         {
            ir_inst inst{};
            inst.opcode = ir_op::i32_eqz;
            inst.type = types::i32;
            inst.flags = IR_NONE;
            inst.dest = is_normal;
            inst.rr.src1 = sjresult;
            inst.rr.src2 = ir_vreg_none;
            _func->emit(inst);
         }

         // Create a "try body gate" block that only serves as the entry point
         // for normal flow. Separate from try_entry.block_idx so that WASM-level
         // br to the try_table still targets the exit (non-loop semantics).
         uint32_t try_body_gate = _func->new_block();
         _func->blocks[try_body_gate].branch_to_entry = 1;

         {
            ir_inst inst{};
            inst.opcode = ir_op::br_if;
            inst.type = types::pseudo;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = 0; // internal dispatch — no depth_change, no eh_leave_count
            inst.br.target = try_body_gate;
            inst.br.src1 = is_normal;
            _func->emit(inst);
         }

         // ── 6. Exception dispatch (fallthrough from br_if when sjresult != 0) ──
         uint32_t match_vreg = _func->alloc_vreg(types::i32);
         {
            ir_inst inst{};
            inst.opcode = ir_op::eh_get_match;
            inst.type = types::i32;
            inst.flags = IR_SIDE_EFFECT;
            inst.dest = match_vreg;
            _func->emit(inst);
         }

         // For multi-clause dispatch: compare match_vreg against each clause
         // index and branch to the right handler. Last clause is the fallthrough.
         std::vector<branch_t> result;

         // Create handler blocks for each clause (except the last, which is fallthrough)
         std::vector<uint32_t> handler_blocks;
         for (uint32_t ci = 0; ci < num_catches; ++ci) {
            handler_blocks.push_back(_func->new_block());
            _func->blocks[handler_blocks.back()].branch_to_entry = 1;
         }

         // Emit dispatch comparisons for multi-clause
         if (num_catches > 1) {
            for (uint32_t ci = 0; ci < num_catches - 1; ++ci) {
               // Compare match_vreg == ci
               uint32_t cmp_result = _func->alloc_vreg(types::i32);
               uint32_t const_vreg = _func->alloc_vreg(types::i32);
               {
                  ir_inst cinst{};
                  cinst.opcode = ir_op::const_i32;
                  cinst.type = types::i32;
                  cinst.flags = IR_NONE;
                  cinst.dest = const_vreg;
                  cinst.imm64 = ci;
                  _func->emit(cinst);
               }
               {
                  ir_inst cinst{};
                  cinst.opcode = ir_op::i32_eq;
                  cinst.type = types::i32;
                  cinst.flags = IR_NONE;
                  cinst.dest = cmp_result;
                  cinst.rr.src1 = match_vreg;
                  cinst.rr.src2 = const_vreg;
                  _func->emit(cinst);
               }
               // Branch to handler block if match
               {
                  ir_inst binst{};
                  binst.opcode = ir_op::br_if;
                  binst.type = types::pseudo;
                  binst.flags = IR_SIDE_EFFECT;
                  binst.dest = 0; // internal dispatch — no depth_change, no eh_leave_count
                  binst.br.target = handler_blocks[ci];
                  binst.br.src1 = cmp_result;
                  _func->emit(binst);
               }
            }
            // Fall through to last handler (default)
         }

         // Emit each handler block
         for (uint32_t ci = 0; ci < num_catches; ++ci) {
            auto& clause = clauses[ci];
            bool has_exnref = (clause.kind == 1 || clause.kind == 3);
            uint32_t tag_payload = has_exnref ? (clause.payload_count > 0 ? clause.payload_count - 1 : 0) : clause.payload_count;

            // For non-last single-clause or last clause: inline. Others: start handler block.
            if (num_catches > 1 && ci < num_catches - 1) {
               // Non-last clause: br_if already targets this block
               // The previous clause's br_if or the default fallthrough will reach here
            }
            // For multi-clause non-last handlers, start the block
            if (num_catches > 1 && ci < num_catches - 1) {
               _func->start_block(handler_blocks[ci]);
            }
            // For single-clause or last clause: the code is inline (fallthrough)

            // Read payload values from staging area into temp vregs
            std::vector<uint32_t> payload_vregs;
            for (uint32_t pi = 0; pi < tag_payload; ++pi) {
               uint32_t pv = _func->alloc_vreg(types::i64);
               ir_inst inst{};
               inst.opcode = ir_op::eh_get_payload;
               inst.type = types::i64;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = pv;
               inst.ri.imm = static_cast<int32_t>(pi);
               inst.ri.src1 = ir_vreg_none;
               _func->emit(inst);
               payload_vregs.push_back(pv);
            }
            if (has_exnref) {
               uint32_t ev = _func->alloc_vreg(types::i64);
               ir_inst inst{};
               inst.opcode = ir_op::eh_get_exnref;
               inst.type = types::i64;
               inst.flags = IR_SIDE_EFFECT;
               inst.dest = ev;
               _func->emit(inst);
               payload_vregs.push_back(ev);
            }

            // Emit mov instructions to copy payload to target block's merge vregs.
            // Per WASM spec, catch clause label 0 = enclosing scope (not the
            // try_table itself), so resolve against ctrl_stack directly.
            // try_entry has NOT been pushed yet, so ctrl_stack_top-1 is the
            // innermost enclosing block (same indexing as emit_br).
            ir_control_entry* target = nullptr;
            if (clause.label < _func->ctrl_stack_top) {
               target = &_func->ctrl_stack[_func->ctrl_stack_top - 1 - clause.label];
            }
            if (target && !target->is_loop) {
               uint32_t n = std::min<uint32_t>(
                  static_cast<uint32_t>(payload_vregs.size()),
                  target->result_count > 1 ? target->result_count : (target->merge_vreg != ir_vreg_none ? 1u : 0u));
               for (uint32_t vi = 0; vi < n; ++vi) {
                  uint32_t merge = (target->result_count > 1)
                     ? target->merge_vregs[vi] : target->merge_vreg;
                  if (merge != ir_vreg_none && payload_vregs[vi] != merge) {
                     ir_inst mov{};
                     mov.opcode = ir_op::mov;
                     mov.type = types::i64;
                     mov.flags = IR_NONE;
                     mov.dest = merge;
                     mov.rr.src1 = payload_vregs[vi];
                     mov.rr.src2 = ir_vreg_none;
                     _func->emit(mov);
                  }
               }
            }

            // Emit eh_leave for any try_table scopes crossed by this catch branch.
            // The matching try_table's frame is already popped by try_match_exception
            // at runtime, but outer try_tables between here and the target need cleanup.
            for (uint32_t li = 0; li < clause.eh_leave_count; ++li) {
               emit_eh_leave();
            }

            // Emit branch to catch target (unresolved — parser calls fix_branch).
            // dest encodes depth_change in the low 16 bits (eh_leave_count is 0 here
            // because we emitted eh_leave ops separately above). depth_change is
            // the number of operand-stack slots the target scope expects popped
            // vs. the operand depth captured at try_table entry — without this,
            // the JIT's emit_branch_multipop leaves those slots on rsp, leaking
            // depth_change*8 bytes of native stack per catch-taken execution.
            // For a loop-targeting catch inside a larger loop, the leak
            // accumulates every iteration and eventually clobbers the caller's
            // frame (see mismatch_9677_seed1776497367 in KNOWN_ISSUES.md).
            ir_inst br{};
            br.opcode = ir_op::br;
            br.type = types::pseudo;
            br.flags = IR_SIDE_EFFECT;
            br.dest = clause.depth_change & 0xFFFFu;
            br.br.target = UINT32_MAX;
            br.br.src1 = ir_vreg_none;
            result.push_back(_func->current_inst_index());
            _func->emit(br);
         }

         // ── 7. Start try body gate block (only reachable via br_if on normal flow) ──
         _func->start_block(try_body_gate);

         // ── 8. Start try body block (falls through from gate) ──
         _func->start_block(try_entry.block_idx);

         // ── 9. Push control entry ──
         _func->ctrl_push(try_entry);

         return result;
      }
      void emit_throw_ref() {
         if (_unreachable) return;
         uint32_t exnref = _func->vpop();
         ir_inst inst{};
         inst.opcode = ir_op::eh_throw_ref;
         inst.type = types::pseudo;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = ir_vreg_none;
         inst.rr.src1 = exnref;
         inst.rr.src2 = ir_vreg_none;
         _func->emit(inst);
         _unreachable = true;
      }
      void emit_eh_leave() {
         if (_unreachable) return;
         ir_inst inst{};
         inst.opcode = ir_op::eh_leave;
         inst.type = types::pseudo;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = ir_vreg_none;
         _func->emit(inst);
      }

      // ──── Branch fixup ────
      // Patch a br/br_if IR instruction's target to the resolved block index.
      void fix_branch(branch_t inst_idx, label_t block_idx) {
         if (_func && inst_idx < _func->inst_count && block_idx != UINT32_MAX) {
            _func->insts[inst_idx].br.target = block_idx;
         }
      }

    private:
      // ──── IR building helpers ────

      void ir_binop(ir_op op, uint8_t result_type) {
         if (_unreachable) return;
         uint32_t rhs = _func->vpop();
         uint32_t lhs = _func->vpop();
         uint32_t dest = _func->alloc_vreg(result_type);
         ir_inst inst{};
         inst.opcode = op;
         inst.type = result_type;
         inst.dest = dest;
         inst.rr.src1 = lhs;
         inst.rr.src2 = rhs;
         _func->emit(inst);
         _func->vpush(dest);
      }

      void ir_unop(ir_op op, uint8_t result_type) {
         if (_unreachable) return;
         uint32_t src = _func->vpop();
         uint32_t dest = _func->alloc_vreg(result_type);
         ir_inst inst{};
         inst.opcode = op;
         inst.type = result_type;
         inst.dest = dest;
         inst.rr.src1 = src;
         inst.rr.src2 = ir_vreg_none;
         _func->emit(inst);
         _func->vpush(dest);
      }

      void ir_emit_nullary(ir_op op, uint8_t type) {
         if (_unreachable) return;
         ir_inst inst{};
         inst.opcode = op;
         inst.type = type;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = ir_vreg_none;
         inst.rr.src1 = ir_vreg_none;
         inst.rr.src2 = ir_vreg_none;
         _func->emit(inst);
      }

      void ir_load(ir_op op, uint8_t result_type, uint32_t offset) {
         if (_unreachable) return;
         uint32_t addr = _func->vpop();
         uint32_t dest = _func->alloc_vreg(result_type);
         ir_inst inst{};
         inst.opcode = op;
         inst.type = result_type;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = dest;
         inst.ri.src1 = addr;
         inst.ri.imm = static_cast<int32_t>(offset);
         _func->emit(inst);
         _func->vpush(dest);
      }

      void ir_store(ir_op op, uint8_t type, uint32_t offset) {
         if (_unreachable) return;
         uint32_t val = _func->vpop();
         uint32_t addr = _func->vpop();
         ir_inst inst{};
         inst.opcode = op;
         inst.type = type;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = val;  // reuse dest field for value vreg (stores have no result)
         inst.ri.src1 = addr;
         inst.ri.imm = static_cast<int32_t>(offset);
         _func->emit(inst);
      }

      // ──── SIMD vstack tracking + IR emission ────
      void ir_simd_emit(simd_sub sub) {
         ir_inst inst{};
         inst.opcode = ir_op::v128_op;
         inst.type = types::v128;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = static_cast<uint32_t>(sub);
         inst.simd.v_src1 = 0xFFFF;
         inst.simd.v_src2 = 0xFFFF;
         inst.simd.v_dest = 0xFFFF;
         _func->emit(inst);
      }
      void ir_simd_emit_with_offset(simd_sub sub, uint32_t offset, uint32_t addr_vreg = ir_vreg_none,
                                    uint16_t vs1 = 0xFFFF, uint16_t vd = 0xFFFF) {
         ir_inst inst{};
         inst.opcode = ir_op::v128_op;
         inst.type = types::v128;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = static_cast<uint32_t>(sub);
         inst.simd.offset = offset;
         inst.simd.addr = addr_vreg;
         inst.simd.v_src1 = vs1;
         inst.simd.v_src2 = 0xFFFF;
         inst.simd.v_dest = vd;
         _func->emit(inst);
      }
      void ir_simd_emit_with_offset_lane(simd_sub sub, uint32_t offset, uint8_t lane, uint32_t addr_vreg = ir_vreg_none,
                                          uint16_t vs1 = 0xFFFF, uint16_t vd = 0xFFFF) {
         ir_inst inst{};
         inst.opcode = ir_op::v128_op;
         inst.type = types::v128;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = static_cast<uint32_t>(sub);
         inst.simd.offset = offset;
         inst.simd.addr = addr_vreg;
         inst.simd.lane = lane;
         inst.simd.v_src1 = vs1;
         inst.simd.v_src2 = 0xFFFF;
         inst.simd.v_dest = vd;
         _func->emit(inst);
      }
      void ir_simd_emit_with_lane(simd_sub sub, uint8_t lane) {
         ir_inst inst{};
         inst.opcode = ir_op::v128_op;
         inst.type = types::v128;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = static_cast<uint32_t>(sub);
         inst.simd.v_src1 = 0xFFFF;
         inst.simd.v_src2 = 0xFFFF;
         inst.simd.v_dest = 0xFFFF;
         inst.simd.lane = lane;
         _func->emit(inst);
      }
      void ir_simd_load(simd_sub sub, uint32_t offset) {
         if (_unreachable) return;
         uint32_t addr = _func->vpop();  // address vreg
         uint32_t d1 = _func->alloc_vreg(types::v128);
         uint32_t d2 = _func->alloc_vreg(types::v128);
         ir_simd_emit_with_offset(sub, offset, addr, 0xFFFF, static_cast<uint16_t>(d1));
         _func->vpush(d1); _func->vpush(d2);
      }
      void ir_simd_store(simd_sub sub, uint32_t offset) {
         if (_unreachable) return;
         _func->vpop(); uint32_t s1 = _func->vpop();  // v128 value (low vreg)
         uint32_t addr = _func->vpop(); // address vreg
         ir_simd_emit_with_offset(sub, offset, addr, static_cast<uint16_t>(s1));
      }
      void ir_simd_load_lane(simd_sub sub, uint32_t offset, uint8_t lane) {
         if (_unreachable) return;
         _func->has_simd = true;
         _func->vpop(); uint32_t s1 = _func->vpop();  // v128 (low vreg)
         uint32_t addr = _func->vpop(); // address
         uint32_t d1 = _func->alloc_vreg(types::v128);
         uint32_t d2 = _func->alloc_vreg(types::v128);
         ir_simd_emit_with_offset_lane(sub, offset, lane, addr,
                                        static_cast<uint16_t>(s1), static_cast<uint16_t>(d1));
         _func->vpush(d1); _func->vpush(d2);
      }
      void ir_simd_store_lane(simd_sub sub, uint32_t offset, uint8_t lane) {
         if (_unreachable) return;
         _func->has_simd = true;
         _func->vpop(); uint32_t s1 = _func->vpop();  // v128 (low vreg)
         uint32_t addr = _func->vpop(); // address
         ir_simd_emit_with_offset_lane(sub, offset, lane, addr, static_cast<uint16_t>(s1));
      }
      void ir_simd_unop(simd_sub sub) {
         if (_unreachable) return;
         _func->has_simd = true;
         _func->vpop(); uint32_t s1 = _func->vpop(); // v128 src (low vreg)
         uint32_t d1 = _func->alloc_vreg(types::v128);
         uint32_t d2 = _func->alloc_vreg(types::v128);
         ir_inst inst{};
         inst.opcode = ir_op::v128_op;
         inst.type = types::v128;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = static_cast<uint32_t>(sub);
         inst.simd.v_src1 = static_cast<uint16_t>(s1);
         inst.simd.v_src2 = 0xFFFF;
         inst.simd.v_dest = static_cast<uint16_t>(d1);
         _func->emit(inst);
         _func->vpush(d1); _func->vpush(d2);
      }
      void ir_simd_binop(simd_sub sub) {
         if (_unreachable) return;
         _func->has_simd = true;
         _func->vpop(); uint32_t s2 = _func->vpop(); // v128 src2 (low vreg)
         _func->vpop(); uint32_t s1 = _func->vpop(); // v128 src1 (low vreg)
         uint32_t d1 = _func->alloc_vreg(types::v128);
         uint32_t d2 = _func->alloc_vreg(types::v128);
         ir_inst inst{};
         inst.opcode = ir_op::v128_op;
         inst.type = types::v128;
         inst.flags = IR_SIDE_EFFECT;
         inst.dest = static_cast<uint32_t>(sub);
         inst.simd.v_src1 = static_cast<uint16_t>(s1);
         inst.simd.v_src2 = static_cast<uint16_t>(s2);
         inst.simd.v_dest = static_cast<uint16_t>(d1);
         _func->emit(inst);
         _func->vpush(d1); _func->vpush(d2);
      }
      void ir_simd_shift(simd_sub sub) {
         if (_unreachable) return;
         _func->has_simd = true;
         uint32_t shift_amt = _func->vpop();  // shift amount (i32)
         _func->vpop(); uint32_t s1 = _func->vpop();  // v128 (low vreg)
         uint32_t d1 = _func->alloc_vreg(types::v128);
         uint32_t d2 = _func->alloc_vreg(types::v128);
         // Store shift amount vreg in offset field so codegen can load from register
         ir_simd_emit_with_offset_lane(sub, shift_amt, 0, ir_vreg_none,
                                        static_cast<uint16_t>(s1), static_cast<uint16_t>(d1));
         _func->vpush(d1); _func->vpush(d2);
      }
      void ir_simd_extract(simd_sub sub, uint8_t lane) {
         if (_unreachable) return;
         _func->has_simd = true;
         _func->vpop(); uint32_t s1 = _func->vpop();  // v128 (low vreg)
         uint32_t d = _func->alloc_vreg(types::i64);
         // Store result vreg in addr field so register path can pop scalar to vreg
         ir_simd_emit_with_offset_lane(sub, 0, lane, d, static_cast<uint16_t>(s1));
         _func->vpush(d);
      }
      void ir_simd_replace(simd_sub sub, uint8_t lane) {
         if (_unreachable) return;
         _func->has_simd = true;
         uint32_t scalar = _func->vpop();  // scalar value vreg
         _func->vpop(); uint32_t s1 = _func->vpop();  // v128 (low vreg)
         uint32_t d1 = _func->alloc_vreg(types::v128);
         uint32_t d2 = _func->alloc_vreg(types::v128);
         ir_simd_emit_with_offset_lane(sub, scalar, lane, ir_vreg_none,
                                        static_cast<uint16_t>(s1), static_cast<uint16_t>(d1));
         _func->vpush(d1); _func->vpush(d2);
      }
      void ir_simd_splat(simd_sub sub) {
         if (_unreachable) return;
         _func->has_simd = true;
         uint32_t scalar = _func->vpop();  // scalar value vreg
         uint32_t d1 = _func->alloc_vreg(types::v128);
         uint32_t d2 = _func->alloc_vreg(types::v128);
         ir_simd_emit_with_offset(sub, 0, scalar, 0xFFFF, static_cast<uint16_t>(d1));
         _func->vpush(d1); _func->vpush(d2);
      }
      void ir_simd_test(simd_sub sub) {
         if (_unreachable) return;
         _func->has_simd = true;
         _func->vpop(); uint32_t s1 = _func->vpop();  // v128 (low vreg)
         uint32_t d = _func->alloc_vreg(types::i32);
         // Store result vreg in addr field so register path can pop scalar to vreg
         ir_simd_emit_with_offset(sub, 0, d, static_cast<uint16_t>(s1));
         _func->vpush(d);
      }
      void ir_emit_arg(uint32_t vreg) {
         ir_inst a{};
         a.opcode = ir_op::arg;
         a.type = types::pseudo;
         a.flags = IR_NONE;
         a.dest = ir_vreg_none;
         a.rr.src1 = vreg;
         a.rr.src2 = ir_vreg_none;
         _func->emit(a);
      }

      void ir_bulk_mem3() {
         if (_unreachable) return;
         // Pop 3 operands and emit arg instructions so register mode
         // pushes them to the native stack for the bulk memory handler.
         uint32_t v2 = _func->vpop(); // count (top)
         uint32_t v1 = _func->vpop(); // val/src
         uint32_t v0 = _func->vpop(); // dest
         ir_emit_arg(v0);
         ir_emit_arg(v1);
         ir_emit_arg(v2);
      }

      // ──── State ────
      std::size_t _source_bytes;
      bool _unreachable = false;
      const uint8_t* _wasm_pc = nullptr;
      // Phase 4 gas-metering: points at the current function's most
      // recently entered loop's loop_gas_extra field. Reset on each
      // emit_prologue so the value does not leak across functions.
      int64_t* _last_loop_gas_ptr = nullptr;
      // Side-channel for multi-value result types: parser fills these before the
      // next emit_block/loop/if/try_table/return so the IR writer can size the
      // vstack correctly for v128 (which occupies 2 slots, not 1). Consumed and
      // cleared by the emit_* call.
      const uint8_t* _pending_result_types = nullptr;
      uint32_t       _pending_result_types_count = 0;
    protected:
      bool _enable_backtrace;
      bool _stack_limit_is_bytes;
      growable_allocator& _allocator;       // Code segment (native code, permanent)
      growable_allocator _ir_alloc;         // IR data (per-function, reset after compilation)
      module& _mod;
      jit_scratch_allocator _scratch;       // Watermark wrapper for _ir_alloc
      ir_function* _functions = nullptr;
      uint32_t _num_functions = 0;
      ir_function* _func = nullptr;
      pzam_compile_result* _compile_result = nullptr;
      bool _skip_codegen = false;           // Set by subclasses to bypass native codegen
      std::size_t _post_array_offset = 0;   // IR alloc offset after _functions array
      // Lazy-initialized codegen and scratch allocators (created on first finalize)
      growable_allocator _codegen_scratch;
      growable_allocator _opt_scratch_alloc;
      std::optional<codegen_t> _codegen;
      // Parallel compilation state
      uint32_t _compile_threads = 0;          // 0 or 1 = serial, >1 = parallel threads
      std::size_t _max_func_bytes = 0;        // Largest function body in module

#if !defined(__wasi__)
      parse_callback_t _parse_callback;

      // Reactor-pattern parallel compilation: workers parse + optimize + regalloc + codegen.
      // Phase 1 (workers): each claims functions via atomic counter, parse+compile each one.
      // Between phases (main): compute prefix sums, allocate final buffer.
      // Phase 2 (workers): copy own code, adjust offsets, patch cross-worker calls.
      void parallel_parse_compile_all() {
         uint32_t N = _compile_threads;
         if (_num_functions == 0) return;
         if (N > _num_functions) N = _num_functions;

         struct worker_state {
            growable_allocator ir_alloc;       // Per-function IR, reset after each
            growable_allocator code_alloc;     // Accumulates native code
            growable_allocator codegen_scratch;
            growable_allocator opt_scratch_alloc;
            std::optional<codegen_t> codegen;
            void* code_base = nullptr;
            size_t code_size = 0;             // Total native code bytes
            size_t final_offset = 0;          // Offset into merged buffer
            uint64_t max_stack = 0;           // Per-worker maximum_stack accumulator
            std::vector<std::pair<uint32_t, uint32_t>> pending_calls;
         };

         auto workers = std::make_unique<worker_state[]>(N);

         // Track which worker compiled each function (for merge phase)
         auto func_worker = std::make_unique<uint32_t[]>(_num_functions);

         // Get persistent reactor
         auto& reactor = get_compile_reactor(N);

         // Phase 1: parallel parse + compile
         reactor.run_batch(
            _num_functions,

            // work_fn(worker_id, func_idx): parse + compile one function
            [this, &workers, &func_worker](uint32_t worker_id, uint32_t func_idx) {
               auto& ws = workers[worker_id];

               // Lazy init worker state on first use
               if (!ws.codegen) {
#ifdef PSIZAM_COMPILE_ONLY
                  // Conservative estimate: will grow via realloc if needed
                  ws.ir_alloc.use_fixed_memory(growable_allocator::align_to_page(
                     _max_func_bytes * 250 + 1024 * 1024));
                  // Worst case: largest function may need max_func_bytes * 64 bytes of
                  // native code, plus average ~2KB per remaining function.
                  size_t code_est = (_num_functions / _compile_threads + 1) * 2048 + 2 * 1024 * 1024;
                  size_t max_func_code = static_cast<size_t>(_max_func_bytes) * 64 + 1024 * 1024;
                  if (max_func_code > code_est) code_est = max_func_code;
                  ws.code_alloc.use_fixed_memory(growable_allocator::align_to_page(code_est));
                  size_t scratch_size = std::max<size_t>(32 * 1024 * 1024,
                     growable_allocator::align_to_page(_max_func_bytes * 20 + 4 * 1024 * 1024));
                  ws.codegen_scratch.use_fixed_memory(scratch_size);
                  ws.opt_scratch_alloc.use_fixed_memory(scratch_size);
#else
                  ws.ir_alloc.use_default_memory();
                  ws.code_alloc.use_default_memory();
                  ws.codegen_scratch.use_default_memory();
                  ws.opt_scratch_alloc.use_default_memory();
#endif
                  ws.codegen.emplace(ws.code_alloc, _mod, ws.codegen_scratch,
                                   _enable_backtrace, _stack_limit_is_bytes);
                  ws.codegen->emit_entry_and_error_handlers();
                  ws.code_base = ws.codegen->get_code_segment_base();
               }

               // Create per-function worker writer (lightweight, shares _functions array)
               // The writer's _func and _unreachable are per-instance, so thread-safe.
               ir_writer_impl worker_writer(_mod, _functions, _num_functions,
                                           ws.ir_alloc, _enable_backtrace, _stack_limit_is_bytes);

               // Parse bytecodes → IR via callback from parser
               _parse_callback(func_idx, worker_writer, ws.max_stack);

               // Optimize → regalloc → codegen
               auto& func = _functions[func_idx];
               {
                  jit_scratch_allocator scratch(ws.opt_scratch_alloc);
                  jit_optimizer::optimize(func, scratch);
                  jit_regalloc::compute_live_intervals(func, scratch);
                  jit_regalloc::allocate_registers(func);
               }
               ws.codegen->compile_function(func, _mod.code[func_idx]);

               // Reset IR allocator (only need 1 function's IR at a time)
               ws.ir_alloc._offset = 0;

               func_worker[func_idx] = worker_id;
            },

            // between_fn: compute prefix sums, adjust function offsets, allocate final buffer
            [this, &workers, &func_worker, N]() {
               // Compute per-worker code sizes
               for (uint32_t w = 0; w < N; ++w) {
                  auto& ws = workers[w];
                  if (!ws.codegen) continue;
                  ws.code_size = ws.code_alloc._offset -
                     static_cast<size_t>(static_cast<char*>(ws.code_base) - ws.code_alloc._base);
               }

               // Prefix sum → offsets into final buffer
               void* final_base = _allocator.start_code();
               size_t running = 0;
               for (uint32_t w = 0; w < N; ++w) {
                  workers[w].final_offset = running;
                  running += workers[w].code_size;
               }
               _allocator.alloc<unsigned char>(running);

               // Adjust all function code offsets (serial, O(num_functions))
               // jit_code_offset was set by compile_function relative to the worker's
               // code buffer. Add the worker's final_offset so it's relative to the
               // merged buffer. Must be done before call patching in merge phase.
               for (uint32_t f = 0; f < _num_functions; ++f) {
                  _mod.code[f].jit_code_offset += workers[func_worker[f]].final_offset;
               }

               // Collect pending cross-worker calls (before merge uses them)
               for (uint32_t w = 0; w < N; ++w) {
                  if (workers[w].codegen)
                     workers[w].codegen->collect_pending_relocs(workers[w].pending_calls);
               }

               // Merge per-worker max_stack into module
               for (uint32_t w = 0; w < N; ++w) {
                  _mod.maximum_stack = std::max(_mod.maximum_stack, workers[w].max_stack);
               }

               // Store final_base for merge phase
               _merge_final_base = final_base;
            },

            // merge_fn(worker_id): parallel copy + patch
            [this, &workers](uint32_t worker_id) {
               auto& ws = workers[worker_id];
               if (!ws.codegen) return;

               void* final_base = _merge_final_base;
               auto* dest = static_cast<unsigned char*>(final_base) + ws.final_offset;

               // Copy this worker's native code into the merged buffer
               std::memcpy(dest, ws.code_base, ws.code_size);

               // Patch cross-worker calls for this worker's pending list.
               // All function offsets were already adjusted in between_fn.
               uint32_t num_imported = _mod.get_imported_functions_size();
               for (auto& [branch_offset, target_func] : ws.pending_calls) {
                  void* branch_addr = static_cast<char*>(final_base) + ws.final_offset + branch_offset;
                  void* target_addr;
                  if (target_func < num_imported) {
                     // Imported function: use worker 0's host thunk
                     void* thunk = workers[0].codegen->get_func_addr(target_func);
                     size_t thunk_off = static_cast<char*>(thunk) -
                                        static_cast<char*>(workers[0].code_base);
                     target_addr = static_cast<char*>(final_base) + workers[0].final_offset + thunk_off;
                  } else {
                     // WASM function: offset already adjusted in between_fn
                     uint32_t wasm_idx = target_func - num_imported;
                     target_addr = static_cast<char*>(final_base) + _mod.code[wasm_idx].jit_code_offset;
                  }
                  codegen_t::fix_branch(branch_addr, target_addr);
               }
            }
         ); // end run_batch

         // Post-merge: finalize the merged code buffer
         _allocator.end_code<true>(_merge_final_base);

         // Collect PIC relocations from all workers
         if (_compile_result) {
            for (uint32_t w = 0; w < N; ++w) {
               auto& ws = workers[w];
               if (!ws.codegen) continue;
               const auto& entries = ws.codegen->relocations().entries();
               for (const auto& e : entries) {
                  _compile_result->relocs.push_back(
                     {e.code_offset + static_cast<uint32_t>(ws.final_offset),
                      e.symbol, e.type, e.reserved, e.addend});
               }
            }
         }

         // Patch element table entries
         uint32_t num_imported = _mod.get_imported_functions_size();
         uint32_t num_functions_total = _mod.get_functions_total();
         for (auto& elem : _mod.elements) {
            for (auto& entry : elem.elems) {
               void* addr = nullptr;
               if (entry.index < num_functions_total) {
                  if (entry.index < num_imported) {
                     void* thunk = workers[0].codegen->get_func_addr(entry.index);
                     if (thunk) {
                        size_t thunk_off = static_cast<char*>(thunk) -
                                           static_cast<char*>(workers[0].code_base);
                        addr = _mod.allocator._code_base + workers[0].final_offset + thunk_off;
                     }
                  } else {
                     uint32_t wasm_idx = entry.index - num_imported;
                     addr = _mod.allocator._code_base + _mod.code[wasm_idx].jit_code_offset;
                  }
               }
               if (!addr) {
                  void* handler = workers[0].codegen->get_call_indirect_handler();
                  size_t handler_off = static_cast<char*>(handler) -
                                       static_cast<char*>(workers[0].code_base);
                  addr = _mod.allocator._code_base + workers[0].final_offset + handler_off;
               }
               entry.code_ptr = addr;
            }
         }

#if defined(PSIZAM_JIT_SIGNAL_DIAGNOSTICS) && !defined(PSIZAM_COMPILE_ONLY)
         {
            uint32_t num_imp = _mod.get_imported_functions_size();
            auto* ranges = new jit_func_range[_num_functions];
            for (uint32_t i = 0; i < _num_functions; ++i) {
               auto& body = _mod.code[i];
               ranges[i] = { static_cast<uint32_t>(body.jit_code_offset),
                              body.jit_code_size, i + num_imp };
            }
            jit_func_ranges = ranges;
            jit_func_range_count = _num_functions;
         }
#endif
      }

      void* _merge_final_base = nullptr;  // Shared between phases
#endif
    public:
      void set_compile_result(pzam_compile_result* r) { _compile_result = r; }
#if !defined(__wasi__)
      void set_parse_callback(parse_callback_t cb) { _parse_callback = std::move(cb); }
#endif
   };

   // Architecture-specific aliases
   using ir_writer_x64 = ir_writer_impl<jit_codegen>;
   using ir_writer_a64 = ir_writer_impl<jit_codegen_a64>;

   // Default alias for the native platform
#ifdef __aarch64__
   using ir_writer = ir_writer_a64;
#else
   using ir_writer = ir_writer_x64;
#endif

} // namespace psizam::detail
