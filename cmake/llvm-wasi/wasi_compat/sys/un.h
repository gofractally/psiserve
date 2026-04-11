// Extended sys/un.h for WASI — provides full sockaddr_un with sun_path.

#ifndef _WASI_COMPAT_SYS_UN_H
#define _WASI_COMPAT_SYS_UN_H

#include <__typedef_sa_family_t.h>

// Override WASI's minimal sockaddr_un with a full one
struct sockaddr_un {
   sa_family_t sun_family;
   char sun_path[108];
};

#endif // _WASI_COMPAT_SYS_UN_H
