#pragma once

// Typed C++ API for loading and interacting with .pzam WASM modules.
//
// Uses psio reflection to provide zero-boilerplate typed access to WASM
// exports and automatic registration of host function imports.
//
// Usage:
//
//   // Define your host functions (WASM imports):
//   struct my_host {
//      void log(uint32_t ptr, uint32_t len);
//      uint32_t get_time();
//   };
//   PSIO_REFLECT(my_host, method(log, ptr, len), method(get_time))
//
//   // Define WASM exports you want to call:
//   struct my_exports {
//      uint32_t init();
//      uint32_t handle(uint32_t path_ptr, uint32_t path_len);
//   };
//   PSIO_REFLECT(my_exports, method(init), method(handle, path_ptr, path_len))
//
//   // Load and use:
//   my_host host_impl;
//   auto instance = pzam_load<my_host, my_exports>(pzam_data, host_impl);
//   instance.exports().init();
//   instance.exports().handle(ptr, len);

#include <psizam/backend.hpp>
#include <psizam/host_function_table.hpp>
#include <psizam/pzam_cache.hpp>
#include <psizam/pzam_format.hpp>
#include <psizam/pzam_metadata.hpp>

#include <psio/reflect.hpp>

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

#include <sys/mman.h>
#include <unistd.h>

namespace psizam {

   // =========================================================================
   // Import auto-registration
   // =========================================================================

   namespace detail {
      template<auto... Funcs>
      void register_methods(host_function_table& table, const std::string& module_name,
                            psio::MemberList<Funcs...>*,
                            const std::initializer_list<const char*>* names) {
         std::size_t i = 0;
         (table.add<Funcs>(module_name, std::string(*names[i++].begin())), ...);
      }
   } // namespace detail

   /// Auto-register all reflected methods of Host as WASM imports.
   /// Each method in Host becomes a host function under the given module name.
   ///
   ///   struct my_host {
   ///      void log(uint32_t ptr, uint32_t len);
   ///   };
   ///   PSIO_REFLECT(my_host, method(log, ptr, len))
   ///
   ///   host_function_table table;
   ///   register_reflected<my_host>(table, "env");
   ///   // Equivalent to: table.add<&my_host::log>("env", "log");
   ///
   template<typename Host>
   void register_reflected(host_function_table& table, const std::string& module_name) {
      using R = psio::reflect<Host>;
      detail::register_methods(table, module_name,
         static_cast<typename R::member_functions*>(nullptr),
         R::member_function_names);
   }

   // =========================================================================
   // Export proxy — calling WASM from C++
   // =========================================================================

   namespace detail {
      // Convert operand_stack_elem to a specific C++ type.
      template<typename T>
      T from_wasm_result(const std::optional<operand_stack_elem>& result) {
         if constexpr (std::is_same_v<T, void>) {
            return;
         } else if constexpr (std::is_same_v<T, uint32_t> || std::is_same_v<T, int32_t>) {
            if (!result) return T{};
            return static_cast<T>(result->to_ui32());
         } else if constexpr (std::is_same_v<T, uint64_t> || std::is_same_v<T, int64_t>) {
            if (!result) return T{};
            return static_cast<T>(result->to_ui64());
         } else if constexpr (std::is_same_v<T, float>) {
            if (!result) return T{};
            return result->to_f32();
         } else if constexpr (std::is_same_v<T, double>) {
            if (!result) return T{};
            return result->to_f64();
         } else {
            static_assert(sizeof(T) == 0, "Unsupported return type for WASM export");
         }
      }
   } // namespace detail

   /// ProxyObject for psio::reflect proxy pattern.
   /// Dispatches method calls to WASM exported functions via the execution context.
   template<typename Exports>
   class pzam_export_dispatch {
      detail::jit_execution_context<>* _ctx;
      module*                  _mod;
      void*                    _host;

      // Cached function indices: UINT32_MAX = not yet looked up
      mutable std::vector<uint32_t> _func_indices;

      uint32_t resolve_index(std::size_t method_index) const {
         if (_func_indices.empty())
            _func_indices.resize(sizeof(psio::reflect<Exports>::member_function_names) /
                                 sizeof(psio::reflect<Exports>::member_function_names[0]),
                                 UINT32_MAX);

         if (_func_indices[method_index] == UINT32_MAX) {
            const char* name = *psio::reflect<Exports>::member_function_names[method_index].begin();
            _func_indices[method_index] = _mod->get_exported_function(name);
         }
         return _func_indices[method_index];
      }

   public:
      pzam_export_dispatch(detail::jit_execution_context<>* ctx, module* mod, void* host)
         : _ctx(ctx), _mod(mod), _host(host) {}

      template<std::size_t I, auto MemberPtr, typename... Args>
      decltype(auto) call(Args&&... args) {
         uint32_t func_idx = resolve_index(I);
         auto result = _ctx->execute(_host, detail::jit_visitor{nullptr}, func_idx,
                                     std::forward<Args>(args)...);
         using R = std::invoke_result_t<decltype(MemberPtr), Exports, Args...>;
         if constexpr (std::is_void_v<R>) {
            return;
         } else {
            return detail::from_wasm_result<R>(result);
         }
      }

      template<std::size_t I, auto MemberPtr>
      decltype(auto) get() {
         static_assert(sizeof(MemberPtr) == 0,
            "pzam_export_dispatch: data member access not supported, use methods only");
      }
   };

   /// The typed export proxy. Wraps pzam_export_dispatch with psio's proxy pattern
   /// so you can call WASM exports with natural C++ syntax.
   template<typename Exports>
   using pzam_export_proxy = typename psio::reflect<Exports>::template proxy<pzam_export_dispatch<Exports>>;

   // =========================================================================
   // pzam_instance — loaded module ready to execute
   // =========================================================================

   /// A loaded and ready-to-execute .pzam instance with typed imports and exports.
   template<typename Imports, typename Exports>
   class pzam_instance {
      module                    _mod;
      wasm_allocator            _wa;
      detail::jit_execution_context<>   _ctx;
      host_function_table       _table;
      Imports*                  _host;
      void*                     _exec_code = nullptr;
      size_t                    _code_alloc_size = 0;

   public:
      pzam_instance(pzam_file&& pzam, Imports& host, const std::string& import_module = "env")
         : _mod(), _ctx(_mod, 8192), _host(&host)
      {
         // Find code section for this platform
         auto expected_arch =
#if defined(__x86_64__)
            pzam_arch::x86_64;
#elif defined(__aarch64__)
            pzam_arch::aarch64;
#else
            pzam_arch{};
#endif
         const pzam_code_section* cs = nullptr;
         for (const auto& section : pzam.code_sections) {
            if (static_cast<pzam_arch>(section.arch) == expected_arch) {
               cs = &section;
               break;
            }
         }
         if (!cs)
            throw std::runtime_error("pzam_instance: no code section for this architecture");

         // Restore module from metadata
         _mod = restore_module(pzam.metadata);
         _mod.allocator.use_default_memory();

         if (cs->functions.size() != _mod.code.size())
            throw std::runtime_error("pzam_instance: function count mismatch");

         // Register host functions from reflection
         register_reflected<Imports>(_table, import_module);
         _table.resolve(_mod);

         // Build symbol table for relocation
         void* symbol_table[static_cast<size_t>(reloc_symbol::NUM_SYMBOLS)];
#if defined(__aarch64__)
         std::memset(symbol_table, 0, sizeof(symbol_table));
         using jit_cg = detail::jit_codegen_a64;
         symbol_table[static_cast<uint32_t>(reloc_symbol::call_host_function)]     = reinterpret_cast<void*>(&jit_cg::call_host_function);
         symbol_table[static_cast<uint32_t>(reloc_symbol::current_memory)]         = reinterpret_cast<void*>(&jit_cg::current_memory);
         symbol_table[static_cast<uint32_t>(reloc_symbol::grow_memory)]            = reinterpret_cast<void*>(&jit_cg::grow_memory);
         symbol_table[static_cast<uint32_t>(reloc_symbol::memory_fill)]            = reinterpret_cast<void*>(&jit_cg::memory_fill_impl);
         symbol_table[static_cast<uint32_t>(reloc_symbol::memory_copy)]            = reinterpret_cast<void*>(&jit_cg::memory_copy_impl);
         symbol_table[static_cast<uint32_t>(reloc_symbol::memory_init)]            = reinterpret_cast<void*>(&jit_cg::memory_init_impl);
         symbol_table[static_cast<uint32_t>(reloc_symbol::data_drop)]              = reinterpret_cast<void*>(&jit_cg::data_drop_impl);
         symbol_table[static_cast<uint32_t>(reloc_symbol::table_init)]             = reinterpret_cast<void*>(&jit_cg::table_init_impl);
         symbol_table[static_cast<uint32_t>(reloc_symbol::elem_drop)]              = reinterpret_cast<void*>(&jit_cg::elem_drop_impl);
         symbol_table[static_cast<uint32_t>(reloc_symbol::table_copy)]             = reinterpret_cast<void*>(&jit_cg::table_copy_impl);
         symbol_table[static_cast<uint32_t>(reloc_symbol::on_unreachable)]         = reinterpret_cast<void*>(&jit_cg::on_unreachable);
         symbol_table[static_cast<uint32_t>(reloc_symbol::on_fp_error)]            = reinterpret_cast<void*>(&jit_cg::on_fp_error);
         symbol_table[static_cast<uint32_t>(reloc_symbol::on_memory_error)]        = reinterpret_cast<void*>(&jit_cg::on_memory_error);
         symbol_table[static_cast<uint32_t>(reloc_symbol::on_call_indirect_error)] = reinterpret_cast<void*>(&jit_cg::on_call_indirect_error);
         symbol_table[static_cast<uint32_t>(reloc_symbol::on_type_error)]          = reinterpret_cast<void*>(&jit_cg::on_type_error);
         symbol_table[static_cast<uint32_t>(reloc_symbol::on_stack_overflow)]      = reinterpret_cast<void*>(&jit_cg::on_stack_overflow);
#else
         build_symbol_table<detail::jit_codegen>(symbol_table);
#endif
         build_llvm_symbol_table(symbol_table);

         // Build relocations
         std::vector<code_relocation> relocs(cs->relocations.size());
         for (size_t j = 0; j < cs->relocations.size(); j++) {
            relocs[j].code_offset = cs->relocations[j].code_offset;
            relocs[j].symbol      = static_cast<reloc_symbol>(cs->relocations[j].symbol);
            relocs[j].type        = static_cast<reloc_type>(cs->relocations[j].type);
            relocs[j].addend      = cs->relocations[j].addend;
         }

         // Allocate executable memory
         size_t page_size = static_cast<size_t>(sysconf(_SC_PAGESIZE));
         size_t total_code_size = cs->code_blob.size();

#if defined(__aarch64__)
         // Generate veneers for external symbols
         std::unordered_map<uint16_t, uint32_t> veneer_offsets;
         size_t veneer_start = (total_code_size + 3) & ~size_t(3);
         for (auto& r : relocs) {
            if (r.type == reloc_type::aarch64_call26 &&
                r.symbol != reloc_symbol::code_blob_self) {
               auto sym_idx = static_cast<uint16_t>(r.symbol);
               if (veneer_offsets.find(sym_idx) == veneer_offsets.end()) {
                  veneer_offsets[sym_idx] = static_cast<uint32_t>(veneer_start + veneer_offsets.size() * 20);
               }
            }
         }
         total_code_size = veneer_start + veneer_offsets.size() * 20;
#endif

         _code_alloc_size = (total_code_size + page_size - 1) & ~(page_size - 1);
         auto& jit_alloc = jit_allocator::instance();
         _exec_code = jit_alloc.alloc(_code_alloc_size);

         mprotect(_exec_code, _code_alloc_size, PROT_READ | PROT_WRITE);
         std::memcpy(_exec_code, cs->code_blob.data(), cs->code_blob.size());

#if defined(__aarch64__)
         // Write veneers
         for (auto& [sym_idx, veneer_off] : veneer_offsets) {
            uint64_t target = reinterpret_cast<uint64_t>(symbol_table[sym_idx]);
            uint32_t* v = reinterpret_cast<uint32_t*>(static_cast<char*>(_exec_code) + veneer_off);
            v[0] = 0xD2800010u | ((static_cast<uint32_t>(target >>  0) & 0xFFFF) << 5);
            v[1] = 0xF2A00010u | ((static_cast<uint32_t>(target >> 16) & 0xFFFF) << 5);
            v[2] = 0xF2C00010u | ((static_cast<uint32_t>(target >> 32) & 0xFFFF) << 5);
            v[3] = 0xF2E00010u | ((static_cast<uint32_t>(target >> 48) & 0xFFFF) << 5);
            v[4] = 0xD61F0200u;
         }
         for (auto& r : relocs) {
            if (r.type == reloc_type::aarch64_call26 &&
                r.symbol != reloc_symbol::code_blob_self) {
               auto it = veneer_offsets.find(static_cast<uint16_t>(r.symbol));
               if (it != veneer_offsets.end()) {
                  r.symbol = reloc_symbol::code_blob_self;
                  r.addend = static_cast<int32_t>(it->second);
               }
            }
         }
#endif

         symbol_table[static_cast<uint32_t>(reloc_symbol::code_blob_self)] = _exec_code;

         apply_relocations(static_cast<char*>(_exec_code), relocs.data(),
                           static_cast<uint32_t>(relocs.size()), symbol_table);

         mprotect(_exec_code, _code_alloc_size, PROT_READ | PROT_EXEC);
#if defined(__aarch64__)
         __builtin___clear_cache(static_cast<char*>(_exec_code),
                                 static_cast<char*>(_exec_code) + total_code_size);
#endif

         // Update module function entries
         bool is_jit = static_cast<pzam_opt_tier>(cs->opt_tier) == pzam_opt_tier::jit1 ||
                       static_cast<pzam_opt_tier>(cs->opt_tier) == pzam_opt_tier::jit2;
         if (is_jit) {
            _mod.allocator._code_base = static_cast<char*>(_exec_code);
            for (size_t j = 0; j < cs->functions.size(); j++) {
               _mod.code[j].jit_code_offset = cs->functions[j].code_offset;
               _mod.code[j].jit_code_size   = cs->functions[j].code_size;
               _mod.code[j].stack_size       = cs->functions[j].stack_size;
            }
         } else {
            auto code_base_addr = reinterpret_cast<uintptr_t>(_exec_code);
            for (size_t j = 0; j < cs->functions.size(); j++) {
               _mod.code[j].jit_code_offset = code_base_addr + cs->functions[j].code_offset;
               _mod.code[j].jit_code_size   = cs->functions[j].code_size;
               _mod.code[j].stack_size       = cs->functions[j].stack_size;
            }
         }
         _mod.maximum_stack      = cs->max_stack;
         _mod.stack_limit_is_bytes = cs->stack_limit_mode != 0;

         // Fix up element segment code_ptr fields for JIT dispatch
         if (is_jit) {
            uint32_t num_imports = _mod.get_imported_functions_size();
            for (auto& elem_seg : _mod.elements) {
               for (auto& entry : elem_seg.elems) {
                  if (entry.index >= num_imports &&
                      entry.index < num_imports + cs->functions.size()) {
                     uint32_t code_idx = entry.index - num_imports;
                     entry.code_ptr = _mod.allocator._code_base + cs->functions[code_idx].code_offset;
                  }
               }
            }
         }

         // Initialize execution context
         _ctx = detail::jit_execution_context<>(_mod, 8192);
         _ctx.set_wasm_allocator(&_wa);
         _ctx.set_host_table(&_table);
         _ctx.reset();

         // Run start function if present
         if (_mod.start != std::numeric_limits<uint32_t>::max()) {
            _ctx.execute(_host, detail::jit_visitor{nullptr}, _mod.start);
         }
      }

      /// Get the typed export proxy for calling WASM functions.
      pzam_export_proxy<Exports> exports() {
         return pzam_export_proxy<Exports>(&_ctx, &_mod, _host);
      }

      /// Direct access to the execution context.
      detail::jit_execution_context<>& context() { return _ctx; }

      /// Direct access to the module.
      module& mod() { return _mod; }

      /// Access to linear memory.
      char* memory() { return _wa.get_base_ptr<char>(); }
   };

   // =========================================================================
   // Top-level load function
   // =========================================================================

   /// Load a .pzam file and create a typed instance.
   ///
   ///   struct my_host { void log(uint32_t, uint32_t); };
   ///   PSIO_REFLECT(my_host, method(log, ptr, len))
   ///
   ///   struct my_exports { uint32_t init(); };
   ///   PSIO_REFLECT(my_exports, method(init))
   ///
   ///   my_host host;
   ///   auto instance = pzam_load<my_host, my_exports>(pzam_data, host);
   ///   instance.exports().init();
   ///
   template<typename Imports, typename Exports>
   pzam_instance<Imports, Exports> pzam_load(std::span<const char> data, Imports& host,
                                             const std::string& import_module = "env") {
      if (!pzam_validate(data))
         throw std::runtime_error("pzam_load: invalid .pzam file");

      auto pzam = psizam::pzam_load(data);
      if (pzam.magic != PZAM_MAGIC)
         throw std::runtime_error("pzam_load: bad magic");

      return pzam_instance<Imports, Exports>(std::move(pzam), host, import_module);
   }

} // namespace psizam
