#pragma once

// Differential testing runner: executes the same WASM module through
// all available backends and compares results for consistency.

#include <psizam/psizam.hpp>

#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace psizam::compliance {

   struct engine_result {
      engine      eng;
      std::string engine_name;

      // Exactly one of these is set:
      std::optional<operand_stack_elem> value;  // successful return (nullopt = void)
      std::string                       error;  // exception message (empty = no error)
      bool                              had_error = false;
   };

   inline const char* engine_name(engine e) {
      switch (e) {
         case engine::interpreter: return "interpreter";
#if defined(__x86_64__) || defined(__aarch64__)
         case engine::jit:         return "jit";
         case engine::jit2:        return "jit2";
#endif
      }
      return "unknown";
   }

   inline std::vector<engine> all_engines() {
      return {
         engine::interpreter,
#if defined(__x86_64__) || defined(__aarch64__)
         engine::jit,
         engine::jit2,
#endif
      };
   }

   /// Run a WASM function through all backends and collect results.
   template <typename... Args>
   std::vector<engine_result> run_differential(
         std::span<const uint8_t> wasm,
         std::string_view func_name,
         Args&&... args) {

      std::vector<engine_result> results;

      for (auto eng : all_engines()) {
         engine_result r;
         r.eng = eng;
         r.engine_name = engine_name(eng);

         try {
            wasm_allocator wa;
            host_function_table table;
            runtime rt(wasm, table, &wa, nullptr, {.eng = eng});
            r.value = rt.call_with_return("env", func_name,
                                          std::forward<Args>(args)...);
         } catch (const std::exception& e) {
            r.had_error = true;
            r.error = e.what();
         } catch (...) {
            r.had_error = true;
            r.error = "unknown exception";
         }

         results.push_back(std::move(r));
      }

      return results;
   }

   /// Run with a host_function_table and host pointer.
   template <typename... Args>
   std::vector<engine_result> run_differential(
         std::span<const uint8_t> wasm,
         host_function_table& table,
         void* host,
         std::string_view func_name,
         Args&&... args) {

      std::vector<engine_result> results;

      for (auto eng : all_engines()) {
         engine_result r;
         r.eng = eng;
         r.engine_name = engine_name(eng);

         try {
            wasm_allocator wa;
            runtime rt(wasm, table, &wa, host, {.eng = eng});
            r.value = rt.call_with_return("env", func_name,
                                          std::forward<Args>(args)...);
         } catch (const std::exception& e) {
            r.had_error = true;
            r.error = e.what();
         } catch (...) {
            r.had_error = true;
            r.error = "unknown exception";
         }

         results.push_back(std::move(r));
      }

      return results;
   }

   /// Compare raw i64 bits of operand_stack_elem (type-punned comparison).
   inline uint64_t result_bits(const operand_stack_elem& elem) {
      // operand_stack_elem is a variant<i32_const_t, i64_const_t, f32_const_t, f64_const_t, v128_const_t>
      // Access raw bits through the i64 accessor (union punning)
      uint64_t bits = 0;
      std::memcpy(&bits, &elem, sizeof(uint64_t));
      return bits;
   }

   /// Check that all engine results agree.
   /// Returns empty string on success, or a diagnostic message on disagreement.
   inline std::string check_agreement(const std::vector<engine_result>& results) {
      if (results.size() < 2) return "";

      const auto& ref = results[0];

      for (size_t i = 1; i < results.size(); i++) {
         const auto& other = results[i];

         // Both should agree on error/success
         if (ref.had_error != other.had_error) {
            return ref.engine_name + " " +
                   (ref.had_error ? "errored" : "succeeded") +
                   " but " + other.engine_name + " " +
                   (other.had_error ? "errored" : "succeeded");
         }

         if (ref.had_error) continue; // both errored — don't compare messages

         // Both should agree on void/non-void
         bool ref_has   = ref.value.has_value();
         bool other_has = other.value.has_value();
         if (ref_has != other_has) {
            return ref.engine_name + " returned " +
                   (ref_has ? "a value" : "void") +
                   " but " + other.engine_name + " returned " +
                   (other_has ? "a value" : "void");
         }

         if (!ref_has) continue; // both void

         // Compare raw bits
         uint64_t ref_bits   = result_bits(*ref.value);
         uint64_t other_bits = result_bits(*other.value);
         if (ref_bits != other_bits) {
            return ref.engine_name + " returned 0x" +
                   std::to_string(ref_bits) +
                   " but " + other.engine_name + " returned 0x" +
                   std::to_string(other_bits);
         }
      }

      return ""; // all agree
   }

   /// Assert all engines agree — throws on disagreement (for use in tests).
   inline void assert_all_agree(const std::vector<engine_result>& results) {
      auto msg = check_agreement(results);
      if (!msg.empty()) {
         throw std::runtime_error("Differential test disagreement: " + msg);
      }
   }

} // namespace psizam::compliance
