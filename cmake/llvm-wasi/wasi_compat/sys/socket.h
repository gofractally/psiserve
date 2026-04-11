// Extended sys/socket.h for WASI — wraps the real WASI socket.h and adds
// socket functions that are excluded on wasip1 but LLVM references.

#ifndef _WASI_COMPAT_SYS_SOCKET_H
#define _WASI_COMPAT_SYS_SOCKET_H

#include_next <sys/socket.h>

#ifdef __cplusplus
extern "C" {
#endif

// These are excluded from wasip1 by: #if ... || !(defined __wasip1__)
// We declare them here so LLVM compiles; stubs return -1/ENOSYS.
#ifdef __wasip1__
int socket(int domain, int type, int protocol);
int connect(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int bind(int sockfd, const struct sockaddr* addr, socklen_t addrlen);
int listen(int sockfd, int backlog);
int accept(int sockfd, struct sockaddr* addr, socklen_t* addrlen);
#endif

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_SYS_SOCKET_H
