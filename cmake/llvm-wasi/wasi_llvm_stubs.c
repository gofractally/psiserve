// POSIX stubs for functions LLVM references but WASI lacks.
//
// LLVM is built with LLVM_ON_UNIX=1 so that the Support library selects Unix
// code paths (rather than the non-existent Windows paths). Most are disabled
// by LLVM_ENABLE_THREADS=OFF, LLVM_ENABLE_BACKTRACES=OFF, etc., but some
// symbols still show up. This file provides stub definitions.
//
// This file is iteratively populated as link errors arise.

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <dlfcn.h>

// ── signals ────────────────────────────────────────────────────────────────

int sigemptyset(sigset_t* set) {
   memset(set, 0, sizeof(*set));
   return 0;
}

int sigfillset(sigset_t* set) {
   memset(set, 0xff, sizeof(*set));
   return 0;
}

int sigaddset(sigset_t* set, int signo) {
   (void)set; (void)signo;
   return 0;
}

int sigdelset(sigset_t* set, int signo) {
   (void)set; (void)signo;
   return 0;
}

int sigismember(const sigset_t* set, int signo) {
   (void)set; (void)signo;
   return 0;
}

int sigprocmask(int how, const sigset_t* restrict set, sigset_t* restrict oldset) {
   (void)how; (void)set;
   if (oldset) memset(oldset, 0, sizeof(*oldset));
   return 0;
}

int sigaction(int sig, const struct sigaction* restrict act, struct sigaction* restrict oldact) {
   (void)sig; (void)act;
   if (oldact) memset(oldact, 0, sizeof(*oldact));
   return 0;
}

int kill(int pid, int sig) {
   (void)pid; (void)sig;
   errno = ENOSYS;
   return -1;
}

// ── process wait ───────────────────────────────────────────────────────────

typedef int pid_t_wait;
pid_t_wait wait4(pid_t_wait pid, int* status, int options, void* usage) {
   (void)pid; (void)status; (void)options; (void)usage;
   errno = ENOSYS;
   return -1;
}

unsigned int alarm(unsigned int seconds) {
   (void)seconds;
   return 0;
}

// ── process ────────────────────────────────────────────────────────────────

int fork(void) {
   errno = ENOSYS;
   return -1;
}

int execve(const char* path, char* const argv[], char* const envp[]) {
   (void)path; (void)argv; (void)envp;
   errno = ENOSYS;
   return -1;
}

int execv(const char* path, char* const argv[]) {
   (void)path; (void)argv;
   errno = ENOSYS;
   return -1;
}

typedef int pid_t_stub;
pid_t_stub waitpid(pid_t_stub pid, int* status, int options) {
   (void)pid; (void)status; (void)options;
   errno = ENOSYS;
   return -1;
}

// ── dynamic linking ────────────────────────────────────────────────────────

void* dlopen(const char* filename, int flags) {
   (void)filename; (void)flags;
   return NULL;
}

void* dlsym(void* handle, const char* symbol) {
   (void)handle; (void)symbol;
   return NULL;
}

int dlclose(void* handle) {
   (void)handle;
   return -1;
}

char* dlerror(void) {
   return "dlopen not supported on WASI";
}

// ── backtrace ──────────────────────────────────────────────────────────────

int backtrace(void** buffer, int size) {
   (void)buffer; (void)size;
   return 0;
}

char** backtrace_symbols(void* const* buffer, int size) {
   (void)buffer; (void)size;
   return NULL;
}

void backtrace_symbols_fd(void* const* buffer, int size, int fd) {
   (void)buffer; (void)size; (void)fd;
}

int dladdr(const void* addr, Dl_info* info) {
   (void)addr; (void)info;
   return 0;
}

// ── unistd extras ──────────────────────────────────────────────────────────

int dup(int fd) {
   (void)fd;
   errno = ENOSYS;
   return -1;
}

int dup2(int oldfd, int newfd) {
   (void)oldfd; (void)newfd;
   errno = ENOSYS;
   return -1;
}

int pipe(int pipefd[2]) {
   (void)pipefd;
   errno = ENOSYS;
   return -1;
}

int getpid(void) {
   return 1;
}

int getsid(int pid) {
   (void)pid;
   errno = ENOSYS;
   return -1;
}

int setsid(void) {
   errno = ENOSYS;
   return -1;
}

int wait(int* status) {
   (void)status;
   errno = ENOSYS;
   return -1;
}

// ── sockets ────────────────────────────────────────────────────────────────

struct sockaddr;
typedef unsigned int socklen_t_stub;

int socket(int domain, int type, int protocol) {
   (void)domain; (void)type; (void)protocol;
   errno = ENOSYS;
   return -1;
}

int connect(int sockfd, const struct sockaddr* addr, socklen_t_stub addrlen) {
   (void)sockfd; (void)addr; (void)addrlen;
   errno = ENOSYS;
   return -1;
}

int bind(int sockfd, const struct sockaddr* addr, socklen_t_stub addrlen) {
   (void)sockfd; (void)addr; (void)addrlen;
   errno = ENOSYS;
   return -1;
}

int listen(int sockfd, int backlog) {
   (void)sockfd; (void)backlog;
   errno = ENOSYS;
   return -1;
}

int accept(int sockfd, struct sockaddr* addr, socklen_t_stub* addrlen) {
   (void)sockfd; (void)addr; (void)addrlen;
   errno = ENOSYS;
   return -1;
}

// ── pwd ────────────────────────────────────────────────────────────────────

#include <pwd.h>

struct passwd* getpwuid(int uid) {
   (void)uid;
   return NULL;
}

struct passwd* getpwnam(const char* name) {
   (void)name;
   return NULL;
}

int getpwuid_r(int uid, struct passwd* pwd, char* buf, unsigned long buflen, struct passwd** result) {
   (void)uid; (void)pwd; (void)buf; (void)buflen;
   if (result) *result = NULL;
   return -1;
}

int getuid(void) {
   return 0;
}

int getgid(void) {
   return 0;
}

int fchown(int fd, int owner, int group) {
   (void)fd; (void)owner; (void)group;
   errno = ENOSYS;
   return -1;
}

unsigned int umask(unsigned int mask) {
   (void)mask;
   return 0022; // default umask
}

// ── resource limits ────────────────────────────────────────────────────────

struct rlimit_stub { unsigned long long rlim_cur; unsigned long long rlim_max; };

int getrlimit(int resource, struct rlimit_stub* rlim) {
   (void)resource;
   // NB: do NOT return ~0ULL here. LLVM uses the returned rlim_cur as a
   // byte count for stack buffers in a handful of places, and some
   // narrowing casts end up with (uint32_t)-1 = 0xFFFFFFFF, which then
   // crashes as an OOB dereference at runtime. A finite upper bound
   // sidesteps the issue: 64 MiB is more than any realistic wasm stack
   // or descriptor table will need and fits comfortably in uint32_t.
   if (rlim) {
      rlim->rlim_cur = 64ULL << 20;
      rlim->rlim_max = 64ULL << 20;
   }
   return 0;
}

int setrlimit(int resource, const struct rlimit_stub* rlim) {
   (void)resource; (void)rlim;
   errno = ENOSYS;
   return -1;
}

// NB: mmap / munmap / mprotect / msync are NOT stubbed here. wasi-sdk 32's
// libwasi-emulated-mman.a provides working implementations (heap-backed,
// with file-backed writeback for MAP_SHARED | PROT_WRITE). Duplicating
// them here triggers a wasm-ld "duplicate symbol" error at the final
// clang.wasm / wasm-ld.wasm link. Use wasi-libc's versions directly by
// linking -lwasi-emulated-mman in the toolchain.

#include <stdlib.h>
#include <unistd.h>

int posix_madvise(void* addr, unsigned long length, int advice) {
   (void)addr; (void)length; (void)advice;
   return 0; // advisory only, OK to ignore
}

// ── pwd (reentrant) ───────────────────────────────────────────────────────

int getpwnam_r(const char* name, struct passwd* pwd, char* buf, unsigned long buflen, struct passwd** result) {
   (void)name; (void)pwd; (void)buf; (void)buflen;
   if (result) *result = NULL;
   return -1;
}

// ── misc ───────────────────────────────────────────────────────────────────

int getrusage(int who, void* usage) {
   (void)who; (void)usage;
   errno = ENOSYS;
   return -1;
}

int uname(void* buf) {
   (void)buf;
   errno = ENOSYS;
   return -1;
}

// ── C++ ABI (thread_local) ────────────────────────────────────────────────
// lld uses thread_local variables with non-trivial destructors; libc++abi
// needs __cxa_thread_atexit to register them. WASI is single-threaded and
// the wasi variant of libc++abi omits it. No threads means destructors can
// never be invoked anyway, so a no-op stub is sufficient.

int __cxa_thread_atexit(void (*func)(void*), void* arg, void* dso_handle) {
   (void)func; (void)arg; (void)dso_handle;
   return 0;
}

