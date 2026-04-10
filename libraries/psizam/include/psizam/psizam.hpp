#pragma once

// psizam public API — non-templated, runtime-configurable WASM engine interface.
// Uses host_function_table for type-erased host function dispatch.

#include <psizam/allocator.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function_table.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
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
      engine   eng              = engine::interpreter;
      uint32_t max_call_depth   = 251;
   };

   /// Validate a WASM module without compiling it.
   /// Throws wasm_parse_exception on invalid modules.
   inline void validate(std::span<const uint8_t> wasm) {
      wasm_code code(wasm.begin(), wasm.end());
      backend<std::nullptr_t, null_backend> bkend(code, nullptr);
   }

   /// Non-templated WASM runtime. Wraps the appropriate backend
   /// based on compile_options::engine, providing a uniform call interface.
   class runtime {
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

   public:
      runtime(std::span<const uint8_t> wasm,
              host_function_table& table,
              wasm_allocator* alloc,
              void* host = nullptr,
              const compile_options& opts = {})
         : _table(&table), _host(host)
      {
         wasm_code code(wasm.begin(), wasm.end());
         init(code, alloc, opts);
      }

      runtime(wasm_code& code,
              host_function_table& table,
              wasm_allocator* alloc,
              void* host = nullptr,
              const compile_options& opts = {})
         : _table(&table), _host(host)
      {
         init(code, alloc, opts);
      }

      /// Call a WASM exported function by name.
      template <typename... Args>
      bool call(std::string_view mod, std::string_view func, Args&&... args) {
         std::visit([&](auto& bkend) {
            bkend->get_context().template execute<>(_host, interpret_visitor(bkend->get_context()),
                                                    func, std::forward<Args>(args)...);
         }, _backend);
         return true;
      }

      /// Call a WASM exported function by name, returning the result.
      template <typename... Args>
      auto call_with_return(std::string_view mod, std::string_view func, Args&&... args) {
         return std::visit([&](auto& bkend) {
            return bkend->get_context().template execute<>(_host, interpret_visitor(bkend->get_context()),
                                                           func, std::forward<Args>(args)...);
         }, _backend);
      }

      module& get_module() {
         return std::visit([](auto& bkend) -> module& { return bkend->get_module(); }, _backend);
      }

      void set_host(void* host) { _host = host; }

   private:
      void init(wasm_code& code, wasm_allocator* alloc, const compile_options& opts) {
         switch (opts.eng) {
            case engine::interpreter: {
               auto b = std::make_unique<interp_backend>(code, alloc);
               _table->resolve(b->get_module());
               b->get_context()._table = _table;
               _backend = std::move(b);
               break;
            }
#if defined(__x86_64__) || defined(__aarch64__)
            case engine::jit: {
               auto b = std::make_unique<jit_backend>(code, alloc);
               _table->resolve(b->get_module());
               b->get_context()._table = _table;
               _backend = std::move(b);
               break;
            }
            case engine::jit2: {
               auto b = std::make_unique<jit2_backend>(code, alloc);
               _table->resolve(b->get_module());
               b->get_context()._table = _table;
               _backend = std::move(b);
               break;
            }
#endif
#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
            case engine::jit_llvm: {
               auto b = std::make_unique<llvm_backend>(code, alloc);
               _table->resolve(b->get_module());
               b->get_context()._table = _table;
               _backend = std::move(b);
               break;
            }
#endif
         }
      }

      host_function_table*  _table;
      void*                 _host;
      backend_var           _backend;
   };

} // namespace psizam
