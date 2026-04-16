#include <psizam/backend.hpp>
#include <psizam/detail/watchdog.hpp>

using namespace psizam;
using namespace psizam::detail;

// Differential fuzzer: run the same WASM on interpreter and JIT backends.
// Any divergence in results or trap behavior is a bug.

template <typename Impl>
static int run_backend(wasm_code& code, wasm_allocator& wa) {
   try {
      using backend_t = backend<std::nullptr_t, Impl>;
      backend_t bkend(code, &wa);
      bkend.execute_all(null_watchdog());
      return 0;
   } catch (wasm_parse_exception&) {
      return 1;
   } catch (wasm_memory_exception&) {
      return 2;
   } catch (wasm_interpreter_exception&) {
      return 3;
   } catch (timeout_exception&) {
      return 4;
   } catch (...) {
      return 5;
   }
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
   // Make independent copies — each backend owns its code
   wasm_code code_interp(data, data + size);
   wasm_code code_jit(data, data + size);

   wasm_allocator wa_interp;
   wasm_allocator wa_jit;

   int r_interp = run_backend<interpreter>(code_interp, wa_interp);
   int r_jit    = run_backend<jit>(code_jit, wa_jit);

   // Differential check: both backends should agree on parse/trap outcomes
   if (r_interp != r_jit) {
      __builtin_trap(); // ASan will report this with the offending input
   }

   return 0;
}
