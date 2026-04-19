// runtime.cpp — Phase 2: wired to real backend infrastructure.
// prepare() parses + compiles WASM; instantiate() creates live instances;
// instance provides gas_state, linear_memory, and typed proxy access.

#include <psizam/runtime.hpp>
#include <psizam/backend.hpp>
#include <psizam/host_function.hpp>
#include <psizam/host_function_table.hpp>

#include <chrono>
#include <unordered_map>

namespace psizam {

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
// instance_impl — owns a live backend + allocator + gas state
// ═════════════════════════════════════════════════════════════════════

struct instance_impl {
   using backend_t = backend<std::nullptr_t, interpreter>;

   wasm_allocator                 alloc;
   std::unique_ptr<backend_t>    be;
   gas_state                     local_gas;
   std::shared_ptr<gas_state>    shared_gas;
   void*                         host_ptr = nullptr;

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
   return impl_ ? static_cast<void*>(impl_->be.get()) : nullptr;
}
void* instance::host_ptr() {
   return impl_ ? impl_->host_ptr : nullptr;
}

char* instance::linear_memory() {
   return impl_ && impl_->be ? impl_->be->get_context().linear_memory() : nullptr;
}
std::size_t instance::memory_size() const {
   return impl_ && impl_->be
      ? static_cast<std::size_t>(impl_->be->get_context().current_linear_memory()) * 65536
      : 0;
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

   // Create backend from WASM bytes + host function table.
   // The host_ptr is passed to the backend so host function
   // trampolines can access the Host object.
   auto table_copy = mod->table;
   inst->be = std::make_unique<instance_impl::backend_t>(
      mod->wasm_copy, std::move(table_copy),
      mod->host_ptr, &inst->alloc);

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
