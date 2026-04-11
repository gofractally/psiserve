// Minimal setjmp/longjmp stubs for WASI.
// WASI doesn't support setjmp/longjmp (requires exception handling).
// LLVM's CrashRecoveryContext uses setjmp but the crash recovery path
// is disabled (LLVM_ENABLE_CRASH_OVERRIDES=OFF), so these stubs suffice.

#ifndef _WASI_COMPAT_SETJMP_H
#define _WASI_COMPAT_SETJMP_H

typedef int jmp_buf[1];

static inline int setjmp(jmp_buf env) {
   (void)env;
   return 0;
}

static inline void longjmp(jmp_buf env, int val) {
   (void)env;
   (void)val;
   __builtin_trap();
}

// LLVM uses sigsetjmp in some paths
typedef int sigjmp_buf[1];

static inline int sigsetjmp(sigjmp_buf env, int savesigs) {
   (void)env;
   (void)savesigs;
   return 0;
}

static inline void siglongjmp(sigjmp_buf env, int val) {
   (void)env;
   (void)val;
   __builtin_trap();
}

#endif // _WASI_COMPAT_SETJMP_H
