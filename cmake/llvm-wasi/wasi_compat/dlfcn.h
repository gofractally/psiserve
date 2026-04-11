// Extended dlfcn.h for WASI — adds Dl_info and dladdr that LLVM references.

#ifndef _WASI_COMPAT_DLFCN_H
#define _WASI_COMPAT_DLFCN_H

#include_next <dlfcn.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
   const char* dli_fname;
   void* dli_fbase;
   const char* dli_sname;
   void* dli_saddr;
} Dl_info;

int dladdr(const void* addr, Dl_info* info);

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_DLFCN_H
