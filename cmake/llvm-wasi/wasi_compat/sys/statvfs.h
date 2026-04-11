// Extended sys/statvfs.h for WASI — adds missing f_flags alias and MNT_LOCAL.

#ifndef _WASI_COMPAT_SYS_STATVFS_H
#define _WASI_COMPAT_SYS_STATVFS_H

#include_next <sys/statvfs.h>

// WASI's statvfs has f_flag (singular); LLVM expects f_flags (plural) on
// platforms that aren't NetBSD/DragonFly/GNU.  Map it.
#define f_flags f_flag

// MNT_LOCAL is a BSD constant; WASI doesn't have it.
// Define as 0 so is_local() always returns false (conservative: treat as remote).
#ifndef MNT_LOCAL
#define MNT_LOCAL 0
#endif

#endif // _WASI_COMPAT_SYS_STATVFS_H
