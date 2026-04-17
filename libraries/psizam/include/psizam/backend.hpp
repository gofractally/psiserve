#pragma once

//
// BACKEND FEATURE PARITY TRACKER
// Legend: 🟢 done  🟡 partial  🔴 missing  🐛 bug
//
//                              INTERP JIT(x86) JIT2(x86) JIT(a64) JIT2(a64) LLVM
//                              ────── ──────── ───────── ──────── ───────── ────
// MULTI-VALUE RETURNS
//   parse multi-value types      🟢     🟢       🟢        🟢       🟢      🟢
//   function returns (N>1)       🟢     🟢       🟢        🟢       🟢      🟢
//   multi-value block types      🟢     🟢       🟢        🟢       🟢      🟢
//   branch encoding (N vals)     🟢     🟢       🟢        🟢       🟢      🟢
//
// MULTI-TABLE
//   call_indirect w/ table       🟢     🟢       🟢        🟢       🟢      🟢
//   table.copy multi-table       🟢     🟢       🟢        🟢       🟢      🟢
//   table.init multi-table       🟢     🟢       🟢        🟢       🟢      🟢
//   elem.drop                    🟢     🟢       🟢        🟢       🟢      🟢
//
// TABLE ELEMENT OPS
//   table.get                    🟢     🟢       🟢        🟢       🟢      🟢
//   table.set                    🟢     🟢       🟢        🟢       🟢      🟢
//   table.grow                   🟢     🟢       🟢        🟢       🟢      🟢
//   table.size                   🟢     🟢       🟢        🟢       🟢      🟢
//   table.fill                   🟢     🟢       🟢        🟢       🟢      🟢
//
// BULK MEMORY
//   memory.init                  🟢     🟢       🟢        🟢       🟢      🟢
//   memory.copy                  🟢     🟢       🟢        🟢       🟢      🟢
//   memory.fill                  🟢     🟢       🟢        🟢       🟢      🟢
//   data.drop                    🟢     🟢       🟢        🟢       🟢      🟢
//
// SIMD (v128)
//   full v128 ops                🟢     🟢       🟢        🟢       🟢      🟢
//
// REFERENCE TYPES
//   ref.null / ref.is_null       🟢     🟢       🟢        🟢       🟢      🟢
//   ref.func                     🟢     🟢       🟢        🟢       🟢      🟢
//   externref                    🟢     🟢       🟢        🟢       🟢      🟢
//
// SIGN EXTENSION OPS
//   i32_extend8/16_s             🟢     🟢       🟢        🟢       🟢      🟢
//   i64_extend8/16/32_s          🟢     🟢       🟢        🟢       🟢      🟢
//
// NONTRAPPING FLOAT-TO-INT
//   trunc_sat_*                  🟢     🟢       🟢        🟢       🟢      🟢
//
// MUTABLE GLOBALS IMPORT/EXPORT
//   mutable global import        🟢     🟢       🟢        🟢       🟢      🟢
//
// EXCEPTION HANDLING (v2)
//   try_table/throw/throw_ref    🟢     🟢       🟢        🟢       🟢      🟢
//
// THREADS / ATOMICS
//   atomic ops                   🟢     🟢       🟢        🟢       🟢      🟢
//   shared memory                🟢     🟢       🟢        🟢       🟢      🟢
//
// TAIL CALLS
//   return_call                  🟢     🟢       🟢        🟢       🟢      🟢
//   return_call_indirect         🟢     🟢       🟢        🟢       🟢      🟢
//
// MEMORY64
//   i64 addressing               🔴     🔴       🔴        🔴       🔴      🔴
//
// GC PROPOSAL
//   struct/array types           🔴     🔴       🔴        🔴       🔴      🔴
//
// RELAXED SIMD
//   relaxed simd ops             🟢     🟢       🟢        🟢       🟢      🟢
//
// MEMORY BOUNDS CHECKING
//   OOB traps                    🟢     🟢       🟢        🟢       🟢      🟢
//
// CLANG COMPATIBILITY
//   builds with Clang            🟢     🟢       🟢        🟢       🟢      🟢
//   Rosetta 2 (x86 on arm64)    n/a     🟢       🟢        n/a      n/a     n/a
//
// RUNTIME PROPERTIES
//                              INTERP JIT(x86) JIT2(x86) JIT(a64) JIT2(a64) LLVM
//                              ────── ──────── ───────── ──────── ───────── ────
// STACK ISOLATION
//   dedicated WASM stack         🟢     🟢       🟢        🟢       🟢      🟢
//   guard page detection         🟢     🟢       🟢        🟢       🟢      🟢
//   Note: interp/jit/jit2 use a managed operand stack (flat array), so all
//         WASM calls run in a single C stack frame. LLVM compiles each WASM
//         function to a native function with real C stack frames; it uses
//         call_on_stack() to switch to a dedicated mmap'd stack with a guard
//         page at the bottom. SIGSEGV on the guard page is caught by the
//         signal handler and converted to a WASM stack overflow trap.
//
// CALL DEPTH TRACKING
//   bounded call depth           🟢     🟢       🟢        🟢       🟢      🟢
//   Note: interp/jit/jit2 track depth via a counter in execution_context
//         (register-allocated in jit/jit2). LLVM uses __psizam_call_depth_dec/inc
//         runtime helpers in function prologues/epilogues.
//
// MEMORY SANDBOXING
//   guard-page OOB traps         🟢     🟢       🟢        🟢       🟢      🟢
//   Note: All backends use guard-page-based memory sandboxing via wasm_allocator.
//         OOB accesses hit guard pages → SIGSEGV → signal handler → trap.
//
// DETERMINISTIC EXECUTION
//   softfloat support            🟢     🟢       🟢        🟢       🟢      🔴
//   Note: LLVM softfloat integration is Phase 4c (pending).
//
// KNOWN BUGS 🐛
//   [B1] Parser rejects table/memory/global imports (~22 failures per backend)
//
// SPEC TEST RESULTS (per backend, excluding B1 parser import failures):
//   Interpreter:    21 failures (arm64), 21 failures (x86_64 Rosetta)
//   JIT(x86):       21 failures (x86_64 Rosetta) — zero JIT-specific failures
//   JIT2(x86):      21 failures (x86_64 Rosetta) — zero JIT2-specific failures
//   JIT(a64):       23 failures (arm64 native)
//   JIT2(a64):      23 failures (arm64 native)
//   LLVM:           23 failures (arm64 native)
//   Note: x86_64 has ~473 extra "wasm file not found" due to incomplete test
//         wasm generation in x86 cross-build — not real backend failures.
//
// Last updated: 2026-04-10
//

#include <psizam/allocator.hpp>
#include <psizam/detail/bitcode_writer.hpp>
#include <psizam/config.hpp>
#include <psizam/detail/debug_visitor.hpp>
#include <psizam/detail/execution_context.hpp>
#include <psizam/detail/interpret_visitor.hpp>
#include <psizam/detail/null_writer.hpp>
#include <psizam/detail/parser.hpp>
#include <psizam/types.hpp>

#include <psizam/detail/x86_64.hpp>
#include <psizam/detail/aarch64.hpp>
#include <psizam/detail/ir_writer.hpp>
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
#include <psizam/detail/ir_writer_llvm.hpp>
#endif

#include <atomic>
#include <exception>
#include <iostream>
#include <optional>
#include <string_view>
#include <system_error>
#include <vector>

namespace psizam {

#if defined(__x86_64__) || defined(__aarch64__)
   struct jit {
      using context = detail::jit_execution_context<false>;
      template<typename Options, typename DebugInfo>
      using parser = detail::binary_parser<detail::machine_code_writer, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = false;
   };

   struct jit_profile {
      using context = detail::jit_execution_context<true>;
      template<typename Options, typename DebugInfo>
      using parser = detail::binary_parser<detail::machine_code_writer, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = true;
   };

#endif

#if defined(__x86_64__) || defined(__aarch64__)
   struct jit2 {
      using context = detail::jit_execution_context<false>;
      template<typename Options, typename DebugInfo>
      using parser = detail::binary_parser<detail::ir_writer, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = false;
   };
#endif

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
   struct jit_llvm {
      using context = detail::jit_execution_context<false>;
      template<typename Options, typename DebugInfo>
      using parser = detail::binary_parser<detail::ir_writer_llvm, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = false;
   };
#endif

   struct interpreter {
      using context = detail::execution_context;
      template<typename Options, typename DebugInfo>
      using parser = detail::binary_parser<detail::bitcode_writer, Options, DebugInfo>;
      static constexpr bool is_jit = false;
      static constexpr bool enable_backtrace = false;
   };

   struct null_backend {
      using context = detail::null_execution_context;
      template<typename Options, typename DebugInfo>
      using parser = detail::binary_parser<detail::null_writer, Options, DebugInfo>;
      static constexpr bool is_jit = false;
      static constexpr bool enable_backtrace = false;
   };

   template <typename T>
   struct maybe_unique_ptr {
      maybe_unique_ptr(T* ptr = nullptr, bool owns = true) : ptr(ptr), owns(owns) {}
      maybe_unique_ptr(const maybe_unique_ptr&) = delete;
      maybe_unique_ptr& operator=(const maybe_unique_ptr&) = delete;
      maybe_unique_ptr(maybe_unique_ptr&& o) noexcept : ptr(o.ptr), owns(o.owns) { o.ptr = nullptr; o.owns = false; }
      maybe_unique_ptr& operator=(maybe_unique_ptr&& o) noexcept {
         if (this != &o) {
            if (ptr && owns) delete ptr;
            ptr = o.ptr; owns = o.owns;
            o.ptr = nullptr; o.owns = false;
         }
         return *this;
      }
      ~maybe_unique_ptr() {
         if (ptr && owns)
            delete ptr;
      }
      T& operator*() const { return *ptr; }
      T* operator->() const { return ptr; }
      T* get() const { return ptr; }
      void reset(T* new_ptr, bool new_owns = true) {
         if (ptr && owns)
            delete ptr;
         this->ptr = new_ptr;
         this->owns = new_owns;
      }
   private:
      T* ptr;
   public:
      bool owns;
   };

   template <typename HostFunctions = std::nullptr_t, typename Impl = interpreter, typename Options = default_options, typename DebugInfo = detail::null_debug_info>
   class backend {
      using host_t     = detail::host_type_t<HostFunctions>;
      using context_t  = typename Impl::context;
      using parser_t   = typename Impl::template parser<Options, DebugInfo>;
      template<typename XDebugInfo>
      using parser_tpl   = typename Impl::template parser<Options, XDebugInfo>;

      // Build a host_function_table from the legacy registered_host_functions<HostFunctions>.
      // The fast_host_trampoline_t<Cls> = native_value(*)(Cls*, native_value*, char*) has the
      // same ABI as host_trampoline_t = native_value(*)(void*, native_value*, char*), so we
      // reinterpret_cast the existing fast_fwd trampolines directly.
      // For non-fast-eligible functions (custom type converters, pointer/reference params),
      // a slow_dispatch closure rebuilds the operand stack and calls through the full
      // type conversion pipeline.
      static host_function_table build_host_table() {
         if constexpr (std::is_same_v<HostFunctions, std::nullptr_t>) {
            return {};
         } else {
            using host_cls = detail::host_type_t<HostFunctions>;
            using tc_type = detail::type_converter_t<HostFunctions>;
            auto& mappings = HostFunctions::mappings::get();
            uint32_t count = static_cast<uint32_t>(mappings.host_functions.size());

            // Build entries in index order (0..count-1)
            std::vector<host_function_table::entry> entries(count);
            for (auto& [key, idx] : mappings.named_mapping) {
               entries[idx].module_name = key.first;
               entries[idx].func_name   = key.second;
               entries[idx].signature   = mappings.host_functions[idx];
               auto fwd = mappings.fast_fwd[idx];
               entries[idx].raw_func_ptr = (idx < mappings.raw_ptrs.size()) ? mappings.raw_ptrs[idx] : nullptr;
               if (fwd) {
                  entries[idx].trampoline = reinterpret_cast<host_trampoline_t>(fwd);
               } else {
                  // Non-fast-eligible: build a slow_dispatch that reconstructs
                  // the operand stack and calls through the full type conversion pipeline.
                  entries[idx].trampoline = nullptr;
                  entries[idx].slow_dispatch = [idx](void* host, native_value* args, char* memory) -> native_value {
                     auto& m = HostFunctions::mappings::get();
                     auto& fn = m.functions[idx];
                     auto& sig = m.host_functions[idx];

                     // Build operand stack from forward-order native_value args
                     detail::operand_stack temp_stack;
                     for (uint32_t i = 0; i < sig.params.size(); i++) {
                        temp_stack.push(i64_const_t{args[i].i64});
                     }

                     execution_interface ei{memory, &temp_stack};
                     tc_type tc{static_cast<host_cls*>(host), std::move(ei)};

                     fn(static_cast<host_cls*>(host), tc);

                     // Extract result if present
                     if (!sig.ret.empty() && temp_stack.size() > 0) {
                        auto result_elem = temp_stack.pop();
                        native_value nv{uint64_t{0}};
                        visit(overloaded{
                           [&](i32_const_t v) { nv.i32 = v.data.ui; },
                           [&](i64_const_t v) { nv.i64 = v.data.ui; },
                           [&](f32_const_t v) { nv.f32 = v.data.f; },
                           [&](f64_const_t v) { nv.f64 = v.data.f; },
                           [&](auto) {}
                        }, result_elem);
                        return nv;
                     }
                     return native_value{uint64_t{0}};
                  };
               }
            }

            return host_function_table(std::move(entries), mappings.named_mapping);
         }
      }

      void construct(host_t* host=nullptr) {
         mod->finalize();
         if (ctx.owns) {
            ctx->set_wasm_allocator(memory_alloc);
         }
         // Now data required by JIT is finalized; create JIT module
         // such that memory used in parsing can be released.
         if constexpr (Impl::is_jit) {
            assert(mod->allocator._base == nullptr);
         }
         // Resolve host imports (globals, tables, functions) before initializing
         // globals — imported global values must be set before evaluate().
         if constexpr (!std::is_same_v<HostFunctions, std::nullptr_t>) {
            HostFunctions::resolve(*mod);
         }
         if (ctx.owns) {
            ctx->initialize_globals();
         }
         _host_table = build_host_table();
         ctx->set_host_table(&_host_table);

         // Build direct C function pointer and trampoline pointer arrays for JIT host calls.
         // Only JIT contexts (which inherit frame_info_holder) have these fields.
         if constexpr (requires { ctx->_host_direct_ptrs; }) {
            uint32_t num_imports = mod->get_imported_functions_size();
            _direct_ptrs.resize(num_imports, nullptr);
            _trampoline_ptrs.resize(num_imports, nullptr);
            for (uint32_t i = 0; i < num_imports; i++) {
               uint32_t mapped = mod->import_functions[i];
               if (mapped < _host_table.size()) {
                  auto& entry = _host_table.get_entry(mapped);
                  _direct_ptrs[i] = entry.raw_func_ptr;
                  // Prefer reverse-order trampoline for JIT (zero-copy stack pass).
                  // Fall back to forward-order trampoline for functions with custom type converters.
                  _trampoline_ptrs[i] = entry.rev_trampoline ? entry.rev_trampoline : entry.trampoline;
               }
            }
            ctx->_host_direct_ptrs = _direct_ptrs.data();
            ctx->_host_trampoline_ptrs = _trampoline_ptrs.data();
         }

         // FIXME: should not hard code knowledge of null_backend here
         if (ctx.owns) {
            if constexpr (!std::is_same_v<Impl, null_backend>)
               initialize(host);
         }
      }
    public:
      backend() {}
      backend(wasm_code&& code, host_t& host, wasm_allocator* alloc, const Options& options = Options{})
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(new context_t{parse_module(code, options), detail::choose_stack_limit(options)}), mod_sharable{true} {
         ctx->set_max_pages(detail::get_max_pages(options));
         construct(&host);
      }
      backend(wasm_code&& code, wasm_allocator* alloc, const Options& options = Options{})
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(new context_t{parse_module(code, options), detail::choose_stack_limit(options)}), mod_sharable{true} {
         ctx->set_max_pages(detail::get_max_pages(options));
         construct();
      }
      backend(wasm_code& code, host_t& host, wasm_allocator* alloc, const Options& options = Options{})
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(new context_t{parse_module(code, options), detail::choose_stack_limit(options)}), mod_sharable{true} {
         ctx->set_max_pages(detail::get_max_pages(options));
         construct(&host);
      }
      backend(wasm_code& code, wasm_allocator* alloc, const Options& options = Options{})
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(new context_t{(parse_module(code, options)), detail::choose_stack_limit(options)}), mod_sharable{true} {
         ctx->set_max_pages(detail::get_max_pages(options));
         construct();
      }
      /// Construct with a pre-built host_function_table.
      /// The table resolves imports directly; no registered_host_functions needed.
      /// Use call(void* host, func_name) to invoke exports.
      backend(wasm_code& code, host_function_table table, void* host, wasm_allocator* alloc, const Options& options = Options{})
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(new context_t{parse_module(code, options), detail::choose_stack_limit(options)}), mod_sharable{true}, _host_table(std::move(table)) {
         ctx->set_max_pages(detail::get_max_pages(options));
         mod->finalize();
         if (ctx.owns) {
            ctx->set_wasm_allocator(memory_alloc);
         }
         _host_table.resolve(*mod);
         ctx->set_host_table(&_host_table);
         if (ctx.owns) {
            ctx->initialize_globals();
            if constexpr (!std::is_same_v<Impl, null_backend>) {
               if (memory_alloc) {
                  ctx->reset();
                  ctx->execute_start(host, detail::interpret_visitor(*ctx));
               }
            }
         }
      }
      template <typename XDebugInfo>
      backend(wasm_code& code, wasm_allocator* alloc, const Options& options, XDebugInfo& debug)
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(new context_t{(parse_module(code, options, debug)), detail::choose_stack_limit(options)}), mod_sharable{true} {
         ctx->set_max_pages(detail::get_max_pages(options));
         construct();
      }
      backend(wasm_code_ptr& ptr, size_t sz, host_t& host, wasm_allocator* alloc, const Options& options = Options{})
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(new context_t{parse_module2(ptr, sz, options, true), detail::choose_stack_limit(options)}), mod_sharable{true} { // single parsing. original behavior {
         ctx->set_max_pages(detail::get_max_pages(options));
         construct(&host);
      }
      // Leap:
      //  * Contract validation only needs single parsing as the instantiated module is not cached.
      //  * JIT execution needs single parsing only.
      //  * Interpreter execution requires two-passes parsing to prevent memory mappings exhaustion
      //  * Leap reuses execution context per thread; ctx.owns is set
      //  to false when a backend is constructued
      backend(wasm_code_ptr& ptr, size_t sz, wasm_allocator* alloc, const Options& options = Options{}, bool single_parsing = true, bool exec_ctx_by_backend = true)
         : memory_alloc(alloc), mod(std::make_shared<module>()), ctx(nullptr, exec_ctx_by_backend), mod_sharable{true}, initial_max_call_depth(detail::choose_stack_limit(options)), initial_max_pages(detail::get_max_pages(options)) {
         if (ctx.owns) {
            ctx.reset(new context_t{parse_module2(ptr, sz, options, single_parsing), initial_max_call_depth});
            ctx->set_max_pages(initial_max_pages);
         } else {
            parse_module2(ptr, sz, options, single_parsing);
         }
         construct();
      }

      void check_deferred_llvm_exception() {
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
         if constexpr (std::is_same_v<Impl, jit_llvm>) {
            if (detail::llvm_deferred_exception) {
               auto ex = detail::llvm_deferred_exception;
               detail::llvm_deferred_exception = nullptr;
               std::rethrow_exception(ex);
            }
         }
#endif
      }

      module& parse_module(wasm_code& code, const Options& options) {
         mod->allocator.use_default_memory();
         auto& result = parser_t{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module(code, *mod, debug);
         check_deferred_llvm_exception();
         return result;
      }

      template <typename XDebugInfo>
      module& parse_module(wasm_code& code, const Options& options, XDebugInfo& debug) {
         mod->allocator.use_default_memory();
         auto& result = parser_tpl<XDebugInfo>{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module(code, *mod, debug);
         check_deferred_llvm_exception();
         return result;
      }

      module& parse_module2(wasm_code_ptr& ptr, size_t sz, const Options& options, bool single_parsing) {
         if (single_parsing) {
            mod->allocator.use_default_memory();
            auto& result = parser_t{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module2(ptr, sz, *mod, debug);
            check_deferred_llvm_exception();
            return result;
         } else {
            // To prevent large number of memory mappings used, two-passes of
            // parsing are performed.
            wasm_code_ptr orig_ptr = ptr;
            size_t largest_size = 0;

            // First pass: finds max size of memory required by parsing.
            {
               // Memory used by this pass is freed when going out of the scope
               psizam::module first_pass_module;
               first_pass_module.allocator.use_default_memory();
               parser_t{ first_pass_module.allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module2(ptr, sz, first_pass_module, debug);
               check_deferred_llvm_exception();
               first_pass_module.finalize();
               largest_size = first_pass_module.allocator.largest_used_size();
            }

            // Second pass: uses actual required memory for final parsing
            mod->allocator.use_fixed_memory(largest_size);
            auto& result = parser_t{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module2(orig_ptr, sz, *mod, debug);
            check_deferred_llvm_exception();
            return result;
         }
      }

      // Shares compiled module with another backend which never compiles
      // module itself.
      void share(const backend& from) {
         assert(from.mod_sharable);  // `from` backend's mod is sharable
         assert(!mod_sharable); // `to` backend's mod must not be sharable
         mod                          = from.mod;
         ctx.owns  = from.ctx.owns;
         initial_max_call_depth = from.initial_max_call_depth;
         initial_max_pages      = from.initial_max_pages;
      }

      void set_context(context_t* ctx_ptr) {
         // ctx cannot be set if it is created by the backend
         assert(!ctx.owns);
         ctx.reset(ctx_ptr, false);
      }

      inline void reset_max_call_depth() {
         // max_call_depth cannot be reset if ctx is created by the backend
         assert(!ctx.owns);
         ctx->set_max_call_depth(initial_max_call_depth);
      }

      inline void reset_max_pages() {
         // max_pages cannot be reset if ctx is created by the backend
         assert(!ctx.owns);
         ctx->set_max_pages(initial_max_pages);
      }

      // ── Per-instance floating-point execution mode ──
      //
      // Interpreter: read at every FP opcode; setter takes effect immediately.
      //
      // JIT backends (jit, jit_profile, jit2, jit_llvm): the mode is baked
      // into emitted code at construction. Post-construction changes cannot
      // re-emit code, so set_fp_mode is only accepted if the requested mode
      // matches the baked mode (a no-op). A mismatching setter throws to
      // surface silent consensus hazards — callers must reconstruct the
      // backend with the desired mode wired at construction time.
      inline void set_fp_mode(fp_mode m) {
         if constexpr (Impl::is_jit) {
            PSIZAM_ASSERT(m == ctx->fp(), wasm_interpreter_exception,
                          "set_fp_mode on a JIT backend cannot change the baked fp_mode; "
                          "reconstruct the backend with the desired mode");
         }
         ctx->set_fp_mode(m);
      }
      inline fp_mode get_fp_mode() const { return ctx->fp(); }

      template <typename... Args>
      inline auto operator()(detail::stack_manager& alt_stack, host_t& host, const std::string_view& mod, const std::string_view& func, Args... args) {
         return call(alt_stack, host, mod, func, args...);
      }

      template <typename... Args>
      inline auto operator()(host_t& host, const std::string_view& mod, const std::string_view& func, Args... args) {
         return call(host, mod, func, args...);
      }

      template <typename... Args>
      inline bool operator()(const std::string_view& mod, const std::string_view& func, Args... args) {
         return call(mod, func, args...);
      }

      // Only dynamic options matter.  Parser options will be ignored.
      inline backend& initialize(host_t* host, const Options& new_options) {
         ctx->set_max_call_depth(detail::choose_stack_limit(new_options));
         ctx->set_max_pages(detail::get_max_pages(new_options));
         initialize(host);
         return *this;
      }

      inline backend& initialize(host_t* host=nullptr) {
         if (memory_alloc) {
            ctx->reset();
            ctx->execute_start(host, detail::interpret_visitor(*ctx));
         }
         return *this;
      }

      inline backend& initialize(detail::stack_manager& alt_stack, host_t* host=nullptr) {
         if (memory_alloc) {
            ctx->reset();
            ctx->execute_start(alt_stack, host, detail::interpret_visitor(*ctx));
         }
         return *this;
      }

      inline backend& initialize(host_t& host) {
         return initialize(&host);
      }

      using tc_t = detail::type_converter_t<HostFunctions>;

      template <typename... Args>
      inline bool call_indirect(host_t* host, uint32_t func_index, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute_func_table<tc_t>(host, detail::debug_visitor(*ctx), func_index, std::forward<Args>(args)...);
         } else {
            ctx->template execute_func_table<tc_t>(host, detail::interpret_visitor(*ctx), func_index, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(host_t* host, uint32_t func_index, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(host, detail::debug_visitor(*ctx), func_index, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(host, detail::interpret_visitor(*ctx), func_index, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(detail::stack_manager& alt_stack, host_t& host, const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(alt_stack, &host, detail::debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(alt_stack, &host, detail::interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(host_t& host, const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(&host, detail::debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(&host, detail::interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(nullptr, detail::debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(nullptr, detail::interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
         return true;
      }

      /// Call an exported function by name with an opaque host pointer.
      /// The host pointer is forwarded to host function trampolines as void*.
      template <typename... Args>
      inline bool call(void* host, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(host, detail::debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(host, detail::interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline auto call_with_return(host_t& host, const std::string_view& mod, const std::string_view& func, Args&&... args ) {
         if constexpr (psizam_debug) {
            return ctx->template execute<tc_t>(&host, detail::debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            return ctx->template execute<tc_t>(&host, detail::interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
      }

      template <typename... Args>
      inline auto call_with_return(const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            return ctx->template execute<tc_t>(nullptr, detail::debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            return ctx->template execute<tc_t>(nullptr, detail::interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
      }

      template<typename Watchdog, typename F>
      inline void timed_run(Watchdog&& wd, F&& f) {
         //timed_run_has_timed_out -- declared in signal handling code because signal handler needs to inspect it on a SEGV too -- is a thread local
         // so that upon a SEGV the signal handling code can discern if the thread that caused the SEGV has a timed_run that has timed out. This
         // thread local also need to be an atomic because the thread that a Watchdog callback will be called from may not be the same as the
         // executing thread.
         std::atomic<bool>&      _timed_out = detail::timed_run_has_timed_out;
         auto reenable_code = scope_guard{[&](){
            if (_timed_out.load(std::memory_order_acquire)) {
               mod->allocator.enable_code(Impl::is_jit);
               _timed_out.store(false, std::memory_order_release);
            }
         }};
         try {
            auto wd_guard = std::forward<Watchdog>(wd).scoped_run([this,&_timed_out]() {
               _timed_out.store(true, std::memory_order_release);
               mod->allocator.disable_code();
            });
            std::forward<F>(f)();
         } catch(wasm_memory_exception&) {
            if (_timed_out.load(std::memory_order_acquire)) {
               throw timeout_exception{ "execution timed out" };
            } else {
               throw;
            }
         }
      }

      template <typename Watchdog>
      inline void execute_all(Watchdog&& wd, host_t& host) {
         timed_run(std::forward<Watchdog>(wd), [&]() {
            for (int i = 0; i < mod->exports.size(); i++) {
               if (mod->exports[i].kind == external_kind::Function) {
                  std::string s{ (const char*)mod->exports[i].field_str.data(), mod->exports[i].field_str.size() };
                  ctx->execute(&host, detail::interpret_visitor(*ctx), s);
               }
            }
         });
      }

      template <typename Watchdog>
      inline void execute_all(Watchdog&& wd) {
         timed_run(std::forward<Watchdog>(wd), [&]() {
            for (int i = 0; i < mod->exports.size(); i++) {
               if (mod->exports[i].kind == external_kind::Function) {
                  std::string s{ (const char*)mod->exports[i].field_str.data(), mod->exports[i].field_str.size() };
                  ctx->execute(nullptr, detail::interpret_visitor(*ctx), s);
               }
            }
         });
      }

      // =====================================================================
      // Typed function API — compile-time signature, minimal per-call cost
      // =====================================================================

      /// A lightweight callable that invokes a JIT-compiled WASM function with
      /// minimal overhead.  The signal handler and alternate stack must be set
      /// up externally (via scoped_execution) — this does only arg marshaling
      /// and the native call.
      template<typename Sig> class typed_function;

      template<typename R, typename... Args>
      class typed_function<R(Args...)> {
         using jit_ctx = detail::jit_execution_context<>;
         jit_ctx*  _ctx;
         void*     _linear_memory;
         bool      _is_llvm;      // true  → entry is int64_t(*)(void*,void*,native_value*)
                                   // false → use the code_base trampoline
         // For LLVM: absolute fn pointer.  For native JIT: offset-relative fn pointer.
         void*     _fn;
         void*     _code_base;
         // Cached trampoline (code_base) for native JIT
         native_value_extended (*_trampoline)(jit_ctx*, void*, native_value*,
                                                      native_value(*)(void*,void*),
                                                      void*, uint64_t, uint32_t);
         void*     _stack;
      public:
         typed_function() : _ctx(nullptr) {}

         typed_function(jit_ctx* ctx, module& mod, uint32_t func_index, void* stack)
            : _ctx(ctx), _linear_memory(ctx->linear_memory()), _stack(stack)
         {
            _is_llvm = (mod.allocator._code_base == nullptr);
            uint32_t code_idx = func_index - mod.get_imported_functions_size();
            if (_is_llvm) {
               _fn = reinterpret_cast<void*>(mod.code[code_idx].jit_code_offset);
               _code_base = nullptr;
               _trampoline = nullptr;
            } else {
               _fn = reinterpret_cast<void*>(
                  mod.code[code_idx].jit_code_offset + mod.allocator._code_base);
               _code_base = mod.allocator._code_base;
               _trampoline = reinterpret_cast<decltype(_trampoline)>(mod.allocator._code_base);
            }
         }

         R operator()(Args... args) {
            constexpr std::size_t args_count = sizeof...(Args);
            constexpr std::size_t buf_count = args_count < 2 ? 2 : args_count;
            native_value args_raw[buf_count];
            {
               native_value* p = args_raw;
               ((pack_arg(args, p)), ...);
            }

            native_value_extended result;
            if (_is_llvm) {
               using llvm_fn_t = int64_t(*)(void*, void*, native_value*);
               auto entry = reinterpret_cast<llvm_fn_t>(_fn);
               result.scalar.i64 = entry(_ctx, _linear_memory, args_raw);
            } else {
               auto fn = reinterpret_cast<native_value(*)(void*, void*)>(_fn);
               void* stack = _stack;
#ifdef __x86_64__
               if (stack) stack = static_cast<char*>(stack) - 24;
#endif
               result = _trampoline(_ctx, _linear_memory, args_raw, fn, stack,
                                    static_cast<uint64_t>(args_count), false);
            }

            if constexpr (std::is_void_v<R>) {
               return;
            } else if constexpr (std::is_same_v<R, int32_t> || std::is_same_v<R, uint32_t>) {
               return static_cast<R>(result.scalar.i32);
            } else if constexpr (std::is_same_v<R, int64_t> || std::is_same_v<R, uint64_t>) {
               return static_cast<R>(result.scalar.i64);
            } else if constexpr (std::is_same_v<R, float>) {
               float f;
               std::memcpy(&f, &result.scalar.f32, sizeof(f));
               return f;
            } else if constexpr (std::is_same_v<R, double>) {
               double d;
               std::memcpy(&d, &result.scalar.f64, sizeof(d));
               return d;
            } else {
               static_assert(sizeof(R) == 0, "Unsupported return type");
            }
         }

      private:
         template<typename T>
         static void pack_arg(T value, native_value*& out) {
            std::memset(out, 0, sizeof(*out));
            if constexpr (std::is_same_v<T, int32_t> || std::is_same_v<T, uint32_t>) {
               out->i32 = static_cast<uint32_t>(value);
            } else if constexpr (std::is_same_v<T, int64_t> || std::is_same_v<T, uint64_t>) {
               out->i64 = static_cast<uint64_t>(value);
            } else if constexpr (std::is_same_v<T, float>) {
               std::memcpy(&out->f32, &value, sizeof(value));
            } else if constexpr (std::is_same_v<T, double>) {
               std::memcpy(&out->f64, &value, sizeof(value));
            } else {
               static_assert(sizeof(T) == 0, "Unsupported arg type");
            }
            ++out;
         }
      };

      /// Get a typed function handle for an exported WASM function.
      /// The returned callable has minimal per-call overhead — arg marshaling
      /// and a direct native call.  Must be called within a scoped_execution().
      template<typename Sig>
      typed_function<Sig> get_typed_function(const std::string_view& name) {
         uint32_t fi = mod->get_exported_function(name);
         stack_allocator alt_stack(ctx->get_maximum_stack_size());
         return typed_function<Sig>(ctx.get(), *mod, fi, alt_stack.top());
      }

      /// Execute a callable within a signal-handler session.  Sets up the
      /// signal handler once, allocates an alternate stack once, and lets F
      /// make arbitrarily many typed_function calls within.
      template<typename F>
      auto scoped_execution(F&& f) {
         stack_allocator alt_stack(ctx->get_maximum_stack_size());

         // Register the stack guard page
         if (alt_stack.guard_base()) {
            detail::stack_guard_range = std::span<std::byte>(
               static_cast<std::byte*>(alt_stack.guard_base()), alt_stack.guard_size());
         }

         return detail::invoke_with_signal_handler(
            std::forward<F>(f),
            &context_t::handle_signal,
            mod->allocator,
            memory_alloc);
      }

      /// Convenience: get a typed function that captures its own stack allocation.
      /// Returns a pair of (typed_function, stack_allocator) — the stack_allocator
      /// must outlive the typed_function.
      template<typename Sig>
      typed_function<Sig> get_typed_function(const std::string_view& name,
                                             stack_allocator& alt_stack) {
         uint32_t fi = mod->get_exported_function(name);
         return typed_function<Sig>(ctx.get(), *mod, fi, alt_stack.top());
      }

      inline void set_wasm_allocator(wasm_allocator* alloc) {
         memory_alloc = alloc;
         ctx->set_wasm_allocator(memory_alloc);
      }

      inline module&         get_module() { return *mod; }
      inline void            exit(const std::error_code& ec) { ctx->exit(ec); }
      inline auto&           get_context() { return *ctx; }

      const DebugInfo& get_debug() const { return debug; }

    private:
      wasm_allocator* memory_alloc = nullptr; // non owning pointer
      std::shared_ptr<module> mod = nullptr;
      DebugInfo       debug;
      maybe_unique_ptr<context_t> ctx = nullptr;
      host_function_table _host_table;
      std::vector<void*>  _direct_ptrs;   // raw C function pointers indexed by import number
      std::vector<host_trampoline_t> _trampoline_ptrs; // trampoline pointers indexed by import number
      bool            mod_sharable = false; // true if mod is sharable (compiled by the backend)
      uint32_t        initial_max_call_depth = 0;
      uint32_t        initial_max_pages = 0;
   };
} // namespace psizam
