// runtime.cpp — Phase 2 + Steps 1, 5 of psizam-runtime-api-maturation.
// prepare() parses + compiles WASM; instantiate() creates live instances
// dispatched on the requested backend kind via the abstract `instance_be`.
// instance provides gas_state, linear_memory, run_start, and typed proxy
// access.

#include <psizam/runtime.hpp>
#include <psizam/backend.hpp>
#include <psizam/detail/instance_be.hpp>
#include <psizam/detail/pzam_loader.hpp>
#include <psizam/detail/wasi_host.hpp>
#include <psizam/detail/wit_section_finder.hpp>
#include <psizam/host_function.hpp>
#include <psizam/host_function_table.hpp>

#include <psio/wit_parser.hpp>
#include <psio/wit_types.hpp>

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

   std::optional<operand_stack_elem>
      call_export_canonical(void*            host,
                            std::string_view name,
                            const uint64_t*  args,
                            std::size_t      count) override
   {
      // Canonical ABI flat call: always 16 u64 slots. Pad with zero
      // for callers that pass fewer (e.g. shorter argument lists).
      auto slot = [&](std::size_t i) -> uint64_t {
         return i < count ? args[i] : uint64_t{0};
      };
      return be_->call_with_return(
         host, name,
         slot(0),  slot(1),  slot(2),  slot(3),
         slot(4),  slot(5),  slot(6),  slot(7),
         slot(8),  slot(9),  slot(10), slot(11),
         slot(12), slot(13), slot(14), slot(15));
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
   // Common
   host_function_table   table;
   instance_policy       policy;
   void*                 host_ptr = nullptr;
   uint32_t              compile_ms = 0;

   // prepare()-path state — raw WASM bytes; backend constructed at
   // instantiate() time.
   std::vector<uint8_t>  wasm_copy;

   // load_cached()-path state — fully relocated executable code +
   // restored module ready for `jit_execution_context<>` construction
   // at instantiate() time. Track A lands the loader; Track B will
   // wire instantiate() to consume this state.
   std::unique_ptr<detail::pzam_load_result> pzam;

   module_handle_impl() = default;

   ~module_handle_impl() {
      // Release the executable code page back to the JIT allocator if we
      // own one. This is the only owner of the pzam exec_code; instances
      // hold a non-owning view via the module reference.
      if (pzam && pzam->exec_code) {
         jit_allocator::instance().free(pzam->exec_code);
         pzam->exec_code = nullptr;
      }
   }
};

host_function_table& module_handle::table() { return impl_->table; }
void module_handle::set_host_ptr(void* p) { impl_->host_ptr = p; }

uint32_t module_handle::compile_time_ms() const {
   return impl_ ? impl_->compile_ms : 0;
}
uint32_t module_handle::native_code_size() const {
   if (!impl_) return 0;
   if (impl_->pzam) return static_cast<uint32_t>(impl_->pzam->total_code_size);
   return 0;   // prepare()-path: real value lands with Track B's compile.
}
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
detail::instance_be* instance::get_instance_be() {
   return impl_ ? impl_->be.get() : nullptr;
}

char* instance::linear_memory() {
   return impl_ && impl_->be ? impl_->be->linear_memory() : nullptr;
}
std::size_t instance::memory_size() const {
   return impl_ && impl_->be
      ? static_cast<std::size_t>(impl_->be->current_pages()) * 65536
      : 0;
}

// Enum value parity (one source of truth, two declarations) — public
// `psizam::backend_kind` and internal `detail::backend_kind` must stay
// in lockstep because instance::kind() casts between them.
static_assert(static_cast<uint8_t>(backend_kind::interpreter) ==
              static_cast<uint8_t>(detail::backend_kind::interpreter));
static_assert(static_cast<uint8_t>(backend_kind::jit) ==
              static_cast<uint8_t>(detail::backend_kind::jit));
static_assert(static_cast<uint8_t>(backend_kind::jit2) ==
              static_cast<uint8_t>(detail::backend_kind::jit2));
static_assert(static_cast<uint8_t>(backend_kind::jit_llvm) ==
              static_cast<uint8_t>(detail::backend_kind::jit_llvm));

backend_kind instance::kind() const {
   if (!impl_ || !impl_->be) return backend_kind::interpreter;
   return static_cast<backend_kind>(impl_->be->kind());
}

int instance::run_start() {
   if (!impl_ || !impl_->be) return -1;
   return impl_->be->run_start(impl_->host_ptr);
}

// ═════════════════════════════════════════════════════════════════════
// runtime_impl
// ═════════════════════════════════════════════════════════════════════

// Stored bytes for a registered library — the runtime keeps the bytes
// for future link / lookup; archive parsing into member .o files is left
// to whatever consumes them downstream (this layer just owns the data).
struct registered_library {
   enum class kind { wasm, archive };
   kind                   kind;
   std::vector<uint8_t>   bytes;
};

// Stable 64-bit hash over a wasm byte range. Used as the module-cache
// key. Track B will replace this with module_cache_key (composing
// wasm_hash + compile_hash + env_hash) once compile_policy lands.
inline uint64_t hash_wasm_bytes(std::span<const uint8_t> bytes) noexcept {
   return std::hash<std::string_view>{}(std::string_view{
      reinterpret_cast<const char*>(bytes.data()), bytes.size()});
}

struct runtime_impl {
   runtime_config config;

   // wasm_hash → prepared module. weak_ptr keeps the cache from
   // pinning modules indefinitely; an entry whose module_handle has
   // been dropped by every external owner is garbage on the next
   // lookup. Track B will key on the full module_cache_key
   // (wasm + compile_policy + env) once compile_policy lands.
   std::unordered_map<uint64_t, std::weak_ptr<module_handle_impl>>
      module_cache;

   // Library name → bytes + kind. Lookup-by-name is what consumers
   // need; archive parsing happens on demand at link time.
   std::unordered_map<std::string, registered_library>
      libraries;
};

runtime::runtime(runtime_config config)
   : impl_(std::make_unique<runtime_impl>(runtime_impl{config})) {}

runtime::~runtime() = default;
runtime::runtime(runtime&&) noexcept = default;
runtime& runtime::operator=(runtime&&) noexcept = default;

void runtime::register_library(std::string_view name, archive_bytes archive) {
   if (!impl_) return;
   registered_library lib;
   lib.kind = registered_library::kind::archive;
   lib.bytes.assign(archive.data.begin(), archive.data.end());
   impl_->libraries[std::string{name}] = std::move(lib);
}

void runtime::register_library(std::string_view name, wasm_bytes wasm) {
   if (!impl_) return;
   registered_library lib;
   lib.kind = registered_library::kind::wasm;
   lib.bytes.assign(wasm.data.begin(), wasm.data.end());
   impl_->libraries[std::string{name}] = std::move(lib);
}

module_handle runtime::prepare(wasm_bytes wasm, const instance_policy& policy) {
   const uint64_t key = hash_wasm_bytes(wasm.data);

   // Cache hit? Track A keys only on wasm bytes; Track B will extend the
   // key to include compile_policy so divergent policies don't collide.
   if (impl_) {
      auto it = impl_->module_cache.find(key);
      if (it != impl_->module_cache.end()) {
         if (auto live = it->second.lock())
            return module_handle{std::move(live)};
         impl_->module_cache.erase(it);
      }
   }

   const auto t0 = std::chrono::high_resolution_clock::now();

   auto mod = std::make_shared<module_handle_impl>();
   mod->wasm_copy.assign(wasm.data.begin(), wasm.data.end());
   mod->policy = policy;

   const auto t1 = std::chrono::high_resolution_clock::now();
   mod->compile_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

   if (impl_)
      impl_->module_cache[key] = std::weak_ptr<module_handle_impl>{mod};

   return module_handle{std::move(mod)};
}

module_handle runtime::load_cached(pzam_bytes pzam) {
   const auto t0 = std::chrono::high_resolution_clock::now();

   // Parse the .pzam container envelope.
   const std::span<const char> raw{
      reinterpret_cast<const char*>(pzam.data.data()),
      pzam.data.size()};
   if (!pzam_validate(raw))
      throw std::runtime_error{"runtime::load_cached: invalid .pzam"};

   pzam_file file = pzam_load(raw);
   if (file.magic != PZAM_MAGIC)
      throw std::runtime_error{"runtime::load_cached: bad .pzam magic"};

   // Run the full loader: pick code section, restore module, build symbol
   // table, generate aarch64 veneers, allocate exec memory, apply
   // relocations, flip RX, fix up function entries + element segments,
   // derive trampoline direction.
   auto loaded = std::make_unique<detail::pzam_load_result>(
      detail::load_pzam(file));

   auto mod = std::make_shared<module_handle_impl>();
   mod->pzam = std::move(loaded);

   const auto t1 = std::chrono::high_resolution_clock::now();
   mod->compile_ms = static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());

   return module_handle{std::move(mod)};
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

void runtime::bind(module_handle& consumer_mod, instance& provider,
                   std::string_view interface_name)
{
   if (!consumer_mod || !provider) return;

   auto* mod_impl = consumer_mod.get();
   if (mod_impl->wasm_copy.empty())
      throw std::runtime_error{
         "runtime::bind(dynamic): consumer module has no WASM bytes; "
         "load_cached-path modules don't carry WIT custom sections — "
         "use the typed bind<InterfaceTag>() overload instead"};

   // Find the `component-wit:<interface_name>` custom section in the
   // consumer's raw WASM module.
   std::span<const uint8_t> wit_payload;
   const std::span<const uint8_t> wasm{mod_impl->wasm_copy};
   if (!detail::find_component_wit_section(wasm, interface_name, wit_payload))
      throw std::runtime_error{
         std::string{"runtime::bind(dynamic): consumer has no "
                     "component-wit:"} + std::string{interface_name} +
         " custom section"};

   // Parse the WIT text. The component-wit section payload is utf-8 text.
   const std::string_view wit_text{
      reinterpret_cast<const char*>(wit_payload.data()), wit_payload.size()};
   psio::wit_world world = psio::wit_parser::parse_wit(wit_text);

   // Locate the interface by name. Consumer-side: the named interface
   // is what the consumer IMPORTS from the provider, so search imports
   // first; fall back to exports for symmetric / round-tripped WIT.
   const psio::wit_interface* iface = nullptr;
   for (const auto& candidate : world.imports)
      if (candidate.name == interface_name) { iface = &candidate; break; }
   if (!iface)
      for (const auto& candidate : world.exports)
         if (candidate.name == interface_name) { iface = &candidate; break; }
   if (!iface)
      throw std::runtime_error{
         std::string{"runtime::bind(dynamic): interface `"} +
         std::string{interface_name} +
         "` not found in consumer's WIT world"};

   // For each WIT-declared method, register a bridge entry on the
   // consumer's host function table. The entry's slow_dispatch routes
   // calls into the provider via the erased `call_export_canonical` —
   // works regardless of which backend kind the provider runs on.
   //
   // The bridge uses the canonical-ABI 16-slot flat call convention
   // (matches the typed bind<InterfaceTag>() in runtime.hpp). The
   // consumer's import import-side stub is generated by PSIO_WIT to
   // pack args into the 16-slot flat shape, and the provider's
   // exported function unpacks them — both sides agree on the shape
   // regardless of the WIT-declared logical signature.
   detail::instance_be* provider_be = provider.get_instance_be();
   void*                provider_host = provider.host_ptr();
   if (!provider_be)
      throw std::runtime_error{
         "runtime::bind(dynamic): provider instance has no live backend"};

   for (uint32_t func_idx : iface->func_idxs) {
      if (func_idx >= world.funcs.size()) continue;
      const psio::wit_func& wf = world.funcs[func_idx];

      host_function_table::entry e;
      e.module_name = std::string{interface_name};
      e.func_name   = wf.name;
      e.signature.params.assign(16, types::i64);
      e.signature.ret = {types::i64};

      const std::string export_name = wf.name;
      e.slow_dispatch = [provider_be, provider_host, export_name](
         void*, native_value* args, char*) -> native_value
      {
         uint64_t slots[16];
         for (int i = 0; i < 16; ++i)
            slots[i] = static_cast<uint64_t>(args[i].i64);
         auto r = provider_be->call_export_canonical(
            provider_host, std::string_view{export_name}, slots, 16);
         native_value rv;
         rv.i64 = r ? r->to_ui64() : 0;
         return rv;
      };

      mod_impl->table.add_entry(std::move(e));
   }
}

runtime::cache_stats runtime::stats() const {
   if (!impl_) return {0, 0, 0};

   std::size_t modules            = 0;
   std::size_t pzam_bytes_total   = 0;
   std::size_t compile_time_total = 0;
   for (const auto& [_, weak] : impl_->module_cache) {
      if (auto live = weak.lock()) {
         ++modules;
         if (live->pzam) pzam_bytes_total += live->pzam->total_code_size;
         compile_time_total += live->compile_ms;
      }
   }
   return {modules, pzam_bytes_total, compile_time_total};
}

void runtime::evict(wasm_bytes wasm) {
   if (!impl_) return;
   impl_->module_cache.erase(hash_wasm_bytes(wasm.data));
}

void runtime::clear_cache() {
   if (!impl_) return;
   impl_->module_cache.clear();
   impl_->libraries.clear();
}

} // namespace psizam
