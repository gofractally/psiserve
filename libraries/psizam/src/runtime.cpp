// runtime.cpp — Phase 2 + Steps 1, 5 of psizam-runtime-api-maturation.
// prepare() parses + compiles WASM; instantiate() creates live instances
// dispatched on the requested backend kind via the abstract `instance_be`.
// instance provides gas_state, linear_memory, run_start, and typed proxy
// access.

#include <psizam/runtime.hpp>
#include <psizam/backend.hpp>
#include <psizam/detail/instance_be.hpp>
#include <psizam/detail/wasi_host.hpp>
#include <psizam/host_function.hpp>
#include <psizam/host_function_table.hpp>

#include <chrono>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <unordered_map>

namespace psizam {

// ═════════════════════════════════════════════════════════════════════
// detail::instance_be_impl<Impl>
// ═════════════════════════════════════════════════════════════════════
//
// One concrete realization per backend variant. Each owns a fresh
// wasm_allocator and the typed `backend<std::nullptr_t, Impl>` that
// drives execution.

namespace detail {

template <typename Impl>
class instance_be_impl final : public instance_be {
   using backend_t = backend<std::nullptr_t, Impl>;

   wasm_allocator             alloc_;
   std::unique_ptr<backend_t> be_;
   backend_kind               kind_;

public:
   instance_be_impl(backend_kind kind,
                    const std::vector<uint8_t>& wasm,
                    host_function_table table,
                    void* host_ptr)
      : kind_(kind)
   {
      // backend<>'s wasm_code constructor expects a non-const reference.
      // The runtime owns the bytes for the lifetime of the backend, so
      // a const_cast here is safe (the backend reads but does not mutate
      // the source bytes through this reference).
      auto& code = const_cast<std::vector<uint8_t>&>(wasm);
      be_ = std::make_unique<backend_t>(
         code, std::move(table), host_ptr, &alloc_);
   }

   backend_kind kind()         const noexcept override { return kind_; }
   void*        raw_backend()        noexcept override { return be_.get(); }

   char* linear_memory() noexcept override {
      return be_->get_context().linear_memory();
   }
   uint32_t current_pages() const noexcept override {
      return be_->get_context().current_linear_memory();
   }

   int run_start(void* host_ptr) override {
      try {
         auto& mod = be_->get_module();
         if (mod.start != std::numeric_limits<uint32_t>::max()) {
            be_->call_by_index(host_ptr, mod.start);
         }
         be_->call(host_ptr, std::string_view{"_start"});
      } catch (const wasi_host::wasi_exit_exception& e) {
         return e.code;
      }
      return 0;
   }
};

std::unique_ptr<instance_be> make_instance_be(
   backend_kind kind,
   const std::vector<uint8_t>& wasm,
   host_function_table table,
   void* host_ptr)
{
   switch (kind) {
      case backend_kind::interpreter:
         return std::make_unique<instance_be_impl<interpreter>>(
            kind, wasm, std::move(table), host_ptr);

#if defined(__x86_64__) || defined(__aarch64__)
      case backend_kind::jit:
         return std::make_unique<instance_be_impl<jit>>(
            kind, wasm, std::move(table), host_ptr);
      case backend_kind::jit2:
         return std::make_unique<instance_be_impl<jit2>>(
            kind, wasm, std::move(table), host_ptr);
#else
      case backend_kind::jit:
      case backend_kind::jit2:
         throw std::runtime_error{
            "psizam: jit / jit2 backend requires x86_64 or aarch64"};
#endif

#if defined(PSIZAM_ENABLE_LLVM_BACKEND)
      case backend_kind::jit_llvm:
         return std::make_unique<instance_be_impl<jit_llvm>>(
            kind, wasm, std::move(table), host_ptr);
#else
      case backend_kind::jit_llvm:
         throw std::runtime_error{
            "psizam: jit_llvm backend not built (PSIZAM_ENABLE_LLVM_BACKEND=OFF)"};
#endif
   }
   throw std::runtime_error{"psizam: unknown backend_kind"};
}

// Map instance_policy::compile_tier → detail::backend_kind. Lives here
// (rather than the public header) so the policy enum can keep its
// runtime-facing names while the dispatch switch reads the canonical
// backend identity.
backend_kind tier_to_backend_kind(instance_policy::compile_tier t) noexcept {
   switch (t) {
      case instance_policy::compile_tier::interpret: return backend_kind::interpreter;
      case instance_policy::compile_tier::jit1:      return backend_kind::jit;
      case instance_policy::compile_tier::jit2:      return backend_kind::jit2;
      case instance_policy::compile_tier::jit_llvm:  return backend_kind::jit_llvm;
   }
   return backend_kind::interpreter;
}

} // namespace detail

// ═════════════════════════════════════════════════════════════════════
// module_handle_impl — owns the WASM bytes and host function table
// template. The backend is created per-instance (each gets its own
// linear memory) but the module/table are shared.
// ═════════════════════════════════════════════════════════════════════

struct module_handle_impl {
   std::vector<uint8_t>  wasm_copy;
   host_function_table   table;
   instance_policy       policy;
   void*                 host_ptr = nullptr;
   uint32_t              compile_ms = 0;

   module_handle_impl() = default;
};

host_function_table& module_handle::table() { return impl_->table; }
void module_handle::set_host_ptr(void* p) { impl_->host_ptr = p; }

uint32_t module_handle::compile_time_ms() const {
   return impl_ ? impl_->compile_ms : 0;
}
uint32_t module_handle::native_code_size() const { return 0; }
uint32_t module_handle::wasm_size() const {
   return impl_ ? static_cast<uint32_t>(impl_->wasm_copy.size()) : 0;
}
std::vector<std::string> module_handle::wit_sections() const { return {}; }

// ═════════════════════════════════════════════════════════════════════
// instance_impl — owns a polymorphic backend holder + gas state
// ═════════════════════════════════════════════════════════════════════

struct instance_impl {
   std::unique_ptr<detail::instance_be> be;
   gas_state                            local_gas;
   std::shared_ptr<gas_state>           shared_gas;
   void*                                host_ptr = nullptr;

   gas_state* active_gas() {
      return shared_gas ? shared_gas.get() : &local_gas;
   }
};

instance::instance() = default;
instance::instance(std::unique_ptr<instance_impl> p) : impl_(std::move(p)) {}
instance::~instance() = default;
instance::instance(instance&&) noexcept = default;
instance& instance::operator=(instance&&) noexcept = default;

uint64_t instance::gas_consumed() const {
   return impl_ ? impl_->active_gas()->consumed : 0;
}
uint64_t instance::gas_deadline() const {
   return impl_ ? impl_->active_gas()->deadline : 0;
}
void instance::set_deadline(uint64_t d) {
   if (impl_) impl_->active_gas()->deadline = d;
}
void instance::interrupt() {
   if (impl_) impl_->active_gas()->deadline = 0;
}
gas_state* instance::gas() {
   return impl_ ? impl_->active_gas() : nullptr;
}
const gas_state* instance::gas() const {
   return impl_ ? impl_->active_gas() : nullptr;
}
void* instance::backend_ptr() {
   return impl_ && impl_->be ? impl_->be->raw_backend() : nullptr;
}
void* instance::host_ptr() {
   return impl_ ? impl_->host_ptr : nullptr;
}

char* instance::linear_memory() {
   return impl_ && impl_->be ? impl_->be->linear_memory() : nullptr;
}
std::size_t instance::memory_size() const {
   return impl_ && impl_->be
      ? static_cast<std::size_t>(impl_->be->current_pages()) * 65536
      : 0;
}

int instance::run_start() {
   if (!impl_ || !impl_->be) return -1;
   return impl_->be->run_start(impl_->host_ptr);
}

// ═════════════════════════════════════════════════════════════════════
// runtime_impl
// ═════════════════════════════════════════════════════════════════════

struct runtime_impl {
   runtime_config config;
   // TODO: .pzam cache, library registry, memory pools
};

runtime::runtime(runtime_config config)
   : impl_(std::make_unique<runtime_impl>(runtime_impl{config})) {}

runtime::~runtime() = default;
runtime::runtime(runtime&&) noexcept = default;
runtime& runtime::operator=(runtime&&) noexcept = default;

void runtime::register_library(std::string_view, archive_bytes) {
   // TODO: parse .a archive, cache member .o files
}

void runtime::register_library(std::string_view, wasm_bytes) {
   // TODO: parse standalone .wasm library
}

module_handle runtime::prepare(wasm_bytes wasm, const instance_policy& policy) {
   auto t0 = std::chrono::high_resolution_clock::now();

   auto mod = std::make_shared<module_handle_impl>();
   mod->wasm_copy.assign(wasm.data.begin(), wasm.data.end());
   mod->policy = policy;

   auto t1 = std::chrono::high_resolution_clock::now();
   mod->compile_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

   return module_handle{std::move(mod)};
}

module_handle runtime::load_cached(pzam_bytes) {
   // TODO: load pre-compiled .pzam
   return module_handle{std::make_shared<module_handle_impl>()};
}

std::vector<unresolved_import> runtime::check(wasm_bytes) const {
   // TODO: parse imports, check against registered libraries + host
   return {};
}

instance runtime::instantiate(const module_handle& tmpl) {
   return instantiate(tmpl, tmpl.get()->policy);
}

instance runtime::instantiate(const module_handle& tmpl,
                               const instance_policy& policy) {
   auto* mod = tmpl.get();
   if (!mod) return instance{};

   auto inst = std::make_unique<instance_impl>();
   inst->host_ptr = mod->host_ptr;
   inst->shared_gas = policy.shared_gas;
   if (!inst->shared_gas) {
      inst->local_gas.deadline = policy.gas_budget;
   }

   // Pick the backend impl per `policy.initial`. The factory throws on
   // backends gated out of this build (jit/jit2 on non-x86/arm,
   // jit_llvm without PSIZAM_ENABLE_LLVM_BACKEND).
   auto be_kind = detail::tier_to_backend_kind(policy.initial);
   auto table_copy = mod->table;
   inst->be = detail::make_instance_be(
      be_kind, mod->wasm_copy, std::move(table_copy), mod->host_ptr);

   return instance{std::move(inst)};
}

void runtime::bind(module_handle&, instance&, std::string_view) {
   // TODO: dynamic WIT-driven bind (reads WIT from module, generates bridge)
}

runtime::cache_stats runtime::stats() const {
   return {0, 0, 0};
}

void runtime::evict(wasm_bytes) {}
void runtime::clear_cache() {}

} // namespace psizam
