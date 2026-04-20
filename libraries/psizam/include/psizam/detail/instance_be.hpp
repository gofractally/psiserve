#pragma once

// psizam/detail/instance_be.hpp — backend-kind dispatch for instance_impl.
//
// `runtime::instantiate` constructs one of four concrete `backend<Host, Impl>`
// types depending on `instance_policy.initial`. The rest of the runtime needs
// to talk to a uniform interface — `instance_be` — that erases the Impl type.
// Per-backend `instance_be_impl<Impl>` (defined in runtime.cpp) holds the
// typed backend and exposes the polymorphic accessors.
//
// Step 1 of psizam-runtime-api-maturation. Step 10 will extend this interface
// with `call_with_return_erased(...)` so `instance::as<Tag>()` can drive any
// backend kind via the proxy adapter.

#include <psizam/gas.hpp>   // gas_state, gas_handler_t

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace psizam {

class wasm_allocator;
class host_function_table;

namespace detail {

class operand_stack_elem;   // detail/stack_elem.hpp

enum class backend_kind : uint8_t {
   interpreter,
   jit,
   jit2,
   jit_llvm,
};

constexpr const char* to_string(backend_kind k) noexcept {
   switch (k) {
      case backend_kind::interpreter: return "interpreter";
      case backend_kind::jit:         return "jit";
      case backend_kind::jit2:        return "jit2";
      case backend_kind::jit_llvm:    return "jit_llvm";
   }
   return "unknown";
}

// Type-erased instance backend. Concrete implementation
// `instance_be_impl<Impl>` lives in runtime.cpp where the per-backend
// template instantiations are realized exactly once.
class instance_be {
public:
   virtual ~instance_be() = default;

   // Which backend variant this is. Lets `instance::as<Tag>()` and
   // future call-erasure code dispatch correctly.
   virtual backend_kind kind() const noexcept = 0;

   // Pointer to the underlying `backend<std::nullptr_t, Impl>`.
   // Callers that know the kind (e.g. typed proxy adapters in Step 10)
   // can `static_cast` to the matching backend type.
   virtual void* raw_backend() noexcept = 0;

   // Linear memory + page count, type-erased. Always valid post-construction.
   virtual char*    linear_memory()       noexcept = 0;
   virtual uint32_t current_pages() const noexcept = 0;

   // Run module.start (if present) then resolve and call `_start`. The host
   // pointer flows through to host-function trampolines. Returns the WASI
   // exit code captured from `wasi_host::wasi_exit_exception`; returns 0 if
   // `_start` returns normally without proc_exit. Other psizam / std
   // exceptions propagate to the caller.
   //
   // Step 5 of psizam-runtime-api-maturation. Lets pzam-run's main shrink
   // to `return inst.run_start(&host);`.
   virtual int run_start(void* host_ptr) = 0;

   // Call an exported function by name with up to 16 u64 arguments.
   // Matches the `backend::call_with_return(host, name, u64×16)` shape
   // that `invoke_canonical_export_void` already uses — the canonical-ABI
   // 16-slot flat-call convention.
   //
   // Step 10 of psizam-runtime-api-maturation: this is the call-erasure
   // path that lets `instance::as<Tag>()` work for any backend variant.
   // Args beyond `count` are zero-padded to 16 by the implementation.
   virtual std::optional<operand_stack_elem>
      call_export_canonical(void*              host,
                            std::string_view   name,
                            const uint64_t*    args,
                            std::size_t        count) = 0;

   // ── Gas plumbing (backend-erased accessors on execution_context) ──
   // Multi-module gas tracking sets these at instantiate time (and reads
   // them at teardown to credit unused lease back to a shared pool).

   virtual void set_gas_budget(uint64_t budget) = 0;
   virtual void set_gas_handler(void (*handler)(gas_state*, void*),
                                void* user_data) = 0;
   virtual uint64_t gas_consumed_raw() const = 0;
   virtual uint64_t gas_deadline_raw() const = 0;
};

// Factory: pick the impl for `kind`, construct the concrete backend with
// (wasm, table, host_ptr) over a freshly-owned wasm_allocator. Throws
// `unsupported_backend` if `kind` is gated out of this build (e.g. jit_llvm
// without PSIZAM_ENABLE_LLVM_BACKEND, jit / jit2 on platforms without
// __x86_64__ / __aarch64__).
std::unique_ptr<instance_be> make_instance_be(
   backend_kind kind,
   const std::vector<uint8_t>& wasm,
   host_function_table table,
   void* host_ptr);

} // namespace detail
} // namespace psizam
