// Minimal execinfo.h for WASI — LLVM references backtrace() for stack traces.

#ifndef _WASI_COMPAT_EXECINFO_H
#define _WASI_COMPAT_EXECINFO_H

#ifdef __cplusplus
extern "C" {
#endif

int backtrace(void** buffer, int size);
char** backtrace_symbols(void* const* buffer, int size);
void backtrace_symbols_fd(void* const* buffer, int size, int fd);

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_EXECINFO_H
