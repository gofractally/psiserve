// runtime.cpp — Stub implementations for psizam::runtime Phase 1.
// Just enough to compile and link the hello_runtime example.
// Real implementations come in Phase 2.

#include <psizam/runtime.hpp>

namespace psizam {

// ── module_handle stubs ─────────────────────────────────────────────

struct module_handle_impl {};

uint32_t module_handle::compile_time_ms() const { return 0; }
uint32_t module_handle::native_code_size() const { return 0; }
uint32_t module_handle::wasm_size() const { return 0; }
std::vector<std::string> module_handle::wit_sections() const { return {}; }

// ── instance stubs ──────────────────────────────────────────────────

struct instance_impl {
   gas_state local_gas;
   std::shared_ptr<gas_state> shared_gas;

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
char* instance::linear_memory() { return nullptr; }
std::size_t instance::memory_size() const { return 0; }

// ── runtime stubs ───────────────────────────────────────────────────

struct runtime_impl {
   runtime_config config;
};

runtime::runtime(runtime_config config)
   : impl_(std::make_unique<runtime_impl>(runtime_impl{config})) {}

runtime::~runtime() = default;
runtime::runtime(runtime&&) noexcept = default;
runtime& runtime::operator=(runtime&&) noexcept = default;

void runtime::register_library(std::string_view, archive_bytes) {}
void runtime::register_library(std::string_view, wasm_bytes) {}

module_handle runtime::prepare(wasm_bytes, const instance_policy&) {
   return module_handle{std::make_shared<module_handle_impl>()};
}

module_handle runtime::load_cached(pzam_bytes) {
   return module_handle{std::make_shared<module_handle_impl>()};
}

std::vector<unresolved_import> runtime::check(wasm_bytes) const {
   return {};
}

instance runtime::instantiate(const module_handle&) {
   return instance{std::make_unique<instance_impl>()};
}

instance runtime::instantiate(const module_handle&, const instance_policy& p) {
   auto inst = std::make_unique<instance_impl>();
   inst->shared_gas = p.shared_gas;
   if (!inst->shared_gas) {
      inst->local_gas.deadline = p.gas_budget;
   }
   return instance{std::move(inst)};
}

void runtime::bind(instance&, instance&, std::string_view) {}

runtime::cache_stats runtime::stats() const {
   return {0, 0, 0};
}

void runtime::evict(wasm_bytes) {}
void runtime::clear_cache() {}

} // namespace psizam
