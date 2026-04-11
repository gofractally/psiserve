// Stack-switching trampoline for executing WASM code on a dedicated stack.
// Used by the LLVM backend (and potentially others) to isolate WASM
// execution from the host C stack.

#include <cstdint>
#include <exception>

namespace psizam {
   using llvm_entry_fn_t = int64_t(*)(void*, void*, void*);
}

// Implementation function that runs ON the alternate stack.
// Must be noinline and extern "C" for a stable, platform-portable symbol name.
extern "C" __attribute__((noinline))
int64_t __psizam_call_on_stack_impl(
   psizam::llvm_entry_fn_t fn, void* ctx, void* mem,
   void* args, std::exception_ptr* exc_out)
{
   try {
      return fn(ctx, mem, args);
   } catch (...) {
      *exc_out = std::current_exception();
      return 0;
   }
}

namespace psizam {

// Platform-specific symbol prefix for assembly references
#ifdef __APPLE__
#define PSIZAM_ASM_SYM(name) "_" name
#else
#define PSIZAM_ASM_SYM(name) name
#endif

#if defined(__x86_64__)
   // Switch RSP to the alternate stack, call fn in try/catch, switch back.
   __attribute__((naked))
   int64_t call_on_stack(void* /*stack_top*/, llvm_entry_fn_t /*fn*/,
                         void* /*ctx*/, void* /*mem*/, void* /*args*/,
                         std::exception_ptr* /*exc_out*/) {
      // SysV ABI: rdi=stack_top, rsi=fn, rdx=ctx, rcx=mem, r8=args, r9=exc_out
      asm volatile(
         "pushq %%rbp\n"
         "movq %%rsp, %%rbp\n"
         "pushq %%rbx\n"
         "pushq %%r12\n"

         // Save original RSP in callee-save register
         "movq %%rsp, %%rbx\n"

         // Switch to alternate stack (16-byte aligned)
         "movq %%rdi, %%rsp\n"
         "andq $-16, %%rsp\n"

         // Set up args for __psizam_call_on_stack_impl(fn, ctx, mem, args, exc_out)
         "movq %%rsi, %%rdi\n"   // fn
         "movq %%rdx, %%rsi\n"   // ctx
         "movq %%rcx, %%rdx\n"   // mem
         "movq %%r8, %%rcx\n"    // args
         "movq %%r9, %%r8\n"     // exc_out
         "callq " PSIZAM_ASM_SYM("__psizam_call_on_stack_impl") "\n"

         // Restore original RSP (result already in rax)
         "movq %%rbx, %%rsp\n"

         "popq %%r12\n"
         "popq %%rbx\n"
         "popq %%rbp\n"
         "retq\n"
         ::: "memory"
      );
   }
#elif defined(__aarch64__)
   // ARM64 version
   __attribute__((naked))
   int64_t call_on_stack(void* /*stack_top*/, llvm_entry_fn_t /*fn*/,
                         void* /*ctx*/, void* /*mem*/, void* /*args*/,
                         std::exception_ptr* /*exc_out*/) {
      // AAPCS64: x0=stack_top, x1=fn, x2=ctx, x3=mem, x4=args, x5=exc_out
      asm volatile(
         "stp x29, x30, [sp, #-16]!\n"
         "mov x29, sp\n"
         "stp x19, x20, [sp, #-16]!\n"

         // Save original SP in callee-save register
         "mov x19, sp\n"

         // Switch to alternate stack (16-byte aligned)
         "and x0, x0, #~0xf\n"
         "mov sp, x0\n"

         // Set up args for __psizam_call_on_stack_impl(fn, ctx, mem, args, exc_out)
         "mov x0, x1\n"    // fn
         "mov x1, x2\n"    // ctx
         "mov x2, x3\n"    // mem
         "mov x3, x4\n"    // args
         "mov x4, x5\n"    // exc_out
         "bl " PSIZAM_ASM_SYM("__psizam_call_on_stack_impl") "\n"

         // Restore original SP (result already in x0)
         "mov sp, x19\n"

         "ldp x19, x20, [sp], #16\n"
         "ldp x29, x30, [sp], #16\n"
         "ret\n"
         ::: "memory"
      );
   }
#else
#error "call_on_stack not implemented for this architecture"
#endif

#undef PSIZAM_ASM_SYM

} // namespace psizam
