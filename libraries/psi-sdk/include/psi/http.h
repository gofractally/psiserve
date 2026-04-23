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

   /* http_read_request may read past the header terminator. The
    * overflow bytes (already-buffered body) live at raw + raw_len,
    * with length buf_total - raw_len.  http_read_body uses this to
    * avoid re-reading the socket for data it already has. */
   int         buf_total;
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
   req->raw       = buf;
   req->raw_len   = header_end;
   req->buf_total = total;

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

/* Parse the Content-Length header as an int.  Returns 0 when the
 * header is missing or malformed — callers should treat that as
 * "no body expected". */
static inline int http_content_length(const http_request* req)
{
   int vlen = 0;
   const char* v = http_find_header(req, "Content-Length", 14, &vlen);
   if (!v) return 0;
   int n = 0;
   for (int i = 0; i < vlen; ++i)
   {
      if (v[i] < '0' || v[i] > '9') break;
      n = n * 10 + (v[i] - '0');
   }
   return n;
}

/* Read the request body into `out` (at most out_cap bytes).
 * Consumes both the already-buffered overflow from http_read_request
 * AND any additional bytes still pending on the socket.  Returns the
 * number of bytes written to out (== min(Content-Length, out_cap)),
 * or <0 on I/O error. */
static inline int http_read_body(tcp_conn* c, const http_request* req,
                                 char* out, int out_cap)
{
   int cl = http_content_length(req);
   if (cl <= 0) return 0;
   if (cl > out_cap) cl = out_cap;

   int buffered = req->buf_total - req->raw_len;
   if (buffered < 0) buffered = 0;
   if (buffered > cl) buffered = cl;

   for (int i = 0; i < buffered; ++i)
      out[i] = req->raw[req->raw_len + i];

   int got = buffered;
   while (got < cl)
   {
      int n = tcp_read(c, out + got, cl - got);
      if (n <= 0) return n == 0 ? got : n;
      got += n;
   }
   return got;
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

/* ── Response helpers ─────────────────────────────────────────────── */

/* Send an HTTP response with status code, content type, and body.
 * Handles the full response: status line, headers, body.
 * Uses cork/uncork for efficient TCP segment packing. */
static inline void http_respond(tcp_conn* c, int status,
                                const char* content_type,
                                const char* body, int body_len)
{
   const char* reason = "OK";
   if (status == 200) reason = "OK";
   else if (status == 400) reason = "Bad Request";
   else if (status == 404) reason = "Not Found";
   else if (status == 500) reason = "Internal Server Error";
   else if (status == 503) reason = "Service Unavailable";

   char header[512];
   int hlen = 0;

   /* Status line */
   const char* pre = "HTTP/1.1 ";
   for (int i = 0; pre[i]; ++i) header[hlen++] = pre[i];
   if (status >= 100) header[hlen++] = '0' + (status / 100);
   header[hlen++] = '0' + ((status / 10) % 10);
   header[hlen++] = '0' + (status % 10);
   header[hlen++] = ' ';
   for (int i = 0; reason[i]; ++i) header[hlen++] = reason[i];
   header[hlen++] = '\r'; header[hlen++] = '\n';

   /* Content-Type */
   const char* ct = "Content-Type: ";
   for (int i = 0; ct[i]; ++i) header[hlen++] = ct[i];
   for (int i = 0; content_type[i]; ++i) header[hlen++] = content_type[i];
   header[hlen++] = '\r'; header[hlen++] = '\n';

   /* Content-Length */
   const char* cl = "Content-Length: ";
   for (int i = 0; cl[i]; ++i) header[hlen++] = cl[i];
   {
      char digits[16];
      int dlen = 0;
      int tmp = body_len;
      if (tmp == 0) { digits[dlen++] = '0'; }
      else { while (tmp > 0) { digits[dlen++] = '0' + (tmp % 10); tmp /= 10; } }
      for (int i = dlen - 1; i >= 0; --i) header[hlen++] = digits[i];
   }
   header[hlen++] = '\r'; header[hlen++] = '\n';

   /* Connection: close for simplicity */
   const char* cc = "Connection: close\r\n";
   for (int i = 0; cc[i]; ++i) header[hlen++] = cc[i];

   /* CORS header for browser fetch */
   const char* cors = "Access-Control-Allow-Origin: *\r\n";
   for (int i = 0; cors[i]; ++i) header[hlen++] = cors[i];

   /* End of headers */
   header[hlen++] = '\r'; header[hlen++] = '\n';

   tcp_cork(c);
   tcp_write_all(c, header, hlen);
   if (body_len > 0)
      tcp_write_all(c, body, body_len);
   tcp_uncork(c);
}

/* Convenience: send a string body with a given content type. */
static inline void http_respond_str(tcp_conn* c, int status,
                                    const char* content_type,
                                    const char* body)
{
   int len = 0;
   while (body[len]) ++len;
   http_respond(c, status, content_type, body, len);
}

/* Convenience: send a JSON response. */
static inline void http_respond_json(tcp_conn* c, int status,
                                     const char* json_body)
{
   http_respond_str(c, status, "application/json", json_body);
}

/* Convenience: send an HTML response. */
static inline void http_respond_html(tcp_conn* c, int status,
                                     const char* html_body)
{
   http_respond_str(c, status, "text/html; charset=utf-8", html_body);
}

/* Parse a query string parameter value by key.
 * Searches query_string for "key=value" and returns pointer to value.
 * Sets *out_len to the value length. Returns NULL if not found.
 * Does not decode URL encoding. */
static inline const char* http_query_param(const char* query, int query_len,
                                           const char* key, int key_len,
                                           int* out_len)
{
   for (int i = 0; i + key_len < query_len; ++i)
   {
      if ((i == 0 || query[i-1] == '&') &&
          _mem_eq(query + i, key, key_len) &&
          query[i + key_len] == '=')
      {
         int vstart = i + key_len + 1;
         int vend = vstart;
         while (vend < query_len && query[vend] != '&') ++vend;
         *out_len = vend - vstart;
         return query + vstart;
      }
   }
   *out_len = 0;
   return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* PSI_HTTP_H */
