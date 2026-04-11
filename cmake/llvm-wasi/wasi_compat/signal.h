// Extended signal.h for WASI — wraps the real WASI signal.h and adds
// POSIX signal types that LLVM's Support library references.
// All functions are stub implementations (in wasi_llvm_stubs.c).

#ifndef _WASI_COMPAT_SIGNAL_H
#define _WASI_COMPAT_SIGNAL_H

// Include the real WASI signal.h first
#include_next <signal.h>

#ifdef __cplusplus
extern "C" {
#endif

// --- Missing constants ---
#ifndef SIG_BLOCK
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2
#endif

#ifndef SIGBUS
#define SIGBUS  10
#endif
#ifndef SIGTRAP
#define SIGTRAP  5
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif

#ifndef SA_SIGINFO
#define SA_SIGINFO 4
#define SA_RESTART 0x10000000
#define SA_NODEFER 0x40000000
#define SA_RESETHAND 0x80000000
#define SA_ONSTACK 0x08000000
#endif

#ifndef SA_ONSTACK
#define SA_ONSTACK 0x08000000
#endif

// --- sigset_t (may already be defined via WASI headers) ---
#ifndef __DEFINED_sigset_t
typedef struct { unsigned long __bits[128/sizeof(unsigned long)]; } sigset_t;
#define __DEFINED_sigset_t
#endif

// --- siginfo_t ---
#ifndef _WASI_COMPAT_SIGINFO_T
#define _WASI_COMPAT_SIGINFO_T
typedef struct {
   int si_signo;
   int si_errno;
   int si_code;
   int si_pid;
   int si_uid;
   char __pad[128 - 5*sizeof(int)];
} siginfo_t;
#endif

// --- struct sigaction ---
#ifndef _WASI_COMPAT_SIGACTION
#define _WASI_COMPAT_SIGACTION
struct sigaction {
   union {
      void (*sa_handler)(int);
      void (*sa_sigaction)(int, siginfo_t*, void*);
   } __sa_handler;
   sigset_t sa_mask;
   int sa_flags;
   void (*sa_restorer)(void);
};
#define sa_handler   __sa_handler.sa_handler
#define sa_sigaction __sa_handler.sa_sigaction
#endif

// --- Signal set functions ---
int sigemptyset(sigset_t*);
int sigfillset(sigset_t*);
int sigaddset(sigset_t*, int);
int sigdelset(sigset_t*, int);
int sigismember(const sigset_t*, int);
int sigprocmask(int, const sigset_t* __restrict, sigset_t* __restrict);
int sigaction(int, const struct sigaction* __restrict, struct sigaction* __restrict);

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_SIGNAL_H
