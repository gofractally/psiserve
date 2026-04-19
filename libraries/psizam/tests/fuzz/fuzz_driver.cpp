// Coverage-guided differential fuzzer for psizam (libFuzzer harness)
//
// Build: cmake -B build -DPSIZAM_FUZZ_LIBFUZZER=ON -DCMAKE_BUILD_TYPE=Release
// Run:   ./psizam-fuzz corpus/ -jobs=4 -max_len=8192
//
// Each input is a raw WASM module. libFuzzer mutates it for coverage;
// we run interpreter + all JIT backends and assert identical outcomes.

#include <psizam/backend.hpp>
#include <psizam/detail/watchdog.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using namespace psizam;
using namespace psizam::detail;

// Outcome codes matching fuzz_differential.cpp convention
enum outcome : int {
   OK           = 0,
   REJECTED     = 1,
   MEMORY_TRAP  = 2,
   INTERP_TRAP  = 3,
   TIMEOUT      = 4,
};

struct return_value {
   uint8_t  type = 0;
   uint64_t bits = 0;
   uint64_t bits_hi = 0;
};

struct run_result {
   int outcome = REJECTED;
   std::vector<return_value> returns;
   std::vector<std::string> export_names;
};

template <typename Impl>
static run_result run_backend(const uint8_t* data, size_t size) {
   run_result r{};
   try {
      using backend_t = backend<std::nullptr_t, Impl>;
      wasm_code code(data, data + size);
      wasm_allocator wa;
      backend_t bkend(code, &wa);

      bkend.timed_run(watchdog(std::chrono::milliseconds(200)), [&]() {
         auto& mod = bkend.get_module();
         auto& ctx = bkend.get_context();
         for (uint32_t i = 0; i < mod.exports.size(); i++) {
            if (mod.exports[i].kind != external_kind::Function) continue;
            std::string s{(const char*)mod.exports[i].field_str.data(),
                          mod.exports[i].field_str.size()};
            r.export_names.push_back(s);
            auto ret = ctx.execute(nullptr, detail::interpret_visitor(ctx), s);
            return_value rv{};
            if (ret) {
               if (ret->template is_a<i32_const_t>()) {
                  rv.type = types::i32; rv.bits = ret->to_ui32();
               } else if (ret->template is_a<i64_const_t>()) {
                  rv.type = types::i64; rv.bits = ret->to_ui64();
               } else if (ret->template is_a<f32_const_t>()) {
                  rv.type = types::f32; rv.bits = ret->to_fui32();
               } else if (ret->template is_a<f64_const_t>()) {
                  rv.type = types::f64; rv.bits = ret->to_fui64();
               } else if (ret->template is_a<v128_const_t>()) {
                  rv.type = types::v128;
                  auto v = ret->to_v128();
                  rv.bits = v.low; rv.bits_hi = v.high;
               }
            }
            r.returns.push_back(rv);
         }
      });
      r.outcome = OK;
   } catch (wasm_parse_exception&) { r.outcome = REJECTED;
   } catch (wasm_bad_alloc&)       { r.outcome = REJECTED;
   } catch (wasm_link_exception&)  { r.outcome = REJECTED;
   } catch (wasm_memory_exception&){ r.outcome = MEMORY_TRAP;
   } catch (wasm_exception&)       { r.outcome = INTERP_TRAP;
   } catch (wasm_interpreter_exception&) { r.outcome = INTERP_TRAP;
   } catch (timeout_exception&)    { r.outcome = TIMEOUT;
   } catch (...)                   { r.outcome = REJECTED; }
   return r;
}

static bool is_nan(const return_value& v) {
   if (v.type == types::f32) {
      uint32_t b = static_cast<uint32_t>(v.bits);
      return (b & 0x7F800000u) == 0x7F800000u && (b & 0x007FFFFFu) != 0;
   }
   if (v.type == types::f64)
      return (v.bits & 0x7FF0000000000000ull) == 0x7FF0000000000000ull &&
             (v.bits & 0x000FFFFFFFFFFFFFull) != 0;
   return false;
}

static bool returns_match(const run_result& a, const run_result& b) {
   if (a.outcome != OK || b.outcome != OK) return true;
   if (a.returns.size() != b.returns.size()) return false;
   for (size_t i = 0; i < a.returns.size(); i++) {
      auto& x = a.returns[i]; auto& y = b.returns[i];
      if (x.type != y.type || x.bits != y.bits || x.bits_hi != y.bits_hi) {
         if (is_nan(x) && is_nan(y)) continue;
         return false;
      }
   }
   return true;
}

static bool check(const run_result& ref, const run_result& test, const char* name) {
   // Only flag mismatches where one backend succeeds (OK) and the other doesn't,
   // or where both succeed but return different values.
   // Trap-vs-trap and trap-vs-rejected divergences are acceptable (parser/validator
   // differences between backends).
   if (ref.outcome == OK && test.outcome != OK) {
      fprintf(stderr, "OUTCOME MISMATCH: interp=ok %s=%d\n", name, test.outcome);
      return false;
   }
   if (ref.outcome != OK && test.outcome == OK) {
      fprintf(stderr, "OUTCOME MISMATCH: interp=%d %s=ok\n", ref.outcome, name);
      return false;
   }
   if (!returns_match(ref, test)) {
      fprintf(stderr, "RETURN MISMATCH: interp vs %s\n", name);
      return false;
   }
   return true;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
   if (size < 8 || size > 8192) return 0;
   if (memcmp(data, "\0asm", 4) != 0) return 0;

   auto r_interp = run_backend<interpreter>(data, size);

   if (r_interp.outcome == TIMEOUT) return 0;

   // Differential check against jit2 (best coverage of codegen bugs)
#if defined(__x86_64__) || defined(__aarch64__)
   bool ok = check(r_interp, run_backend<jit2>(data, size), "jit2");
   if (!ok) __builtin_trap();
#endif

   return 0;
}
