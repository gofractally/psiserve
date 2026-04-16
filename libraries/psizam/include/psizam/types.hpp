#pragma once

/*
 * definitions from https://github.com/WebAssembly/design/blob/master/BinaryEncoding.md
 */

#include <psizam/allocator.hpp>
#include <psizam/detail/guarded_ptr.hpp>
#include <psizam/detail/opcodes.hpp>
#include <psizam/detail/vector.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <string_view>
#include <vector>

namespace psizam {
   using detail::guarded_ptr;
   using detail::managed_vector;
   using detail::v128_t;
   using detail::opcode;
   using detail::opcodes;

   enum types { i32 = 0x7f, i64 = 0x7e, f32 = 0x7d, f64 = 0x7c, v128 = 0x7b, anyfunc = 0x70, funcref = anyfunc, externref = 0x6f, exnref = 0x69, func = 0x60, pseudo = 0x40, ret_void };

   enum external_kind { Function = 0, Table = 1, Memory = 2, Global = 3, Tag = 4 };

   typedef uint8_t value_type;
   typedef uint8_t block_type;
   typedef uint8_t elem_type;

   template <typename T>
   using guarded_vector = managed_vector<T, growable_allocator>;

   struct activation_frame {
      opcode* pc;
      uint32_t last_op_index;
      uint32_t frame_size;  // for tail call depth tracking
   };

   struct resizable_limits {
      bool     flags;
      uint32_t initial;
      uint32_t maximum = 0;
   };

   struct func_type {
      value_type                 form; // value for the func type constructor
      std::vector<value_type>    param_types;
      std::vector<value_type>    return_types;
      uint64_t                   sig_hash = 0; // precomputed signature hash for fast call_indirect checks

      // Convenience accessors for the common 0-or-1 return case
      uint8_t    return_count  = 0;
      value_type return_type   = 0;

      void finalize_returns() {
         return_count = static_cast<uint8_t>(return_types.size());
         return_type  = return_types.empty() ? value_type(0) : return_types[0];
      }

      void compute_sig_hash() {
         // FNV-1a over the signature components
         uint64_t h = 0xcbf29ce484222325ull;
         auto mix = [&](uint8_t byte) { h ^= byte; h *= 0x100000001b3ull; };
         mix(form);
         mix(static_cast<uint8_t>(param_types.size()));
         mix(static_cast<uint8_t>(param_types.size() >> 8));
         for (auto pt : param_types) mix(pt);
         mix(static_cast<uint8_t>(return_types.size()));
         for (auto rt : return_types) mix(rt);
         sig_hash = h;
      }
   };

   inline bool operator==(const func_type& lhs, const func_type& rhs) {
      if (lhs.sig_hash != rhs.sig_hash) return false;
      return lhs.form == rhs.form &&
        lhs.param_types.size() == rhs.param_types.size() &&
        std::equal(lhs.param_types.data(), lhs.param_types.data() + lhs.param_types.size(), rhs.param_types.data()) &&
        lhs.return_types.size() == rhs.return_types.size() &&
        std::equal(lhs.return_types.data(), lhs.return_types.data() + lhs.return_types.size(), rhs.return_types.data());
   }

   union expr_value {
      int32_t  i32;
      int64_t  i64;
      uint32_t f32;
      uint64_t f64;
      v128_t   v128;
   };

   struct init_expr {
      expr_value value;
      uint8_t    opcode;
      std::vector<uint8_t> raw_expr; // non-empty for extended const expressions (i32.add/sub/mul, i64.add/sub/mul)

      // Evaluate this init expression, resolving global.get references against the provided globals.
      // For simple expressions, returns value directly. For extended expressions, runs a mini stack machine.
      expr_value evaluate(const std::vector<init_expr>& globals) const {
         if (raw_expr.empty()) {
            if (opcode == opcodes::get_global) {
               return globals[value.i32].value;
            }
            return value;
         }
         // Mini stack machine for extended const expressions.
         // Max depth is bounded by the parser (only const/global.get push, add/sub/mul are net-zero).
         static constexpr int max_stack = 16;
         int64_t stack[max_stack];
         int sp = 0;
         size_t pos = 0;
         auto read_leb_i32 = [&]() -> int32_t {
            int32_t v = 0;
            unsigned shift = 0;
            uint8_t b;
            do {
               PSIZAM_ASSERT(pos < raw_expr.size(), wasm_interpreter_exception, "init_expr: read past end");
               b = raw_expr[pos++];
               v |= static_cast<int32_t>(b & 0x7f) << shift;
               shift += 7;
            } while (b & 0x80);
            if (shift < 32 && (b & 0x40))
               v |= -(static_cast<int32_t>(1) << shift);
            return v;
         };
         auto read_leb_u32 = [&]() -> uint32_t {
            uint32_t v = 0;
            unsigned shift = 0;
            uint8_t b;
            do {
               PSIZAM_ASSERT(pos < raw_expr.size(), wasm_interpreter_exception, "init_expr: read past end");
               b = raw_expr[pos++];
               v |= static_cast<uint32_t>(b & 0x7f) << shift;
               shift += 7;
            } while (b & 0x80);
            return v;
         };
         auto read_leb_i64 = [&]() -> int64_t {
            int64_t v = 0;
            unsigned shift = 0;
            uint8_t b;
            do {
               PSIZAM_ASSERT(pos < raw_expr.size(), wasm_interpreter_exception, "init_expr: read past end");
               b = raw_expr[pos++];
               v |= static_cast<int64_t>(b & 0x7f) << shift;
               shift += 7;
            } while (b & 0x80);
            if (shift < 64 && (b & 0x40))
               v |= -(static_cast<int64_t>(1) << shift);
            return v;
         };
         auto push = [&](int64_t v) { PSIZAM_ASSERT(sp < max_stack, wasm_interpreter_exception, "init_expr: stack overflow"); stack[sp++] = v; };
         auto pop  = [&]() -> int64_t { PSIZAM_ASSERT(sp > 0, wasm_interpreter_exception, "init_expr: stack underflow"); return stack[--sp]; };
         while (pos < raw_expr.size()) {
            uint8_t op = raw_expr[pos++];
            switch (op) {
               case opcodes::i32_const: push(read_leb_i32()); break;
               case opcodes::i64_const: push(read_leb_i64()); break;
               case opcodes::get_global: {
                  uint32_t idx = read_leb_u32();
                  PSIZAM_ASSERT(idx < globals.size(), wasm_interpreter_exception, "init_expr: global index out of range");
                  push(globals[idx].value.i64);
                  break;
               }
               case opcodes::i32_add: { auto b = pop(); auto a = pop(); push(static_cast<int32_t>(static_cast<int32_t>(a) + static_cast<int32_t>(b))); break; }
               case opcodes::i32_sub: { auto b = pop(); auto a = pop(); push(static_cast<int32_t>(static_cast<int32_t>(a) - static_cast<int32_t>(b))); break; }
               case opcodes::i32_mul: { auto b = pop(); auto a = pop(); push(static_cast<int32_t>(static_cast<int32_t>(a) * static_cast<int32_t>(b))); break; }
               case opcodes::i64_add: { auto b = pop(); auto a = pop(); push(a + b); break; }
               case opcodes::i64_sub: { auto b = pop(); auto a = pop(); push(a - b); break; }
               case opcodes::i64_mul: { auto b = pop(); auto a = pop(); push(a * b); break; }
               case opcodes::end:
               default:
                  pos = raw_expr.size(); // exit loop
                  break;
            }
         }
         expr_value result;
         result.i64 = (sp > 0) ? stack[0] : 0;
         return result;
      }
   };

   struct global_type {
      value_type content_type;
      bool       mutability;
   };

   struct global_variable {
      global_type type;
      init_expr   init;
   };

   struct table_type {
      elem_type        element_type;
      resizable_limits limits;
   };

   struct table_entry {
      std::uint32_t type;
      std::uint32_t index;
      // The code writer is responsible for filling this field
      const void*   code_ptr;
   };

   struct memory_type {
      resizable_limits limits;
      bool is_memory64 = false;  // memory64 proposal: i64 addressing
   };

   struct tag_type {
      uint8_t  attribute;   // always 0 (exception)
      uint32_t type_index;  // index into module.types (params = exception payload)
   };

   // WASM 3.0 try_table catch clause kinds
   enum catch_kind : uint8_t {
      catch_tag     = 0x00,  // catch tag_idx label — push payload
      catch_tag_ref = 0x01,  // catch_ref tag_idx label — push payload + exnref
      catch_all_    = 0x02,  // catch_all label — no payload
      catch_all_ref = 0x03,  // catch_all_ref label — push exnref only
   };

   struct catch_clause {
      uint8_t  kind;       // catch_kind
      uint32_t tag_index;  // tag index (only for catch_tag/catch_tag_ref)
      uint32_t label;      // branch target (depth into pc_stack)
   };

   union import_type {
      import_type() {}
      uint32_t    func_t;
      table_type  table_t;
      memory_type mem_t;
      global_type global_t;
   };

   struct import_entry {
      std::vector<uint8_t>    module_str;
      std::vector<uint8_t>    field_str;
      external_kind           kind;
      import_type             type;
   };

   struct export_entry {
      std::vector<uint8_t>    field_str;
      external_kind           kind;
      uint32_t                index;
   };

   enum class elem_mode { active, passive, declarative };

   struct elem_segment {
      uint32_t                    index;
      init_expr                   offset;
      elem_mode                   mode;
      uint8_t                     type = types::funcref;
      std::vector<table_entry>    elems;
   };

   struct local_entry {
      uint32_t   count;
      value_type type;
   };

   union native_value {
      native_value() = default;
      constexpr native_value(uint32_t arg) : i32(arg) {}
      constexpr native_value(uint64_t arg) : i64(arg) {}
      constexpr native_value(float arg) : f32(arg) {}
      constexpr native_value(double arg) : f64(arg) {}
      uint32_t i32;
      uint64_t i64;
      float f32;
      double f64;
   };

   union native_value_extended {
      native_value scalar;
      v128_t vector;
   };

   struct branch_hint {
      uint32_t offset;   // byte offset within function body (from local_count byte)
      uint8_t  value;    // 0 = unlikely, 1 = likely
   };

   struct function_body {
      uint32_t                    size;
      std::vector<local_entry>    locals;
      opcode*                     code;
      std::size_t                 jit_code_offset;
      std::uint32_t               jit_code_size = 0;
      std::uint32_t               stack_size = 0;
      const uint8_t*              body_start = nullptr;  // pointer to local_count byte in WASM binary
      std::vector<branch_hint>    branch_hints;          // sorted by offset
   };

   struct data_segment {
      uint32_t                index;
      init_expr               offset;
      bool                    passive;
      std::vector<uint8_t>    data;
   };

   using wasm_code     = std::vector<uint8_t>;
   using wasm_code_ptr = guarded_ptr<uint8_t>;
   typedef std::uint32_t  wasm_ptr_t;
   typedef std::uint32_t  wasm_size_t;

   struct name_assoc {
      std::uint32_t idx;
      std::vector<uint8_t> name;
   };
   struct indirect_name_assoc {
      std::uint32_t idx;
      std::vector<name_assoc> namemap;
   };
   struct name_section {
      std::optional<std::vector<uint8_t>> module_name;
      std::optional<std::vector<name_assoc>> function_names;
      std::optional<std::vector<indirect_name_assoc>> local_names;
   };

   struct module {
      growable_allocator              allocator;
      uint32_t                        start     = std::numeric_limits<uint32_t>::max();
      std::vector<func_type>          types;
      std::vector<import_entry>       imports;
      std::vector<uint32_t>           functions;
      std::vector<table_type>         tables;
      std::vector<memory_type>        memories;
      std::vector<global_variable>    globals;
      std::vector<export_entry>       exports;
      std::vector<elem_segment>       elements;
      std::vector<function_body>      code;
      std::vector<data_segment>       data;
      std::vector<tag_type>           tags;

      // Custom sections:
      std::optional<name_section> names;

      // not part of the spec for WASM
      std::vector<uint32_t>    import_functions;
      uint32_t                 num_imported_tables   = 0;
      uint32_t                 num_imported_memories = 0;
      uint32_t                 num_imported_globals  = 0;
      uint64_t                 maximum_stack    = 0;
      // The stack limit can be tracked as either frames or bytes
      bool                     stack_limit_is_bytes = false;
      // If non-null, indicates that the parser encountered an error
      // that would prevent successful instantiation.  Must refer
      // to memory with static storage duration.
      const char *             error            = nullptr;
      // Type-erased holder for LLVM JIT engine — keeps compiled code alive
      std::shared_ptr<void>    jit_engine;

      void finalize() {
         import_functions.resize(get_imported_functions_size());
         allocator.finalize();
      }
      uint32_t get_imported_functions_size() const {
         return get_imported_functions_size_impl(imports);
      }
      template<typename Imports>
      static uint32_t get_imported_functions_size_impl(const Imports& imports) {
         uint32_t number_of_imports = 0;
         const auto sz = imports.size();
         // we don't want to use `imports[i]` or `imports.at(i)` since these do an unnecessary check
         // `PSIZAM_ASSERT(i < _size)`. The check is unnecessary since we iterate from `0` to `_size`.
         // So get the pointer to the first element and dereference it directly.
         // ------------------------------------------------------------------------------------------------
         const auto data = imports.data();
         for (uint32_t i = 0; i < sz; i++) {
            if (data[i].kind == external_kind::Function)
               number_of_imports++;
         }
         return number_of_imports;
      }
      inline uint32_t get_functions_size() const { return functions.size(); }
      inline uint32_t get_functions_total() const { return get_imported_functions_size() + get_functions_size(); }
      inline opcode* get_function_pc( uint32_t fidx ) const {
         PSIZAM_ASSERT( fidx >= get_imported_functions_size(), wasm_interpreter_exception, "trying to get the PC of an imported function" );
         return code.at(fidx-get_imported_functions_size()).code;
      }

      inline uint32_t get_function_locals_size(uint32_t index) const {
         PSIZAM_ASSERT(index >= get_imported_functions_size(), wasm_interpreter_exception, "imported functions do not have locals");
         return code.at(index - get_imported_functions_size()).locals.size();
      }

      auto& get_function_type(uint32_t index) const {
         if (index < get_imported_functions_size())
            return types[imports[index].type.func_t];
         uint32_t local_idx = index - get_imported_functions_size();
         PSIZAM_ASSERT(local_idx < functions.size(), wasm_parse_exception, "function index out of range");
         return types.at(functions[local_idx]);
      }

      uint32_t get_function_stack_size(uint32_t index) const {
         if (!stack_limit_is_bytes) {
            return 1;
         } else if (index < get_imported_functions_size()) {
            return 0;
         } else {
            return code.at(index - get_imported_functions_size()).stack_size;
         }
      }

      uint32_t get_exported_function(const std::string_view str) {
         return get_exported_function_impl(exports, str);
      }

      template<typename Exports>
      static uint32_t get_exported_function_impl(const Exports& exports, const std::string_view str) {
         uint32_t index = std::numeric_limits<uint32_t>::max();
         for (uint32_t i = 0; i < exports.size(); i++) {
            if (exports[i].kind == external_kind::Function && exports[i].field_str.size() == str.size() &&
                memcmp((const char*)str.data(), (const char*)exports[i].field_str.data(), exports[i].field_str.size()) ==
                      0) {
               index = exports[i].index;
               break;
            }
         }
         return index;
      }

      uint32_t get_exported_global(const std::string_view str) {
         uint32_t index = std::numeric_limits<uint32_t>::max();
         for (uint32_t i = 0; i < exports.size(); i++) {
            if (exports[i].kind == external_kind::Global && exports[i].field_str.size() == str.size() &&
                memcmp((const char*)str.data(), (const char*)exports[i].field_str.data(), exports[i].field_str.size()) ==
                      0) {
               index = exports[i].index;
               break;
            }
         }
         return index;
      }

      static std::size_t get_global_offset(std::uint32_t idx)
      {
         return idx * sizeof(init_expr) + offsetof(init_expr, value);
      }

      bool indirect_table(std::size_t i)
      {
         return indirect_table_impl(*this, i);
      }

      static bool indirect_table_impl(auto& mod, std::size_t i) {
#ifdef PSIZAM_COMPILE_ONLY
         // In compile-only mode, table allocation layout doesn't matter
         return false;
#else
         if (i >= mod.tables.size()) return false;
         // Large tables that don't fit in the prefix area need heap allocation + pointer
         bool large = mod.tables[i].limits.initial * sizeof(table_entry) > (wasm_allocator::table_size()) - sizeof(void*);
         // Growable table 0 needs indirect because table.grow moves entries to the heap.
         // The JIT must use pointer indirection so the updated pointer is followed.
         bool growable = (i == 0) &&
            (mod.tables[i].limits.flags == 0 || mod.tables[i].limits.maximum > mod.tables[i].limits.initial);
         return large || growable;
#endif
      }
   };
} // namespace psizam
