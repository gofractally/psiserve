#pragma once

#include <psizam/allocator.hpp>
#include <psizam/exceptions.hpp>
#include <psizam/detail/span.hpp>
#include <psizam/utils.hpp>

#include <atomic>
#include <cstdlib>
#include <exception>
#include <utility>

#ifdef PSIZAM_COMPILE_ONLY

// Compile-only mode: these functions provide addresses for relocations but abort if called.
// No signal handling, no thread-locals, no setjmp/longjmp.
namespace psizam::detail {
   template<typename E>
   [[noreturn]] inline void signal_throw(const char* msg) {
      std::abort();
   }

   inline std::span<std::byte> stack_guard_range;

   template<typename F>
   inline void longjmp_on_exception(F&& f) {
      f();
   }

   template<typename F, typename E>
   auto invoke_with_signal_handler(F&& f, E&&, growable_allocator&, wasm_allocator*) {
      return f();
   }

   template<typename F>
   auto invoke_with_signal_handler(F&& f) {
      return f();
   }
} // namespace psizam::detail

#else // !PSIZAM_COMPILE_ONLY

#include <signal.h>
#include <setjmp.h>

namespace psizam::detail {

   // Pointer to the active setjmp buffer for trap escape.  Uses setjmp/longjmp
   // (not sigsetjmp/siglongjmp) to avoid the ~400ns per-call cost of
   // saving/restoring the signal mask via kernel syscalls.  Each
   // invoke_with_signal_handler allocates a jmp_buf on its stack frame and sets
   // this pointer, supporting nested calls (WASM→host→WASM).  Signal handlers
   // longjmp through this pointer; post-longjmp code throws a C++ exception.
   // This matches what Wasmtime and V8 do for guard-page traps.
   __attribute__((visibility("default")))
   inline thread_local jmp_buf* trap_jmp_ptr{nullptr};

   __attribute__((visibility("default")))
   inline thread_local std::span<std::byte> code_memory_range;

   __attribute__((visibility("default")))
   inline thread_local std::span<std::byte> memory_range;

   __attribute__((visibility("default")))
   inline thread_local std::span<std::byte> stack_guard_range;

   __attribute__((visibility("default")))
   inline thread_local std::atomic<bool> timed_run_has_timed_out{false};

#if defined(PSIZAM_JIT_SIGNAL_DIAGNOSTICS)
   // Function table for crash diagnostics — set by ir_writer after finalize
   struct jit_func_range { uint32_t offset; uint32_t size; uint32_t func_index; };
   __attribute__((visibility("default")))
   inline thread_local const jit_func_range* jit_func_ranges{nullptr};
   __attribute__((visibility("default")))
   inline thread_local uint32_t jit_func_range_count{0};
#endif

   // Fixes a duplicate symbol build issue when building with `-fvisibility=hidden`
   __attribute__((visibility("default")))
   inline thread_local std::exception_ptr saved_exception{nullptr};

   template<int Sig>
   inline struct sigaction prev_signal_handler;

   inline void signal_handler(int sig, siginfo_t* info, void* uap) {
      if (trap_jmp_ptr) {
         const void* addr = info->si_addr;

#if defined(PSIZAM_JIT_SIGNAL_DIAGNOSTICS) && defined(__aarch64__) && defined(__APPLE__)
         {
            ucontext_t* uc = (ucontext_t*)uap;
            uint64_t pc_val = uc->uc_mcontext->__ss.__pc;
            uint32_t faulting_instr = 0;
            // Only read the faulting instruction if the PC is in a readable range.
            // If PC is 0 or unmapped, memcpy would cause a nested SIGSEGV.
            bool pc_readable = pc_val != 0 &&
               ((!code_memory_range.empty() &&
                 pc_val >= (uint64_t)code_memory_range.data() &&
                 pc_val < (uint64_t)code_memory_range.data() + code_memory_range.size()) ||
                (pc_val >= 0x100000000ULL && pc_val < 0x800000000000ULL)); // heuristic: in app text segment
            if (pc_readable)
               memcpy(&faulting_instr, (void*)pc_val, 4);
            bool in_code = !code_memory_range.empty() &&
               pc_val >= (uint64_t)code_memory_range.data() &&
               pc_val < (uint64_t)code_memory_range.data() + code_memory_range.size();
            bool in_mem = !memory_range.empty() &&
               (uint64_t)addr >= (uint64_t)memory_range.data() &&
               (uint64_t)addr < (uint64_t)memory_range.data() + memory_range.size();
            bool addr_in_code = !code_memory_range.empty() &&
               (uint64_t)addr >= (uint64_t)code_memory_range.data() &&
               (uint64_t)addr < (uint64_t)code_memory_range.data() + code_memory_range.size();
            fprintf(stderr, "JIT FAULT sig=%d addr=%p pc=0x%llx instr=0x%08x in_code=%d in_mem=%d addr_in_code=%d\n",
                    sig, addr, pc_val, faulting_instr, in_code, in_mem, addr_in_code);
            fprintf(stderr, "  code_range=[%p, %p) mem_range=[%p, %p)\n",
                    code_memory_range.data(), code_memory_range.data() + code_memory_range.size(),
                    memory_range.data(), memory_range.data() + memory_range.size());
            auto* ss = &uc->uc_mcontext->__ss;
            fprintf(stderr, "  X0=%016llx  X1=%016llx  X8=%016llx  X9=%016llx\n",
                    ss->__x[0], ss->__x[1], ss->__x[8], ss->__x[9]);
            fprintf(stderr, "  X19(ctx)=%016llx  X20(mem)=%016llx  X21(depth)=%016llx\n",
                    ss->__x[19], ss->__x[20], ss->__x[21]);
            fprintf(stderr, "  FP=%016llx  LR=%016llx  SP=%016llx\n",
                    ss->__fp, ss->__lr, ss->__sp);
            if (in_code && in_mem) {
               uint64_t mem_base = ss->__x[20];
               int64_t offset_from_base = (int64_t)((uint64_t)addr - mem_base);
               fprintf(stderr, "  Fault offset from X20(mem_base): %lld (0x%llx)\n",
                       offset_from_base, (uint64_t)offset_from_base);
            }
            if (in_code && jit_func_ranges && jit_func_range_count > 0) {
               uint64_t code_base = (uint64_t)code_memory_range.data();
               uint64_t pc_off = pc_val - code_base;
               uint64_t lr_off = ss->__lr - code_base;
               for (uint32_t fi = 0; fi < jit_func_range_count; ++fi) {
                  auto& r = jit_func_ranges[fi];
                  if (pc_off >= r.offset && pc_off < r.offset + r.size) {
                     fprintf(stderr, "  Crash in func[%u] at +%llu (code offset %u, size %u)\n",
                             r.func_index, (unsigned long long)(pc_off - r.offset), r.offset, r.size);
                  }
                  if (lr_off >= r.offset && lr_off < r.offset + r.size) {
                     fprintf(stderr, "  Caller func[%u] at +%llu\n",
                             r.func_index, (unsigned long long)(lr_off - r.offset));
                  }
               }
            }
         }
#endif

         //neither range set means legacy catch-all behavior; useful for some of the old tests
         if (code_memory_range.empty() && memory_range.empty())
            longjmp(*trap_jmp_ptr, sig);

         //a failure on the stack guard page means stack overflow
         if (!stack_guard_range.empty() &&
             addr >= stack_guard_range.data() && addr < stack_guard_range.data() + stack_guard_range.size())
            longjmp(*trap_jmp_ptr, sig);

         //a failure in the memory range is always jumped out of
         if (addr >= memory_range.data() && addr < memory_range.data() + memory_range.size())
            longjmp(*trap_jmp_ptr, sig);

         //a failure in the code range...
         if (addr >= code_memory_range.data() && addr < code_memory_range.data() + code_memory_range.size()) {
            //a SEGV/BUS in the code range when timed_run_has_timed_out=false is due to a _different_ thread's execution activating a deadline
            // timer. Return and retry executing the same code again. Eventually timed_run() on the other thread will reset the page
            // permissions and progress on this thread can continue
            //on linux no SIGBUS handler is registered (see setup_signal_handler_impl()) so it will never occur here
            if ((sig == SIGSEGV || sig == SIGBUS) && timed_run_has_timed_out.load(std::memory_order_acquire) == false)
               return;
            //otherwise, jump out
            longjmp(*trap_jmp_ptr, sig);
         }

         // If the PC (not the fault address) is in the code range, the fault
         // was caused by JIT code accessing an address outside any known range.
         // This is still a WASM trap — longjmp out rather than terminating.
#if defined(__aarch64__) && defined(__APPLE__)
         {
            ucontext_t* uc = (ucontext_t*)uap;
            uint64_t pc_val = uc->uc_mcontext->__ss.__pc;
            if (!code_memory_range.empty() &&
                pc_val >= (uint64_t)code_memory_range.data() &&
                pc_val < (uint64_t)code_memory_range.data() + code_memory_range.size())
               longjmp(*trap_jmp_ptr, sig);
         }
#elif defined(__x86_64__)
         {
            ucontext_t* uc = (ucontext_t*)uap;
#if defined(__APPLE__)
            uint64_t pc_val = uc->uc_mcontext->__ss.__rip;
#else
            uint64_t pc_val = uc->uc_mcontext.gregs[REG_RIP];
#endif
            if (!code_memory_range.empty() &&
                pc_val >= (uint64_t)code_memory_range.data() &&
                pc_val < (uint64_t)code_memory_range.data() + code_memory_range.size())
               longjmp(*trap_jmp_ptr, sig);
         }
#endif

         // Fallback: if we're in a WASM execution context (trap_jmp_ptr set) but
         // no known range matched, this is likely a fault in a C helper function
         // called from JIT code (e.g., due to operand stack underflow producing
         // garbage values). Trap cleanly rather than crashing the host process.
         longjmp(*trap_jmp_ptr, sig);
      }

      struct sigaction* prev_action;
      switch(sig) {
         case SIGSEGV: prev_action = &prev_signal_handler<SIGSEGV>; break;
         case SIGBUS: prev_action = &prev_signal_handler<SIGBUS>; break;
         case SIGFPE: prev_action = &prev_signal_handler<SIGFPE>; break;
         default: std::abort();
      }
      if (!prev_action) std::abort();
      if (prev_action->sa_flags & SA_SIGINFO) {
         // FIXME: We need to be at least as strict as the original
         // flags and relax the mask as needed.
         prev_action->sa_sigaction(sig, info, uap);
      } else {
         if(prev_action->sa_handler == SIG_DFL) {
            // The default for all three signals is to terminate the process.
            sigaction(sig, prev_action, nullptr);
            raise(sig);
         } else if(prev_action->sa_handler == SIG_IGN) {
            // Do nothing
         } else {
            prev_action->sa_handler(sig);
         }
      }
   }

   // Only valid when trap_jmp_ptr is non-null (inside invoke_with_signal_handler).
   // This is a workaround for the fact that it is currently unsafe to throw
   // an exception through a JIT frame (no .eh_frame unwind data).
   template<typename F>
   inline void longjmp_on_exception(F&& f) {
      static_assert(std::is_trivially_destructible_v<std::decay_t<F>>, "longjmp has undefined behavior when it bypasses destructors.");
      bool caught_exception = false;
      try {
         f();
      } catch(...) {
         saved_exception = std::current_exception();
         // Cannot safely longjmp from inside the catch,
         // as that will leak the exception.
         caught_exception = true;
      }
      if (caught_exception) {
         longjmp(*trap_jmp_ptr, -1);
      }
   }

   template<typename E>
   [[noreturn]] inline void signal_throw(const char* msg) {
      saved_exception = std::make_exception_ptr(E{msg});
      longjmp(*trap_jmp_ptr, -1);
   }

   inline void setup_signal_handler_impl() {
      struct sigaction sa;
      sa.sa_sigaction = &signal_handler;
      sigemptyset(&sa.sa_mask);
      sigaddset(&sa.sa_mask, SIGPROF);
      sa.sa_flags = SA_NODEFER | SA_SIGINFO | SA_ONSTACK;
      sigaction(SIGSEGV, &sa, &prev_signal_handler<SIGSEGV>);
#ifndef __linux__
      sigaction(SIGBUS, &sa, &prev_signal_handler<SIGBUS>);
#endif
      sigaction(SIGFPE, &sa, &prev_signal_handler<SIGFPE>);
   }

   // Set up a per-thread alternate signal stack so that stack overflow
   // signals can be delivered even when the execution stack is exhausted.
   inline void setup_thread_signal_stack() {
      thread_local bool initialized = [] {
         static constexpr std::size_t min_size = 65536;
         std::size_t alt_size = SIGSTKSZ > min_size ? SIGSTKSZ : min_size;
         void* mem = ::mmap(nullptr, alt_size, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
         if (mem == MAP_FAILED) return false;
         stack_t ss{};
         ss.ss_sp = mem;
         ss.ss_size = alt_size;
         ss.ss_flags = 0;
         ::sigaltstack(&ss, nullptr);
         return true;
      }();
      ignore_unused_variable_warning(initialized);
   }

   // One-shot per-thread unblocking of signals used for guard page traps.
   // Called once per thread instead of per-call (was the ~200ns pthread_sigmask
   // in every invoke_with_signal_handler call).
   inline void ensure_signals_unblocked() {
      thread_local bool done = [] {
         sigset_t unblock_mask;
         sigemptyset(&unblock_mask);
         sigaddset(&unblock_mask, SIGSEGV);
         sigaddset(&unblock_mask, SIGBUS);
         sigaddset(&unblock_mask, SIGFPE);
         pthread_sigmask(SIG_UNBLOCK, &unblock_mask, nullptr);
         return true;
      }();
      ignore_unused_variable_warning(done);
   }

   inline void setup_signal_handler() {
      static int init_helper = (setup_signal_handler_impl(), 0);
      ignore_unused_variable_warning(init_helper);
      setup_thread_signal_stack();
      ensure_signals_unblocked();
   }

   /// Call a function with trap handling.  If a signal (SIGSEGV, SIGBUS, SIGFPE)
   /// fires during f(), the signal handler longjmps back here, and the error
   /// handler e is called with the signal number.  If f() or anything it calls
   /// uses signal_throw() or longjmp_on_exception(), those also land here and
   /// rethrow the saved exception.
   ///
   /// Uses setjmp/longjmp (not sigsetjmp/siglongjmp) to avoid ~400ns of
   /// per-call kernel syscalls for signal mask save/restore.  Signals are
   /// unblocked once per thread in setup_signal_handler().
   ///
   // Make this noinline to prevent possible corruption of the caller's local variables.
   template<typename F, typename E>
   [[gnu::noinline]] auto invoke_with_signal_handler(F&& f, E&& e, growable_allocator& code_allocator, wasm_allocator* mem_allocator) {
      setup_signal_handler();
      jmp_buf dest;
      const auto old_code_memory_range = code_memory_range;
      const auto old_memory_range = memory_range;
      const auto old_stack_guard_range = stack_guard_range;
      jmp_buf* old_trap_ptr = trap_jmp_ptr;
      code_memory_range = code_allocator.get_code_span();
      memory_range = mem_allocator->get_span();
      int sig;
      if((sig = setjmp(dest)) == 0) {
         trap_jmp_ptr = &dest;
         try {
            f();
            trap_jmp_ptr = old_trap_ptr;
            memory_range = old_memory_range;
            code_memory_range = old_code_memory_range;
            stack_guard_range = old_stack_guard_range;
         } catch(...) {
            trap_jmp_ptr = old_trap_ptr;
            memory_range = old_memory_range;
            code_memory_range = old_code_memory_range;
            stack_guard_range = old_stack_guard_range;
            throw;
         }
      } else {
         trap_jmp_ptr = old_trap_ptr;
         memory_range = old_memory_range;
         code_memory_range = old_code_memory_range;
         stack_guard_range = old_stack_guard_range;
         if (sig == -1) {
            std::exception_ptr exception = std::move(saved_exception);
            saved_exception = nullptr;
            std::rethrow_exception(exception);
         } else {
            e(sig);
         }
      }
   }

} // namespace psizam::detail

#endif // PSIZAM_COMPILE_ONLY
