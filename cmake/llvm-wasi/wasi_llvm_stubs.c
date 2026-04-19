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
   if (rlim) { rlim->rlim_cur = ~0ULL; rlim->rlim_max = ~0ULL; }
   return 0;
}

int setrlimit(int resource, const struct rlimit_stub* rlim) {
   (void)resource; (void)rlim;
   errno = ENOSYS;
   return -1;
}

// ── mmap (heap-backed) ────────────────────────────────────────────────────
//
// WASI has no virtual memory. We implement mmap by allocating from the WASM
// linear memory heap via malloc. For writable file-backed MAP_SHARED mappings
// we flush contents back to the file on munmap; lld uses FileOutputBuffer's
// mmap path to write the output .wasm, and without writeback the file stays
// as a zero-filled truncated block on disk.

#include <stdlib.h>
#include <unistd.h>

#ifndef MAP_FAILED
#define MAP_FAILED ((void*)-1)
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS 0x20
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif
#ifndef MAP_SHARED
#define MAP_SHARED 0x01
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 0x2
#endif

// A small header precedes every mmap allocation; munmap uses it to find the
// fd+offset and write back if the mapping was shared+writable+file-backed.
// Aligned to 16 so the returned pointer stays aligned.
struct _mmap_hdr {
   int           fd;
   int           flags;
   int           prot;
   int           _pad;
   long long     offset;
   unsigned long length;
   unsigned long _pad2;
};

void* mmap(void* addr, unsigned long length, int prot, int flags, int fd, long long offset) {
   (void)addr;

   const unsigned long hdr_size = sizeof(struct _mmap_hdr);
   char* raw = (char*)malloc(hdr_size + length);
   if (!raw) {
      errno = ENOMEM;
      return MAP_FAILED;
   }

   struct _mmap_hdr* h = (struct _mmap_hdr*)raw;
   h->fd     = fd;
   h->flags  = flags;
   h->prot   = prot;
   h->offset = offset;
   h->length = length;

   void* ptr = raw + hdr_size;

   if (fd >= 0 && !(flags & (MAP_ANONYMOUS | MAP_ANON))) {
      // File-backed mapping: read file contents into the buffer.
      memset(ptr, 0, length);
      long long saved = lseek(fd, 0, SEEK_CUR);
      if (saved >= 0)
         lseek(fd, offset, SEEK_SET);
      unsigned long total = 0;
      while (total < length) {
         long n = read(fd, (char*)ptr + total, length - total);
         if (n <= 0) break;
         total += n;
      }
      if (saved >= 0)
         lseek(fd, saved, SEEK_SET);
   } else {
      // Anonymous mapping: zero-fill.
      memset(ptr, 0, length);
   }

   return ptr;
}

int munmap(void* addr, unsigned long length) {
   (void)length;
   if (!addr) return 0;

   const unsigned long hdr_size = sizeof(struct _mmap_hdr);
   struct _mmap_hdr* h = (struct _mmap_hdr*)((char*)addr - hdr_size);

   // Writeback: MAP_SHARED + PROT_WRITE + file-backed = flush to disk so lld's
   // output actually lands in the file instead of being discarded with the
   // buffer. Ignore errors — munmap has no way to surface them.
   if (h->fd >= 0
       && (h->flags & MAP_SHARED)
       && (h->prot & PROT_WRITE)
       && !(h->flags & (MAP_ANONYMOUS | MAP_ANON))) {
      long long saved = lseek(h->fd, 0, SEEK_CUR);
      if (saved >= 0)
         lseek(h->fd, h->offset, SEEK_SET);
      unsigned long total = 0;
      while (total < h->length) {
         long n = write(h->fd, (char*)addr + total, h->length - total);
         if (n <= 0) break;
         total += n;
      }
      if (saved >= 0)
         lseek(h->fd, saved, SEEK_SET);
   }

   free(h);
   return 0;
}

int msync(void* addr, unsigned long length, int flags) {
   (void)flags;
   if (!addr) return 0;

   const unsigned long hdr_size = sizeof(struct _mmap_hdr);
   struct _mmap_hdr* h = (struct _mmap_hdr*)((char*)addr - hdr_size);

   if (h->fd < 0
       || !(h->flags & MAP_SHARED)
       || !(h->prot & PROT_WRITE)
       || (h->flags & (MAP_ANONYMOUS | MAP_ANON))) {
      return 0;
   }

   long long saved = lseek(h->fd, 0, SEEK_CUR);
   if (saved >= 0)
      lseek(h->fd, h->offset, SEEK_SET);
   unsigned long total = 0;
   unsigned long bytes = length < h->length ? length : h->length;
   while (total < bytes) {
      long n = write(h->fd, (char*)addr + total, bytes - total);
      if (n <= 0) break;
      total += n;
   }
   if (saved >= 0)
      lseek(h->fd, saved, SEEK_SET);
   return 0;
}

int mprotect(void* addr, unsigned long length, int prot) {
   (void)addr; (void)length; (void)prot;
   return 0; // permissions are meaningless in WASM
}

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

