#pragma once

//
// BACKEND FEATURE PARITY TRACKER
// Legend: 🟢 done  🟡 partial  🔴 missing  🐛 bug
//
//                              INTERP JIT(x86) JIT2(x86) JIT2(a64) LLVM
//                              ────── ──────── ───────── ───────── ────
// MULTI-VALUE RETURNS
//   parse multi-value types      🟢     🟢       🟢        🟢      🟢
//   function returns (N>1)       🟢     🟢       🟢        🟢      🟢
//   multi-value block types      🟢     🟢       🟢        🟢      🟢
//   branch encoding (N vals)     🟢     🟢       🟢        🟢      🟢
//
// MULTI-TABLE
//   call_indirect w/ table       🟢     🟢       🟢        🟢      🟢
//   table.copy multi-table       🟢     🟢       🟢        🟢      🟢
//   table.init multi-table       🟢     🟢       🟢        🟢      🟢
//   elem.drop                    🟢     🟢       🟢        🟢      🟢
//
// TABLE ELEMENT OPS
//   table.get                    🟢     🟢       🟢        🟢      🟢
//   table.set                    🟢     🟢       🟢        🟢      🟢
//   table.grow                   🟢     🟢       🟢        🟢      🟢
//   table.size                   🟢     🟢       🟢        🟢      🟢
//   table.fill                   🟢     🟢       🟢        🟢      🟢
//
// BULK MEMORY
//   memory.init                  🟢     🟢       🟢        🟢      🟢
//   memory.copy                  🟢     🟢       🟢        🟢      🟢
//   memory.fill                  🟢     🟢       🟢        🟢      🟢
//   data.drop                    🟢     🟢       🟢        🟢      🟢
//
// SIMD (v128)
//   full v128 ops                🟢     🟢       🟢        🟡      🟡
//
// REFERENCE TYPES
//   ref.null / ref.is_null       🟢     🟢       🟢        🟢      🟢
//   ref.func                     🟢     🟢       🟢        🟢      🟢
//   externref                    🟢     🟢       🟢        🟢      🟢
//
// SIGN EXTENSION OPS
//   i32_extend8/16_s             🟢     🟢       🟢        🟢      🟢
//   i64_extend8/16/32_s          🟢     🟢       🟢        🟢      🟢
//
// NONTRAPPING FLOAT-TO-INT
//   trunc_sat_*                  🟢     🟢       🟢        🟢      🟢
//
// MUTABLE GLOBALS IMPORT/EXPORT
//   mutable global import        🟢     🟢       🟢        🟢      🟢
//
// EXCEPTION HANDLING
//   try/catch/throw              🟢     🟢       🟢        🟢      🟢
//
// THREADS / ATOMICS
//   atomic ops                   🟢     🟢       🟢        🟢      🟢
//   shared memory                🟢     🟢       🟢        🟢      🟢
//
// TAIL CALLS
//   return_call                  🟢     🟢       🟢        🟢      🟢
//   return_call_indirect         🟢     🟢       🟢        🟢      🟢
//
// MEMORY64
//   i64 addressing               🔴     🔴       🔴        🔴      🔴
//
// GC PROPOSAL
//   struct/array types           🔴     🔴       🔴        🔴      🔴
//
// RELAXED SIMD
//   relaxed simd ops             🟢     🟢       🟢        🟢      🟢
//
// KNOWN BUGS 🐛
//   [B1] SIMD float ops incomplete on aarch64 softfloat (~60 spec failures)
//   [B2] v128_load32_zero guard page crash in interpreter (simd_const_385/387)
//   [B3] JIT2(x86) call_0_wasm SIGABRT (pre-existing)
//   [B4] LLVM OOB load/trap failures (pending fix)
//   [B5] error_codes_def.hpp extraneous brace warnings on aarch64 tools
//
// Last updated: 2026-04-10
//

#include <psizam/allocator.hpp>
#include <psizam/bitcode_writer.hpp>
#include <psizam/config.hpp>
#include <psizam/debug_visitor.hpp>
#include <psizam/execution_context.hpp>
#include <psizam/interpret_visitor.hpp>
#include <psizam/null_writer.hpp>
#include <psizam/parser.hpp>
#include <psizam/types.hpp>

#ifdef __x86_64__
#include <psizam/x86_64.hpp>
#elif defined(__aarch64__)
#include <psizam/aarch64.hpp>
#endif
#if defined(__x86_64__) || defined(__aarch64__)
#include <psizam/ir_writer.hpp>
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
#include <psizam/ir_writer_llvm.hpp>
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
      using context = jit_execution_context<false>;
      template<typename Options, typename DebugInfo>
      using parser = binary_parser<machine_code_writer, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = false;
   };

   struct jit_profile {
      using context = jit_execution_context<true>;
      template<typename Options, typename DebugInfo>
      using parser = binary_parser<machine_code_writer, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = true;
   };

#endif

#if defined(__x86_64__) || defined(__aarch64__)
   struct jit2 {
      using context = jit_execution_context<false>;
      template<typename Options, typename DebugInfo>
      using parser = binary_parser<ir_writer, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = false;
   };
#endif

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
   struct jit_llvm {
      using context = jit_execution_context<false>;
      template<typename Options, typename DebugInfo>
      using parser = binary_parser<ir_writer_llvm, Options, DebugInfo>;
      static constexpr bool is_jit = true;
      static constexpr bool enable_backtrace = false;
   };
#endif

   struct interpreter {
      using context = execution_context;
      template<typename Options, typename DebugInfo>
      using parser = binary_parser<bitcode_writer, Options, DebugInfo>;
      static constexpr bool is_jit = false;
      static constexpr bool enable_backtrace = false;
   };

   struct null_backend {
      using context = null_execution_context;
      template<typename Options, typename DebugInfo>
      using parser = binary_parser<null_writer, Options, DebugInfo>;
      static constexpr bool is_jit = false;
      static constexpr bool enable_backtrace = false;
   };

   template <typename T>
   struct maybe_unique_ptr {
      maybe_unique_ptr(T* ptr = nullptr, bool owns = true) : ptr(ptr), owns(owns) {}
      maybe_unique_ptr(const maybe_unique_ptr&) = delete;
      maybe_unique_ptr& operator=(const maybe_unique_ptr&) = delete;
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

   template <typename HostFunctions = std::nullptr_t, typename Impl = interpreter, typename Options = default_options, typename DebugInfo = null_debug_info>
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
                     operand_stack temp_stack;
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
         if (ctx.owns) {
            ctx->initialize_globals();
         }
         // Build the host_function_table and wire it into the execution context.
         // Even for standalone_function_t (nullptr_t), we need a valid (empty) table
         // so that call_indirect to unresolved imports throws instead of crashing.
         if constexpr (!std::is_same_v<HostFunctions, std::nullptr_t>) {
            HostFunctions::resolve(*mod);
         }
         _host_table = build_host_table();
         ctx->set_host_table(&_host_table);
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

      module& parse_module(wasm_code& code, const Options& options) {
         mod->allocator.use_default_memory();
         return parser_t{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module(code, *mod, debug);
      }

      template <typename XDebugInfo>
      module& parse_module(wasm_code& code, const Options& options, XDebugInfo& debug) {
         mod->allocator.use_default_memory();
         return parser_tpl<XDebugInfo>{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module(code, *mod, debug);
      }

      module& parse_module2(wasm_code_ptr& ptr, size_t sz, const Options& options, bool single_parsing) {
         if (single_parsing) {
            mod->allocator.use_default_memory();
            return parser_t{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module2(ptr, sz, *mod, debug);
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
               first_pass_module.finalize();
               largest_size = first_pass_module.allocator.largest_used_size();
            }

            // Second pass: uses actual required memory for final parsing
            mod->allocator.use_fixed_memory(largest_size);
            return parser_t{ mod->allocator, options, Impl::enable_backtrace, detail::has_max_stack_bytes<Options> }.parse_module2(orig_ptr, sz, *mod, debug);
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

      template <typename... Args>
      inline auto operator()(stack_manager& alt_stack, host_t& host, const std::string_view& mod, const std::string_view& func, Args... args) {
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
            ctx->execute_start(host, interpret_visitor(*ctx));
         }
         return *this;
      }

      inline backend& initialize(stack_manager& alt_stack, host_t* host=nullptr) {
         if (memory_alloc) {
            ctx->reset();
            ctx->execute_start(alt_stack, host, interpret_visitor(*ctx));
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
            ctx->template execute_func_table<tc_t>(host, debug_visitor(*ctx), func_index, std::forward<Args>(args)...);
         } else {
            ctx->template execute_func_table<tc_t>(host, interpret_visitor(*ctx), func_index, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(host_t* host, uint32_t func_index, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(host, debug_visitor(*ctx), func_index, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(host, interpret_visitor(*ctx), func_index, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(stack_manager& alt_stack, host_t& host, const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(alt_stack, &host, debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(alt_stack, &host, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(host_t& host, const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(&host, debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(&host, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline bool call(const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            ctx->template execute<tc_t>(nullptr, debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            ctx->template execute<tc_t>(nullptr, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
         return true;
      }

      template <typename... Args>
      inline auto call_with_return(host_t& host, const std::string_view& mod, const std::string_view& func, Args&&... args ) {
         if constexpr (psizam_debug) {
            return ctx->template execute<tc_t>(&host, debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            return ctx->template execute<tc_t>(&host, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
      }

      template <typename... Args>
      inline auto call_with_return(const std::string_view& mod, const std::string_view& func, Args&&... args) {
         if constexpr (psizam_debug) {
            return ctx->template execute<tc_t>(nullptr, debug_visitor(*ctx), func, std::forward<Args>(args)...);
         } else {
            return ctx->template execute<tc_t>(nullptr, interpret_visitor(*ctx), func, std::forward<Args>(args)...);
         }
      }

      template<typename Watchdog, typename F>
      inline void timed_run(Watchdog&& wd, F&& f) {
         //timed_run_has_timed_out -- declared in signal handling code because signal handler needs to inspect it on a SEGV too -- is a thread local
         // so that upon a SEGV the signal handling code can discern if the thread that caused the SEGV has a timed_run that has timed out. This
         // thread local also need to be an atomic because the thread that a Watchdog callback will be called from may not be the same as the
         // executing thread.
         std::atomic<bool>&      _timed_out = timed_run_has_timed_out;
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
                  ctx->execute(&host, interpret_visitor(*ctx), s);
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
                  ctx->execute(nullptr, interpret_visitor(*ctx), s);
               }
            }
         });
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
      bool            mod_sharable = false; // true if mod is sharable (compiled by the backend)
      uint32_t        initial_max_call_depth = 0;
      uint32_t        initial_max_pages = 0;
   };
} // namespace psizam
