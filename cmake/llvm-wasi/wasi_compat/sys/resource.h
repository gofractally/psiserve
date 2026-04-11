// Extended sys/resource.h for WASI — adds rlimit/getrlimit/setrlimit.

#ifndef _WASI_COMPAT_SYS_RESOURCE_H
#define _WASI_COMPAT_SYS_RESOURCE_H

#include_next <sys/resource.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned long long rlim_t;

struct rlimit {
   rlim_t rlim_cur;
   rlim_t rlim_max;
};

#ifndef RLIMIT_CORE
#define RLIMIT_CPU     0
#define RLIMIT_FSIZE   1
#define RLIMIT_DATA    2
#define RLIMIT_STACK   3
#define RLIMIT_CORE    4
#define RLIMIT_RSS     5
#define RLIMIT_NPROC   6
#define RLIMIT_NOFILE  7
#define RLIMIT_MEMLOCK 8
#define RLIMIT_AS      9
#endif

#ifndef RLIM_INFINITY
#define RLIM_INFINITY (~0ULL)
#endif

int getrlimit(int resource, struct rlimit *rlim);
int setrlimit(int resource, const struct rlimit *rlim);

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_SYS_RESOURCE_H
