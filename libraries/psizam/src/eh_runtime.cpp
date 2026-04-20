// WASM Exception Handling runtime helpers for JIT backends.
// Called from JIT-generated native code to implement try_table/throw/throw_ref.
//
// These helpers manage the jit_eh_stack on jit_execution_context. The throw
// path searches for a matching catch clause and longjmps to the try_table's
// setjmp point. If no handler is found, the exception escapes to the host
// via the existing trap_jmp_ptr / saved_exception mechanism.

#include <psizam/detail/llvm_runtime_helpers.hpp>
#include <psizam/detail/execution_context.hpp>
#include <psizam/exceptions.hpp>

#include <cstring>
#include <setjmp.h>

namespace {
   using namespace psizam;
   using namespace psizam::detail;
   using ctx_t = jit_execution_context<false>;

   ctx_t& as_ctx(void* ctx) {
      return *static_cast<ctx_t*>(ctx);
   }

   // Match algorithm — same logic as interpreter's dispatch_exception().
   // Searches catch clauses for a matching handler.
   // Returns true if a match was found (staging area populated).
   bool try_match_exception(ctx_t& c, uint32_t tag_index,
                            const uint64_t* payload, uint32_t payload_count) {
      auto& stack   = c.jit_eh_stack();
      auto& catches = c.jit_eh_catches();

      while (!stack.empty()) {
         auto& frame = stack.back();
         for (uint32_t ci = 0; ci < frame.catch_count; ++ci) {
            auto& entry = catches[frame.first_catch_idx + ci];
            bool matched = false;
            switch (entry.kind) {
               case 0: // catch_tag
               case 1: // catch_tag_ref
                  matched = (entry.tag_index == tag_index);
                  break;
               case 2: // catch_all
               case 3: // catch_all_ref
                  matched = true;
                  break;
            }
            if (matched) {
               // Stage the match results
               c.jit_eh_matched_catch = ci;
               c.jit_eh_exception = wasm_exception(tag_index, payload, payload_count);

               // For _ref kinds, push the exception onto the exnref stack
               if (entry.kind == 1 || entry.kind == 3) {
                  c.jit_eh_exnref = c.jit_push_caught_exception(tag_index, payload, payload_count);
               }

               // Copy the jmpbuf before popping (longjmp target)
               jmp_buf dest;
               std::memcpy(&dest, &frame.jmpbuf, sizeof(jmp_buf));

               // Pop this frame and all its catch entries
               catches.resize(frame.first_catch_idx);
               stack.pop_back();

               // longjmp to the try_table's setjmp point
               longjmp(dest, 1);
               // unreachable
            }
         }
         // No match in this frame — pop it and try outer frames
         catches.resize(frame.first_catch_idx);
         stack.pop_back();
      }
      return false; // no handler found
   }
}

extern "C" {

void* __psizam_eh_enter(void* ctx, uint32_t catch_count, const uint64_t* catch_data) {
   return as_ctx(ctx).jit_eh_enter(catch_count, catch_data);
}

void __psizam_eh_leave(void* ctx) {
   as_ctx(ctx).jit_eh_leave();
}

[[noreturn]] void __psizam_eh_throw(void* ctx, uint32_t tag_index,
                                     const uint64_t* payload, uint32_t payload_count) {
   auto& c = as_ctx(ctx);

   // try_match_exception does not return if a handler is found (it longjmps)
   try_match_exception(c, tag_index, payload, payload_count);

   // No WASM handler found — escape to host via trap_jmp_ptr
   // This is the same pattern as signal_throw / on_unreachable:
   // store exception in saved_exception and longjmp to invoke_with_signal_handler
   if (trap_jmp_ptr) {
      saved_exception = std::make_exception_ptr(
         wasm_exception(tag_index, payload, payload_count));
      longjmp(*trap_jmp_ptr, -1);
   }
   // Fallback: throw directly (should not happen in normal JIT execution)
   throw wasm_exception(tag_index, payload, payload_count);
}

[[noreturn]] void __psizam_eh_throw_ref(void* ctx, uint32_t exnref_idx) {
   if (exnref_idx == UINT32_MAX) {
      if (trap_jmp_ptr) {
         saved_exception = std::make_exception_ptr(
            wasm_interpreter_exception{"null exception reference"});
         longjmp(*trap_jmp_ptr, -1);
      }
      throw wasm_interpreter_exception{"null exception reference"};
   }
   auto& c = as_ctx(ctx);
   const auto& exn = c.jit_get_caught_exception(exnref_idx);
   __psizam_eh_throw(ctx, exn.tag_index, exn.values.data(),
                     static_cast<uint32_t>(exn.values.size()));
}

uint32_t __psizam_eh_get_match(void* ctx) {
   return as_ctx(ctx).jit_eh_matched_catch;
}

uint64_t __psizam_eh_get_payload(void* ctx, uint32_t i) {
   auto& exn = as_ctx(ctx).jit_eh_exception;
   if (i < exn.payload_count) return exn.payload[i];
   return 0;
}

uint32_t __psizam_eh_get_payload_count(void* ctx) {
   return as_ctx(ctx).jit_eh_exception.payload_count;
}

uint64_t __psizam_eh_get_exnref(void* ctx) {
   return as_ctx(ctx).jit_eh_exnref;
}

// Wrapper for setjmp callable from JIT-generated code.
// setjmp is often a macro, so we can't take its address directly.
//
// CRITICAL: This MUST be a naked tail-jump to setjmp, not a regular wrapper.
// If it creates a stack frame (e.g., STP X29/X30 on aarch64), the try body's
// operand stack will overwrite that frame between setjmp and longjmp. When
// longjmp returns through the wrapper, the corrupted frame causes a crash.
//
// Naked tail-jump: no stack frame is created. setjmp saves LR/registers into
// the jmp_buf directly. After longjmp, setjmp returns directly to the JIT
// code's BLR call site with no stack frame to corrupt.
#if defined(__aarch64__)
__attribute__((naked))
int __psizam_setjmp(void* jmpbuf) {
   __asm__ volatile(
#ifdef __APPLE__
      "b _setjmp"
#else
      "b setjmp"
#endif
   );
}
#elif defined(__x86_64__)
__attribute__((naked))
int __psizam_setjmp(void* jmpbuf) {
   __asm__ volatile(
#ifdef __APPLE__
      "jmp _setjmp"
#else
      "jmp setjmp@PLT"
#endif
   );
}
#else
// Fallback: regular wrapper. May have stack frame corruption issues
// if used with JIT EH. Only safe for interpreter path.
__attribute__((noinline))
int __psizam_setjmp(void* jmpbuf) {
   return setjmp(*static_cast<jmp_buf*>(jmpbuf));
}
#endif

} // extern "C"
