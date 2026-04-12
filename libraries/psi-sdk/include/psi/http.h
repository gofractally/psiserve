/*
 * psi/http.h — Minimal HTTP/1.x helpers for psi WASM services.
 *
 * Provides request-line parsing and header reading on top of tcp_conn.
 * Not a full HTTP library — just enough for proxies and simple servers.
 */

#ifndef PSI_HTTP_H
#define PSI_HTTP_H

#include <psi/tcp.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── HTTP request (parsed from raw bytes) ────────────────────────── */

typedef struct
{
   /* Pointers into the header buffer (NOT null-terminated) */
   const char* method;
   int         method_len;
   const char* url;        /* full URL: path for direct, absolute for proxy */
   int         url_len;
   const char* version;    /* "HTTP/1.0" or "HTTP/1.1" */
   int         version_len;

   /* Parsed from URL (for CONNECT or absolute-URI proxy requests) */
   const char* host;
   int         host_len;
   int         port;       /* 0 if not specified */

   /* Full header block including request line */
   const char* raw;
   int         raw_len;    /* total bytes up to and including \r\n\r\n */
} http_request;

/* ── Parsing ─────────────────────────────────────────────────────── */

static int _lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

static int _mem_eq_ci(const char* a, const char* b, int len)
{
   for (int i = 0; i < len; ++i)
      if (_lower(a[i]) != _lower(b[i])) return 0;
   return 1;
}

static int _mem_eq(const char* a, const char* b, int len)
{
   for (int i = 0; i < len; ++i)
      if (a[i] != b[i]) return 0;
   return 1;
}

/* Read HTTP headers from a connection into buf.
 * Returns total bytes read (including \r\n\r\n), or <=0 on error/EOF.
 * Populates req with parsed fields. */
static inline int http_read_request(tcp_conn* c, http_request* req,
                                    char* buf, int buf_size)
{
   int total = 0;
   int header_end = -1;

   while (total < buf_size - 1)
   {
      int n = tcp_read(c, buf + total, buf_size - 1 - total);
      if (n <= 0) return n == 0 ? total : n;
      total += n;

      /* Scan for \r\n\r\n */
      int scan_from = (total - n > 3) ? (total - n - 3) : 0;
      for (int i = scan_from; i + 3 < total; ++i)
      {
         if (buf[i] == '\r' && buf[i+1] == '\n' &&
             buf[i+2] == '\r' && buf[i+3] == '\n')
         {
            header_end = i + 4;
            goto done;
         }
      }
   }
   return -1; /* headers too large */

done:
   req->raw     = buf;
   req->raw_len = header_end;

   /* Parse request line: METHOD SP URL SP VERSION CRLF */
   int pos = 0;

   /* Method */
   req->method = buf;
   while (pos < header_end && buf[pos] != ' ') ++pos;
   req->method_len = pos;
   ++pos; /* skip space */

   /* URL */
   req->url = buf + pos;
   while (pos < header_end && buf[pos] != ' ') ++pos;
   req->url_len = pos - (int)(req->url - buf);
   ++pos; /* skip space */

   /* Version */
   req->version = buf + pos;
   while (pos < header_end && buf[pos] != '\r') ++pos;
   req->version_len = pos - (int)(req->version - buf);

   /* Parse host:port from URL */
   req->host     = 0;
   req->host_len = 0;
   req->port     = 0;

   /* CONNECT method: URL is host:port */
   if (req->method_len == 7 && _mem_eq(req->method, "CONNECT", 7))
   {
      req->host = req->url;
      /* Find colon separating host:port */
      for (int i = 0; i < req->url_len; ++i)
      {
         if (req->url[i] == ':')
         {
            req->host_len = i;
            /* Parse port */
            int p = 0;
            for (int j = i + 1; j < req->url_len; ++j)
               p = p * 10 + (req->url[j] - '0');
            req->port = p;
            break;
         }
      }
      if (req->host_len == 0)
      {
         req->host_len = req->url_len;
         req->port     = 443; /* default for CONNECT */
      }
   }
   /* Absolute URI: http://host[:port]/path */
   else if (req->url_len > 7 && _mem_eq_ci(req->url, "http://", 7))
   {
      const char* h = req->url + 7;
      int hlen = req->url_len - 7;

      /* Find end of host (: or / or end) */
      int host_end = 0;
      while (host_end < hlen && h[host_end] != ':' && h[host_end] != '/')
         ++host_end;

      req->host     = h;
      req->host_len = host_end;
      req->port     = 80;

      if (host_end < hlen && h[host_end] == ':')
      {
         int p = 0;
         int j = host_end + 1;
         while (j < hlen && h[j] != '/')
            p = p * 10 + (h[j++] - '0');
         req->port = p;
      }
   }

   return header_end;
}

/* Find a header value by name (case-insensitive).
 * Returns pointer to the value (after ": "), sets *out_len.
 * Returns NULL if not found. */
static inline const char* http_find_header(const http_request* req,
                                           const char* name, int name_len,
                                           int* out_len)
{
   const char* buf = req->raw;
   int total = req->raw_len;

   for (int i = 0; i + name_len + 1 < total; ++i)
   {
      if (buf[i] == '\r' && buf[i+1] == '\n')
      {
         int h = i + 2;
         if (h + name_len + 1 < total && buf[h + name_len] == ':' &&
             _mem_eq_ci(buf + h, name, name_len))
         {
            int v = h + name_len + 1;
            while (v < total && buf[v] == ' ') ++v;
            int ve = v;
            while (ve < total && buf[ve] != '\r') ++ve;
            *out_len = ve - v;
            return buf + v;
         }
      }
   }
   *out_len = 0;
   return 0;
}

/* Check if the request method matches (exact, case-sensitive). */
static inline int http_method_is(const http_request* req,
                                 const char* method)
{
   int len = 0;
   while (method[len]) ++len;
   return req->method_len == len && _mem_eq(req->method, method, len);
}

/* Extract the path from an absolute URL (for forwarding).
 * For "http://host:port/path", returns pointer to "/path".
 * For non-absolute URLs, returns the URL as-is. */
static inline const char* http_path(const http_request* req, int* out_len)
{
   if (req->url_len > 7 && _mem_eq_ci(req->url, "http://", 7))
   {
      const char* p = req->url + 7;
      int remaining = req->url_len - 7;
      int i = 0;
      while (i < remaining && p[i] != '/') ++i;
      if (i < remaining)
      {
         *out_len = remaining - i;
         return p + i;
      }
      /* No path — use "/" */
      *out_len = 1;
      return "/";
   }
   *out_len = req->url_len;
   return req->url;
}

#ifdef __cplusplus
}
#endif

#endif /* PSI_HTTP_H */
