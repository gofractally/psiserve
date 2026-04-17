#pragma once

#include <psizam/config.hpp>
#include <psizam/detail/wasm_stack.hpp>
#include <psizam/utils.hpp>
#include <psizam/exceptions.hpp>
#include <cstring>
#include <cstdint>
#include <limits>

namespace psizam::detail {

   // Normalize any NaN bit pattern arriving from the host boundary to the
   // WASM canonical NaN in deterministic modes. NaNs produced by host code
   // can carry architecture-specific payloads (esp. signaling NaNs); without
   // normalization, a WASM module that bit-casts FP→int would observe the
   // difference and consensus would fork.
   //
   // In fp_mode::fast, passthrough — divergence is allowed by contract.
   inline float canon_host_f32(float v, fp_mode m) {
      if (m == fp_mode::fast) return v;
      std::uint32_t bits;
      std::memcpy(&bits, &v, 4);
      if ((bits & 0x7FFFFFFFu) > 0x7F800000u) {
         bits = 0x7FC00000u;
         std::memcpy(&v, &bits, 4);
      }
      return v;
   }
   inline double canon_host_f64(double v, fp_mode m) {
      if (m == fp_mode::fast) return v;
      std::uint64_t bits;
      std::memcpy(&bits, &v, 8);
      if ((bits & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL) {
         bits = 0x7FF8000000000000ULL;
         std::memcpy(&v, &bits, 8);
      }
      return v;
   }

   // interface used for the host function system to use
   // clients can create their own interface to overlay their own implementations
   struct execution_interface {
      inline execution_interface( char* memory, operand_stack* os, fp_mode fp = fp_mode::fast )
         : memory(memory), os(os), _fp(fp) {}
      inline void* get_memory() const { return memory; }
      inline fp_mode fp() const { return _fp; }
      inline void trim_operands(std::size_t amt) { os->trim(amt); }

      template <typename T>
      inline void push_operand(T&& op) { os->push(std::forward<T>(op)); }
      inline auto pop_operand() { return os->pop(); }
      inline const auto& operand_from_back(std::size_t index) const { return os->get_back(index); }

      template <typename T>
      inline void* validate_pointer(wasm_ptr_t ptr, wasm_size_t len) const {
         auto result = memory + ptr;
         validate_pointer<T>(result, len);
         return result;
      }

      template <typename T>
      inline void validate_pointer(const void* ptr, wasm_size_t len) const {
         PSIZAM_ASSERT( len <= std::numeric_limits<wasm_size_t>::max() / (wasm_size_t)sizeof(T), wasm_interpreter_exception, "length will overflow" );
         volatile auto check_addr = *(reinterpret_cast<const char*>(ptr) + (len * sizeof(T)) - 1);
         ignore_unused_variable_warning(check_addr);
      }

      inline void* validate_null_terminated_pointer(wasm_ptr_t ptr) const {
         auto result = memory + ptr;
         validate_null_terminated_pointer(result);
         return result;
      }

      inline void validate_null_terminated_pointer(const void* ptr) const {
         // Use strnlen with max_useable_memory as upper bound to prevent unbounded scan
         volatile auto check_addr = strnlen(static_cast<const char*>(ptr), constants::max_useable_memory);
         ignore_unused_variable_warning(check_addr);
      }
      char* memory;
      operand_stack* os;
      fp_mode _fp;
   };
} // namespace psizam::detail
