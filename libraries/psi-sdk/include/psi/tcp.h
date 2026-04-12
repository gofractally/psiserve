/*
 * psi/tcp.h — Clean TCP API for psi WASM services.
 *
 * Wraps the raw psi_read/psi_write/psi_connect/psi_accept host calls
 * in a structured, easy-to-use interface.  All operations are fiber-aware
 * (they yield the calling fiber on EAGAIN, not the OS thread).
 *
 * Usage:
 *
 *   // Accept incoming connection
 *   tcp_conn client = tcp_accept(0);  // fd 0 = listen socket
 *
 *   // Read request
 *   char buf[4096];
 *   int n = tcp_read(&client, buf, sizeof(buf));
 *
 *   // Outbound connection
 *   tcp_conn upstream = tcp_connect("example.com", 80);
 *   tcp_write_all(&upstream, request, request_len);
 *   int resp_len = tcp_read(&upstream, buf, sizeof(buf));
 *
 *   tcp_close(&upstream);
 *   tcp_close(&client);
 */

#ifndef PSI_TCP_H
#define PSI_TCP_H

#include <psi/host.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Types ───────────────────────────────────────────────────────── */

typedef struct
{
   int fd;
} tcp_conn;

/* ── Accepting ───────────────────────────────────────────────────── */

/* Accept an incoming connection on a listening socket.
 * Blocks the fiber until a connection arrives.
 * Returns a tcp_conn with fd < 0 on error. */
static inline tcp_conn tcp_accept(int listen_fd)
{
   tcp_conn c;
   c.fd = psi_accept(listen_fd);
   return c;
}

/* ── Outbound connect ────────────────────────────────────────────── */

/* Connect to a remote host by name and port.
 * Performs DNS resolution and TCP connect (both fiber-blocking).
 * Returns a tcp_conn with fd < 0 on error. */
static inline tcp_conn tcp_connect(const char* host, int port)
{
   tcp_conn c;
   int len = 0;
   while (host[len]) ++len;
   c.fd = psi_connect(host, len, port);
   return c;
}

/* ── Reading ─────────────────────────────────────────────────────── */

/* Read up to len bytes.  Returns bytes read, 0 on EOF, <0 on error. */
static inline int tcp_read(tcp_conn* c, char* buf, int len)
{
   return psi_read(c->fd, buf, len);
}

/* Read exactly len bytes (loops until complete).
 * Returns total bytes read, or <0 on error. */
static inline int tcp_read_all(tcp_conn* c, char* buf, int len)
{
   int total = 0;
   while (total < len)
   {
      int n = psi_read(c->fd, buf + total, len - total);
      if (n <= 0) return n == 0 ? total : n;
      total += n;
   }
   return total;
}

/* Read until a line terminator (\n) is found or buffer is full.
 * Returns length including the \n, 0 on EOF, <0 on error.
 * The line is NOT null-terminated. */
static inline int tcp_read_line(tcp_conn* c, char* buf, int max_len)
{
   int pos = 0;
   while (pos < max_len)
   {
      int n = psi_read(c->fd, buf + pos, 1);
      if (n <= 0) return n == 0 ? pos : n;
      ++pos;
      if (buf[pos - 1] == '\n') break;
   }
   return pos;
}

/* ── Writing ─────────────────────────────────────────────────────── */

/* Write up to len bytes.  Returns bytes written, or <0 on error. */
static inline int tcp_write(tcp_conn* c, const char* buf, int len)
{
   return psi_write(c->fd, buf, len);
}

/* Write all len bytes (loops until complete). */
static inline void tcp_write_all(tcp_conn* c, const char* buf, int len)
{
   psi_write_all(c->fd, buf, len);
}

/* Write a null-terminated string. */
static inline void tcp_write_str(tcp_conn* c, const char* s)
{
   int len = 0;
   while (s[len]) ++len;
   psi_write_all(c->fd, s, len);
}

/* ── Batching ────────────────────────────────────────────────────── */

/* Enable TCP cork — batches small writes into one TCP segment. */
static inline void tcp_cork(tcp_conn* c) { psi_cork(c->fd); }

/* Disable TCP cork — flushes batched data. */
static inline void tcp_uncork(tcp_conn* c) { psi_uncork(c->fd); }

/* ── Lifecycle ───────────────────────────────────────────────────── */

/* Close the connection. */
static inline void tcp_close(tcp_conn* c)
{
   if (c->fd >= 0)
   {
      psi_close(c->fd);
      c->fd = -1;
   }
}

/* Check if the connection is valid. */
static inline int tcp_is_open(const tcp_conn* c)
{
   return c->fd >= 0;
}

/* ── Fiber helpers ───────────────────────────────────────────────── */

/* Spawn a fiber to handle a connection.
 * The handler receives the connection fd as its argument.
 * The caller should NOT close the connection — the handler owns it. */
static inline void tcp_spawn(void (*handler)(int), tcp_conn c)
{
   psi_spawn(handler, c.fd);
}

#ifdef __cplusplus
}
#endif

#endif /* PSI_TCP_H */
