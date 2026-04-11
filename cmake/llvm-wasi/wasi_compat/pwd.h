// Minimal pwd.h for WASI — LLVM's Path.inc references getpwuid.

#ifndef _WASI_COMPAT_PWD_H
#define _WASI_COMPAT_PWD_H

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

struct passwd {
   char* pw_name;
   char* pw_passwd;
   int pw_uid;
   int pw_gid;
   char* pw_gecos;
   char* pw_dir;
   char* pw_shell;
};

struct passwd* getpwuid(int uid);
struct passwd* getpwnam(const char* name);
int getpwuid_r(int uid, struct passwd* pwd, char* buf, unsigned long buflen, struct passwd** result);
int getpwnam_r(const char* name, struct passwd* pwd, char* buf, unsigned long buflen, struct passwd** result);

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_PWD_H
