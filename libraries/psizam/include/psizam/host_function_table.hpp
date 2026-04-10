#pragma once

#include <psizam/host_function.hpp>

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace psizam {

   /// Unified trampoline signature for all host function dispatch.
   /// All executors (interpreter, JIT) call through this single signature.
   /// Args are always in forward order: args[0] = first WASM parameter.
   /// The trampoline knows the expected arg count at compile time.
   using host_trampoline_t = native_value(*)(void* host, native_value* args, char* memory);

   namespace detail {
      /// Extract host class type from an auto function pointer.
      /// Free functions -> standalone_function_t, member functions -> class type.
      template<auto Func>
      using host_class_t = std::conditional_t<
         is_member_function_v<Func>,
         class_from_member_t<Func>,
         standalone_function_t>;
   }

   namespace detail {
      /// Fast trampoline wrapper: casts void* to Cls*, delegates to fast_trampoline_fwd_impl
      template<auto Func, typename Cls, typename R, typename Args>
      native_value fast_void_trampoline(void* host, native_value* args, char* memory) {
         return fast_trampoline_fwd_impl<Func, Cls, R, Args>(
            static_cast<Cls*>(host), args, memory,
            std::make_index_sequence<std::tuple_size_v<Args>>{});
      }

      /// Slow trampoline: constructs a type_converter, dispatches through the full
      /// type conversion pipeline. Used for functions with custom from_wasm/to_wasm
      /// conversions or preconditions.
      template<auto Func, typename Cls, typename R, typename Args>
      native_value slow_void_trampoline(void* host, native_value* args, char* memory) {
         // Build operand stack from forward-order native_value args.
         // Push in forward order; type_converter reads from back using
         // total_operands-based indexing.
         constexpr uint32_t num_args = std::tuple_size_v<Args>;
         operand_stack temp_stack;
         for (uint32_t i = 0; i < num_args; i++) {
            // Push as i64 to preserve all 64 bits. The type_converter's
            // as_value<T> reinterprets via the operand_stack_elem union.
            temp_stack.push(i64_const_t{args[i].i64});
         }
         execution_interface ei{memory, &temp_stack};
         using TC = type_converter<Cls>;
         TC tc{static_cast<Cls*>(host), std::move(ei)};

         if constexpr (std::is_void_v<R>) {
            invoke_with_host<Func, std::tuple<>, Args>(
               tc, static_cast<Cls*>(host),
               std::make_index_sequence<std::tuple_size_v<Args>>{});
            return native_value{uint64_t{0}};
         } else {
            auto result = invoke_with_host<Func, std::tuple<>, Args>(
               tc, static_cast<Cls*>(host),
               std::make_index_sequence<std::tuple_size_v<Args>>{});
            return detail::write_native_result(result);
         }
      }
   } // namespace detail

   /// Non-templated runtime host function dispatch table.
   /// Replaces the templated registered_host_functions<Host> for the new API.
   class host_function_table {
   public:
      struct entry {
         host_trampoline_t  trampoline = nullptr;
         /// Slow-path dispatch for non-fast-eligible functions (those with custom
         /// type converters, preconditions, or pointer/reference parameters).
         /// Used when trampoline is null.
         std::function<native_value(void*, native_value*, char*)> slow_dispatch;
         host_function      signature;
         std::string        module_name;
         std::string        func_name;
      };

      host_function_table() = default;

      /// Construct from pre-built entries and name map (used by compatibility wrapper).
      host_function_table(std::vector<entry> entries,
                          const std::unordered_map<host_func_pair, uint32_t, host_func_pair_hash>& name_map)
         : _entries(std::move(entries)), _name_map(name_map) {}

      /// Register a host function with automatic trampoline generation.
      /// The host type is deduced from the function signature:
      ///   - Free functions: host pointer is unused (standalone)
      ///   - Member functions: void* host is cast to the class type
      template<auto Func>
      void add(const std::string& mod, const std::string& name) {
         using host_cls = detail::host_class_t<Func>;
         using TC = type_converter<host_cls>;
         add_impl<Func, TC>(mod, name);
      }

      /// Register with a custom type converter.
      template<auto Func, typename TypeConverter>
      void add(const std::string& mod, const std::string& name) {
         add_impl<Func, TypeConverter>(mod, name);
      }

      /// Resolve WASM module imports against this table.
      /// Sets mod.import_functions[i] to the table index for each import.
      void resolve(module& mod) const {
         for (std::size_t i = 0; i < mod.imports.size(); i++) {
            if (mod.imports[i].kind != Function)
               continue;
            std::string mod_name(reinterpret_cast<const char*>(mod.imports[i].module_str.data()),
                                 mod.imports[i].module_str.size());
            std::string fn_name(reinterpret_cast<const char*>(mod.imports[i].field_str.data()),
                                mod.imports[i].field_str.size());
            auto it = _name_map.find({mod_name, fn_name});
            PSIZAM_ASSERT(it != _name_map.end(), wasm_link_exception,
                          std::string("no mapping for imported function ") + fn_name);
            mod.import_functions[i] = it->second;
            PSIZAM_ASSERT(_entries[it->second].signature == mod.types[mod.imports[i].type.func_t],
                          wasm_link_exception,
                          std::string("wrong type for imported function ") + fn_name);
         }
      }

      /// Call a host function by mapped index.
      inline native_value call(void* host, uint32_t mapped_idx,
                               native_value* args, char* memory) const {
         PSIZAM_ASSERT(mapped_idx < _entries.size(), wasm_link_exception,
                       "unresolved imported function");
         auto& e = _entries[mapped_idx];
         if (e.trampoline)
            return e.trampoline(host, args, memory);
         PSIZAM_ASSERT(e.slow_dispatch, wasm_link_exception,
                       "unresolved imported function");
         return e.slow_dispatch(host, args, memory);
      }

      const entry& get_entry(uint32_t idx) const {
         PSIZAM_ASSERT(idx < _entries.size(), wasm_link_exception,
                       "unresolved imported function");
         return _entries[idx];
      }
      uint32_t size() const { return static_cast<uint32_t>(_entries.size()); }
      const std::vector<entry>& entries() const { return _entries; }

   private:
      std::vector<entry>                                                _entries;
      std::unordered_map<host_func_pair, uint32_t, host_func_pair_hash> _name_map;

      template<auto Func, typename TypeConverter>
      void add_impl(const std::string& mod, const std::string& name) {
         using args     = flatten_parameters_t<AUTO_PARAM_WORKAROUND(Func)>;
         using ret      = return_type_t<AUTO_PARAM_WORKAROUND(Func)>;
         using host_cls = detail::host_class_t<Func>;

         entry e;
         e.module_name = mod;
         e.func_name   = name;

         // Generate the WASM signature
         e.signature = function_types_provider<TypeConverter, ret, args>(
            std::make_index_sequence<std::tuple_size_v<args>>{});

         // Generate the trampoline
         if constexpr (detail::all_fast_eligible_v<args> &&
                       detail::is_simple_wasm_return_v<ret>) {
            e.trampoline = &detail::fast_void_trampoline<Func, host_cls, ret, args>;
         } else {
            e.trampoline = &detail::slow_void_trampoline<Func, host_cls, ret, args>;
         }

         uint32_t idx = static_cast<uint32_t>(_entries.size());
         _name_map[{mod, name}] = idx;
         _entries.push_back(std::move(e));
      }
   };

} // namespace psizam
