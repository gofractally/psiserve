/*
 * psi/host.h — Host function declarations for the psi platform.
 *
 * These are the WASM imports provided by the psiserve runtime.
 * Link against libpsi.a to get SDK utilities (websocket, etc.)
 * that build on top of these primitives.
 */

#ifndef PSI_HOST_H
#define PSI_HOST_H

#ifdef __cplusplus
extern "C" {
#endif

/* ── I/O ──────────────────────────────────────────────────────────── */

__attribute__((import_module("psi"), import_name("accept")))
int psi_accept(int listen_fd);

__attribute__((import_module("psi"), import_name("read")))
int psi_read(int fd, void* buf, int len);

__attribute__((import_module("psi"), import_name("write")))
int psi_write(int fd, const void* buf, int len);

__attribute__((import_module("psi"), import_name("open")))
int psi_open(int dir_fd, const void* path, int path_len);

__attribute__((import_module("psi"), import_name("fstat")))
int psi_fstat(int fd, void* out);

__attribute__((import_module("psi"), import_name("close")))
void psi_close(int fd);

/* ── High-performance I/O ─────────────────────────────────────────── */

/* Zero-copy file-to-socket transfer.  Uses OS sendfile for plain TCP,
 * falls back to buffered copy for TLS connections.
 * Returns total bytes sent, or negative PsiError. */
__attribute__((import_module("psi"), import_name("sendfile")))
int psi_sendfile(int sock_fd, int file_fd, long long len);

/* TCP cork: buffer small writes into one TCP segment.
 * Call psi_cork before a burst of small writes, psi_uncork after. */
__attribute__((import_module("psi"), import_name("cork")))
void psi_cork(int fd);

__attribute__((import_module("psi"), import_name("uncork")))
void psi_uncork(int fd);

/* ── UDP ─────────────────────────────────────────────────────────── */

/* Peer address for recvfrom/sendto — 8 bytes, network byte order. */
struct psi_addr
{
   unsigned int   ip4;    /* IPv4 address */
   unsigned short port;   /* port number */
   unsigned short _pad;
};

__attribute__((import_module("psi"), import_name("udp_bind")))
int psi_udp_bind(int port);

__attribute__((import_module("psi"), import_name("recvfrom")))
int psi_recvfrom(int fd, void* buf, int len, struct psi_addr* addr);

__attribute__((import_module("psi"), import_name("sendto")))
int psi_sendto(int fd, const void* buf, int len, const struct psi_addr* addr);

/* ── Concurrency ──────────────────────────────────────────────────── */

__attribute__((import_module("psi"), import_name("spawn")))
void psi_spawn(void (*func)(int), int arg);

/* ── Time ─────────────────────────────────────────────────────────── */

/* Clock IDs (WASI-compatible semantics):
 *   0 = REALTIME  — wall-clock, nanoseconds since Unix epoch
 *   1 = MONOTONIC — steady clock, nanoseconds since arbitrary origin
 */
#define PSI_CLOCK_REALTIME  0
#define PSI_CLOCK_MONOTONIC 1

__attribute__((import_module("psi"), import_name("clock")))
long long psi_clock(int clock_id);

__attribute__((import_module("psi"), import_name("sleep_until")))
void psi_sleep_until(long long deadline_ns);

/* Convenience: sleep for `ms` milliseconds (relative). */
static inline void psi_sleep(int ms)
{
   long long now = psi_clock(PSI_CLOCK_MONOTONIC);
   psi_sleep_until(now + (long long)ms * 1000000LL);
}

/* ── Helpers ──────────────────────────────────────────────────────── */

static inline void psi_write_all(int fd, const char* buf, int len)
{
   while (len > 0)
   {
      int n = psi_write(fd, buf, len);
      if (n <= 0) break;
      buf += n;
      len -= n;
   }
}

#ifdef __cplusplus
}
#endif

#endif /* PSI_HOST_H */
