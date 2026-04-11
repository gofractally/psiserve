// Extended unistd.h for WASI — wraps the real WASI unistd.h and adds
// POSIX functions that LLVM references but WASI doesn't declare.

#ifndef _WASI_COMPAT_UNISTD_H
#define _WASI_COMPAT_UNISTD_H

#include_next <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

// Functions LLVM's Unix code paths reference
int dup(int fd);
int dup2(int oldfd, int newfd);
int pipe(int pipefd[2]);
int getpid(void);
int getsid(int pid);
int setsid(void);
unsigned int sleep(unsigned int seconds);
unsigned int alarm(unsigned int seconds);
int getuid(void);
int getgid(void);
int fchown(int fd, int owner, int group);
unsigned int umask(unsigned int mask);

// Process creation
int fork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
int execv(const char* path, char* const argv[]);
int kill(int pid, int sig);

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_UNISTD_H
