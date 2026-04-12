#pragma once

// psizam public API — Module/Instance separation.
//
// compiled_module: owns compilation artifacts (immutable after init)
// instance:        lightweight execution context (one per fiber)
//
// Usage:
//   host_function_table table;
//   table.add<&MyHost::myFunc>("env", "myFunc");
//
//   compiled_module mod(code, std::move(table), &wa, {engine::jit});
//   auto inst = mod.create_instance();
//   inst.set_host(&my_host);
//
//   auto start = inst.get_function<void()>("_start");
//   start();

#include <psizam/backend.hpp>
#include <psizam/host_function_table.hpp>

#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <string_view>
#include <variant>

namespace psizam {

   enum class engine {
      interpreter,
#if defined(__x86_64__) || defined(__aarch64__)
      jit,
      jit2,
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
      jit_llvm,
#endif
   };

   struct compile_options {
      engine   eng            = engine::interpreter;
      uint32_t max_call_depth = 251;
   };

   /// Validate a WASM module without compiling it.
   inline void validate(std::span<const uint8_t> wasm) {
      wasm_code code(wasm.begin(), wasm.end());
      backend<std::nullptr_t, null_backend> bkend(code, nullptr);
   }

   // ── Result extraction helper ──────────────────────────────────────────────

   namespace detail {
      template <typename R>
      R extract_result(const operand_stack_elem& elem) {
         if constexpr (std::is_same_v<R, int32_t>)       return elem.to_i32();
         else if constexpr (std::is_same_v<R, uint32_t>) return elem.to_ui32();
         else if constexpr (std::is_same_v<R, int64_t>)  return elem.to_i64();
         else if constexpr (std::is_same_v<R, uint64_t>) return elem.to_ui64();
         else if constexpr (std::is_same_v<R, float>)    return elem.to_f32();
         else if constexpr (std::is_same_v<R, double>)   return elem.to_f64();
         else static_assert(!sizeof(R*), "unsupported return type for get_function");
      }
   }

   // ── Forward declarations ──────────────────────────────────────────────────

   class instance;

   // ── compiled_module ───────────────────────────────────────────────────────

   class compiled_module {
   public:
      compiled_module(wasm_code code, host_function_table table,
                      wasm_allocator* alloc, const compile_options& opts = {});

      /// Create a fresh execution instance (clones context from template).
      instance create_instance();

      /// Create a fiber instance sharing linear memory but with own stacks/globals.
      /// Allocates wasm_stack_pages pages of WASM stack and sets __stack_pointer.
      instance create_fiber_instance(uint32_t wasm_stack_pages = 2);

      module& get_module();

      struct impl;

   private:
      std::shared_ptr<impl> _impl;
   };

   // ── instance ──────────────────────────────────────────────────────────────

   class instance {
   public:
      instance() = default;
      instance(instance&&) = default;
      instance& operator=(instance&&) = default;

      /// Bind host object for all subsequent calls through get_function/get_table_function.
      void set_host(void* host) { _host = host; }

      /// Access WASM linear memory.
      char* linear_memory();

      /// Resolve an exported function by name → typed callable.
      /// Name lookup happens once; the returned std::function calls directly.
      template <typename Sig>
      std::function<Sig> get_function(std::string_view name);

      /// Resolve a function table entry → typed callable.
      template <typename Sig>
      std::function<Sig> get_table_function(uint32_t table_index);

      /// Call an exported function by name with raw return.
      template <typename... Args>
      std::optional<operand_stack_elem> call_with_return(std::string_view func_name, Args&&... args);

   private:
      friend class compiled_module;

      using context_var = std::variant<
         std::unique_ptr<detail::execution_context>,
         std::unique_ptr<detail::jit_execution_context<false>>
      >;

      std::shared_ptr<compiled_module::impl> _impl;
      context_var                            _ctx;
      void*                                  _host = nullptr;

      template <typename R, typename... Args>
      std::function<R(Args...)> make_caller(uint32_t func_index);
   };

   // ── compiled_module::impl ─────────────────────────────────────────────────

   struct compiled_module::impl {
      host_function_table table;

      using interp_backend = backend<std::nullptr_t, ::psizam::interpreter>;
#if defined(__x86_64__) || defined(__aarch64__)
      using jit_backend    = backend<std::nullptr_t, ::psizam::jit>;
      using jit2_backend   = backend<std::nullptr_t, ::psizam::jit2>;
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
      using llvm_backend   = backend<std::nullptr_t, ::psizam::jit_llvm>;
      using backend_var = std::variant<
         std::unique_ptr<interp_backend>,
         std::unique_ptr<jit_backend>,
         std::unique_ptr<jit2_backend>,
         std::unique_ptr<llvm_backend>>;
#else
      using backend_var = std::variant<
         std::unique_ptr<interp_backend>,
         std::unique_ptr<jit_backend>,
         std::unique_ptr<jit2_backend>>;
#endif
#else
      using backend_var = std::variant<std::unique_ptr<interp_backend>>;
#endif

      backend_var bkend;

      impl(wasm_code& code, host_function_table tbl, wasm_allocator* alloc,
           const compile_options& opts)
         : table(std::move(tbl))
      {
         switch (opts.eng) {
            case engine::interpreter:
               bkend = std::make_unique<interp_backend>(code, alloc);
               break;
#if defined(__x86_64__) || defined(__aarch64__)
            case engine::jit:
               bkend = std::make_unique<jit_backend>(code, alloc);
               break;
            case engine::jit2:
               bkend = std::make_unique<jit2_backend>(code, alloc);
               break;
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
            case engine::jit_llvm:
               bkend = std::make_unique<llvm_backend>(code, alloc);
               break;
#endif
         }

         // Post-construct: resolve imports against our table and wire it in
         std::visit([this](auto& b) {
            table.resolve(b->get_module());
            b->get_context().set_host_table(&table);
         }, bkend);
      }

      module& get_module() {
         return std::visit([](auto& b) -> module& { return b->get_module(); }, bkend);
      }
   };

   // ── compiled_module method implementations ────────────────────────────────

   inline compiled_module::compiled_module(wasm_code code, host_function_table table,
                                           wasm_allocator* alloc, const compile_options& opts)
      : _impl(std::make_shared<impl>(code, std::move(table), alloc, opts))
   {}

   inline instance compiled_module::create_instance() {
      instance inst;
      inst._impl = _impl;
      std::visit([&](auto& bkend_ptr) {
         auto ctx = bkend_ptr->get_context().create_fiber_context();
         ctx->set_host_table(&_impl->table);
         inst._ctx = std::move(ctx);
      }, _impl->bkend);
      return inst;
   }

   inline instance compiled_module::create_fiber_instance(uint32_t wasm_stack_pages) {
      instance inst;
      inst._impl = _impl;
      std::visit([&](auto& bkend_ptr) {
         auto ctx = bkend_ptr->get_context().create_fiber_context();

         // Allocate WASM stack in shared linear memory
         auto* wa = ctx->get_wasm_allocator();
         wa->template alloc<char>(wasm_stack_pages);
         uint32_t stack_top = wa->get_current_page() * 65536;

         // Set __stack_pointer global for this fiber
         uint32_t sp_idx = bkend_ptr->get_module().get_exported_global("__stack_pointer");
         if (sp_idx != std::numeric_limits<uint32_t>::max())
            ctx->_globals[sp_idx].value.i32 = static_cast<int32_t>(stack_top);

         ctx->set_host_table(&_impl->table);
         inst._ctx = std::move(ctx);
      }, _impl->bkend);
      return inst;
   }

   inline module& compiled_module::get_module() {
      return _impl->get_module();
   }

   inline char* instance::linear_memory() {
      return std::visit([](auto& ctx_ptr) -> char* {
         return ctx_ptr->linear_memory();
      }, _ctx);
   }

   // ── instance template method implementations (require complete impl) ─────

   template <typename Sig>
   std::function<Sig> instance::get_function(std::string_view name) {
      // Decompose Sig via function pointer trick
      return [&]<typename R, typename... Args>(R(*)(Args...)) {
         uint32_t func_index = _impl->get_module().get_exported_function(name);
         return make_caller<R, Args...>(func_index);
      }(static_cast<Sig*>(nullptr));
   }

   template <typename Sig>
   std::function<Sig> instance::get_table_function(uint32_t table_index) {
      return [&]<typename R, typename... Args>(R(*)(Args...)) {
         uint32_t func_index = std::visit([&](auto& ctx_ptr) {
            return ctx_ptr->table_elem(table_index);
         }, _ctx);
         return make_caller<R, Args...>(func_index);
      }(static_cast<Sig*>(nullptr));
   }

   template <typename... Args>
   std::optional<operand_stack_elem> instance::call_with_return(
         std::string_view func_name, Args&&... args) {
      return std::visit([&](auto& ctx_ptr) -> std::optional<operand_stack_elem> {
         return ctx_ptr->execute(_host, detail::interpret_visitor(*ctx_ptr), func_name,
                                 std::forward<Args>(args)...);
      }, _ctx);
   }

   template <typename R, typename... Args>
   std::function<R(Args...)> instance::make_caller(uint32_t func_index) {
      return std::visit([&](auto& ctx_ptr) -> std::function<R(Args...)> {
         auto* ctx = ctx_ptr.get();
         auto* host_slot = &_host;

         return [ctx, host_slot, func_index](Args... args) -> R {
            auto result = ctx->execute(
               *host_slot, detail::interpret_visitor(*ctx), func_index, args...);

            if constexpr (!std::is_void_v<R>) {
               return detail::extract_result<R>(*result);
            }
         };
      }, _ctx);
   }

} // namespace psizam
