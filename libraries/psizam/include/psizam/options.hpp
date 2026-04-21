#pragma once

#include <cstdint>

namespace psizam {

enum class max_func_local_bytes_flags_t {
   params = 1,
   locals = 2,
   stack  = 4
};
constexpr max_func_local_bytes_flags_t operator|(max_func_local_bytes_flags_t lhs, max_func_local_bytes_flags_t rhs) {
   return static_cast<max_func_local_bytes_flags_t>(static_cast<int>(lhs) | static_cast<int>(rhs));
}
constexpr max_func_local_bytes_flags_t operator&(max_func_local_bytes_flags_t lhs, max_func_local_bytes_flags_t rhs) {
   return static_cast<max_func_local_bytes_flags_t>(static_cast<int>(lhs) & static_cast<int>(rhs));
}

enum class mem_safety : unsigned char { guarded, checked, unchecked, memory16 };
enum class checked_mode : unsigned char { strict, relaxed };

struct options {
   mem_safety    memory_mode   = mem_safety::guarded;
   checked_mode  checked_kind  = checked_mode::strict;

   std::uint64_t max_mutable_global_bytes;
   std::uint32_t max_table_elements;
   std::uint32_t max_section_elements;
   std::uint32_t max_type_section_elements;
   std::uint32_t max_import_section_elements;
   std::uint32_t max_function_section_elements;
   std::uint32_t max_global_section_elements;
   std::uint32_t max_export_section_elements;
   std::uint32_t max_element_section_elements;
   std::uint32_t max_data_section_elements;
   // code is the same as functions
   // memory and tables are both 1.
   std::uint32_t max_element_segment_elements;
   std::uint32_t max_data_segment_bytes;
   std::uint32_t max_linear_memory_init;
   std::uint64_t max_func_local_bytes;
   std::uint32_t max_local_sets;
   std::uint32_t max_nested_structures;
   std::uint32_t max_br_table_elements;
   // The maximum length of symbols used for import and export
   std::uint32_t max_symbol_bytes;
   // The maximum offset used for load and store
   std::uint32_t max_memory_offset;
   // The maximum size of a function body
   std::uint32_t max_code_bytes;
   // The maximum size of linear memory in page units.
   std::uint32_t max_pages;
   // The maximum function call depth. Cannot be used with with max_stack_bytes
   std::uint32_t max_call_depth;
   // The maximum total stack size in bytes. Cannot be used with max_call_depth
   // std::uint32_t max_stack_bytes
   // Determines which components are counted towards max_function_local_bytes
   max_func_local_bytes_flags_t max_func_local_bytes_flags = max_func_local_bytes_flags_t::locals | max_func_local_bytes_flags_t::stack;
};

struct default_options {
};


}
