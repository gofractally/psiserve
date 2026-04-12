#pragma once

// LLVM AOT codegen writer — subclass of ir_writer that translates IR to LLVM IR,
// then compiles to a relocatable object file via LLVM TargetMachine (AOT).
//
// Per-function compilation: each function gets its own LLVM context/module,
// is translated, optimized, and emitted independently. This bounds peak memory
// to module_metadata + one_function_IR + one_LLVM_module, which is critical
// for wasm32-hosted compilation where address space is limited to 4GB.
//
// Cross-function calls become code_blob_self relocations with negative addends
// encoding the target function index. After all functions are compiled, these
// are resolved to actual code blob offsets.

#include <psizam/ir_writer.hpp>
#include <psizam/llvm_ir_translator.hpp>
#include <psizam/llvm_aot_compiler.hpp>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

namespace psizam {

   class ir_writer_llvm_aot : public ir_writer {
    public:
      ir_writer_llvm_aot(growable_allocator& alloc, std::size_t source_bytes, module& mod,
                         bool enable_backtrace = false, bool stack_limit_is_bytes = false,
                         uint32_t compile_threads = 0)
         : ir_writer_impl(alloc, source_bytes, mod, enable_backtrace, stack_limit_is_bytes, compile_threads)
      {
         _skip_codegen = true;
      }

      // Shadow base class: compile each function via LLVM immediately after parsing.
      // Creates a fresh LLVM module per function, translates, optimizes, emits,
      // then discards — bounding LLVM memory to one function at a time.
      void finalize(function_body& body) {
#if !defined(__wasi__)
         if (_compile_threads > 1) {
            // Parallel mode: skip serial compilation, will be done in destructor
            ir_writer_impl::finalize(body);
            _ir_alloc._offset = _post_array_offset;
            return;
         }
#endif
         if (_compile_result && _func && !_had_error) {
            compile_current_function();
         }
         // Base class clears _func and resets IR allocator (since _skip_codegen,
         // base finalize just clears _func — IR reset done here)
         ir_writer_impl::finalize(body);
         // Reset IR allocator to reclaim this function's IR data
         _ir_alloc._offset = _post_array_offset;
      }

      ~ir_writer_llvm_aot() noexcept {
#ifdef __EXCEPTIONS
         if (std::uncaught_exceptions() > 0) return;
#endif
         if (!_compile_result) return;
         if (_had_error) return;

         if (_compile_result->target_triple.empty()) {
            _compile_result->error = "ir_writer_llvm_aot: target_triple not set on compile_result";
            return;
         }

#if !defined(__wasi__)
         if (_compile_threads > 1 && _parse_callback) {
            parallel_llvm_compile_all();
            return;
         }
#endif

         resolve_cross_function_relocs();

         _compile_result->relocs = std::move(_accumulated_relocs);
         _compile_result->code_blob = std::move(_accumulated_code);
         _compile_result->function_offsets = std::move(_function_offsets);
      }

    private:

      void resolve_cross_function_relocs() {
         // Resolve pending cross-function relocations.
         // Negative addends encode function indices:
         //   Body refs:  addend = -(code_idx + 1)              [range -1..-N]
         //   Entry refs: addend = -(code_idx + 1 + code_count) [range -(N+1)..-(2N)]
         uint32_t code_count = static_cast<uint32_t>(_function_offsets.size());
         for (auto& r : _accumulated_relocs) {
            if (r.symbol == reloc_symbol::code_blob_self && r.addend < 0) {
               uint32_t neg_val = static_cast<uint32_t>(-r.addend);
               if (neg_val <= code_count) {
                  // Body ref: -(code_idx + 1), resolve to wasm_func_N offset
                  uint32_t code_idx = neg_val - 1;
                  if (code_idx < _func_body_offsets.size()) {
                     r.addend = static_cast<int32_t>(_func_body_offsets[code_idx].first);
                  }
               } else if (neg_val <= 2 * code_count) {
                  // Entry ref: -(code_idx + 1 + code_count), resolve to wasm_entry_N offset
                  uint32_t code_idx = neg_val - 1 - code_count;
                  if (code_idx < _function_offsets.size()) {
                     r.addend = static_cast<int32_t>(_function_offsets[code_idx].first);
                  }
               }
            }
         }
      }

#if !defined(__wasi__)
      void parallel_llvm_compile_all() {
         uint32_t N = _compile_threads;
         if (_num_functions == 0) return;
         if (N > _num_functions) N = _num_functions;

         // Per-function results (populated by workers, read by merge)
         struct func_result_t {
            std::vector<uint8_t> code;
            std::vector<code_relocation> relocations;
            std::vector<std::pair<uint32_t, uint32_t>> function_offsets;
            std::vector<std::pair<uint32_t, uint32_t>> body_offsets;
            uint32_t blob_offset = 0;  // set during between phase
         };
         auto func_results = std::make_unique<func_result_t[]>(_num_functions);

         // Per-worker state
         struct worker_state {
            growable_allocator ir_alloc;
            uint64_t max_stack = 0;
            bool had_error = false;
            std::string error;
         };
         auto workers = std::make_unique<worker_state[]>(N);

         // Build translate options (shared, read-only)
         llvm_translate_options topts;
         topts.opt_level        = 2;
         topts.deterministic    = true;
         topts.per_function     = true;
         topts.softfloat        = _compile_result->softfloat;
         topts.enable_backtrace = _compile_result->backtrace;
         const std::string& target_triple = _compile_result->target_triple;

         auto& reactor = get_compile_reactor(N);

         // Initialize function/body offset arrays
         _function_offsets.resize(_num_functions, {0, 0});
         _func_body_offsets.resize(_num_functions, {0, 0});

         std::atomic<bool> any_error{false};

         reactor.run_batch(
            _num_functions,

            // work_fn: parse + translate + LLVM compile one function
            [this, &workers, &func_results, &topts, &target_triple, &any_error]
            (uint32_t worker_id, uint32_t func_idx) {
               if (any_error.load(std::memory_order_relaxed)) return;
               auto& ws = workers[worker_id];

               // Lazy init worker IR allocator
               if (!ws.ir_alloc._base) {
#if defined(PSIZAM_COMPILE_ONLY) && !defined(__LP64__)
                  // WASM32: fixed allocation sized for largest function
                  ws.ir_alloc.use_fixed_memory(growable_allocator::align_to_page(
                     _max_func_bytes * 250 + 1024 * 1024));
#else
                  // Native or 64-bit: demand-paged, only commits what's used
                  ws.ir_alloc.use_default_memory();
#endif
               }

               // Create per-function worker writer for parsing
               ir_writer_impl worker_writer(_mod, _functions, _num_functions,
                                           ws.ir_alloc, _enable_backtrace, _stack_limit_is_bytes);

               // Parse WASM bytecodes → IR
               _parse_callback(func_idx, worker_writer, ws.max_stack);

               auto& func = _functions[func_idx];

               // Translate IR → LLVM IR → native code
               auto do_compile = [&]() {
                  llvm_ir_translator translator(get_ir_module(), topts);
                  translator.translate_function(func);
                  translator.finalize();

                  auto llvm_mod = translator.take_module();
                  auto llvm_ctx = translator.take_context();
                  auto result = llvm_aot_compile(std::move(llvm_mod), std::move(llvm_ctx),
                                                  get_ir_module(), target_triple);

                  if (!result.error.empty()) {
                     ws.had_error = true;
                     ws.error = std::move(result.error);
                     any_error.store(true, std::memory_order_relaxed);
                     return;
                  }

                  func_results[func_idx].code = std::move(result.code);
                  func_results[func_idx].relocations = std::move(result.relocations);
                  func_results[func_idx].function_offsets = std::move(result.function_offsets);
                  func_results[func_idx].body_offsets = std::move(result.body_offsets);
               };

#ifdef __EXCEPTIONS
               try {
                  do_compile();
               } catch (const std::exception& ex) {
                  ws.had_error = true;
                  ws.error = ex.what();
                  any_error.store(true, std::memory_order_relaxed);
               } catch (...) {
                  ws.had_error = true;
                  ws.error = "unknown error during parallel LLVM AOT compilation";
                  any_error.store(true, std::memory_order_relaxed);
               }
#else
               do_compile();
#endif

               // Reset IR allocator — only 1 function's IR alive at a time
               ws.ir_alloc._offset = 0;
            },

            // between_fn: compute prefix sums, set blob offsets
            [this, &workers, &func_results, N]() {
               // Check for errors
               for (uint32_t w = 0; w < N; ++w) {
                  if (workers[w].had_error) {
                     _compile_result->error = std::move(workers[w].error);
                     _had_error = true;
                     return;
                  }
               }

               // Compute total code size and per-function offsets (prefix sum)
               size_t total_size = 0;
               for (uint32_t f = 0; f < _num_functions; ++f) {
                  // Align to 16 bytes
                  size_t align_pad = (16 - (total_size % 16)) % 16;
                  total_size += align_pad;
                  func_results[f].blob_offset = static_cast<uint32_t>(total_size);
                  total_size += func_results[f].code.size();
               }

               // Pre-allocate merged code blob
               _accumulated_code.resize(total_size, 0x00);

               // Set function/body offsets in merged blob
               for (uint32_t f = 0; f < _num_functions; ++f) {
                  uint32_t blob_offset = func_results[f].blob_offset;
                  if (f < func_results[f].function_offsets.size()) {
                     auto [entry_off, entry_sz] = func_results[f].function_offsets[f];
                     _function_offsets[f] = {blob_offset + entry_off, entry_sz};
                  }
                  if (f < func_results[f].body_offsets.size()) {
                     auto [body_off, body_sz] = func_results[f].body_offsets[f];
                     _func_body_offsets[f] = {blob_offset + body_off, body_sz};
                  }
               }

               // Merge per-worker max_stack
               for (uint32_t w = 0; w < N; ++w) {
                  _mod.maximum_stack = std::max(_mod.maximum_stack, workers[w].max_stack);
               }
            },

            // merge_fn: parallel copy + reloc adjustment
            [this, &func_results](uint32_t worker_id) {
               if (_had_error) return;
               // Each worker copies a stripe of functions
               uint32_t funcs_per_worker = (_num_functions + _compile_threads - 1) / _compile_threads;
               uint32_t start = worker_id * funcs_per_worker;
               uint32_t end = std::min(start + funcs_per_worker, _num_functions);

               for (uint32_t f = start; f < end; ++f) {
                  auto& fr = func_results[f];
                  if (fr.code.empty()) continue;

                  // Copy this function's code into merged blob
                  std::memcpy(_accumulated_code.data() + fr.blob_offset,
                              fr.code.data(), fr.code.size());

                  // Adjust relocations and accumulate
                  for (auto& reloc : fr.relocations) {
                     reloc.code_offset += fr.blob_offset;
                     if (reloc.symbol == reloc_symbol::code_blob_self && reloc.addend >= 0) {
                        reloc.addend += static_cast<int32_t>(fr.blob_offset);
                     }
                  }
               }
            }
         ); // end run_batch

         if (_had_error) return;

         // Collect all relocations (serial — order matters for determinism)
         for (uint32_t f = 0; f < _num_functions; ++f) {
            for (auto& reloc : func_results[f].relocations) {
               _accumulated_relocs.push_back(std::move(reloc));
            }
         }

         // Resolve cross-function relocations
         resolve_cross_function_relocs();

         _compile_result->relocs = std::move(_accumulated_relocs);
         _compile_result->code_blob = std::move(_accumulated_code);
         _compile_result->function_offsets = std::move(_function_offsets);
      }
#endif // !__wasi__

      void compile_current_function() {
         if (_compile_result->target_triple.empty()) {
            _compile_result->error = "ir_writer_llvm_aot: target_triple not set";
            _had_error = true;
            return;
         }

         // Initialize function offsets on first call
         if (_function_offsets.empty()) {
            _function_offsets.resize(_num_functions, {0, 0});
            _func_body_offsets.resize(_num_functions, {0, 0});
         }

         // Create per-function LLVM translator + module
         llvm_translate_options topts;
         topts.opt_level        = 2;
         topts.deterministic    = true;
         topts.per_function     = true;
         topts.softfloat        = _compile_result->softfloat;
         topts.enable_backtrace = _compile_result->backtrace;

         auto do_compile = [&]() {
            llvm_ir_translator translator(get_ir_module(), topts);
            translator.translate_function(*_func);
            translator.finalize();

            auto llvm_mod = translator.take_module();
            auto llvm_ctx = translator.take_context();
            auto func_result = llvm_aot_compile(std::move(llvm_mod), std::move(llvm_ctx),
                                                 get_ir_module(), _compile_result->target_triple);

            if (!func_result.error.empty()) {
               _compile_result->error = std::move(func_result.error);
               _had_error = true;
               return;
            }

            // Align to 16 bytes before appending this function's code
            size_t align_pad = (16 - (_accumulated_code.size() % 16)) % 16;
            if (align_pad > 0) {
               _accumulated_code.resize(_accumulated_code.size() + align_pad, 0x00);
            }
            uint32_t blob_offset = static_cast<uint32_t>(_accumulated_code.size());

            _accumulated_code.insert(_accumulated_code.end(),
                                      func_result.code.begin(), func_result.code.end());

            // Adjust relocation offsets and addends relative to merged blob
            for (auto& reloc : func_result.relocations) {
               reloc.code_offset += blob_offset;
               // code_blob_self addends reference positions within the per-function
               // blob and must be adjusted for the merged blob position.
               // Negative addends encode pending function refs (don't adjust those).
               if (reloc.symbol == reloc_symbol::code_blob_self && reloc.addend >= 0) {
                  reloc.addend += static_cast<int32_t>(blob_offset);
               }
               _accumulated_relocs.push_back(reloc);
            }

            // Record this function's entry wrapper and body offsets in the merged blob.
            // llvm_aot_compile returns per-function offsets relative to .text;
            // add blob_offset to get the absolute position in the merged blob.
            uint32_t code_idx = _func->func_index;
            if (code_idx < _function_offsets.size() &&
                code_idx < func_result.function_offsets.size()) {
               auto [entry_off, entry_sz] = func_result.function_offsets[code_idx];
               _function_offsets[code_idx] = {blob_offset + entry_off, entry_sz};
            }
            if (code_idx < _func_body_offsets.size() &&
                code_idx < func_result.body_offsets.size()) {
               auto [body_off, body_sz] = func_result.body_offsets[code_idx];
               _func_body_offsets[code_idx] = {blob_offset + body_off, body_sz};
            }
         };

#ifdef __EXCEPTIONS
         try {
            do_compile();
         } catch (const std::exception& ex) {
            _compile_result->error = ex.what();
            _had_error = true;
         } catch (...) {
            _compile_result->error = "unknown error during LLVM AOT compilation";
            _had_error = true;
         }
#else
         do_compile();
#endif
      }

      std::vector<uint8_t> _accumulated_code;
      std::vector<code_relocation> _accumulated_relocs;
      std::vector<std::pair<uint32_t, uint32_t>> _function_offsets;  // wasm_entry_N offsets
      std::vector<std::pair<uint32_t, uint32_t>> _func_body_offsets; // wasm_func_N offsets
      bool _had_error = false;
   };

} // namespace psizam
