// Minimal sys/wait.h for WASI — LLVM's Program.inc references wait4/waitpid.

#ifndef _WASI_COMPAT_SYS_WAIT_H
#define _WASI_COMPAT_SYS_WAIT_H

#include <sys/types.h>

#define WNOHANG    1
#define WUNTRACED  2

#define WIFEXITED(s)    (((s) & 0x7f) == 0)
#define WEXITSTATUS(s)  (((s) & 0xff00) >> 8)
#define WIFSIGNALED(s)  (((s) & 0x7f) != 0 && ((s) & 0x7f) != 0x7f)
#define WTERMSIG(s)     ((s) & 0x7f)
#define WIFSTOPPED(s)   (((s) & 0xff) == 0x7f)
#define WSTOPSIG(s)     WEXITSTATUS(s)

#ifdef __cplusplus
extern "C" {
#endif

struct rusage;

typedef int pid_t;
pid_t wait(int* status);
pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait4(pid_t pid, int* status, int options, struct rusage* usage);

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_SYS_WAIT_H
