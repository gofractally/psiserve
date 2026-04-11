// Extended sys/ioctl.h for WASI — adds winsize struct and TIOCGWINSZ.

#ifndef _WASI_COMPAT_SYS_IOCTL_H
#define _WASI_COMPAT_SYS_IOCTL_H

#include_next <sys/ioctl.h>

#ifdef __cplusplus
extern "C" {
#endif

struct winsize {
   unsigned short ws_row;
   unsigned short ws_col;
   unsigned short ws_xpixel;
   unsigned short ws_ypixel;
};

#ifndef TIOCGWINSZ
#define TIOCGWINSZ 0x5413
#endif

#ifdef __cplusplus
}
#endif

#endif // _WASI_COMPAT_SYS_IOCTL_H
