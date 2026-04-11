/*
 * freestanding.h — Minimal libc shims for wasm32 -nostdlib builds.
 *
 * Provides memset, memcpy, and stubs for headers that vendor libraries
 * include but don't actually need in freestanding mode.
 */

#ifndef PSI_FREESTANDING_H
#define PSI_FREESTANDING_H

#include <stddef.h>
#include <stdint.h>

static inline void* psi_memset(void* s, int c, size_t n)
{
   unsigned char* p = (unsigned char*)s;
   while (n--) *p++ = (unsigned char)c;
   return s;
}

static inline void* psi_memcpy(void* dst, const void* src, size_t n)
{
   unsigned char*       d = (unsigned char*)dst;
   const unsigned char* s = (const unsigned char*)src;
   while (n--) *d++ = *s++;
   return dst;
}

/* Override libc names so vendor code links without -lc */
#define memset  psi_memset
#define memcpy  psi_memcpy

/* Stub assert for vendor code that includes <assert.h> */
#define assert(x) ((void)0)

#endif /* PSI_FREESTANDING_H */
