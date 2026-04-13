#pragma once

#include <psizam/allocator.hpp>
#include <psizam/constants.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/detail/leb128.hpp>
#include <psizam/options.hpp>
#include <psizam/detail/sections.hpp>
#include <psizam/types.hpp>
#include <psizam/utils.hpp>
#include <psizam/detail/vector.hpp>
#include <psizam/detail/debug_info.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace psizam::detail { struct pzam_compile_result; }

namespace psizam::detail {

   static constexpr unsigned get_size_for_type(uint8_t type) {
      switch(type) {
       case types::i32:
       case types::f32:
          return 4;
       case types::i64:
       case types::f64:
       case types::funcref:
       case types::externref:
          return 8;
       case types::v128:
          return 16;
       default: return 0;
      }
   }

   template<typename Options, typename Enable = void>
   struct max_mutable_globals_checker {
      constexpr void on_mutable_global(const Options&, uint8_t) {}
   };

   template<typename Options>
   using max_mutable_globals_t = decltype(std::declval<Options>().max_mutable_global_bytes);

   template<typename Options>
   struct max_mutable_globals_checker<Options, std::void_t<max_mutable_globals_t<Options>>> {
      static_assert(std::is_unsigned_v<std::decay_t<max_mutable_globals_t<Options>>>, "max_mutable_globals must be an unsigned integer type");
      void on_mutable_global(const Options& options, uint8_t type) {
         unsigned size = get_size_for_type(type);
         _counter += size;
         PSIZAM_ASSERT(_counter <= options.max_mutable_global_bytes && _counter >= size, wasm_parse_exception, "mutable globals exceeded limit");
      }
      std::decay_t<max_mutable_globals_t<Options>> _counter = 0;
   };

#define PARSER_OPTION(name, default_, type)                                   \
   template<typename Options>                                                 \
   type get_ ## name(const Options& options, long) { (void)options; return default_; } \
   template<typename Options>                                                 \
   auto get_ ## name(const Options& options, int) -> decltype(options.name) { \
     return options.name;                                                     \
   }                                                                          \
   template<typename Options>                                                 \
   type get_ ## name(const Options& options) { return get_ ## name(options, 0); }

#define MAX_ELEMENTS(name, default_)\
   PARSER_OPTION(name, default_, std::uint32_t)

   MAX_ELEMENTS(max_table_elements, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_section_elements, 0xFFFFFFFFu)

   MAX_ELEMENTS(max_type_section_elements, get_max_section_elements(options))
   MAX_ELEMENTS(max_import_section_elements, get_max_section_elements(options))
   MAX_ELEMENTS(max_function_section_elements, get_max_section_elements(options))
   MAX_ELEMENTS(max_global_section_elements, get_max_section_elements(options))
   MAX_ELEMENTS(max_export_section_elements, get_max_section_elements(options))
   MAX_ELEMENTS(max_element_section_elements, get_max_section_elements(options))
   MAX_ELEMENTS(max_data_section_elements, get_max_section_elements(options))

   MAX_ELEMENTS(max_element_segment_elements, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_data_segment_bytes, 0xFFFFFFFFu)

   PARSER_OPTION(max_linear_memory_init, 0xFFFFFFFFFFFFFFFFu, std::uint64_t)
   PARSER_OPTION(max_func_local_bytes_flags, max_func_local_bytes_flags_t::locals | max_func_local_bytes_flags_t::stack, max_func_local_bytes_flags_t);

   template<typename Options, typename Enable = void>
   struct max_func_local_bytes_checker {
      explicit max_func_local_bytes_checker(const Options&, const func_type& /*ft*/) {}
      void on_local(const Options&, std::uint8_t, const std::uint32_t) {}
      void push_stack(const Options& /*options*/, std::uint8_t /*type*/) {}
      void pop_stack(std::uint8_t /*type*/) {}
      void push_unreachable() {}
      void pop_unreachable() {}
      static constexpr bool is_defined = false;
   };
   template<typename Options>
   struct max_func_local_bytes_checker<Options, std::void_t<decltype(std::declval<Options>().max_func_local_bytes)>> {
      explicit max_func_local_bytes_checker(const Options& options, const func_type& ft) {
         if ((get_max_func_local_bytes_flags(options) & max_func_local_bytes_flags_t::params) != (max_func_local_bytes_flags_t)0) {
            for(std::uint32_t i = 0; i < ft.param_types.size(); ++i) {
               on_type(options, ft.param_types.at(i));
            }
         }
      }
      void on_type(const Options& options, std::uint8_t type) {
         unsigned size = get_size_for_type(type);
         _count += size;
         PSIZAM_ASSERT(_count <= options.max_func_local_bytes && _count >= size, wasm_parse_exception, "local variable limit exceeded");
      }
      void on_local(const Options& options, std::uint8_t type, std::uint32_t count) {
         if ((get_max_func_local_bytes_flags(options) & max_func_local_bytes_flags_t::locals) != (max_func_local_bytes_flags_t)0) {
            uint64_t size = get_size_for_type(type);
            size *= count;
            _count += size;
            PSIZAM_ASSERT(_count <= options.max_func_local_bytes && _count >= size, wasm_parse_exception, "local variable limit exceeded");
         }
      }
      std::decay_t<decltype(std::declval<Options>().max_func_local_bytes)> _count = 0;
      static constexpr bool is_defined = true;
   };
   template<typename Options>
   constexpr auto get_max_func_local_bytes_no_stack_c(int) -> std::enable_if_t<std::is_pointer_v<decltype(&Options::max_func_local_bytes_flags)>, bool>
   { return (Options::max_func_local_bytes_flags & max_func_local_bytes_flags_t::stack) == (max_func_local_bytes_flags_t)0; }
   template<typename Options>
   constexpr auto get_max_func_local_bytes_no_stack_c(long) -> bool { return false; }

   template<typename Options, typename Enable = void>
   struct max_func_local_bytes_stack_checker : max_func_local_bytes_checker<Options> {
      explicit constexpr max_func_local_bytes_stack_checker(const max_func_local_bytes_checker<Options>& base) : max_func_local_bytes_checker<Options>(base) {}
      void push_stack(const Options& options, std::uint8_t type) {
         if(unreachable_depth == 0 && (get_max_func_local_bytes_flags(options) & max_func_local_bytes_flags_t::stack) != (max_func_local_bytes_flags_t)0) {
            this->on_type(options, type);
         }
      }
      void pop_stack(const Options& options, std::uint8_t type) {
         if(unreachable_depth == 0 && (get_max_func_local_bytes_flags(options) & max_func_local_bytes_flags_t::stack) != (max_func_local_bytes_flags_t)0) {
            this->_count -= get_size_for_type(type);
         }
      }
      void push_unreachable() {
         ++unreachable_depth;
      }
      void pop_unreachable() {
         --unreachable_depth;
      }
      std::uint32_t unreachable_depth = 0;
   };
   template<typename Options>
   struct max_func_local_bytes_stack_checker<Options, std::enable_if_t<!max_func_local_bytes_checker<Options>::is_defined ||
                                                                       get_max_func_local_bytes_no_stack_c<Options>(0)>> {
      explicit constexpr max_func_local_bytes_stack_checker(const max_func_local_bytes_checker<Options>&) {}
      void push_stack(const Options& /*options*/, std::uint8_t /*type*/) {}
      void pop_stack(const Options& /*options*/, std::uint8_t /*type*/) {}
      void push_unreachable() {}
      void pop_unreachable() {}
   };

   MAX_ELEMENTS(max_local_sets, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_nested_structures, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_br_table_elements, 0xFFFFFFFFu)

   template<typename Options, typename Enable = void>
   struct psizam_max_nested_structures_checker {
      void on_control(const Options&) {}
      void on_end(const Options&) {}
   };
   template<typename Options>
   struct psizam_max_nested_structures_checker<Options, std::void_t<decltype(std::declval<Options>().psizam_max_nested_structures)>> {
      void on_control(const Options& options) {
         ++_count;
         PSIZAM_ASSERT(_count <= options.psizam_max_nested_structures, wasm_parse_exception, "Nested depth exceeded");
      }
      void on_end(const Options& options) {
         if(_count == 0) ++_count;
         else --_count;
      }
      std::decay_t<decltype(std::declval<Options>().psizam_max_nested_structures)> _count = 0;
   };

   MAX_ELEMENTS(max_symbol_bytes, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_memory_offset, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_code_bytes, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_pages, 0xFFFFFFFFu)
   MAX_ELEMENTS(max_call_depth, 251)

   PARSER_OPTION(parse_custom_section_name, false, bool);

   PARSER_OPTION(enable_simd, true, bool)
   PARSER_OPTION(enable_bulk_memory, true, bool)
   PARSER_OPTION(enable_sign_ext, true, bool)
   PARSER_OPTION(enable_nontrapping_fptoint, true, bool)
   PARSER_OPTION(compile_threads, static_cast<std::uint32_t>(0), std::uint32_t)

#undef MAX_ELEMENTS
#undef PARSER_OPTION

   template <typename Options>
   concept has_max_stack_bytes = requires(const Options& o) {
       o.max_stack_bytes;
   };

   // Returns either max_call_depth or max_stack_bytes, depending on which
   // on was enabled.
   template <typename Options>
   constexpr std::uint32_t choose_stack_limit(const Options& options)
   {
      if constexpr (has_max_stack_bytes<Options>)
      {
         static_assert(!requires{options.max_call_depth;}, "max_call_depth and max_stack_bytes are incompatible");
         return options.max_stack_bytes;
      }
      else
      {
         return get_max_call_depth(options);
      }
   }

   // max_stack_bytes depends on max_func_local_bytes and uses the
   // same accounting.
   template<typename Options>
   struct max_stack_usage_calculator
   {
      void update(const max_func_local_bytes_stack_checker<Options>&) {}
   };
   template<has_max_stack_bytes Options>
   struct max_stack_usage_calculator<Options>
   {
      void update(const max_func_local_bytes_stack_checker<Options>& x)
      {
         _max = std::max(x._count, _max);
      }
      std::decay_t<decltype(std::declval<Options>().max_stack_bytes)> _max = 0;
   };

   template <typename Writer, typename Options = default_options, typename DebugInfo = null_debug_info>
   class binary_parser {
    public:
      explicit binary_parser(growable_allocator& alloc, const Options& options = Options{},
                            bool enable_backtrace = false, bool stack_limit_is_bytes = false)
         : _allocator(alloc), _options(options),
           _enable_backtrace(enable_backtrace), _stack_limit_is_bytes(stack_limit_is_bytes) {}

      void set_compile_result(pzam_compile_result* r) { _compile_result = r; }

      template <typename T>
      using vec = guarded_vector<T>;

      static inline uint8_t parse_varuint1(wasm_code_ptr& code) { return varuint<1>(code).to(); }

      static inline uint8_t parse_varuint7(wasm_code_ptr& code) { return varuint<7>(code).to(); }

      static inline uint32_t parse_varuint32(wasm_code_ptr& code) { return varuint<32>(code).to(); }

      static inline uint64_t parse_varuint64(wasm_code_ptr& code) { return varuint<64>(code).to(); }

      static inline int8_t parse_varint7(wasm_code_ptr& code) { return varint<7>(code).to(); }

      static inline int32_t parse_varint32(wasm_code_ptr& code) { return varint<32>(code).to(); }

      static inline int64_t parse_varint64(wasm_code_ptr& code) { return varint<64>(code).to(); }

      int validate_utf8_code_point(wasm_code_ptr& code) {
         unsigned char ch = *code++;
         if (ch < 0x80) {
            return 1;
         } else if(ch < 0xE0) {
           PSIZAM_ASSERT((ch & 0xC0) == 0xC0, wasm_parse_exception, "invalid utf8 encoding");
            unsigned char b2 = *code++;
            PSIZAM_ASSERT((b2 & 0xC0) == 0x80, wasm_parse_exception, "invalid utf8 encoding");
            uint32_t code_point =
              (static_cast<uint32_t>(ch - 0xC0u) << 6u) +
              (static_cast<uint32_t>(b2 - 0x80u));
            PSIZAM_ASSERT(0x80 <= code_point && code_point < 0x800, wasm_parse_exception, "invalid utf8 encoding");
            return 2;
         } else if(ch < 0xF0) {
            unsigned char b2 = *code++;
            PSIZAM_ASSERT((b2 & 0xC0) == 0x80, wasm_parse_exception, "invalid utf8 encoding");
            unsigned char b3 = *code++;
            PSIZAM_ASSERT((b3 & 0xC0) == 0x80, wasm_parse_exception, "invalid utf8 encoding");
            uint32_t code_point =
              (static_cast<uint32_t>(ch - 0xE0u) << 12u) +
              (static_cast<uint32_t>(b2 - 0x80u) << 6u) +
              (static_cast<uint32_t>(b3 - 0x80u));
            PSIZAM_ASSERT((0x800 <= code_point && code_point < 0xD800) ||
                          (0xE000 <= code_point && code_point < 0x10000),
                          wasm_parse_exception, "invalid utf8 encoding");
            return 3;
         } else if (ch < 0xF8) {
            unsigned char b2 = *code++;
            PSIZAM_ASSERT((b2 & 0xC0) == 0x80, wasm_parse_exception, "invalid utf8 encoding");
            unsigned char b3 = *code++;
            PSIZAM_ASSERT((b3 & 0xC0) == 0x80, wasm_parse_exception, "invalid utf8 encoding");
            unsigned char b4 = *code++;
            PSIZAM_ASSERT((b4 & 0xC0) == 0x80, wasm_parse_exception, "invalid utf8 encoding");
            uint32_t code_point =
              (static_cast<uint32_t>(ch - 0xF0u) << 18u) +
              (static_cast<uint32_t>(b2 - 0x80u) << 12u) +
              (static_cast<uint32_t>(b3 - 0x80u) << 6u) +
              (static_cast<uint32_t>(b4 - 0x80u));
            PSIZAM_ASSERT((0x10000 <= code_point && code_point < 0x110000),
                          wasm_parse_exception, "invalid utf8 encoding");
            return 4;
         }
         PSIZAM_ASSERT(false, wasm_parse_exception, "invalid utf8 encoding");
      }

      void validate_utf8_string(wasm_code_ptr& code, uint32_t bytes) {
         while(bytes != 0) {
            bytes -= validate_utf8_code_point(code);
         }
      }

      std::vector<uint8_t> parse_utf8_string(wasm_code_ptr& code, std::uint32_t max_size) {
         auto len        = parse_varuint32(code);
         PSIZAM_ASSERT(len <= max_size, wasm_parse_exception, "name too long");
         auto guard = code.scoped_shrink_bounds(len);
         auto result = std::vector<uint8_t>(code.raw(), code.raw() + len);
         validate_utf8_string(code, len);
         return result;
      }

      template<typename T>
      T parse_raw(wasm_code_ptr& code) {
         static_assert(std::is_arithmetic_v<T>, "Can only read builtin types");
         auto guard = code.scoped_shrink_bounds(sizeof(T));
         T result;
         memcpy(&result, code.raw(), sizeof(T));
         code += sizeof(T);
         return result;
      }

      v128_t parse_v128(wasm_code_ptr& code) {
         static_assert(sizeof(v128_t) == 16, "sanity check for layout");
         auto guard = code.scoped_shrink_bounds(sizeof(v128_t));
         v128_t result;
         memcpy(&result, code.raw(), sizeof(v128_t));
         code += sizeof(v128_t);
         return result;
      }

      inline module& parse_module(wasm_code& code, module& mod, DebugInfo& debug) {
         wasm_code_ptr cp(code.data(), code.size());
         parse_module(cp, code.size(), mod, debug);
         return mod;
      }

      inline module& parse_module2(wasm_code_ptr& code_ptr, size_t sz, module& mod, DebugInfo& debug) {
         parse_module(code_ptr, sz, mod, debug);
         return mod;
      }

      static constexpr auto make_section_order() {
         std::array<std::uint8_t, section_id::num_of_elems> result{};
         std::uint8_t i = 1;
         for (std::uint8_t sec : {type_section, import_section, function_section, table_section, memory_section, tag_section, global_section, export_section, start_section, element_section, data_count_section, code_section, data_section}) {
            result[sec] = i++;
         }
         return result;
      }

      void parse_module(wasm_code_ptr& code_ptr, size_t sz, module& mod, DebugInfo& debug) {
         _mod = &mod;
         PSIZAM_ASSERT(parse_magic(code_ptr) == constants::magic, wasm_parse_exception, "magic number did not match");
         PSIZAM_ASSERT(parse_version(code_ptr) == constants::version, wasm_parse_exception,
                       "version number did not match");
         uint8_t highest_section_id = 0;
         constexpr auto order = make_section_order();
         for (;;) {
            if (code_ptr.offset() == sz)
               break;
            auto id = parse_section_id(code_ptr);
            auto len = parse_section_payload_len(code_ptr);

            PSIZAM_ASSERT(id < order.size(), wasm_parse_exception, "invalid section id");
            PSIZAM_ASSERT(id == 0 || order[id] > highest_section_id, wasm_parse_exception, "section out of order");
            highest_section_id = std::max(highest_section_id, order[id]);

            auto section_guard = code_ptr.scoped_consume_items(len);

            switch (id) {
               case section_id::custom_section: parse_custom(code_ptr); break;
               case section_id::type_section: parse_section<section_id::type_section>(code_ptr, mod.types); break;
               case section_id::import_section: parse_section<section_id::import_section>(code_ptr, mod.imports); break;
               case section_id::function_section:
                  parse_section<section_id::function_section>(code_ptr, mod.functions);
                  normalize_types();
                  break;
               case section_id::table_section: parse_section<section_id::table_section>(code_ptr, mod.tables); break;
               case section_id::memory_section:
                  parse_section<section_id::memory_section>(code_ptr, mod.memories);
                  break;
               case section_id::global_section: parse_section<section_id::global_section>(code_ptr, mod.globals); break;
               case section_id::export_section:
                  parse_section<section_id::export_section>(code_ptr, mod.exports);
                  validate_exports();
                  break;
               case section_id::start_section: parse_section<section_id::start_section>(code_ptr, mod.start); break;
               case section_id::element_section:
                  parse_section<section_id::element_section>(code_ptr, mod.elements);
                  break;
               case section_id::code_section: parse_section<section_id::code_section>(code_ptr, mod.code); break;
               case section_id::data_section: parse_section<section_id::data_section>(code_ptr, mod.data); break;
               case section_id::data_count_section: parse_section<section_id::data_count_section>(code_ptr, _datacount); break;
               case section_id::tag_section: parse_section<section_id::tag_section>(code_ptr, mod.tags); break;
               default: PSIZAM_ASSERT(false, wasm_parse_exception, "error invalid section id");
            }
         }
         PSIZAM_ASSERT(_mod->code.size() == _mod->functions.size(), wasm_parse_exception, "code section must have the same size as the function section" );

         _mod->stack_limit_is_bytes = has_max_stack_bytes<Options>;
         debug.set(std::move(imap));
         debug.relocate(_allocator.get_code_start());
      }

      inline uint32_t parse_magic(wasm_code_ptr& code) {
         return parse_raw<uint32_t>(code);
      }
      inline uint32_t parse_version(wasm_code_ptr& code) {
         return parse_raw<uint32_t>(code);
      }
      inline uint8_t  parse_section_id(wasm_code_ptr& code) { return *code++; }
      inline uint32_t parse_section_payload_len(wasm_code_ptr& code) {
         return parse_varuint32(code);
      }

      inline void parse_custom(wasm_code_ptr& code) {
         auto section_name = parse_utf8_string(code, 0xFFFFFFFFu); // ignored, but needs to be validated
         constexpr const char hint_name[] = "metadata.code.branch_hint";
         if(get_parse_custom_section_name(_options) &&
            section_name.size() == 4 && std::memcmp(section_name.data(), "name", 4) == 0) {
            parse_name_section(code);
         } else if(section_name.size() == sizeof(hint_name) - 1 &&
                   std::memcmp(section_name.data(), hint_name, sizeof(hint_name) - 1) == 0) {
            parse_branch_hints_section(code);
         } else {
            // skip to the end of the section
            code += code.bounds() - code.offset();
         }
      }

      inline void parse_name_map(wasm_code_ptr& code, std::vector<name_assoc>& map) {
        for(uint32_t i = 0; i < map.size(); ++i) {
            map[i].idx = parse_varuint32(code);
            map[i].name = parse_utf8_string(code, 0xFFFFFFFFu);
         }
      }

      inline void parse_name_section(wasm_code_ptr& code) {
         _mod->names.emplace();
         if(code.bounds() == code.offset()) return;
         if(*code == 0) {
            ++code;
            auto subsection_guard = code.scoped_consume_items(parse_varuint32(code));
            _mod->names->module_name.emplace(parse_utf8_string(code, 0xFFFFFFFFu));
         }
         if(code.bounds() == code.offset()) return;
         if(*code == 1) {
            ++code;
            auto subsection_guard = code.scoped_consume_items(parse_varuint32(code));
            uint32_t size = parse_varuint32(code);
            _mod->names->function_names.emplace(size);
            parse_name_map(code, *_mod->names->function_names);
         }
         if(code.bounds() == code.offset()) return;
         if(*code == 2) {
            ++code;
            auto subsection_guard = code.scoped_consume_items(parse_varuint32(code));
            uint32_t size = parse_varuint32(code);
            _mod->names->local_names.emplace(size);
            for(uint32_t i = 0; i < size; ++i) {
               auto& [idx,namemap] = (*_mod->names->local_names)[i];
               idx = parse_varuint32(code);
               uint32_t local_size = parse_varuint32(code);
               namemap = std::vector<name_assoc>(local_size);
               parse_name_map(code, namemap);
            }
         }
         if(code.bounds() == code.offset()) return;
         PSIZAM_ASSERT(false, wasm_parse_exception, "Invalid subsection Id");
      }

      inline void parse_branch_hints_section(wasm_code_ptr& code) {
         // Format: vec(func_hints)
         //   func_hints := func_idx:u32, vec(hint)
         //     hint := func_offset:u32, hint_value:u8
         // The section may appear before the code section, so buffer into _branch_hints_temp.
         // Applied to _mod->code[] in the code section parser.
         uint32_t num_funcs = parse_varuint32(code);
         for (uint32_t i = 0; i < num_funcs; ++i) {
            uint32_t func_idx = parse_varuint32(code);
            uint32_t num_hints = parse_varuint32(code);
            auto& hints = _branch_hints_temp[func_idx];
            for (uint32_t j = 0; j < num_hints; ++j) {
               uint32_t offset = parse_varuint32(code);
               uint8_t  value  = *code++;
               hints.push_back({offset, value});
            }
         }
      }

      void parse_import_entry(wasm_code_ptr& code, import_entry& entry) {
         entry.module_str = parse_utf8_string(code, get_max_symbol_bytes(_options));
         entry.field_str = parse_utf8_string(code, get_max_symbol_bytes(_options));
         entry.kind = (external_kind)(*code++);
         switch ((uint8_t)entry.kind) {
            case external_kind::Function: {
               auto type = parse_varuint32(code);
               entry.type.func_t = type;
               PSIZAM_ASSERT(type < _mod->types.size(), wasm_parse_exception, "Invalid function type");
               break;
            }
            case external_kind::Table: {
               parse_table_type(code, entry.type.table_t);
               // Prepend imported table to the tables vector so index space is correct
               _mod->tables.push_back(entry.type.table_t);
               _mod->num_imported_tables++;
               break;
            }
            case external_kind::Memory: {
               parse_memory_type(code, entry.type.mem_t);
               _mod->memories.push_back(entry.type.mem_t);
               _mod->num_imported_memories++;
               break;
            }
            case external_kind::Global: {
               uint8_t ct = *code++;
               entry.type.global_t.content_type = ct;
               PSIZAM_ASSERT(ct == types::i32 || ct == types::i64 || ct == types::f32 || ct == types::f64 ||
                             ct == types::externref || ct == types::funcref ||
                             (ct == types::v128 && get_enable_simd(_options)),
                             wasm_parse_exception, "invalid global content type");
               entry.type.global_t.mutability = parse_varuint1(code);
               // Add imported global with zero-initialized value
               global_variable gv;
               gv.type = entry.type.global_t;
               gv.init = {};
               _mod->globals.push_back(gv);
               _mod->num_imported_globals++;
               break;
            }
            case external_kind::Tag: {
               auto type = parse_varuint32(code);
               PSIZAM_ASSERT(type == 0, wasm_parse_exception, "invalid tag attribute");
               uint32_t tag_type_index = parse_varuint32(code);
               PSIZAM_ASSERT(tag_type_index < _mod->types.size(), wasm_parse_exception, "invalid tag type index");
               _mod->tags.push_back(tag_type{static_cast<uint8_t>(type), tag_type_index});
               break;
            }
            default: PSIZAM_ASSERT(false, wasm_unsupported_import_exception, "unsupported import kind");
         }
      }

      uint8_t parse_flags(wasm_code_ptr& code) {
         PSIZAM_ASSERT(*code <= 0x03, wasm_parse_exception, "invalid flags");
         return *code++;
      }

      void parse_table_type(wasm_code_ptr& code, table_type& tt) {
         tt.element_type   = *code++;
         PSIZAM_ASSERT(tt.element_type == types::funcref || tt.element_type == types::externref, wasm_parse_exception, "table must have type funcref or externref");
         auto raw_flags    = parse_flags(code);
         PSIZAM_ASSERT(raw_flags <= 0x01, wasm_parse_exception, "invalid table limits flags");
         tt.limits.flags   = raw_flags;
         tt.limits.initial = parse_varuint32(code);
         if (tt.limits.flags) {
            tt.limits.maximum = parse_varuint32(code);
            PSIZAM_ASSERT(tt.limits.initial <= tt.limits.maximum, wasm_parse_exception, "table max size less than min size");
         }
         PSIZAM_ASSERT(tt.limits.initial <= get_max_table_elements(_options), wasm_parse_exception, "table size exceeds limit");
      }

      void parse_global_variable(wasm_code_ptr& code, global_variable& gv) {
         uint8_t ct           = *code++;
         gv.type.content_type = ct;
         PSIZAM_ASSERT(ct == types::i32 || ct == types::i64 || ct == types::f32 || ct == types::f64 ||
                       ct == types::funcref || ct == types::externref ||
                       (ct == types::v128 && get_enable_simd(_options)),
                       wasm_parse_exception, "invalid global content type");

         gv.type.mutability = parse_varuint1(code);
         if(gv.type.mutability)
            on_mutable_global(ct);
         parse_init_expr(code, gv.init, ct);
      }

      void parse_memory_type(wasm_code_ptr& code, memory_type& mt) {
         uint8_t raw_flags = *code++;
         PSIZAM_ASSERT(raw_flags <= 0x07, wasm_parse_exception, "invalid memory flags");
         mt.is_memory64    = (raw_flags & 0x04) != 0;
         mt.limits.flags   = (raw_flags & 0x01) != 0;  // has_maximum
         if (mt.is_memory64) {
            uint64_t initial64 = parse_varuint64(code);
            PSIZAM_ASSERT(initial64 <= 65536u, wasm_parse_exception, "initial memory out of range");
            mt.limits.initial = static_cast<uint32_t>(initial64);
         } else {
            mt.limits.initial = parse_varuint32(code);
         }
         // Implementation limits
         PSIZAM_ASSERT(mt.limits.initial <= get_max_pages(_options), wasm_parse_exception, "initial memory out of range");
         // WASM specification
         PSIZAM_ASSERT(mt.limits.initial <= 65536u, wasm_parse_exception, "initial memory out of range");
         // Shared memory (bit 1) requires has_max (bit 0)
         if (raw_flags & 0x02)
            PSIZAM_ASSERT(raw_flags & 0x01, wasm_parse_exception, "shared memory requires maximum");
         if (mt.limits.flags) {
            if (mt.is_memory64) {
               uint64_t max64 = parse_varuint64(code);
               PSIZAM_ASSERT(max64 <= 65536u, wasm_parse_exception, "maximum memory out of range");
               mt.limits.maximum = static_cast<uint32_t>(max64);
            } else {
               mt.limits.maximum = parse_varuint32(code);
            }
            PSIZAM_ASSERT(mt.limits.maximum >= mt.limits.initial, wasm_parse_exception, "maximum must be at least minimum");
            PSIZAM_ASSERT(mt.limits.maximum <= 65536u, wasm_parse_exception, "maximum memory out of range");
         }
      }

      // Returns the address type for memory 0: i64 for memory64, i32 for memory32
      uint8_t mem_addr_type() const {
         return (!_mod->memories.empty() && _mod->memories[0].is_memory64) ? types::i64 : types::i32;
      }

      // Parse memarg offset: varuint64 for memory64, varuint32 for memory32
      uint32_t parse_memarg_offset(wasm_code_ptr& code) {
         if (mem_addr_type() == types::i64) {
            uint64_t off64 = parse_varuint64(code);
            PSIZAM_ASSERT(off64 <= UINT32_MAX, wasm_parse_exception, "memory64 offset exceeds implementation limit");
            return static_cast<uint32_t>(off64);
         }
         return parse_varuint32(code);
      }

      void parse_export_entry(wasm_code_ptr& code, export_entry& entry) {
         entry.field_str = parse_utf8_string(code, get_max_symbol_bytes(_options));
         entry.kind  = (external_kind)(*code++);
         entry.index = parse_varuint32(code);
         switch(entry.kind) {
            case external_kind::Function: PSIZAM_ASSERT(entry.index < _mod->get_functions_total(), wasm_parse_exception, "function export out of range"); break;
            case external_kind::Table: PSIZAM_ASSERT(entry.index < _mod->tables.size(), wasm_parse_exception, "table export out of range"); break;
            case external_kind::Memory: PSIZAM_ASSERT(entry.index < _mod->memories.size(), wasm_parse_exception, "memory export out of range"); break;
            case external_kind::Global:
               PSIZAM_ASSERT(entry.index < _mod->globals.size(), wasm_parse_exception, "global export out of range");
               break;
            case external_kind::Tag:
               PSIZAM_ASSERT(entry.index < _mod->tags.size(), wasm_parse_exception, "tag export out of range");
               break;
            default: PSIZAM_ASSERT(false, wasm_parse_exception, "Unknown export kind"); break;
         }
      }

      void parse_func_type(wasm_code_ptr& code, func_type& ft) {
         ft.form                              = *code++;
         PSIZAM_ASSERT(ft.form == 0x60, wasm_parse_exception, "invalid function type");
         decltype(ft.param_types) param_types(parse_varuint32(code));
         for (size_t i = 0; i < param_types.size(); i++) {
            uint8_t pt        = *code++;
            param_types.at(i) = pt;
            PSIZAM_ASSERT(pt == types::i32 || pt == types::i64 || pt == types::f32 || pt == types::f64 ||
                          pt == types::funcref || pt == types::externref ||
                          (pt == types::v128 && get_enable_simd(_options)),
                          wasm_parse_exception, "invalid function param type");
         }
         ft.param_types  = std::move(param_types);
         uint32_t return_count = parse_varuint32(code);
         ft.return_types.resize(return_count);
         for (uint32_t i = 0; i < return_count; ++i) {
            uint8_t rt = *code++;
            ft.return_types[i] = rt;
            PSIZAM_ASSERT(rt == types::i32 || rt == types::i64 || rt == types::f32 || rt == types::f64 ||
                          rt == types::funcref || rt == types::externref ||
                          (rt == types::v128 && get_enable_simd(_options)),
                          wasm_parse_exception, "invalid function return type");
         }
         ft.finalize_returns();
      }

      void normalize_types() {
         type_aliases.resize(_mod->types.size());
         for (uint32_t i = 0; i < _mod->types.size(); ++i) {
            uint32_t j = 0;
            for (; j < i; ++j) {
               if (_mod->types[j] == _mod->types[i]) {
                  break;
               }
            }
            type_aliases[i] = j;
         }

         uint32_t imported_functions_size = _mod->get_imported_functions_size();
         fast_functions.resize(_mod->functions.size() + imported_functions_size);
         for (uint32_t i = 0; i < imported_functions_size; ++i) {
            fast_functions[i] = type_aliases.at(_mod->imports[i].type.func_t);
         }
         for (uint32_t i = 0; i < _mod->functions.size(); ++i) {
            PSIZAM_ASSERT(_mod->functions[i] < type_aliases.size(), wasm_parse_exception, "function type index out of range");
            fast_functions[i + imported_functions_size] = type_aliases[_mod->functions[i]];
         }
      }

      void parse_elem_segment(wasm_code_ptr& code, elem_segment& es) {
         table_type* tt = nullptr;
         std::uint32_t flags = parse_varuint32(code);
         PSIZAM_ASSERT(es.index == 0 || get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
         PSIZAM_ASSERT(es.index <= 7, wasm_parse_exception, "Illegal flags for elem");
         if (flags == 2 || flags == 6) {
            es.index = parse_varuint32(code);
         } else {
            es.index = 0;
         }
         if ((flags & 1) == 0) {
            parse_init_expr(code, es.offset, types::i32);
            es.mode = elem_mode::active;
            PSIZAM_ASSERT(es.index < _mod->tables.size(), wasm_parse_exception, "wrong table in elem");
            tt = &_mod->tables[es.index];
         } else {
            if (flags & 2) {
               es.mode = elem_mode::declarative;
            } else {
               es.mode = elem_mode::passive;
            }
         }
         uint8_t elem_reftype = types::funcref; // default for flags 0, 4
         if (flags == 1 || flags == 2 || flags == 3) {
            auto elemkind = *code++;
            PSIZAM_ASSERT(elemkind == 0x00, wasm_parse_exception, "elemkind must be funcref");
            elem_reftype = types::funcref;
         } else if (flags == 5 || flags == 6 || flags == 7) {
            elem_reftype = *code++;
            PSIZAM_ASSERT(elem_reftype == types::funcref || elem_reftype == types::externref, wasm_parse_exception, "elem type must be funcref or externref");
         }
         // For active segments, element type must match target table's element type
         if (tt) {
            PSIZAM_ASSERT(elem_reftype == tt->element_type, wasm_parse_exception, "type mismatch");
         }
         uint32_t           size  = parse_varuint32(code);
         PSIZAM_ASSERT(size <= get_max_element_segment_elements(_options), wasm_parse_exception, "elem segment too large");
         decltype(es.elems) elems(size);
         if (flags & 4) {
            for (uint32_t i = 0; i < size; i++) {
               parse_elem_expr(code, elems.at(i), elem_reftype);
            }
         } else {
            for (uint32_t i = 0; i < size; i++) {
               uint32_t index    = parse_varuint32(code);
               PSIZAM_ASSERT(index < _mod->get_functions_total(), wasm_parse_exception,  "elem for undefined function");
               elems.at(i).type  = fast_functions.at(index);
               elems.at(i).index = index;
            }
         }
         es.type  = elem_reftype;
         es.elems = std::move(elems);
      }

      void parse_elem_expr(wasm_code_ptr& code, table_entry& te, uint8_t expected_reftype = types::funcref) {
         auto opcode = *code++;
         switch (opcode) {
            case opcodes::ref_null:
               te.type = te.index = std::numeric_limits<std::uint32_t>::max();
               PSIZAM_ASSERT(*code == types::funcref || *code == types::externref, wasm_parse_exception, "Unknown type for elem");
               PSIZAM_ASSERT(*code == expected_reftype, wasm_parse_exception, "type mismatch");
               ++code;
               break;
            case opcodes::ref_func:
               PSIZAM_ASSERT(expected_reftype == types::funcref, wasm_parse_exception, "type mismatch");
               te.index = parse_varuint32(code);
               PSIZAM_ASSERT(te.index < _mod->get_functions_total(), wasm_parse_exception, "elem for undefined function");
               te.type = fast_functions.at(te.index);
               break;
            case opcodes::get_global: {
               uint32_t global_idx = parse_varuint32(code);
               PSIZAM_ASSERT(global_idx < _mod->globals.size(), wasm_parse_exception, "global.get index out of range in elem");
               // The global holds a funcref; resolve at instantiation time.
               // For now store the function index from the global's init value.
               auto& gv = _mod->globals[global_idx];
               te.index = gv.init.value.i32;
               if (te.index < _mod->get_functions_total())
                  te.type = fast_functions.at(te.index);
               else
                  te.type = std::numeric_limits<std::uint32_t>::max();
               break;
            }
            default:
               PSIZAM_ASSERT(false, wasm_parse_exception,
                             "elem expression can only accept ref.null, ref.func, and global.get");
         }
         PSIZAM_ASSERT((*code++) == opcodes::end, wasm_parse_exception, "no end op found");
      }

      void parse_init_expr(wasm_code_ptr& code, init_expr& ie, uint8_t type) {
         auto* expr_start = code.raw();
         ie.opcode = *code++;
         switch (ie.opcode) {
            case opcodes::i32_const:
               ie.value.i32 = parse_varint32(code);
               break;
            case opcodes::i64_const:
               ie.value.i64 = parse_varint64(code);
               break;
            case opcodes::f32_const:
               ie.value.f32 = parse_raw<uint32_t>(code);
               PSIZAM_ASSERT(type == types::f32, wasm_parse_exception, "expected f32 initializer");
               break;
            case opcodes::f64_const:
               ie.value.f64 = parse_raw<uint64_t>(code);
               PSIZAM_ASSERT(type == types::f64, wasm_parse_exception, "expected f64 initializer");
               break;
            case opcodes::vector_prefix:
               PSIZAM_ASSERT(get_enable_simd(_options), wasm_parse_exception, "SIMD not enabled");
               PSIZAM_ASSERT(parse_varuint32(code) == vec_opcodes::v128_const, wasm_parse_exception, "Expected v128.const");
               ie.value.v128 = parse_v128(code);
               PSIZAM_ASSERT(type == types::v128, wasm_parse_exception, "expected v128 initializer");
               break;
            case opcodes::ref_null:
               PSIZAM_ASSERT(type == types::funcref || type == types::externref, wasm_parse_exception, "expected reference type for ref.null");
               ie.value.i64 = 0;
               ++code; // consume the reference type byte
               break;
            case opcodes::ref_func: {
               uint32_t func_idx = parse_varuint32(code);
               PSIZAM_ASSERT(type == types::funcref, wasm_parse_exception, "expected funcref for ref.func");
               ie.value.i32 = func_idx;
               break;
            }
            case opcodes::get_global: {
               uint32_t global_idx = parse_varuint32(code);
               PSIZAM_ASSERT(global_idx < _mod->num_imported_globals, wasm_parse_exception, "global.get in init must reference an imported global");
               PSIZAM_ASSERT(!_mod->globals[global_idx].type.mutability, wasm_parse_exception, "global.get in init must reference an immutable global");
               ie.value.i32 = global_idx;
               break;
            }
            default:
               PSIZAM_ASSERT(false, wasm_parse_exception,
                             "invalid opcode in constant expression");
         }

         if (*code == opcodes::end) {
            // Simple single-instruction expression — validate type consistency
            switch (ie.opcode) {
               case opcodes::i32_const:
                  PSIZAM_ASSERT(type == types::i32, wasm_parse_exception, "expected i32 initializer");
                  break;
               case opcodes::i64_const:
                  PSIZAM_ASSERT(type == types::i64, wasm_parse_exception, "expected i64 initializer");
                  break;
               case opcodes::get_global:
                  // global.get type is validated elsewhere (the referenced global's type must match)
                  break;
               default:
                  // f32_const, f64_const, v128_const, ref_null, ref_func — already validated above
                  break;
            }
            ++code; // consume end
            return;
         }

         // Extended const expression — continue parsing remaining instructions,
         // then store the entire expression as raw bytes for evaluation at instantiation.
         PSIZAM_ASSERT(type == types::i32 || type == types::i64, wasm_parse_exception, "extended const expressions only produce i32 or i64");
         uint32_t stack_depth = 1; // first instruction already pushed one value
         while (*code != opcodes::end) {
            uint8_t op = *code++;
            switch (op) {
               case opcodes::i32_const: parse_varint32(code); ++stack_depth; break;
               case opcodes::i64_const: parse_varint64(code); ++stack_depth; break;
               case opcodes::get_global: {
                  uint32_t global_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(global_idx < _mod->num_imported_globals, wasm_parse_exception, "global.get in init must reference an imported global");
                  PSIZAM_ASSERT(!_mod->globals[global_idx].type.mutability, wasm_parse_exception, "global.get in init must reference an immutable global");
                  ++stack_depth;
                  break;
               }
               case opcodes::i32_add: case opcodes::i32_sub: case opcodes::i32_mul:
               case opcodes::i64_add: case opcodes::i64_sub: case opcodes::i64_mul:
                  PSIZAM_ASSERT(stack_depth >= 2, wasm_parse_exception, "stack underflow in constant expression");
                  --stack_depth; // pops 2, pushes 1 = net -1
                  break;
               default:
                  PSIZAM_ASSERT(false, wasm_parse_exception, "invalid opcode in extended constant expression");
            }
         }
         PSIZAM_ASSERT(stack_depth == 1, wasm_parse_exception, "constant expression must produce exactly one value");
         ++code; // consume end
         // Store everything from expr_start through end (inclusive)
         ie.raw_expr.assign(expr_start, code.raw());
         ie.opcode = opcodes::i32_const; // placeholder — raw_expr non-empty signals extended eval
      }

      void parse_function_body(wasm_code_ptr& code, function_body& fb, std::size_t idx) {
         fb.size   = parse_varuint32(code);
         PSIZAM_ASSERT(fb.size <= get_max_code_bytes(_options), wasm_parse_exception, "Function body too large");
         fb.body_start = code.raw();  // points to local_count byte — branch hint offsets are relative to this
         const auto&         before    = code.offset();
         const auto&         local_cnt = parse_varuint32(code);
         _current_function_index++;
         PSIZAM_ASSERT(local_cnt <= get_max_local_sets(_options), wasm_parse_exception, "Number of local sets exceeds limit");
         decltype(fb.locals) locals(local_cnt);
         PSIZAM_ASSERT(idx < _mod->functions.size(), wasm_parse_exception, "function index out of range");
         func_type& ft = _mod->types.at(_mod->functions[idx]);
         max_func_local_bytes_checker<Options> local_checker(_options, ft);
         // parse the local entries
         for (size_t i = 0; i < local_cnt; i++) {
            auto count = parse_varuint32(code);
            auto type = *code++;
            PSIZAM_ASSERT(type == types::i32 || type == types::i64 || type == types::f32 || type == types::f64 ||
                          type == types::funcref || type == types::externref || type == types::exnref ||
                          (type == types::v128 && get_enable_simd(_options)),
                          wasm_parse_exception, "invalid local type");
            local_checker.on_local(_options, type, count);
            locals.at(i).count = count;
            locals.at(i).type  = type;
         }
         fb.locals = std::move(locals);

         fb.size -= code.offset() - before;
         auto guard = code.scoped_shrink_bounds(fb.size);
         _function_bodies.emplace_back(wasm_code_ptr{code.raw(), fb.size}, local_checker);

         code += fb.size-1;
         PSIZAM_ASSERT(*code == 0x0B,
                       wasm_parse_exception, "failed parsing function body, expected 'end'");
         ++code;
      }

      // The control stack holds either address of the target of the
      // label (for backward jumps) or a list of instructions to be
      // updated (for forward jumps).
      //
      // Inside an if: The first element refers to the `if` and should
      // jump to `else`.  The remaining elements should branch to `end`
      using label_t = decltype(std::declval<Writer>().emit_end());
      using branch_t = decltype(std::declval<Writer>().emit_if());
      struct pc_element_t {
         uint32_t operand_depth;
         uint32_t expected_result;   // single result for blocks (backward compat)
         uint32_t label_result;      // single label result for blocks
         bool is_if;
         std::variant<label_t, std::vector<branch_t>> relocations;
         // For multi-value: non-empty when the block has >1 results
         std::vector<uint8_t> expected_results;
         std::vector<uint8_t> label_results;
         std::vector<uint8_t> block_params;  // params for multi-value block types
      };

      static constexpr uint8_t any_type = 0x82;
      struct operand_stack_type_tracker {
         explicit operand_stack_type_tracker(const max_func_local_bytes_stack_checker<Options> local_bytes_checker, const Options& options)
          : local_bytes_checker(local_bytes_checker), options(options) {
            stack_usage.update(local_bytes_checker);
         }
         std::vector<uint8_t> state = { scope_tag };
         static constexpr uint8_t unreachable_tag = 0x80;
         static constexpr uint8_t scope_tag = 0x81;
         uint32_t operand_depth = 0;
         uint32_t maximum_operand_depth = 0;
         max_func_local_bytes_stack_checker<Options> local_bytes_checker;
         max_stack_usage_calculator<Options> stack_usage;
         const Options& options;
         void push(uint8_t type) {
            assert(type != unreachable_tag && type != scope_tag);
            assert(type == types::i32 || type == types::i64 || type == types::f32 || type == types::f64 || type == types::funcref || type == types::externref || type == any_type || (type == types::v128 && get_enable_simd(options)));
            PSIZAM_ASSERT(operand_depth < std::numeric_limits<uint32_t>::max(), wasm_parse_exception, "integer overflow in operand depth");
            operand_depth += Writer::get_depth_for_type(type);
            maximum_operand_depth = std::max(operand_depth, maximum_operand_depth);
            state.push_back(type);
            local_bytes_checker.push_stack(options, type);
            stack_usage.update(local_bytes_checker);
         }
         void pop(uint8_t expected) {
            assert(expected != unreachable_tag && expected != scope_tag);
            if(expected == types::pseudo) return;
            PSIZAM_ASSERT(!state.empty(), wasm_parse_exception, "unexpected pop");
            if (state.back() != unreachable_tag) {
               PSIZAM_ASSERT(state.back() == expected || state.back() == any_type, wasm_parse_exception, "wrong type");
               local_bytes_checker.pop_stack(options, expected);
               operand_depth -= Writer::get_depth_for_type(expected);
               state.pop_back();
            }
         }
         uint8_t pop() {
            PSIZAM_ASSERT(!state.empty() && state.back() != scope_tag, wasm_parse_exception, "unexpected pop");
            if (state.back() == unreachable_tag)
               return any_type;
            else {
               uint8_t result = state.back();
               operand_depth -= Writer::get_depth_for_type(result);
               local_bytes_checker.pop_stack(options, result);
               state.pop_back();
               return result;
            }
         }
         void top(uint8_t expected) {
            // Constrain the top of the stack if it was any_type or unreachable_tag.
            pop(expected);
            push(expected);
         }
         void start_unreachable() {
            while(!state.empty() && state.back() != scope_tag) {
               if (state.back() != unreachable_tag)
                  operand_depth -= Writer::get_depth_for_type(state.back());
               state.pop_back();
            }
            local_bytes_checker.push_unreachable();
            state.push_back(unreachable_tag);
         }
         void push_scope() {
            state.push_back(scope_tag);
         }
         void pop_scope(uint8_t expected_result = types::pseudo) {
            pop_scope_multi(expected_result == types::pseudo
                            ? std::vector<uint8_t>{}
                            : std::vector<uint8_t>{expected_result});
         }
         void pop_scope_multi(const std::vector<uint8_t>& expected_results) {
            // Pop expected results in reverse order (top-of-stack first)
            for (int i = static_cast<int>(expected_results.size()) - 1; i >= 0; --i)
               pop(expected_results[i]);
            PSIZAM_ASSERT(!state.empty(), wasm_parse_exception, "unexpected end");
            if (state.back() == unreachable_tag) {
               local_bytes_checker.pop_unreachable();
               state.pop_back();
            }
            PSIZAM_ASSERT(state.back() == scope_tag, wasm_parse_exception, "unexpected end");
            state.pop_back();
            for (auto rt : expected_results)
               push(rt);
         }
         void finish() {
            if (!state.empty() && state.back() == unreachable_tag) {
               state.pop_back();
            }
            PSIZAM_ASSERT(state.empty(), wasm_parse_exception, "stack not empty at scope end");
         }
         uint32_t depth() const { return operand_depth; }
      };

      struct local_types_t {
         local_types_t(const func_type& ft, const std::vector<local_entry>& locals_arg) :
            _ft(ft), _locals(locals_arg) {
            uint32_t count = ft.param_types.size();
            _boundaries.push_back(count);
            for (uint32_t i = 0; i < locals_arg.size(); ++i) {
               // This test cannot overflow.
               PSIZAM_ASSERT (count <= 0xFFFFFFFFu - locals_arg[i].count, wasm_parse_exception, "too many locals");
               count += locals_arg[i].count;
               _boundaries.push_back(count);
            }
         }
         uint8_t operator[](uint32_t local_idx) const {
            PSIZAM_ASSERT(local_idx < _boundaries.back(), wasm_parse_exception, "undefined local");
            auto pos = std::upper_bound(_boundaries.begin(), _boundaries.end(), local_idx);
            if (pos == _boundaries.begin())
               return _ft.param_types[local_idx];
            else
               return _locals[pos - _boundaries.begin() - 1].type;
         }
         uint64_t locals_count() const {
            uint64_t total = _boundaries.back();
            return total - _ft.param_types.size();
         }
         const func_type& _ft;
         const std::vector<local_entry>& _locals;
         std::vector<uint32_t> _boundaries;
      };

      void parse_function_body_code(wasm_code_ptr& code, size_t bounds, const max_func_local_bytes_stack_checker<Options>& local_bytes_checker,
                                    Writer& code_writer, const func_type& ft, const local_types_t& local_types) {
         parse_function_body_code_impl(code, bounds, local_bytes_checker, code_writer, ft, local_types,
                                       _nested_checker, imap, _mod->maximum_stack);
      }

      // Thread-safe version: takes mutable state as parameters so each worker thread
      // can provide its own checker/imap/max_stack accumulator.
      template<typename WriterT, typename Checker, typename DebugInfoBuilder>
      void parse_function_body_code_impl(wasm_code_ptr& code, size_t bounds,
                                    const max_func_local_bytes_stack_checker<Options>& local_bytes_checker,
                                    WriterT& code_writer, const func_type& ft, const local_types_t& local_types,
                                    Checker& nested_checker, DebugInfoBuilder& imap_ref, uint64_t& max_stack_out) {

         // Initialize the control stack with the current function as the sole element
         operand_stack_type_tracker op_stack{local_bytes_checker, _options};
         pc_element_t func_scope;
         func_scope.operand_depth = op_stack.depth();
         func_scope.expected_result = ft.return_count ? ft.return_type : static_cast<uint32_t>(types::pseudo);
         func_scope.label_result = func_scope.expected_result;
         func_scope.is_if = false;
         func_scope.relocations = std::vector<branch_t>{};
         if (ft.return_count > 1) {
            func_scope.expected_results = ft.return_types;
            func_scope.label_results = ft.return_types;
         }
         std::vector<pc_element_t> pc_stack{std::move(func_scope)};

         // writes the continuation of a label to address.  If the continuation
         // is not yet available, address will be recorded in the relocations
         // list for label.
         auto handle_branch_target = [&](uint32_t label, branch_t address) {
            PSIZAM_ASSERT(label < pc_stack.size(), wasm_parse_exception, "invalid label");
            pc_element_t& branch_target = pc_stack[pc_stack.size() - label - 1];
            std::visit(overloaded{ [&](label_t target) { code_writer.fix_branch(address, target); },
                                   [&](std::vector<branch_t>& relocations) { relocations.push_back(address); } },
               branch_target.relocations);
         };

         // Returns the number of operands that need to be popped when
         // branching to label.  If the label has a return value it will
         // be counted in this, and the high bit will be set to signal
         // its presence.
         auto compute_depth_change = [&](uint32_t label) {
            PSIZAM_ASSERT(label < pc_stack.size(), wasm_parse_exception, "invalid label");
            pc_element_t& branch_target = pc_stack[pc_stack.size() - label - 1];
            auto result = op_stack.depth() - branch_target.operand_depth;
            uint32_t result_count;
            if (!branch_target.label_results.empty()) {
               // Multi-value: validate all label results on the stack (top-down)
               for (int i = static_cast<int>(branch_target.label_results.size()) - 1; i >= 0; --i)
                  op_stack.top(branch_target.label_results[i]);
               result_count = static_cast<uint32_t>(branch_target.label_results.size());
            } else if(branch_target.label_result != types::pseudo) {
               op_stack.top(branch_target.label_result);
               result_count = 1;
            } else {
               result_count = 0;
            }
            // For single-value compat, use the first label result type
            uint8_t rt = !branch_target.label_results.empty()
                         ? branch_target.label_results[0]
                         : static_cast<uint8_t>(branch_target.label_result);
            return std::tuple{result, rt, result_count};
         };

         // Handles branches to the end of the scope and pops the pc_stack
         auto exit_scope = [&]() {
            // There must be at least one element
            PSIZAM_ASSERT(pc_stack.size(), wasm_parse_exception, "unexpected end instruction");
            // an if with an empty else requires params == results (identity)
            if (pc_stack.back().is_if) {
               if (!pc_stack.back().block_params.empty() || !pc_stack.back().expected_results.empty()) {
                  PSIZAM_ASSERT(pc_stack.back().block_params == pc_stack.back().expected_results,
                                wasm_parse_exception, "type mismatch: if without else requires matching params and results");
               } else {
                  PSIZAM_ASSERT(pc_stack.back().expected_result == types::pseudo,
                                wasm_parse_exception, "type mismatch: if without else cannot have a return value");
               }
            }
            auto end_pos = code_writer.emit_end();
            if(auto* relocations = std::get_if<std::vector<branch_t>>(&pc_stack.back().relocations)) {
               for(auto branch_op : *relocations) {
                  code_writer.fix_branch(branch_op, end_pos);
               }
            }
            if (!pc_stack.back().expected_results.empty())
               op_stack.pop_scope_multi(pc_stack.back().expected_results);
            else
               op_stack.pop_scope(pc_stack.back().expected_result);
            pc_stack.pop_back();
         };

         auto check_in_bounds = [&]{
            PSIZAM_ASSERT(!pc_stack.empty(),
                          wasm_parse_exception, "code after function end");
         };

         while (code.offset() < bounds) {
            PSIZAM_ASSERT(pc_stack.size() <= get_max_nested_structures(_options), wasm_parse_exception,
                          "nested structures validation failure");

            imap_ref.on_instr_start(code_writer.get_addr(), code.raw());
            if constexpr (requires { code_writer.set_wasm_pc(code.raw()); })
               code_writer.set_wasm_pc(code.raw());

            switch (*code++) {
               case opcodes::unreachable: check_in_bounds(); code_writer.emit_unreachable(); op_stack.start_unreachable(); break;
               case opcodes::nop: code_writer.emit_nop(); break;
               case opcodes::end: {
                  check_in_bounds();
                  exit_scope();
                  PSIZAM_ASSERT(!pc_stack.empty() || code.offset() == bounds, wasm_parse_exception, "function too short");
                  nested_checker.on_end(_options);
                  break;
               }
               case opcodes::return_: {
                  check_in_bounds();
                  uint32_t label = pc_stack.size() - 1;
                  auto [depth_change,rt,rc] = compute_depth_change(label);
                  auto branch = code_writer.emit_return(depth_change, rt, rc);
                  handle_branch_target(label, branch);
                  op_stack.start_unreachable();
               } break;
               case opcodes::block: {
                  // Parse block type: inline valtype (single byte) or s33 type index
                  uint8_t first_byte = *code;
                  uint8_t single_type = types::pseudo;
                  std::vector<uint8_t> bp, br;

                  if (first_byte == types::pseudo || first_byte == types::i32 || first_byte == types::i64 ||
                      first_byte == types::f32 || first_byte == types::f64 || first_byte == types::v128) {
                     code++;
                     PSIZAM_ASSERT(first_byte == types::i32 || first_byte == types::i64 ||
                                   first_byte == types::f32 || first_byte == types::f64 ||
                                   (first_byte == types::v128 && get_enable_simd(_options)) ||
                                   first_byte == types::pseudo, wasm_parse_exception,
                                   "Invalid type code in block");
                     single_type = first_byte;
                  } else {
                     // s33 type index (multi-value block type)
                     int32_t type_idx = parse_varint32(code);
                     PSIZAM_ASSERT(type_idx >= 0 && static_cast<uint32_t>(type_idx) < _mod->types.size(),
                                   wasm_parse_exception, "invalid block type index");
                     const func_type& ft = _mod->types[type_idx];
                     bp.assign(ft.param_types.begin(), ft.param_types.end());
                     br.assign(ft.return_types.begin(), ft.return_types.end());
                     single_type = ft.return_count ? ft.return_type : types::pseudo;
                  }

                  // Pop params from outer stack (multi-value blocks can have inputs)
                  for (int i = static_cast<int>(bp.size()) - 1; i >= 0; --i)
                     op_stack.pop(bp[i]);

                  pc_element_t elem{};
                  elem.operand_depth = op_stack.depth();
                  elem.expected_result = single_type;
                  elem.label_result = single_type;
                  elem.is_if = false;
                  elem.relocations = std::vector<branch_t>{};
                  if (!br.empty()) { elem.expected_results = br; elem.label_results = br; }
                  elem.block_params = std::move(bp);
                  pc_stack.push_back(std::move(elem));
                  code_writer.emit_block(single_type, static_cast<uint32_t>(br.size()));
                  op_stack.push_scope();
                  // Push params back inside the block scope
                  for (auto p : pc_stack.back().block_params)
                     op_stack.push(p);
                  nested_checker.on_control(_options);
               } break;
               case opcodes::loop: {
                  uint8_t first_byte = *code;
                  uint8_t single_type = types::pseudo;
                  std::vector<uint8_t> bp, br;

                  if (first_byte == types::pseudo || first_byte == types::i32 || first_byte == types::i64 ||
                      first_byte == types::f32 || first_byte == types::f64 || first_byte == types::v128) {
                     code++;
                     PSIZAM_ASSERT(first_byte == types::i32 || first_byte == types::i64 ||
                                   first_byte == types::f32 || first_byte == types::f64 ||
                                   (first_byte == types::v128 && get_enable_simd(_options)) ||
                                   first_byte == types::pseudo, wasm_parse_exception,
                                   "Invalid type code in loop");
                     single_type = first_byte;
                  } else {
                     int32_t type_idx = parse_varint32(code);
                     PSIZAM_ASSERT(type_idx >= 0 && static_cast<uint32_t>(type_idx) < _mod->types.size(),
                                   wasm_parse_exception, "invalid block type index");
                     const func_type& ft = _mod->types[type_idx];
                     bp.assign(ft.param_types.begin(), ft.param_types.end());
                     br.assign(ft.return_types.begin(), ft.return_types.end());
                     single_type = ft.return_count ? ft.return_type : types::pseudo;
                  }

                  // Pop params from outer stack
                  for (int i = static_cast<int>(bp.size()) - 1; i >= 0; --i)
                     op_stack.pop(bp[i]);

                  auto pos = code_writer.emit_loop(single_type, static_cast<uint32_t>(br.size()));
                  pc_element_t elem{};
                  elem.operand_depth = op_stack.depth();
                  elem.expected_result = single_type;
                  elem.label_result = types::pseudo;  // loops: labels carry params, not results
                  elem.is_if = false;
                  elem.relocations = pos;
                  if (!br.empty()) elem.expected_results = br;
                  if (!bp.empty()) elem.label_results = bp;  // loop labels carry param types
                  elem.block_params = std::move(bp);
                  pc_stack.push_back(std::move(elem));
                  op_stack.push_scope();
                  // Push params back inside the loop scope
                  for (auto p : pc_stack.back().block_params)
                     op_stack.push(p);
                  nested_checker.on_control(_options);
               } break;
               case opcodes::if_: {
                  check_in_bounds();
                  uint8_t first_byte = *code;
                  uint8_t single_type = types::pseudo;
                  std::vector<uint8_t> bp, br;

                  if (first_byte == types::pseudo || first_byte == types::i32 || first_byte == types::i64 ||
                      first_byte == types::f32 || first_byte == types::f64 || first_byte == types::v128) {
                     code++;
                     PSIZAM_ASSERT(first_byte == types::i32 || first_byte == types::i64 ||
                                   first_byte == types::f32 || first_byte == types::f64 ||
                                   (first_byte == types::v128 && get_enable_simd(_options)) ||
                                   first_byte == types::pseudo, wasm_parse_exception,
                                   "Invalid type code in if");
                     single_type = first_byte;
                  } else {
                     int32_t type_idx = parse_varint32(code);
                     PSIZAM_ASSERT(type_idx >= 0 && static_cast<uint32_t>(type_idx) < _mod->types.size(),
                                   wasm_parse_exception, "invalid block type index");
                     const func_type& ft = _mod->types[type_idx];
                     bp.assign(ft.param_types.begin(), ft.param_types.end());
                     br.assign(ft.return_types.begin(), ft.return_types.end());
                     single_type = ft.return_count ? ft.return_type : types::pseudo;
                  }

                  auto branch = code_writer.emit_if(single_type, static_cast<uint32_t>(br.size()));
                  op_stack.pop(types::i32);  // condition

                  // Pop params from outer stack
                  for (int i = static_cast<int>(bp.size()) - 1; i >= 0; --i)
                     op_stack.pop(bp[i]);

                  pc_element_t elem{};
                  elem.operand_depth = op_stack.depth();
                  elem.expected_result = single_type;
                  elem.label_result = single_type;
                  elem.is_if = true;
                  elem.relocations = std::vector{branch};
                  if (!br.empty()) { elem.expected_results = br; elem.label_results = br; }
                  elem.block_params = std::move(bp);
                  pc_stack.push_back(std::move(elem));
                  op_stack.push_scope();
                  // Push params back inside the if scope
                  for (auto p : pc_stack.back().block_params)
                     op_stack.push(p);
                  nested_checker.on_control(_options);
               } break;
               case opcodes::else_: {
                  check_in_bounds();
                  auto& old_index = pc_stack.back();
                  PSIZAM_ASSERT(old_index.is_if, wasm_parse_exception, "else outside if");
                  auto& relocations = std::get<std::vector<branch_t>>(old_index.relocations);
                  // reset the operand stack to the same state as the if
                  if (!old_index.expected_results.empty()) {
                     for (int i = static_cast<int>(old_index.expected_results.size()) - 1; i >= 0; --i)
                        op_stack.pop(old_index.expected_results[i]);
                  } else {
                     op_stack.pop(old_index.expected_result);
                  }
                  op_stack.pop_scope();
                  op_stack.push_scope();
                  // Re-push params for the else branch
                  for (auto p : old_index.block_params)
                     op_stack.push(p);
                  // Overwrite the branch from the `if` with the `else`.
                  // We're left with a normal relocation list where everything
                  // branches to the corresponding `end`
                  relocations[0] = code_writer.emit_else(relocations[0]);
                  old_index.is_if = false;
                  nested_checker.on_control(_options);
                  break;
               }
               // ── Exception handling opcodes ──
               case opcodes::try_: {
                  // try has the same structure as block (block_type immediate)
                  check_in_bounds();
                  uint8_t first_byte = *code;
                  uint8_t single_type = types::pseudo;
                  std::vector<uint8_t> bp, br;
                  if (first_byte == types::pseudo || first_byte == types::i32 || first_byte == types::i64 ||
                      first_byte == types::f32 || first_byte == types::f64 || first_byte == types::v128) {
                     code++;
                     single_type = first_byte;
                  } else {
                     int32_t type_idx = parse_varint32(code);
                     PSIZAM_ASSERT(type_idx >= 0 && static_cast<uint32_t>(type_idx) < _mod->types.size(),
                                   wasm_parse_exception, "invalid block type index");
                     const func_type& ft = _mod->types[type_idx];
                     bp.assign(ft.param_types.begin(), ft.param_types.end());
                     br.assign(ft.return_types.begin(), ft.return_types.end());
                     single_type = ft.return_count ? ft.return_type : types::pseudo;
                  }
                  for (int i = static_cast<int>(bp.size()) - 1; i >= 0; --i)
                     op_stack.pop(bp[i]);
                  pc_element_t elem{};
                  elem.operand_depth = op_stack.depth();
                  elem.expected_result = single_type;
                  elem.label_result = single_type;
                  elem.is_if = false;
                  elem.relocations = std::vector<branch_t>{};
                  if (!br.empty()) { elem.expected_results = br; elem.label_results = br; }
                  elem.block_params = std::move(bp);
                  pc_stack.push_back(std::move(elem));
                  code_writer.emit_try(single_type, static_cast<uint32_t>(br.size()));
                  op_stack.push_scope();
                  for (auto p : pc_stack.back().block_params)
                     op_stack.push(p);
                  nested_checker.on_control(_options);
               } break;
               case opcodes::catch_: {
                  check_in_bounds();
                  uint32_t tag_index = parse_varuint32(code);
                  PSIZAM_ASSERT(tag_index < _mod->tags.size(), wasm_parse_exception, "invalid tag index");
                  auto& old_index = pc_stack.back();
                  // Pop results from try body
                  if (!old_index.expected_results.empty()) {
                     for (int i = static_cast<int>(old_index.expected_results.size()) - 1; i >= 0; --i)
                        op_stack.pop(old_index.expected_results[i]);
                  } else {
                     op_stack.pop(old_index.expected_result);
                  }
                  op_stack.pop_scope();
                  op_stack.push_scope();
                  // The catch handler receives the tag's parameter types as values
                  const func_type& tag_ft = _mod->types[_mod->tags[tag_index].type_index];
                  for (auto pt : tag_ft.param_types)
                     op_stack.push(pt);
                  auto& relocations = std::get<std::vector<branch_t>>(old_index.relocations);
                  relocations.push_back(code_writer.emit_catch(tag_index));
                  nested_checker.on_control(_options);
               } break;
               case opcodes::throw_: {
                  check_in_bounds();
                  uint32_t tag_index = parse_varuint32(code);
                  PSIZAM_ASSERT(tag_index < _mod->tags.size(), wasm_parse_exception, "invalid tag index");
                  // Pop the tag's parameter types from the stack
                  const func_type& tag_ft = _mod->types[_mod->tags[tag_index].type_index];
                  for (int i = static_cast<int>(tag_ft.param_types.size()) - 1; i >= 0; --i)
                     op_stack.pop(tag_ft.param_types[i]);
                  code_writer.emit_throw(tag_index);
                  op_stack.start_unreachable();
               } break;
               case opcodes::rethrow_: {
                  check_in_bounds();
                  uint32_t label = parse_varuint32(code);
                  auto [depth_change,rt,rc] = compute_depth_change(label);
                  code_writer.emit_rethrow(depth_change, rt, label, rc);
                  op_stack.start_unreachable();
               } break;
               case opcodes::throw_ref_: {
                  check_in_bounds();
                  op_stack.pop(types::exnref);
                  code_writer.emit_throw_ref();
                  op_stack.start_unreachable();
               } break;
               case opcodes::catch_all_: {
                  check_in_bounds();
                  auto& old_index = pc_stack.back();
                  if (!old_index.expected_results.empty()) {
                     for (int i = static_cast<int>(old_index.expected_results.size()) - 1; i >= 0; --i)
                        op_stack.pop(old_index.expected_results[i]);
                  } else {
                     op_stack.pop(old_index.expected_result);
                  }
                  op_stack.pop_scope();
                  op_stack.push_scope();
                  // catch_all receives no values
                  auto& relocations = std::get<std::vector<branch_t>>(old_index.relocations);
                  relocations.push_back(code_writer.emit_catch_all());
                  nested_checker.on_control(_options);
               } break;
               case opcodes::delegate_: {
                  check_in_bounds();
                  uint32_t label = parse_varuint32(code);
                  auto [depth_change,rt,rc] = compute_depth_change(label);
                  code_writer.emit_delegate(depth_change, rt, label, rc);
                  // delegate acts as end of the try block
                  exit_scope();
                  nested_checker.on_end(_options);
               } break;
               case opcodes::br: {
                  check_in_bounds();
                  uint32_t label = parse_varuint32(code);
                  auto [depth_change,rt,rc] = compute_depth_change(label);
                  auto branch = code_writer.emit_br(depth_change, rt, label, rc);
                  handle_branch_target(label, branch);
                  op_stack.start_unreachable();
               } break;
               case opcodes::br_if: {
                  check_in_bounds();
                  uint32_t label = parse_varuint32(code);
                  op_stack.pop(types::i32);
                  auto [depth_change,rt,rc] = compute_depth_change(label);
                  auto branch = code_writer.emit_br_if(depth_change, rt, label, rc);
                  handle_branch_target(label, branch);
               } break;
               case opcodes::br_table: {
                  check_in_bounds();
                  size_t table_size = parse_varuint32(code);
                  PSIZAM_ASSERT(table_size <= get_max_br_table_elements(_options), wasm_parse_exception, "Too many labels in br_table");
                  uint8_t result_type = 0;
                  op_stack.pop(types::i32);
                  auto handler = code_writer.emit_br_table(table_size);
                  for (size_t i = 0; i < table_size; i++) {
                     uint32_t label = parse_varuint32(code);
                     auto [depth_change,rt,rc] = compute_depth_change(label);
                     auto branch = handler.emit_case(depth_change, rt, label, rc);
                     handle_branch_target(label, branch);
                     uint8_t one_result = pc_stack[pc_stack.size() - label - 1].label_result;
                     if(i == 0) {
                        result_type = one_result;
                     } else {
                        PSIZAM_ASSERT(result_type == one_result, wasm_parse_exception, "br_table labels must have the same type");
                     }
                  }
                  uint32_t label = parse_varuint32(code);
                  auto [depth_change,rt,rc] = compute_depth_change(label);
                  auto branch = handler.emit_default(depth_change, rt, label, rc);
                  handle_branch_target(label, branch);
                  PSIZAM_ASSERT(table_size == 0 || result_type == pc_stack[pc_stack.size() - label - 1].label_result,
                                wasm_parse_exception, "br_table labels must have the same type");
                  op_stack.start_unreachable();
               } break;
               case opcodes::call: {
                  check_in_bounds();
                  uint32_t funcnum = parse_varuint32(code);
                  PSIZAM_ASSERT(funcnum < _mod->get_imported_functions_size() + _mod->functions.size(),
                                wasm_parse_exception, "call function index out of range");
                  const func_type& ft = _mod->get_function_type(funcnum);
                  for(uint32_t i = 0; i < ft.param_types.size(); ++i)
                     op_stack.pop(ft.param_types[ft.param_types.size() - i - 1]);
                  for(auto rt : ft.return_types)
                     op_stack.push(rt);
                  code_writer.emit_call(ft, funcnum);
               } break;
               case opcodes::call_indirect: {
                  check_in_bounds();
                  uint32_t functypeidx = parse_varuint32(code);
                  PSIZAM_ASSERT(functypeidx < _mod->types.size(), wasm_parse_exception, "call_indirect type index out of range");
                  const func_type& ft = _mod->types[functypeidx];
                  PSIZAM_ASSERT(_mod->tables.size() > 0, wasm_parse_exception, "call_indirect requires a table");
                  op_stack.pop(types::i32);
                  for(uint32_t i = 0; i < ft.param_types.size(); ++i)
                     op_stack.pop(ft.param_types[ft.param_types.size() - i - 1]);
                  for(auto rt : ft.return_types)
                     op_stack.push(rt);
                  uint32_t table_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "call_indirect table index out of range");
                  code_writer.emit_call_indirect(ft, type_aliases[functypeidx], table_idx);
                  break;
               }
               // Tail calls: optimized frame reuse (no stack growth)
               case 0x12: { // return_call
                  check_in_bounds();
                  uint32_t funcnum = parse_varuint32(code);
                  PSIZAM_ASSERT(funcnum < _mod->get_imported_functions_size() + _mod->functions.size(),
                                wasm_parse_exception, "return_call function index out of range");
                  const func_type& target_ft = _mod->get_function_type(funcnum);
                  PSIZAM_ASSERT(target_ft.return_types == ft.return_types,
                                wasm_parse_exception, "return_call type mismatch");
                  for(uint32_t i = 0; i < target_ft.param_types.size(); ++i)
                     op_stack.pop(target_ft.param_types[target_ft.param_types.size() - i - 1]);
                  for(auto rt : target_ft.return_types)
                     op_stack.push(rt);
                  code_writer.emit_tail_call(target_ft, funcnum);
                  op_stack.start_unreachable();
               } break;
               case 0x13: { // return_call_indirect
                  check_in_bounds();
                  uint32_t functypeidx = parse_varuint32(code);
                  const func_type& target_ft = _mod->types.at(functypeidx);
                  PSIZAM_ASSERT(target_ft.return_types == ft.return_types,
                                wasm_parse_exception, "return_call_indirect type mismatch");
                  PSIZAM_ASSERT(_mod->tables.size() > 0, wasm_parse_exception, "return_call_indirect requires a table");
                  op_stack.pop(types::i32);
                  for(uint32_t i = 0; i < target_ft.param_types.size(); ++i)
                     op_stack.pop(target_ft.param_types[target_ft.param_types.size() - i - 1]);
                  for(auto rt : target_ft.return_types)
                     op_stack.push(rt);
                  uint32_t table_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "return_call_indirect table index out of range");
                  PSIZAM_ASSERT(_mod->tables[table_idx].element_type == types::funcref, wasm_parse_exception, "return_call_indirect requires funcref table");
                  code_writer.emit_tail_call_indirect(target_ft, type_aliases[functypeidx], table_idx);
                  op_stack.start_unreachable();
               } break;
               case opcodes::try_table_: {
                  check_in_bounds();
                  // Parse block type (same as block/if/try)
                  uint8_t first_byte = *code;
                  uint8_t single_type = types::pseudo;
                  std::vector<uint8_t> bp, br;

                  if (first_byte == types::pseudo || first_byte == types::i32 || first_byte == types::i64 ||
                      first_byte == types::f32 || first_byte == types::f64 || first_byte == types::v128) {
                     code++;
                     single_type = first_byte;
                  } else if (first_byte == types::exnref || first_byte == types::funcref || first_byte == types::externref) {
                     code++;
                     single_type = first_byte;
                  } else {
                     int32_t type_idx = parse_varint32(code);
                     PSIZAM_ASSERT(type_idx >= 0 && static_cast<uint32_t>(type_idx) < _mod->types.size(),
                                   wasm_parse_exception, "invalid block type index");
                     const func_type& ft = _mod->types[type_idx];
                     bp.assign(ft.param_types.begin(), ft.param_types.end());
                     br.assign(ft.return_types.begin(), ft.return_types.end());
                     single_type = ft.return_count ? ft.return_type : types::pseudo;
                  }

                  // Parse catch clauses
                  uint32_t num_catches = parse_varuint32(code);
                  std::vector<catch_clause> clauses(num_catches);
                  for (uint32_t i = 0; i < num_catches; ++i) {
                     clauses[i].kind = *code++;
                     PSIZAM_ASSERT(clauses[i].kind <= 3, wasm_parse_exception, "invalid catch kind");
                     if (clauses[i].kind == catch_kind::catch_tag || clauses[i].kind == catch_kind::catch_tag_ref) {
                        clauses[i].tag_index = parse_varuint32(code);
                        PSIZAM_ASSERT(clauses[i].tag_index < _mod->tags.size(), wasm_parse_exception, "invalid tag index");
                     }
                     clauses[i].label = parse_varuint32(code);
                     PSIZAM_ASSERT(clauses[i].label < pc_stack.size(), wasm_parse_exception, "invalid catch label");

                     // Validate that the catch label's expected types match:
                     // catch/catch_ref: label expects tag payload types (+ exnref for _ref)
                     // catch_all: label expects nothing
                     // catch_all_ref: label expects exnref
                     // (Full type validation deferred to a later pass — just validate label is in range)
                  }

                  // Pop params from outer stack (for multi-value blocks)
                  for (int i = static_cast<int>(bp.size()) - 1; i >= 0; --i)
                     op_stack.pop(bp[i]);

                  auto clause_pcs = code_writer.emit_try_table(single_type, static_cast<uint32_t>(br.size()), clauses);

                  // Push try_table onto pc_stack BEFORE resolving catch clause labels,
                  // because catch clause label depths are relative to inside the try_table
                  // (depth 0 = try_table, depth 1 = enclosing block, etc.)
                  pc_element_t elem{};
                  elem.operand_depth = op_stack.depth();
                  elem.expected_result = single_type;
                  elem.label_result = single_type;
                  elem.is_if = false;
                  elem.relocations = std::vector<branch_t>{};
                  if (!br.empty()) { elem.expected_results = br; elem.label_results = br; }
                  elem.block_params = std::move(bp);
                  pc_stack.push_back(std::move(elem));

                  // Now register catch clause PCs for branch target relocation
                  for (uint32_t i = 0; i < num_catches; ++i) {
                     handle_branch_target(clauses[i].label, clause_pcs[i]);
                  }

                  op_stack.push_scope();
                  // Push params back inside the try_table scope
                  for (auto p : pc_stack.back().block_params)
                     op_stack.push(p);
                  nested_checker.on_control(_options);
               } break;
               case opcodes::drop: check_in_bounds(); code_writer.emit_drop(op_stack.pop()); break;
               case opcodes::select: {
                  check_in_bounds();
                  op_stack.pop(types::i32);
                  uint8_t t0 = op_stack.pop();
                  uint8_t t1 = op_stack.pop();
                  PSIZAM_ASSERT(t0 == t1 || t0 == any_type || t1 == any_type, wasm_parse_exception, "incorrect types for select");
                  op_stack.push(t0 != any_type? t0 : t1);
                  code_writer.emit_select(t0);
               } break;
               case opcodes::get_local: {
                  uint32_t local_idx = parse_varuint32(code);
                  op_stack.push(local_types[local_idx]);
                  code_writer.emit_get_local(local_idx, local_types[local_idx]);
               } break;
               case opcodes::set_local: {
                  check_in_bounds();
                  uint32_t local_idx = parse_varuint32(code);
                  op_stack.pop(local_types[local_idx]);
                  code_writer.emit_set_local(local_idx, local_types[local_idx]);
               } break;
               case opcodes::tee_local: {
                  check_in_bounds();
                  uint32_t local_idx = parse_varuint32(code);
                  op_stack.top(local_types[local_idx]);
                  code_writer.emit_tee_local(local_idx, local_types[local_idx]);
               } break;
               case opcodes::get_global: {
                  uint32_t global_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(global_idx < _mod->globals.size(), wasm_parse_exception, "global index out of range");
                  op_stack.push(_mod->globals[global_idx].type.content_type);
                  code_writer.emit_get_global(global_idx);
               } break;
               case opcodes::set_global: {
                  check_in_bounds();
                  uint32_t global_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(global_idx < _mod->globals.size(), wasm_parse_exception, "global index out of range");
                  PSIZAM_ASSERT(_mod->globals[global_idx].type.mutability, wasm_parse_exception, "cannot set const global");
                  op_stack.pop(_mod->globals[global_idx].type.content_type);
                  code_writer.emit_set_global(global_idx);
               } break;
               case opcodes::ref_null: {
                  check_in_bounds();
                  uint8_t ref_type = *code++;
                  PSIZAM_ASSERT(ref_type == types::funcref || ref_type == types::externref,
                                wasm_parse_exception, "ref.null requires funcref or externref type");
                  op_stack.push(ref_type);
                  code_writer.emit_ref_null(ref_type);
               } break;
               case opcodes::ref_is_null: {
                  check_in_bounds();
                  auto ref_type = op_stack.pop();  // pop any ref type
                  PSIZAM_ASSERT(ref_type == types::funcref || ref_type == types::externref ||
                                ref_type == 0x82 /*any_type*/,
                                wasm_parse_exception, "ref.is_null requires reference type");
                  op_stack.push(types::i32);  // push i32 result
                  code_writer.emit_ref_is_null();
               } break;
               case opcodes::ref_func: {
                  check_in_bounds();
                  uint32_t func_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(func_idx < _mod->get_functions_total(),
                                wasm_parse_exception, "ref.func function index out of range");
                  op_stack.push(types::funcref);
                  code_writer.emit_ref_func(func_idx);
               } break;
               case opcodes::table_get: {
                  check_in_bounds();
                  uint32_t table_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "table.get table index out of range");
                  op_stack.pop(types::i32);  // index
                  op_stack.push(_mod->tables[table_idx].element_type);
                  code_writer.emit_table_get(table_idx);
               } break;
               case opcodes::table_set: {
                  check_in_bounds();
                  uint32_t table_idx = parse_varuint32(code);
                  PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "table.set table index out of range");
                  op_stack.pop(_mod->tables[table_idx].element_type);  // value (ref)
                  op_stack.pop(types::i32);  // index
                  code_writer.emit_table_set(table_idx);
               } break;
#define LOAD_OP(op_name, max_align, type)                            \
               case opcodes::op_name: {                              \
                  check_in_bounds();                                 \
                  PSIZAM_ASSERT(_mod->memories.size() > 0, wasm_parse_exception, "load requires memory"); \
                  uint32_t alignment = parse_varuint32(code);        \
                  uint32_t offset;                                   \
                  if (mem_addr_type() == types::i64) {               \
                     uint64_t off64 = parse_varuint64(code);         \
                     PSIZAM_ASSERT(off64 <= UINT32_MAX, wasm_parse_exception, "memory64 offset exceeds implementation limit"); \
                     offset = static_cast<uint32_t>(off64);          \
                  } else {                                           \
                     offset = parse_varuint32(code);                 \
                  }                                                  \
                  PSIZAM_ASSERT(alignment <= uint32_t(max_align), wasm_parse_exception, "alignment cannot be greater than size."); \
                  PSIZAM_ASSERT(offset <= get_max_memory_offset(_options), wasm_parse_exception, "load offset too large."); \
                  op_stack.pop(mem_addr_type());                     \
                  op_stack.push(types::type);                        \
                  code_writer.emit_ ## op_name( alignment, offset ); \
               } break;

               LOAD_OP(i32_load, 2, i32)
               LOAD_OP(i64_load, 3, i64)
               LOAD_OP(f32_load, 2, f32)
               LOAD_OP(f64_load, 3, f64)
               LOAD_OP(i32_load8_s, 0, i32)
               LOAD_OP(i32_load16_s, 1, i32)
               LOAD_OP(i32_load8_u, 0, i32)
               LOAD_OP(i32_load16_u, 1, i32)
               LOAD_OP(i64_load8_s, 0, i64)
               LOAD_OP(i64_load16_s, 1, i64)
               LOAD_OP(i64_load32_s, 2, i64)
               LOAD_OP(i64_load8_u, 0, i64)
               LOAD_OP(i64_load16_u, 1, i64)
               LOAD_OP(i64_load32_u, 2, i64)

#define STORE_OP(op_name, max_align, type)                           \
               case opcodes::op_name: {                              \
                  check_in_bounds();                                 \
                  PSIZAM_ASSERT(_mod->memories.size() > 0, wasm_parse_exception, "store requires memory"); \
                  uint32_t alignment = parse_varuint32(code);        \
                  uint32_t offset;                                   \
                  if (mem_addr_type() == types::i64) {               \
                     uint64_t off64 = parse_varuint64(code);         \
                     PSIZAM_ASSERT(off64 <= UINT32_MAX, wasm_parse_exception, "memory64 offset exceeds implementation limit"); \
                     offset = static_cast<uint32_t>(off64);          \
                  } else {                                           \
                     offset = parse_varuint32(code);                 \
                  }                                                  \
                  PSIZAM_ASSERT(alignment <= uint32_t(max_align), wasm_parse_exception, "alignment cannot be greater than size."); \
                  PSIZAM_ASSERT(offset <= get_max_memory_offset(_options), wasm_parse_exception, "store offset too large."); \
                  op_stack.pop(types::type);                         \
                  op_stack.pop(mem_addr_type());                     \
                  code_writer.emit_ ## op_name( alignment, offset ); \
               } break;

               STORE_OP(i32_store, 2, i32)
               STORE_OP(i64_store, 3, i64)
               STORE_OP(f32_store, 2, f32)
               STORE_OP(f64_store, 3, f64)
               STORE_OP(i32_store8, 0, i32)
               STORE_OP(i32_store16, 1, i32)
               STORE_OP(i64_store8, 0, i64)
               STORE_OP(i64_store16, 1, i64)
               STORE_OP(i64_store32, 2, i64)

               case opcodes::current_memory:
                  PSIZAM_ASSERT(_mod->memories.size() != 0, wasm_parse_exception, "memory.size requires memory");
                  op_stack.push(mem_addr_type());
                  PSIZAM_ASSERT(*code == 0, wasm_parse_exception, "memory.size must end with 0x00");
                  code++;
                  code_writer.emit_current_memory();
                  break;
               case opcodes::grow_memory:
                  check_in_bounds();
                  PSIZAM_ASSERT(_mod->memories.size() != 0, wasm_parse_exception, "memory.grow requires memory");
                  op_stack.pop(mem_addr_type());
                  op_stack.push(mem_addr_type());
                  PSIZAM_ASSERT(*code == 0, wasm_parse_exception, "memory.grow must end with 0x00");
                  code++;
                  code_writer.emit_grow_memory();
                  break;
               case opcodes::i32_const: code_writer.emit_i32_const( parse_varint32(code) ); op_stack.push(types::i32); break;
               case opcodes::i64_const: code_writer.emit_i64_const( parse_varint64(code) ); op_stack.push(types::i64); break;
               case opcodes::f32_const: {
                  code_writer.emit_f32_const( parse_raw<float>(code) );
                  op_stack.push(types::f32);
               } break;
               case opcodes::f64_const: {
                  code_writer.emit_f64_const( parse_raw<double>(code) );
                  op_stack.push(types::f64);
               } break;

#define UNOP(opname) \
               case opcodes::opname: check_in_bounds(); code_writer.emit_ ## opname(); op_stack.pop(types::A); op_stack.push(types::R); break;
#define BINOP(opname) \
               case opcodes::opname: check_in_bounds(); code_writer.emit_ ## opname(); op_stack.pop(types::A); op_stack.pop(types::A); op_stack.push(types::R); break;
#define CASTOP(dst, opname, src)                                         \
               case opcodes::dst ## _ ## opname ## _ ## src: check_in_bounds(); code_writer.emit_ ## dst ## _ ## opname ## _ ## src(); op_stack.pop(types::src); op_stack.push(types::dst); break;

#define R i32
#define A i32
               UNOP(i32_eqz)
               BINOP(i32_eq)
               BINOP(i32_ne)
               BINOP(i32_lt_s)
               BINOP(i32_lt_u)
               BINOP(i32_gt_s)
               BINOP(i32_gt_u)
               BINOP(i32_le_s)
               BINOP(i32_le_u)
               BINOP(i32_ge_s)
               BINOP(i32_ge_u)
#undef A
#define A i64
               UNOP(i64_eqz)
               BINOP(i64_eq)
               BINOP(i64_ne)
               BINOP(i64_lt_s)
               BINOP(i64_lt_u)
               BINOP(i64_gt_s)
               BINOP(i64_gt_u)
               BINOP(i64_le_s)
               BINOP(i64_le_u)
               BINOP(i64_ge_s)
               BINOP(i64_ge_u)
#undef A
#define A f32
               BINOP(f32_eq)
               BINOP(f32_ne)
               BINOP(f32_lt)
               BINOP(f32_gt)
               BINOP(f32_le)
               BINOP(f32_ge)
#undef A
#define A f64
               BINOP(f64_eq)
               BINOP(f64_ne)
               BINOP(f64_lt)
               BINOP(f64_gt)
               BINOP(f64_le)
               BINOP(f64_ge)
#undef A
#undef R
#define R A
#define A i32
               UNOP(i32_clz)
               UNOP(i32_ctz)
               UNOP(i32_popcnt)
               BINOP(i32_add)
               BINOP(i32_sub)
               BINOP(i32_mul)
               BINOP(i32_div_s)
               BINOP(i32_div_u)
               BINOP(i32_rem_s)
               BINOP(i32_rem_u)
               BINOP(i32_and)
               BINOP(i32_or)
               BINOP(i32_xor)
               BINOP(i32_shl)
               BINOP(i32_shr_s)
               BINOP(i32_shr_u)
               BINOP(i32_rotl)
               BINOP(i32_rotr)
#undef A
#define A i64
               UNOP(i64_clz)
               UNOP(i64_ctz)
               UNOP(i64_popcnt)
               BINOP(i64_add)
               BINOP(i64_sub)
               BINOP(i64_mul)
               BINOP(i64_div_s)
               BINOP(i64_div_u)
               BINOP(i64_rem_s)
               BINOP(i64_rem_u)
               BINOP(i64_and)
               BINOP(i64_or)
               BINOP(i64_xor)
               BINOP(i64_shl)
               BINOP(i64_shr_s)
               BINOP(i64_shr_u)
               BINOP(i64_rotl)
               BINOP(i64_rotr)
#undef A
#define A f32
               UNOP(f32_abs)
               UNOP(f32_neg)
               UNOP(f32_ceil)
               UNOP(f32_floor)
               UNOP(f32_trunc)
               UNOP(f32_nearest)
               UNOP(f32_sqrt)
               BINOP(f32_add)
               BINOP(f32_sub)
               BINOP(f32_mul)
               BINOP(f32_div)
               BINOP(f32_min)
               BINOP(f32_max)
               BINOP(f32_copysign)
#undef A
#define A f64
               UNOP(f64_abs)
               UNOP(f64_neg)
               UNOP(f64_ceil)
               UNOP(f64_floor)
               UNOP(f64_trunc)
               UNOP(f64_nearest)
               UNOP(f64_sqrt)
               BINOP(f64_add)
               BINOP(f64_sub)
               BINOP(f64_mul)
               BINOP(f64_div)
               BINOP(f64_min)
               BINOP(f64_max)
               BINOP(f64_copysign)
#undef A
#undef R

               CASTOP(i32, wrap, i64)
               CASTOP(i32, trunc_s, f32)
               CASTOP(i32, trunc_u, f32)
               CASTOP(i32, trunc_s, f64)
               CASTOP(i32, trunc_u, f64)
               CASTOP(i64, extend_s, i32)
               CASTOP(i64, extend_u, i32)
               CASTOP(i64, trunc_s, f32)
               CASTOP(i64, trunc_u, f32)
               CASTOP(i64, trunc_s, f64)
               CASTOP(i64, trunc_u, f64)
               CASTOP(f32, convert_s, i32)
               CASTOP(f32, convert_u, i32)
               CASTOP(f32, convert_s, i64)
               CASTOP(f32, convert_u, i64)
               CASTOP(f32, demote, f64)
               CASTOP(f64, convert_s, i32)
               CASTOP(f64, convert_u, i32)
               CASTOP(f64, convert_s, i64)
               CASTOP(f64, convert_u, i64)
               CASTOP(f64, promote, f32)
               CASTOP(i32, reinterpret, f32)
               CASTOP(i64, reinterpret, f64)
               CASTOP(f32, reinterpret, i32)
               CASTOP(f64, reinterpret, i64)

#undef CASTOP
#undef UNOP
#undef BINOP
                   
#define EXTENDOP(dst, opname)                                           \
               case opcodes::dst ## _ ## opname:                        \
                  check_in_bounds();                                    \
                  PSIZAM_ASSERT(get_enable_sign_ext(_options), wasm_parse_exception, "Sign-extension operators not enabled"); \
                  code_writer.emit_ ## dst ## _ ## opname();            \
                  op_stack.pop(types::dst);                             \
                  op_stack.push(types::dst);                            \
                  break;

               EXTENDOP(i32, extend8_s)
               EXTENDOP(i32, extend16_s)
               EXTENDOP(i64, extend8_s)
               EXTENDOP(i64, extend16_s)
               EXTENDOP(i64, extend32_s)

               case opcodes::vector_prefix: {
                  PSIZAM_ASSERT(get_enable_simd(_options), wasm_parse_exception, "SIMD not enabled");
                  switch(parse_varuint32(code))
                  {
#define opcodes vec_opcodes
                     LOAD_OP(v128_load, 4, v128)
                     LOAD_OP(v128_load8x8_s, 3, v128)
                     LOAD_OP(v128_load8x8_u, 3, v128)
                     LOAD_OP(v128_load16x4_s, 3, v128)
                     LOAD_OP(v128_load16x4_u, 3, v128)
                     LOAD_OP(v128_load32x2_s, 3, v128)
                     LOAD_OP(v128_load32x2_u, 3, v128)
                     LOAD_OP(v128_load8_splat, 0, v128)
                     LOAD_OP(v128_load16_splat, 1, v128)
                     LOAD_OP(v128_load32_splat, 2, v128)
                     LOAD_OP(v128_load64_splat, 3, v128)
                     LOAD_OP(v128_load32_zero, 2, v128)
                     LOAD_OP(v128_load64_zero, 3, v128)
                     STORE_OP(v128_store, 4, v128)
#undef opcodes

#undef LOAD_OP
#undef STORE_OP

#define LOADLANE_OP(op_name, max_align, type)                           \
                     case vec_opcodes::op_name: {                       \
                        check_in_bounds();                              \
                        PSIZAM_ASSERT(_mod->memories.size() > 0, wasm_parse_exception, "load requires memory"); \
                        uint32_t alignment = parse_varuint32(code);     \
                        uint32_t offset;                                \
                        if (mem_addr_type() == types::i64) {            \
                           uint64_t off64 = parse_varuint64(code);      \
                           PSIZAM_ASSERT(off64 <= UINT32_MAX, wasm_parse_exception, "memory64 offset exceeds implementation limit"); \
                           offset = static_cast<uint32_t>(off64);       \
                        } else {                                        \
                           offset = parse_varuint32(code);              \
                        }                                               \
                        uint8_t lane = *code++;                         \
                        PSIZAM_ASSERT(alignment <= uint32_t(max_align), wasm_parse_exception, "alignment cannot be greater than size."); \
                        PSIZAM_ASSERT(offset <= get_max_memory_offset(_options), wasm_parse_exception, "load offset too large."); \
                        PSIZAM_ASSERT(lane < (1 << (4-max_align)), wasm_parse_exception, "laneidx out of bounds"); \
                        op_stack.pop(types::type);                      \
                        op_stack.pop(mem_addr_type());                  \
                        op_stack.push(types::type);                     \
                        code_writer.emit_ ## op_name( alignment, offset, lane ); \
                     } break;

                     LOADLANE_OP(v128_load8_lane, 0, v128)
                     LOADLANE_OP(v128_load16_lane, 1, v128)
                     LOADLANE_OP(v128_load32_lane, 2, v128)
                     LOADLANE_OP(v128_load64_lane, 3, v128)

#undef LOADLANE_OP

#define STORELANE_OP(op_name, max_align, type)                          \
                     case vec_opcodes::op_name: {                       \
                        check_in_bounds();                              \
                        PSIZAM_ASSERT(_mod->memories.size() > 0, wasm_parse_exception, "store requires memory"); \
                        uint32_t alignment = parse_varuint32(code);     \
                        uint32_t offset;                                \
                        if (mem_addr_type() == types::i64) {            \
                           uint64_t off64 = parse_varuint64(code);      \
                           PSIZAM_ASSERT(off64 <= UINT32_MAX, wasm_parse_exception, "memory64 offset exceeds implementation limit"); \
                           offset = static_cast<uint32_t>(off64);       \
                        } else {                                        \
                           offset = parse_varuint32(code);              \
                        }                                               \
                        uint8_t lane = *code++;                         \
                        PSIZAM_ASSERT(alignment <= uint32_t(max_align), wasm_parse_exception, "alignment cannot be greater than size."); \
                        PSIZAM_ASSERT(offset <= get_max_memory_offset(_options), wasm_parse_exception, "store offset too large."); \
                        PSIZAM_ASSERT(lane < (1 << (4-max_align)), wasm_parse_exception, "laneidx out of bounds"); \
                        op_stack.pop(types::type);                      \
                        op_stack.pop(mem_addr_type());                  \
                        code_writer.emit_ ## op_name( alignment, offset, lane ); \
                     } break;

                     STORELANE_OP(v128_store8_lane, 0, v128)
                     STORELANE_OP(v128_store16_lane, 1, v128)
                     STORELANE_OP(v128_store32_lane, 2, v128)
                     STORELANE_OP(v128_store64_lane, 3, v128)

#undef STORELANE_OP

                     case vec_opcodes::v128_const: {
                        check_in_bounds();
                        code_writer.emit_v128_const(parse_v128(code));
                        op_stack.push(types::v128);
                        break;
                     }

                     case vec_opcodes::i8x16_shuffle: {
                        check_in_bounds();
                        uint8_t* lanes = code.raw();
                        code += 16;
                        for(int i = 0; i < 16; ++i)
                        {
                           PSIZAM_ASSERT(lanes[i] < 32, wasm_parse_exception, "shuffle laneidx must be less than 32");
                        }
                        code_writer.emit_i8x16_shuffle(lanes);
                        op_stack.pop(types::v128);
                        op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                     } break;

#define EXTRACT_LANE_OP(opcode, type, N)                                \
                     case vec_opcodes::opcode: {                        \
                        check_in_bounds();                              \
                        uint8_t laneidx = *code++;                      \
                        PSIZAM_ASSERT(laneidx < N, wasm_parse_exception, "laneidx must be smaller than dim(shape)"); \
                        op_stack.pop(types::v128);                      \
                        op_stack.push(types::type);                     \
                        code_writer.emit_ ## opcode(laneidx);           \
                     } break;

#define REPLACE_LANE_OP(opcode, type, N)                                \
                     case vec_opcodes::opcode: {                        \
                        check_in_bounds();                              \
                        uint8_t laneidx = *code++;                      \
                        PSIZAM_ASSERT(laneidx < N, wasm_parse_exception, "laneidx must be smaller than dim(shape)"); \
                        op_stack.pop(types::type);                      \
                        op_stack.pop(types::v128);                      \
                        op_stack.push(types::v128);                     \
                        code_writer.emit_ ## opcode(laneidx);           \
                     } break;

                     EXTRACT_LANE_OP(i8x16_extract_lane_s, i32, 16)
                     EXTRACT_LANE_OP(i8x16_extract_lane_u, i32, 16)
                     REPLACE_LANE_OP(i8x16_replace_lane, i32, 16)
                     EXTRACT_LANE_OP(i16x8_extract_lane_s, i32, 8)
                     EXTRACT_LANE_OP(i16x8_extract_lane_u, i32, 8)
                     REPLACE_LANE_OP(i16x8_replace_lane, i32, 8)
                     EXTRACT_LANE_OP(i32x4_extract_lane, i32, 4)
                     REPLACE_LANE_OP(i32x4_replace_lane, i32, 4)
                     EXTRACT_LANE_OP(i64x2_extract_lane, i64, 2)
                     REPLACE_LANE_OP(i64x2_replace_lane, i64, 2)
                     EXTRACT_LANE_OP(f32x4_extract_lane, f32, 4)
                     REPLACE_LANE_OP(f32x4_replace_lane, f32, 4)
                     EXTRACT_LANE_OP(f64x2_extract_lane, f64, 2)
                     REPLACE_LANE_OP(f64x2_replace_lane, f64, 2)

#undef EXTRACT_LANE_OP

#define INPUTS_0()
#define INPUTS_1(t0) op_stack.pop(types::t0);
#define INPUTS_2(t0, t1) op_stack.pop(types::t1); INPUTS_1(t0);
#define INPUTS_3(t0, t1, t2) op_stack.pop(types::t2); INPUTS_2(t1, t0);

#define OUTPUTS_0()
#define OUTPUTS_1(t0) op_stack.push(types::t0);

#define CAT_I(x, y) x ## y
#define CAT(x, y) CAT_I(x, y)
#define VA_SZ_EMPTY() ~,~,~,~
#define VA_SZ_II(a0, a1, a2, a3, n, ...) n
#define VA_SZ_I(...) VA_SZ_II(__VA_ARGS__, 0, 3, 2, 1, 0, ~)
#define VA_SZ(...) VA_SZ_I( VA_SZ_EMPTY __VA_ARGS__ () )

#define NUMERIC_OP(opcode, inputs, outputs)                             \
                     case vec_opcodes::opcode: {                        \
                        check_in_bounds();                              \
                        CAT(INPUTS_, VA_SZ inputs) inputs               \
                        CAT(OUTPUTS_, VA_SZ outputs) outputs            \
                        code_writer.emit_ ## opcode();                  \
                     } break;

                     NUMERIC_OP(i8x16_swizzle, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_splat, (i32), (v128))
                     NUMERIC_OP(i16x8_splat, (i32), (v128))
                     NUMERIC_OP(i32x4_splat, (i32), (v128))
                     NUMERIC_OP(i64x2_splat, (i64), (v128))
                     NUMERIC_OP(f32x4_splat, (f32), (v128))
                     NUMERIC_OP(f64x2_splat, (f64), (v128))

                     NUMERIC_OP(i8x16_eq, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_ne, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_lt_s, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_lt_u, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_gt_s, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_gt_u, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_le_s, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_le_u, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_ge_s, (v128, v128), (v128))
                     NUMERIC_OP(i8x16_ge_u, (v128, v128), (v128))

                     NUMERIC_OP(i16x8_eq, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_ne, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_lt_s, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_lt_u, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_gt_s, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_gt_u, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_le_s, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_le_u, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_ge_s, (v128, v128), (v128))
                     NUMERIC_OP(i16x8_ge_u, (v128, v128), (v128))

                     NUMERIC_OP(i32x4_eq, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_ne, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_lt_s, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_lt_u, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_gt_s, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_gt_u, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_le_s, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_le_u, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_ge_s, (v128, v128), (v128))
                     NUMERIC_OP(i32x4_ge_u, (v128, v128), (v128))

                     NUMERIC_OP(i64x2_eq, (v128, v128), (v128))
                     NUMERIC_OP(i64x2_ne, (v128, v128), (v128))
                     NUMERIC_OP(i64x2_lt_s, (v128, v128), (v128))
                     NUMERIC_OP(i64x2_gt_s, (v128, v128), (v128))
                     NUMERIC_OP(i64x2_le_s, (v128, v128), (v128))
                     NUMERIC_OP(i64x2_ge_s, (v128, v128), (v128))

                     NUMERIC_OP(f32x4_eq, (v128, v128), (v128))
                     NUMERIC_OP(f32x4_ne, (v128, v128), (v128))
                     NUMERIC_OP(f32x4_lt, (v128, v128), (v128))
                     NUMERIC_OP(f32x4_gt, (v128, v128), (v128))
                     NUMERIC_OP(f32x4_le, (v128, v128), (v128))
                     NUMERIC_OP(f32x4_ge, (v128, v128), (v128))

                     NUMERIC_OP(f64x2_eq, (v128, v128), (v128))
                     NUMERIC_OP(f64x2_ne, (v128, v128), (v128))
                     NUMERIC_OP(f64x2_lt, (v128, v128), (v128))
                     NUMERIC_OP(f64x2_gt, (v128, v128), (v128))
                     NUMERIC_OP(f64x2_le, (v128, v128), (v128))
                     NUMERIC_OP(f64x2_ge, (v128, v128), (v128))

                     NUMERIC_OP(v128_not, (v128), (v128));
                     NUMERIC_OP(v128_and, (v128, v128), (v128));
                     NUMERIC_OP(v128_andnot, (v128, v128), (v128));
                     NUMERIC_OP(v128_or, (v128, v128), (v128));
                     NUMERIC_OP(v128_xor, (v128, v128), (v128));
                     NUMERIC_OP(v128_bitselect, (v128, v128, v128), (v128));
                     NUMERIC_OP(v128_any_true, (v128), (i32));

                     NUMERIC_OP(i8x16_abs, (v128), (v128));
                     NUMERIC_OP(i8x16_neg, (v128), (v128));
                     NUMERIC_OP(i8x16_popcnt, (v128), (v128));
                     NUMERIC_OP(i8x16_all_true, (v128), (i32));
                     NUMERIC_OP(i8x16_bitmask, (v128), (i32));
                     NUMERIC_OP(i8x16_narrow_i16x8_s, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_narrow_i16x8_u, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_shl, (v128, i32), (v128));
                     NUMERIC_OP(i8x16_shr_s, (v128, i32), (v128));
                     NUMERIC_OP(i8x16_shr_u, (v128, i32), (v128));
                     NUMERIC_OP(i8x16_add, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_add_sat_s, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_add_sat_u, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_sub, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_sub_sat_s, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_sub_sat_u, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_min_s, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_min_u, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_max_s, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_max_u, (v128, v128), (v128));
                     NUMERIC_OP(i8x16_avgr_u, (v128, v128), (v128));

                     NUMERIC_OP(i16x8_extadd_pairwise_i8x16_s, (v128), (v128));
                     NUMERIC_OP(i16x8_extadd_pairwise_i8x16_u, (v128), (v128));
                     NUMERIC_OP(i16x8_abs, (v128), (v128));
                     NUMERIC_OP(i16x8_neg, (v128), (v128));
                     NUMERIC_OP(i16x8_q15mulr_sat_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_all_true, (v128), (i32));
                     NUMERIC_OP(i16x8_bitmask, (v128), (i32));
                     NUMERIC_OP(i16x8_narrow_i32x4_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_narrow_i32x4_u, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_extend_low_i8x16_s, (v128), (v128));
                     NUMERIC_OP(i16x8_extend_high_i8x16_s, (v128), (v128));
                     NUMERIC_OP(i16x8_extend_low_i8x16_u, (v128), (v128));
                     NUMERIC_OP(i16x8_extend_high_i8x16_u, (v128), (v128));
                     NUMERIC_OP(i16x8_shl, (v128, i32), (v128));
                     NUMERIC_OP(i16x8_shr_s, (v128, i32), (v128));
                     NUMERIC_OP(i16x8_shr_u, (v128, i32), (v128));
                     NUMERIC_OP(i16x8_add, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_add_sat_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_add_sat_u, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_sub, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_sub_sat_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_sub_sat_u, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_mul, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_min_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_min_u, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_max_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_max_u, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_avgr_u, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_extmul_low_i8x16_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_extmul_high_i8x16_s, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_extmul_low_i8x16_u, (v128, v128), (v128));
                     NUMERIC_OP(i16x8_extmul_high_i8x16_u, (v128, v128), (v128));

                     NUMERIC_OP(i32x4_extadd_pairwise_i16x8_s, (v128), (v128));
                     NUMERIC_OP(i32x4_extadd_pairwise_i16x8_u, (v128), (v128));
                     NUMERIC_OP(i32x4_abs, (v128), (v128));
                     NUMERIC_OP(i32x4_neg, (v128), (v128));
                     NUMERIC_OP(i32x4_all_true, (v128), (i32));
                     NUMERIC_OP(i32x4_bitmask, (v128), (i32));
                     NUMERIC_OP(i32x4_extend_low_i16x8_s, (v128), (v128));
                     NUMERIC_OP(i32x4_extend_high_i16x8_s, (v128), (v128));
                     NUMERIC_OP(i32x4_extend_low_i16x8_u, (v128), (v128));
                     NUMERIC_OP(i32x4_extend_high_i16x8_u, (v128), (v128));
                     NUMERIC_OP(i32x4_shl, (v128, i32), (v128));
                     NUMERIC_OP(i32x4_shr_s, (v128, i32), (v128));
                     NUMERIC_OP(i32x4_shr_u, (v128, i32), (v128));
                     NUMERIC_OP(i32x4_add, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_sub, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_mul, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_min_s, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_min_u, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_max_s, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_max_u, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_dot_i16x8_s, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_extmul_low_i16x8_s, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_extmul_high_i16x8_s, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_extmul_low_i16x8_u, (v128, v128), (v128));
                     NUMERIC_OP(i32x4_extmul_high_i16x8_u, (v128, v128), (v128));

                     NUMERIC_OP(i64x2_abs, (v128), (v128));
                     NUMERIC_OP(i64x2_neg, (v128), (v128));
                     NUMERIC_OP(i64x2_all_true, (v128), (i32));
                     NUMERIC_OP(i64x2_bitmask, (v128), (i32));
                     NUMERIC_OP(i64x2_extend_low_i32x4_s, (v128), (v128));
                     NUMERIC_OP(i64x2_extend_high_i32x4_s, (v128), (v128));
                     NUMERIC_OP(i64x2_extend_low_i32x4_u, (v128), (v128));
                     NUMERIC_OP(i64x2_extend_high_i32x4_u, (v128), (v128));
                     NUMERIC_OP(i64x2_shl, (v128, i32), (v128));
                     NUMERIC_OP(i64x2_shr_s, (v128, i32), (v128));
                     NUMERIC_OP(i64x2_shr_u, (v128, i32), (v128));
                     NUMERIC_OP(i64x2_add, (v128, v128), (v128));
                     NUMERIC_OP(i64x2_sub, (v128, v128), (v128));
                     NUMERIC_OP(i64x2_mul, (v128, v128), (v128));
                     NUMERIC_OP(i64x2_extmul_low_i32x4_s, (v128, v128), (v128));
                     NUMERIC_OP(i64x2_extmul_high_i32x4_s, (v128, v128), (v128));
                     NUMERIC_OP(i64x2_extmul_low_i32x4_u, (v128, v128), (v128));
                     NUMERIC_OP(i64x2_extmul_high_i32x4_u, (v128, v128), (v128));

                     NUMERIC_OP(f32x4_ceil, (v128), (v128));
                     NUMERIC_OP(f32x4_floor, (v128), (v128));
                     NUMERIC_OP(f32x4_trunc, (v128), (v128));
                     NUMERIC_OP(f32x4_nearest, (v128), (v128));
                     NUMERIC_OP(f32x4_abs, (v128), (v128));
                     NUMERIC_OP(f32x4_neg, (v128), (v128));
                     NUMERIC_OP(f32x4_sqrt, (v128), (v128));
                     NUMERIC_OP(f32x4_add, (v128, v128), (v128));
                     NUMERIC_OP(f32x4_sub, (v128, v128), (v128));
                     NUMERIC_OP(f32x4_mul, (v128, v128), (v128));
                     NUMERIC_OP(f32x4_div, (v128, v128), (v128));
                     NUMERIC_OP(f32x4_min, (v128, v128), (v128));
                     NUMERIC_OP(f32x4_max, (v128, v128), (v128));
                     NUMERIC_OP(f32x4_pmin, (v128, v128), (v128));
                     NUMERIC_OP(f32x4_pmax, (v128, v128), (v128));

                     NUMERIC_OP(f64x2_ceil, (v128), (v128));
                     NUMERIC_OP(f64x2_floor, (v128), (v128));
                     NUMERIC_OP(f64x2_trunc, (v128), (v128));
                     NUMERIC_OP(f64x2_nearest, (v128), (v128));
                     NUMERIC_OP(f64x2_abs, (v128), (v128));
                     NUMERIC_OP(f64x2_neg, (v128), (v128));
                     NUMERIC_OP(f64x2_sqrt, (v128), (v128));
                     NUMERIC_OP(f64x2_add, (v128, v128), (v128));
                     NUMERIC_OP(f64x2_sub, (v128, v128), (v128));
                     NUMERIC_OP(f64x2_mul, (v128, v128), (v128));
                     NUMERIC_OP(f64x2_div, (v128, v128), (v128));
                     NUMERIC_OP(f64x2_min, (v128, v128), (v128));
                     NUMERIC_OP(f64x2_max, (v128, v128), (v128));
                     NUMERIC_OP(f64x2_pmin, (v128, v128), (v128));
                     NUMERIC_OP(f64x2_pmax, (v128, v128), (v128));

                     NUMERIC_OP(i32x4_trunc_sat_f32x4_s, (v128), (v128));
                     NUMERIC_OP(i32x4_trunc_sat_f32x4_u, (v128), (v128));
                     NUMERIC_OP(f32x4_convert_i32x4_s, (v128), (v128));
                     NUMERIC_OP(f32x4_convert_i32x4_u, (v128), (v128));
                     NUMERIC_OP(i32x4_trunc_sat_f64x2_s_zero, (v128), (v128));
                     NUMERIC_OP(i32x4_trunc_sat_f64x2_u_zero, (v128), (v128));
                     NUMERIC_OP(f64x2_convert_low_i32x4_s, (v128), (v128));
                     NUMERIC_OP(f64x2_convert_low_i32x4_u, (v128), (v128));
                     NUMERIC_OP(f32x4_demote_f64x2_zero, (v128), (v128));
                     NUMERIC_OP(f64x2_promote_low_f32x4, (v128), (v128));

#undef NUMERIC_OP
#undef VA_SZ
#undef VA_SZ_I
#undef VA_SZ_II
#undef VA_SZ_EMPTY
#undef CAT
#undef CAT_I
#undef OUTPUTS_1
#undef OUTPUTS_0
#undef INPUTS_2
#undef INPUTS_1
#undef INPUTS_0

                     // ── Relaxed SIMD ──
                     // Relaxed semantics allow delegation to strict equivalents.
                     case 0x100: // i8x16.relaxed_swizzle
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_i8x16_swizzle();
                        break;
                     case 0x101: // i32x4.relaxed_trunc_f32x4_s
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.push(types::v128);
                        code_writer.emit_i32x4_trunc_sat_f32x4_s();
                        break;
                     case 0x102: // i32x4.relaxed_trunc_f32x4_u
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.push(types::v128);
                        code_writer.emit_i32x4_trunc_sat_f32x4_u();
                        break;
                     case 0x103: // i32x4.relaxed_trunc_f64x2_s_zero
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.push(types::v128);
                        code_writer.emit_i32x4_trunc_sat_f64x2_s_zero();
                        break;
                     case 0x104: // i32x4.relaxed_trunc_f64x2_u_zero
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.push(types::v128);
                        code_writer.emit_i32x4_trunc_sat_f64x2_u_zero();
                        break;
                     case 0x105: // f32x4.relaxed_madd (a, b, c) -> a*b+c
                        // Decompose: stack has [a, b, c] -> mul(a,b) then add(result, c)
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        // c is on top, then b, then a. We need: a*b+c
                        // Emit: swap top two (get b on top of c), then do mul, then add
                        // Actually the WASM stack order for ternary: a is deepest, c on top
                        // We need to: mul(a,b), add(result, c)
                        // But c is already popped first by the writer's emit_f32x4_mul...
                        // Simplest: emit neg(nothing) + mul + add won't work directly.
                        // Use the ternary → binary decomposition via explicit stack manipulation:
                        // The parser already validated types. The code_writers just need the right sequence.
                        // For ternary ops, we need the writer to have a ternary emit or decompose here.
                        // Since relaxed_madd(a,b,c) = a*b+c, and stack is [a, b, c] (c on top):
                        // We can't easily decompose without extra local/temp. Instead, add proper ternary support.
                        // For now, decompose: this requires reordering — not trivial with the writer API.
                        // Use a simpler approach: treat as mul+add with stack reorder
                        // Actually, let me just call emit_f32x4_mul then emit_f32x4_add.
                        // The issue is stack ordering. Stack top is c, then b, then a.
                        // mul pops 2 values (b, a) and pushes result. But c is on top of b.
                        // We need to save c, do mul(a,b), then add(result, c).
                        // Without a save mechanism, we can't decompose at parser level easily.
                        // Let's add proper ternary ops to the writers instead.
                        code_writer.emit_f32x4_relaxed_madd();
                        break;
                     case 0x106: // f32x4.relaxed_nmadd (a, b, c) -> -a*b+c
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_f32x4_relaxed_nmadd();
                        break;
                     case 0x107: // f64x2.relaxed_madd (a, b, c) -> a*b+c
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_f64x2_relaxed_madd();
                        break;
                     case 0x108: // f64x2.relaxed_nmadd (a, b, c) -> -a*b+c
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_f64x2_relaxed_nmadd();
                        break;
                     case 0x109: // i8x16.relaxed_laneselect
                     case 0x10A: // i16x8.relaxed_laneselect
                     case 0x10B: // i32x4.relaxed_laneselect
                     case 0x10C: // i64x2.relaxed_laneselect
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_v128_bitselect();
                        break;
                     case 0x10D: // f32x4.relaxed_min
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_f32x4_pmin();
                        break;
                     case 0x10E: // f32x4.relaxed_max
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_f32x4_pmax();
                        break;
                     case 0x10F: // f64x2.relaxed_min
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_f64x2_pmin();
                        break;
                     case 0x110: // f64x2.relaxed_max
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_f64x2_pmax();
                        break;
                     case 0x111: // i16x8.relaxed_q15mulr_s
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_i16x8_q15mulr_sat_s();
                        break;
                     case 0x112: // i16x8.relaxed_dot_i8x16_i7x16_s
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_i16x8_relaxed_dot_i8x16_i7x16_s();
                        break;
                     case 0x113: // i32x4.relaxed_dot_i8x16_i7x16_add_s
                        check_in_bounds();
                        op_stack.pop(types::v128); op_stack.pop(types::v128); op_stack.pop(types::v128);
                        op_stack.push(types::v128);
                        code_writer.emit_i32x4_relaxed_dot_i8x16_i7x16_add_s();
                        break;

                     default: PSIZAM_ASSERT(false, wasm_parse_exception, "Illegal instruction");
                  }
               } break;
               case opcodes::ext_prefix: {
                  switch(parse_varuint32(code))
                  {
#define TRUNC_SAT_OP(dest, src, sign)                                                   \
                     case ext_opcodes::dest ## _trunc_sat_ ## src ## _ ## sign: {       \
                        check_in_bounds();                                              \
                        PSIZAM_ASSERT(get_enable_nontrapping_fptoint(_options), wasm_parse_exception, "Non-trapping float-to-int conversions not enabled");\
                        op_stack.pop(src);                                              \
                        op_stack.push(dest);                                            \
                        code_writer.emit_ ## dest ## _trunc_sat_ ## src ## _ ## sign(); \
                     } break;
                     TRUNC_SAT_OP(i32, f32, s)
                     TRUNC_SAT_OP(i32, f32, u)
                     TRUNC_SAT_OP(i32, f64, s)
                     TRUNC_SAT_OP(i32, f64, u)
                     TRUNC_SAT_OP(i64, f32, s)
                     TRUNC_SAT_OP(i64, f32, u)
                     TRUNC_SAT_OP(i64, f64, s)
                     TRUNC_SAT_OP(i64, f64, u)
#undef TRUNC_SAT_OP
                     case ext_opcodes::memory_init: {
                        check_in_bounds();
                        PSIZAM_ASSERT(get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
                        PSIZAM_ASSERT(_mod->memories.size() != 0, wasm_parse_exception, "memory.init requires memory");
                        // memory.init(dst: addr_type, src: i32, len: i32)
                        op_stack.pop(types::i32);  // len (always i32 for memory.init)
                        op_stack.pop(types::i32);  // src offset in data segment
                        op_stack.pop(mem_addr_type());  // dst in linear memory
                        auto x = parse_varuint32(code);
                        PSIZAM_ASSERT(!!_datacount, wasm_parse_exception, "memory.init requires datacount section");
                        PSIZAM_ASSERT(x < *_datacount, wasm_parse_exception, "data segment does not exist");
                        PSIZAM_ASSERT(*code == 0, wasm_parse_exception, "memory.init must end with 0x00");
                        code++;
                        code_writer.emit_memory_init(x);
                     } break;
                     case ext_opcodes::data_drop: {
                        check_in_bounds();
                        PSIZAM_ASSERT(get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
                        auto x = parse_varuint32(code);
                        PSIZAM_ASSERT(!!_datacount, wasm_parse_exception, "data.drop requires datacount section");
                        PSIZAM_ASSERT(x < *_datacount, wasm_parse_exception, "data segment does not exist");
                        code_writer.emit_data_drop(x);
                     } break;
                     case ext_opcodes::memory_copy:
                        check_in_bounds();
                        PSIZAM_ASSERT(get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
                        PSIZAM_ASSERT(_mod->memories.size() != 0, wasm_parse_exception, "memory.copy requires memory");
                        // memory.copy(dst: addr_type, src: addr_type, len: addr_type)
                        op_stack.pop(mem_addr_type());
                        op_stack.pop(mem_addr_type());
                        op_stack.pop(mem_addr_type());
                        PSIZAM_ASSERT(*code == 0, wasm_parse_exception, "memory.copy must end with 0x00 0x00");
                        code++;
                        PSIZAM_ASSERT(*code == 0, wasm_parse_exception, "memory.copy must end with 0x00 0x00");
                        code++;
                        code_writer.emit_memory_copy();
                        break;
                     case ext_opcodes::memory_fill:
                        check_in_bounds();
                        PSIZAM_ASSERT(get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
                        PSIZAM_ASSERT(_mod->memories.size() != 0, wasm_parse_exception, "memory.fill requires memory");
                        // memory.fill(dst: addr_type, val: i32, len: addr_type)
                        op_stack.pop(mem_addr_type());  // len
                        op_stack.pop(types::i32);       // val (always i32)
                        op_stack.pop(mem_addr_type());  // dst
                        PSIZAM_ASSERT(*code == 0, wasm_parse_exception, "memory.fill must end with 0x00");
                        code++;
                        code_writer.emit_memory_fill();
                        break;
                     case ext_opcodes::table_init: {
                        check_in_bounds();
                        PSIZAM_ASSERT(get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
                        PSIZAM_ASSERT(_mod->tables.size() != 0, wasm_parse_exception, "table.init requires table");
                        auto elem_idx = parse_varuint32(code);
                        auto table_idx = parse_varuint32(code);
                        PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "table.init table index out of range");
                        PSIZAM_ASSERT(elem_idx < _mod->elements.size(), wasm_parse_exception, "elem segment does not exist");
                        PSIZAM_ASSERT(_mod->elements[elem_idx].type == _mod->tables[table_idx].element_type,
                                      wasm_parse_exception, "type mismatch");
                        op_stack.pop(types::i32);
                        op_stack.pop(types::i32);
                        op_stack.pop(types::i32);
                        code_writer.emit_table_init(elem_idx, table_idx);
                     } break;
                     case ext_opcodes::elem_drop: {
                        check_in_bounds();
                        PSIZAM_ASSERT(get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
                        auto x = parse_varuint32(code);
                        PSIZAM_ASSERT(x < _mod->elements.size(), wasm_parse_exception, "elem segment does not exist");
                        code_writer.emit_elem_drop(x);
                     } break;
                     case ext_opcodes::table_copy: {
                        check_in_bounds();
                        PSIZAM_ASSERT(get_enable_bulk_memory(_options), wasm_parse_exception, "Bulk memory not enabled");
                        PSIZAM_ASSERT(_mod->tables.size() != 0, wasm_parse_exception, "table.copy requires table");
                        auto dst_table = parse_varuint32(code);
                        auto src_table = parse_varuint32(code);
                        PSIZAM_ASSERT(dst_table < _mod->tables.size(), wasm_parse_exception, "table.copy dst table index out of range");
                        PSIZAM_ASSERT(src_table < _mod->tables.size(), wasm_parse_exception, "table.copy src table index out of range");
                        op_stack.pop(types::i32);
                        op_stack.pop(types::i32);
                        op_stack.pop(types::i32);
                        code_writer.emit_table_copy(dst_table, src_table);
                     } break;
                     case ext_opcodes::table_grow: {
                        check_in_bounds();
                        auto table_idx = parse_varuint32(code);
                        PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "table.grow table index out of range");
                        op_stack.pop(types::i32);  // delta
                        op_stack.pop(_mod->tables[table_idx].element_type);  // init value (ref)
                        op_stack.push(types::i32); // previous size or -1
                        code_writer.emit_table_grow(table_idx);
                     } break;
                     case ext_opcodes::table_size: {
                        check_in_bounds();
                        auto table_idx = parse_varuint32(code);
                        PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "table.size table index out of range");
                        op_stack.push(types::i32);
                        code_writer.emit_table_size(table_idx);
                     } break;
                     case ext_opcodes::table_fill: {
                        check_in_bounds();
                        auto table_idx = parse_varuint32(code);
                        PSIZAM_ASSERT(table_idx < _mod->tables.size(), wasm_parse_exception, "table.fill table index out of range");
                        op_stack.pop(types::i32);  // n
                        op_stack.pop(_mod->tables[table_idx].element_type);  // value (ref)
                        op_stack.pop(types::i32);  // i (start index)
                        code_writer.emit_table_fill(table_idx);
                     } break;
                     default: PSIZAM_ASSERT(false, wasm_parse_exception, "Illegal instruction");
                  }
               } break;
               case opcodes::atomic_prefix: {
                  auto sub = parse_varuint32(code);
                  auto asub = static_cast<atomic_sub>(sub);
                  if (asub == atomic_sub::atomic_fence) {
                     check_in_bounds();
                     PSIZAM_ASSERT(*code == 0, wasm_parse_exception, "atomic.fence must have 0x00 byte");
                     code++;
                     code_writer.emit_atomic_op(asub, 0, 0);
                  } else if (asub == atomic_sub::memory_atomic_notify) {
                     check_in_bounds();
                     auto align = parse_varuint32(code);
                     auto offset = parse_memarg_offset(code);
                     op_stack.pop(types::i32); // count
                     op_stack.pop(mem_addr_type()); // addr
                     op_stack.push(types::i32); // result
                     code_writer.emit_atomic_op(asub, align, offset);
                  } else if (asub == atomic_sub::memory_atomic_wait32) {
                     check_in_bounds();
                     auto align = parse_varuint32(code);
                     auto offset = parse_memarg_offset(code);
                     op_stack.pop(types::i64); // timeout
                     op_stack.pop(types::i32); // expected
                     op_stack.pop(mem_addr_type()); // addr
                     op_stack.push(types::i32); // result
                     code_writer.emit_atomic_op(asub, align, offset);
                  } else if (asub == atomic_sub::memory_atomic_wait64) {
                     check_in_bounds();
                     auto align = parse_varuint32(code);
                     auto offset = parse_memarg_offset(code);
                     op_stack.pop(types::i64); // timeout
                     op_stack.pop(types::i64); // expected
                     op_stack.pop(mem_addr_type()); // addr
                     op_stack.push(types::i32); // result
                     code_writer.emit_atomic_op(asub, align, offset);
                  } else if (sub >= 0x10 && sub <= 0x16) {
                     // Atomic loads
                     check_in_bounds();
                     auto align = parse_varuint32(code);
                     auto offset = parse_memarg_offset(code);
                     op_stack.pop(mem_addr_type()); // addr
                     if (sub <= 0x13) // i32 loads
                        op_stack.push(types::i32);
                     else // i64 loads
                        op_stack.push(types::i64);
                     code_writer.emit_atomic_op(asub, align, offset);
                  } else if (sub >= 0x17 && sub <= 0x1D) {
                     // Atomic stores
                     check_in_bounds();
                     auto align = parse_varuint32(code);
                     auto offset = parse_memarg_offset(code);
                     if (sub <= 0x1A) { // i32 stores
                        op_stack.pop(types::i32); // value
                     } else { // i64 stores
                        op_stack.pop(types::i64); // value
                     }
                     op_stack.pop(mem_addr_type()); // addr
                     code_writer.emit_atomic_op(asub, align, offset);
                  } else if (sub >= 0x1E && sub <= 0x4E) {
                     // Atomic RMW + cmpxchg
                     check_in_bounds();
                     auto align = parse_varuint32(code);
                     auto offset = parse_memarg_offset(code);
                     // Determine if i32 or i64 op by sub-opcode pattern
                     bool is_cmpxchg = (sub >= 0x48);
                     bool is_i64 = false;
                     if (is_cmpxchg) {
                        is_i64 = (sub == 0x49 || sub >= 0x4C);
                     } else {
                        // RMW ops: 7-op groups (full,full,8,16,8,16,32)
                        // i64 variants are at odd offsets from group start + 1
                        uint8_t in_group = (sub - 0x1E) % 7;
                        is_i64 = (in_group == 1 || in_group >= 4);
                     }
                     if (is_cmpxchg) {
                        if (is_i64) {
                           op_stack.pop(types::i64); // replacement
                           op_stack.pop(types::i64); // expected
                        } else {
                           op_stack.pop(types::i32);
                           op_stack.pop(types::i32);
                        }
                     } else {
                        op_stack.pop(is_i64 ? types::i64 : types::i32); // value
                     }
                     op_stack.pop(mem_addr_type()); // addr
                     op_stack.push(is_i64 ? types::i64 : types::i32); // old value
                     code_writer.emit_atomic_op(asub, align, offset);
                  } else {
                     PSIZAM_ASSERT(false, wasm_parse_exception, "Illegal atomic instruction");
                  }
               } break;
               default: PSIZAM_ASSERT(false, wasm_parse_exception, "Illegal instruction");
            }
         }
         if constexpr (has_max_stack_bytes<Options>)
         {
            code_writer.set_stack_usage(op_stack.stack_usage._max);
         }
         PSIZAM_ASSERT( pc_stack.empty(), wasm_parse_exception, "function body too long" );
         max_stack_out = std::max(max_stack_out, static_cast<uint64_t>(op_stack.maximum_operand_depth) + local_types.locals_count());
      }

      void parse_data_segment(wasm_code_ptr& code, data_segment& ds) {
         ds.index = parse_varuint32(code);
         if (ds.index == 0 || !get_enable_bulk_memory(_options))
         {
            ds.passive = false;
            parse_init_expr(code, ds.offset, types::i32);
            PSIZAM_ASSERT(_mod->memories.size() != 0, wasm_parse_exception, "data requires memory");
         }
         else if (ds.index == 1)
         {
            ds.passive = true;
            ds.offset = {.value = {.i32 = 0}, .opcode = opcodes::i32_const};
         }
         else if (ds.index == 2)
         {
            ds.passive = false;
            ds.index = parse_varuint32(code);
            parse_init_expr(code, ds.offset, types::i32);
            PSIZAM_ASSERT(ds.index < _mod->memories.size(), wasm_parse_exception, "Data uses nonexistent memory");
         }
         else
         {
            PSIZAM_ASSERT(false, wasm_parse_exception, "Unexpected flag for data");
         }
         auto len =  parse_varuint32(code);
         PSIZAM_ASSERT(len <= get_max_data_segment_bytes(_options), wasm_parse_exception, "data segment too large.");
         if (ds.offset.raw_expr.empty()) {
            PSIZAM_ASSERT(static_cast<uint64_t>(static_cast<uint32_t>(ds.offset.value.i32)) + len <= get_max_linear_memory_init(_options),
                          wasm_parse_exception, "out-of-bounds data section");
         }
         auto guard = code.scoped_shrink_bounds(len);
         ds.data.assign(code.raw(), code.raw() + len);
         code += len;
      }

      template <typename Elem, typename ParseFunc>
      inline void parse_section_impl(wasm_code_ptr& code, vec<Elem>& elems, std::uint32_t max_elements, ParseFunc&& elem_parse) {
         auto count = parse_varuint32(code);
         PSIZAM_ASSERT(count <= max_elements, wasm_parse_exception, "number of section elements exceeded limit");
         elems      = vec<Elem>{ _allocator, count };
         for (size_t i = 0; i < count; i++) { elem_parse(code, elems.at(i), i); }
      }

      template <typename Elem, typename ParseFunc>
      inline void parse_section_impl(wasm_code_ptr& code, std::vector<Elem>& elems, std::uint32_t max_elements, ParseFunc&& elem_parse) {
         auto count = parse_varuint32(code);
         PSIZAM_ASSERT(count <= max_elements, wasm_parse_exception, "number of section elements exceeded limit");
         elems.resize(count);
         for (size_t i = 0; i < count; i++) { elem_parse(code, elems.at(i), i); }
      }

      template <uint8_t id>
      requires (id == section_id::type_section)
      inline void parse_section(wasm_code_ptr&          code,
                                std::vector<func_type>& elems) {
         parse_section_impl(code, elems, get_max_type_section_elements(_options),
                            [&](wasm_code_ptr& code, func_type& ft, std::size_t /*idx*/) { parse_func_type(code, ft); ft.compute_sig_hash(); });
      }
      template <uint8_t id>
      requires (id == section_id::import_section)
      inline void parse_section(wasm_code_ptr&             code,
                                std::vector<import_entry>& elems) {
         parse_section_impl(code, elems, get_max_import_section_elements(_options),
                            [&](wasm_code_ptr& code, import_entry& ie, std::size_t /*idx*/) { parse_import_entry(code, ie); });
      }
      template <uint8_t id>
      requires (id == section_id::function_section)
      inline void parse_section(wasm_code_ptr&         code,
                                std::vector<uint32_t>& elems) {
         parse_section_impl(code, elems, get_max_function_section_elements(_options),
                            [&](wasm_code_ptr& code, uint32_t& elem, std::size_t /*idx*/) { elem = parse_varuint32(code); });
      }
      template <uint8_t id>
      requires (id == section_id::table_section)
      inline void parse_section(wasm_code_ptr&           code,
                                std::vector<table_type>& elems) {
         auto count = parse_varuint32(code);
         PSIZAM_ASSERT(count <= get_max_section_elements(_options), wasm_parse_exception, "number of section elements exceeded limit");
         auto base = elems.size(); // imported tables already present
         elems.resize(base + count);
         for (size_t i = 0; i < count; i++) { parse_table_type(code, elems.at(base + i)); }
      }
      template <uint8_t id>
      requires (id == section_id::memory_section)
      inline void parse_section(wasm_code_ptr&            code,
                                std::vector<memory_type>& elems) {
         auto count = parse_varuint32(code);
         PSIZAM_ASSERT(count <= 1, wasm_parse_exception, "number of section elements exceeded limit");
         PSIZAM_ASSERT(elems.size() + count <= 1, wasm_parse_exception, "only one memory is permitted");
         auto base = elems.size();
         elems.resize(base + count);
         for (size_t i = 0; i < count; i++) { parse_memory_type(code, elems.at(base + i)); }
      }
      template <uint8_t id>
      requires (id == section_id::global_section)
      inline void parse_section(wasm_code_ptr&                code,
                                std::vector<global_variable>& elems) {
         auto count = parse_varuint32(code);
         PSIZAM_ASSERT(count <= get_max_global_section_elements(_options), wasm_parse_exception, "number of section elements exceeded limit");
         auto base = elems.size(); // imported globals already present
         elems.resize(base + count);
         for (size_t i = 0; i < count; i++) { parse_global_variable(code, elems.at(base + i)); }
      }
      template <uint8_t id>
      requires (id == section_id::export_section)
      inline void parse_section(wasm_code_ptr&             code,
                                std::vector<export_entry>& elems) {
         parse_section_impl(code, elems, get_max_export_section_elements(_options),
                            [&](wasm_code_ptr& code, export_entry& ee, std::size_t /*idx*/) { parse_export_entry(code, ee); });
      }
      template <uint8_t id>
      inline void parse_section(wasm_code_ptr&                                                        code,
                                typename std::enable_if_t<id == section_id::start_section, uint32_t>& start) {
         start = parse_varuint32(code);
         const func_type& ft = _mod->get_function_type(start);
         PSIZAM_ASSERT(ft.return_count == 0 && ft.param_types.size() == 0, wasm_parse_exception, "wrong type for start");
      }
      template <uint8_t id>
      requires (id == section_id::element_section)
      inline void parse_section(wasm_code_ptr&             code,
                                std::vector<elem_segment>& elems) {
         parse_section_impl(code, elems, get_max_element_section_elements(_options),
                            [&](wasm_code_ptr& code, elem_segment& es, std::size_t /*idx*/) { parse_elem_segment(code, es); });
      }
      template <uint8_t id>
      requires (id == section_id::code_section)
      inline void parse_section(wasm_code_ptr&              code,
                                std::vector<function_body>& elems) {
         const void* code_start = code.raw() - code.offset();
         parse_section_impl(code, elems, get_max_function_section_elements(_options),
                            [&](wasm_code_ptr& code, function_body& fb, std::size_t idx) { parse_function_body(code, fb, idx); });
         PSIZAM_ASSERT( elems.size() == _mod->functions.size(), wasm_parse_exception, "code section must have the same size as the function section" );

         // Apply buffered branch hints from metadata.code.branch_hint custom section
         if (!_branch_hints_temp.empty()) {
            uint32_t num_imports = static_cast<uint32_t>(_mod->import_functions.size());
            for (auto& [func_idx, hints] : _branch_hints_temp) {
               if (func_idx >= num_imports && (func_idx - num_imports) < elems.size()) {
                  elems[func_idx - num_imports].branch_hints = std::move(hints);
               }
            }
            _branch_hints_temp.clear();
         }

         write_code_out(_allocator, code, code_start);
      }

      void write_code_out(growable_allocator& allocator, wasm_code_ptr& code, const void* code_start) {
         auto _compile_threads = get_compile_threads(_options);
         Writer code_writer(allocator, code.bounds() - code.offset(), *_mod,
                            _enable_backtrace, _stack_limit_is_bytes, _compile_threads);
         if constexpr (requires { code_writer.set_compile_result(_compile_result); }) {
            code_writer.set_compile_result(_compile_result);
         }
#if !defined(__wasi__)
         if constexpr (requires { code_writer.set_parse_callback(std::declval<typename Writer::parse_callback_t>()); }) {
            if (_compile_threads > 1) {
               // Parallel mode: pass a parse callback to the writer.
               // Each worker thread calls this with its own writer instance
               // (ir_writer_impl base type), so use base class reference.
               code_writer.set_parse_callback(
                  [this](uint32_t func_idx, typename Writer::base_writer_t& w, uint64_t& max_stack) {
                     function_body& fb = _mod->code[func_idx];
                     func_type& ft = _mod->types.at(_mod->functions.at(func_idx));
                     local_types_t local_types(ft, fb.locals);
                     w.emit_prologue(ft, fb.locals, func_idx);
                     psizam_max_nested_structures_checker<Options> checker;
                     null_debug_info::builder dummy_imap;
                     parse_function_body_code_impl(
                        _function_bodies[func_idx].first, fb.size,
                        _function_bodies[func_idx].second, w, ft, local_types,
                        checker, dummy_imap, max_stack);
                     w.emit_epilogue(ft, fb.locals, func_idx);
                  });
               // Writer destructor triggers parallel compilation
               return;
            }
         }
#endif
         imap.on_code_start(code_writer.get_base_addr(), code_start);
         for (size_t i = 0; i < _function_bodies.size(); i++) {
            function_body& fb = _mod->code[i];
            func_type& ft = _mod->types.at(_mod->functions.at(i));
            local_types_t local_types(ft, fb.locals);
            imap.on_function_start(code_writer.get_addr(), _function_bodies[i].first.raw());
            code_writer.emit_prologue(ft, fb.locals, i);
            parse_function_body_code(_function_bodies[i].first, fb.size, _function_bodies[i].second, code_writer, ft, local_types);
            code_writer.emit_epilogue(ft, fb.locals, i);
            imap.on_function_end(code_writer.get_addr(), _function_bodies[i].first.bnds);
            code_writer.finalize(fb);
         }
         imap.on_code_end(code_writer.get_addr(), code.raw());
      }

      template <uint8_t id>
      requires (id == section_id::data_section)
      inline void parse_section(wasm_code_ptr&             code,
                                std::vector<data_segment>& elems) {
         parse_section_impl(code, elems, get_max_data_section_elements(_options),
                            [&](wasm_code_ptr& code, data_segment& ds, std::size_t /*idx*/) { parse_data_segment(code, ds); });
         if (_datacount) {
            PSIZAM_ASSERT(*_datacount == elems.size(), wasm_parse_exception, "data count does not match data");
         }
      }
      template <uint8_t id>
      requires (id == section_id::data_count_section)
      inline void parse_section(wasm_code_ptr& code, std::optional<std::uint32_t>& n)
      {
         n = parse_varuint32(code);
      }

      template <uint8_t id>
      requires (id == section_id::tag_section)
      inline void parse_section(wasm_code_ptr& code, std::vector<tag_type>& elems) {
         parse_section_impl(code, elems, get_max_section_elements(_options),
                            [&](wasm_code_ptr& code, tag_type& tt, std::size_t /*idx*/) {
            tt.attribute = *code++;
            PSIZAM_ASSERT(tt.attribute == 0, wasm_parse_exception, "invalid tag attribute");
            tt.type_index = parse_varuint32(code);
            PSIZAM_ASSERT(tt.type_index < _mod->types.size(), wasm_parse_exception, "invalid tag type index");
         });
      }

      template <size_t N>
      varint<N> parse_varint(const wasm_code& code, size_t index) {
         varint<N> result(0);
         result.set(code, index);
         return result;
      }

      template <size_t N>
      varuint<N> parse_varuint(const wasm_code& code, size_t index) {
         varuint<N> result(0);
         result.set(code, index);
         return result;
      }

      void on_mutable_global(uint8_t type) {
         _globals_checker.on_mutable_global(_options, type);
      }

      void validate_exports() const {
         std::vector<const std::vector<uint8_t>*> export_names;
         export_names.reserve(_mod->exports.size());
         for (uint32_t i = 0; i < _mod->exports.size(); ++i) {
            export_names.push_back(&_mod->exports[i].field_str);
         }
         std::sort(export_names.begin(), export_names.end(), [](auto* lhs, auto* rhs) {
            return *lhs < *rhs;
         });
         auto it = std::adjacent_find(export_names.begin(), export_names.end(), [](auto* lhs, auto* rhs) {
            return *lhs == *rhs;
         });
         PSIZAM_ASSERT(it == export_names.end(), wasm_parse_exception, "duplicate export name");
      }

    private:
      growable_allocator& _allocator;
      Options             _options;
      bool                _enable_backtrace;
      bool                _stack_limit_is_bytes;
      pzam_compile_result* _compile_result = nullptr;
      module*             _mod; // non-owning weak pointer
      int64_t             _current_function_index = -1;
      uint64_t            _maximum_function_stack_usage = 0; // non-parameter locals + stack
      std::vector<std::pair<wasm_code_ptr, max_func_local_bytes_stack_checker<Options>>>  _function_bodies;
      max_mutable_globals_checker<Options> _globals_checker;
      psizam_max_nested_structures_checker<Options> _nested_checker;
      std::optional<std::uint32_t> _datacount;
      typename DebugInfo::builder imap;
      std::vector<uint32_t> type_aliases;
      std::vector<uint32_t> fast_functions;
      std::unordered_map<uint32_t, std::vector<branch_hint>> _branch_hints_temp;
   };
} // namespace psizam::detail
