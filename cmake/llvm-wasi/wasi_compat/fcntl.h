// Extended fcntl.h for WASI — adds POSIX file locking constants.

#ifndef _WASI_COMPAT_FCNTL_H
#define _WASI_COMPAT_FCNTL_H

#include_next <fcntl.h>

// POSIX file locking constants missing from WASI
#ifndef F_GETLK
#define F_GETLK  5
#define F_SETLK  6
#define F_SETLKW 7
#endif

// File lock type constants
#ifndef F_RDLCK
#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#endif

#endif // _WASI_COMPAT_FCNTL_H
