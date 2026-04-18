// consumer_guest.cpp — the consumer WASM module.
// Imports the greeter interface (provided by the provider module).
// Exports the processor interface (called by the host).

#include "shared.hpp"

#include <psio/guest_alloc.hpp>
#include <psizam/module.hpp>

#include <string.h>

// WIT custom sections
PSIO_WIT_SECTION(processor)
PSIO_WIT_SECTION(greeter)
PSIO_WIT_SECTION(env)

// Guest-side import thunks for env (canonical types)
PSIO_GUEST_IMPORTS(env, log_string)

void env::log_string(std::string_view msg) {
   _psio_import_call_env_log_string(msg);
}

// ── Greeter import declarations ─────────────────────────────────────
// The consumer imports greeter functions. Scalar functions use the
// 16-wide flat_val convention directly. Non-scalar functions (concat)
// need manual return-area handling.

// Raw WASM import for add (scalar: two i32 args, i32 return)
extern "C" __attribute__((import_module("greeter"), import_name("add")))
::psizam::flat_val _psio_raw_greeter_add(
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val);

uint32_t greeter::add(uint32_t a, uint32_t b) {
   ::psizam::flat_val slots[16] = {};
   slots[0] = static_cast<::psizam::flat_val>(a);
   slots[1] = static_cast<::psizam::flat_val>(b);
   auto r = _psio_raw_greeter_add(
      slots[0],  slots[1],  slots[2],  slots[3],
      slots[4],  slots[5],  slots[6],  slots[7],
      slots[8],  slots[9],  slots[10], slots[11],
      slots[12], slots[13], slots[14], slots[15]);
   return static_cast<uint32_t>(r);
}

// Raw WASM import for double_it (scalar: one i64 arg, i64 return)
extern "C" __attribute__((import_module("greeter"), import_name("double_it")))
::psizam::flat_val _psio_raw_greeter_double_it(
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val);

uint64_t greeter::double_it(uint64_t v) {
   ::psizam::flat_val slots[16] = {};
   slots[0] = static_cast<::psizam::flat_val>(v);
   auto r = _psio_raw_greeter_double_it(
      slots[0],  slots[1],  slots[2],  slots[3],
      slots[4],  slots[5],  slots[6],  slots[7],
      slots[8],  slots[9],  slots[10], slots[11],
      slots[12], slots[13], slots[14], slots[15]);
   return static_cast<uint64_t>(r);
}

// Raw WASM import for concat (string args + string return via return area)
extern "C" __attribute__((import_module("greeter"), import_name("concat")))
::psizam::flat_val _psio_raw_greeter_concat(
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val,
   ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val, ::psizam::flat_val);

wit::string greeter::concat(std::string_view a, std::string_view b) {
   // Lower string args into flat slots
   ::psizam::flat_val slots[16] = {};
   std::size_t idx = 0;
   ::psizam::guest_import_lower(slots, idx, a);
   ::psizam::guest_import_lower(slots, idx, b);

   // Allocate a return area for the string result: {ptr:u32, len:u32}
   uint32_t* ret_area = static_cast<uint32_t*>(
      cabi_realloc(nullptr, 0, 4, 8));

   // Pass retptr as the next slot after args
   slots[idx] = static_cast<::psizam::flat_val>(
      reinterpret_cast<uintptr_t>(ret_area));

   // Call the import
   _psio_raw_greeter_concat(
      slots[0],  slots[1],  slots[2],  slots[3],
      slots[4],  slots[5],  slots[6],  slots[7],
      slots[8],  slots[9],  slots[10], slots[11],
      slots[12], slots[13], slots[14], slots[15]);

   // Read {ptr, len} from the return area
   uint32_t s_ptr = ret_area[0];
   uint32_t s_len = ret_area[1];

   // Free the return area (shrink to 0)
   cabi_realloc(ret_area, 8, 4, 0);

   // Adopt the string buffer — the buffer was allocated in consumer's
   // memory by the bridge's cabi_realloc call
   return wit::string::adopt(
      reinterpret_cast<char*>(static_cast<uintptr_t>(s_ptr)), s_len);
}

// ── Processor implementation ────────────────────────────────────────

struct processor_impl
{
   uint32_t test_add(uint32_t x, uint32_t y)
   {
      return greeter::add(x, y);
   }

   wit::string test_concat(std::string_view a, std::string_view b)
   {
      env::log_string("consumer: calling provider concat");
      return greeter::concat(a, b);
   }

   uint64_t test_double(uint64_t v)
   {
      return greeter::double_it(v);
   }
};

PSIO_MODULE(processor_impl,
            test_add, test_concat, test_double)
