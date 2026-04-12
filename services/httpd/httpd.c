/*
 * httpd.c — Static file HTTP/1.1 server for psiserve
 *
 * Uses psi-sdk host declarations.  Spawns per-connection fibers for
 * concurrent request handling.  Uses sendfile for zero-copy file
 * transfer and cork/uncork to batch response headers.
 *
 *   fd 0 = listen socket (pre-opened by runtime)
 *   fd 1 = webroot directory (pre-opened by runtime)
 *
 * Compile:
 *   clang --target=wasm32 -nostdlib -O2 -fno-builtin \
 *       -I libraries/psi-sdk/include \
 *       -Wl,--no-entry -Wl,--export=_start \
 *       -Wl,--export=__stack_pointer \
 *       -Wl,--export=handle_connection \
 *       -Wl,--export-table \
 *       -o services/httpd/httpd.wasm services/httpd/httpd.c
 */

#include <psi/host.h>

/* ── Utilities ────────────────────────────────────────────────────── */

static int str_len(const char* s)
{
   int n = 0;
   while (s[n]) ++n;
   return n;
}

static int str_eq(const char* a, const char* b, int len)
{
   for (int i = 0; i < len; ++i)
      if (a[i] != b[i]) return 0;
   return 1;
}

static void write_str(int fd, const char* s)
{
   psi_write_all(fd, s, str_len(s));
}

static int itoa(unsigned long long val, char* buf, int buflen)
{
   if (val == 0)
   {
      buf[0] = '0';
      return 1;
   }
   char tmp[20];
   int  n = 0;
   while (val > 0 && n < 20)
   {
      tmp[n++] = '0' + (val % 10);
      val /= 10;
   }
   if (n > buflen) n = buflen;
   for (int i = 0; i < n; ++i)
      buf[i] = tmp[n - 1 - i];
   return n;
}

/* ── MIME type detection ──────────────────────────────────────────── */

static const char* content_type_for(const char* path, int path_len)
{
   int dot = -1;
   for (int i = path_len - 1; i >= 0; --i)
   {
      if (path[i] == '.') { dot = i; break; }
      if (path[i] == '/') break;
   }

   if (dot < 0)
      return "application/octet-stream";

   int ext_len = path_len - dot - 1;
   const char* ext = path + dot + 1;

   if (ext_len == 4 && str_eq(ext, "html", 4)) return "text/html; charset=utf-8";
   if (ext_len == 3 && str_eq(ext, "htm", 3))  return "text/html; charset=utf-8";
   if (ext_len == 3 && str_eq(ext, "css", 3))  return "text/css; charset=utf-8";
   if (ext_len == 2 && str_eq(ext, "js", 2))   return "text/javascript; charset=utf-8";
   if (ext_len == 4 && str_eq(ext, "json", 4)) return "application/json";
   if (ext_len == 3 && str_eq(ext, "png", 3))  return "image/png";
   if (ext_len == 3 && str_eq(ext, "jpg", 3))  return "image/jpeg";
   if (ext_len == 4 && str_eq(ext, "jpeg", 4)) return "image/jpeg";
   if (ext_len == 3 && str_eq(ext, "gif", 3))  return "image/gif";
   if (ext_len == 3 && str_eq(ext, "svg", 3))  return "image/svg+xml";
   if (ext_len == 3 && str_eq(ext, "ico", 3))  return "image/x-icon";
   if (ext_len == 3 && str_eq(ext, "txt", 3))  return "text/plain; charset=utf-8";
   if (ext_len == 3 && str_eq(ext, "xml", 3))  return "application/xml";
   if (ext_len == 4 && str_eq(ext, "wasm", 4)) return "application/wasm";
   if (ext_len == 3 && str_eq(ext, "pdf", 3))  return "application/pdf";
   if (ext_len == 4 && str_eq(ext, "woff", 4)) return "font/woff";
   if (ext_len == 5 && str_eq(ext, "woff2", 5))return "font/woff2";

   return "application/octet-stream";
}

/* ── Request handling ─────────────────────────────────────────────── */

static void send_error(int conn, const char* status, const char* body)
{
   psi_cork(conn);
   write_str(conn, "HTTP/1.1 ");
   write_str(conn, status);
   write_str(conn, "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
   write_str(conn, "<h1>");
   write_str(conn, status);
   write_str(conn, "</h1><p>");
   write_str(conn, body);
   write_str(conn, "</p>\n");
   psi_uncork(conn);
}

/* Returns 1 to keep connection alive, 0 to close */
static int handle_request(int conn, const char* method, int method_len,
                          const char* path, int path_len, int keep_alive)
{
   if (method_len != 3 || !str_eq(method, "GET", 3))
   {
      send_error(conn, "405 Method Not Allowed", "Only GET is supported.");
      return 0;
   }

   const char* file_path = path;
   int         file_path_len = path_len;
   if (path_len == 1 && path[0] == '/')
   {
      file_path = "/index.html";
      file_path_len = 11;
   }

   int file_fd = psi_open(1, file_path, file_path_len);
   if (file_fd < 0)
   {
      send_error(conn, "404 Not Found", "The requested file was not found.");
      return 0;
   }

   unsigned long long file_size = 0;
   if (psi_fstat(file_fd, &file_size) < 0)
   {
      psi_close(file_fd);
      send_error(conn, "500 Internal Server Error", "Could not stat file.");
      return 0;
   }

   /* Send response headers — corked so they go in one TCP segment */
   const char* ctype = content_type_for(file_path, file_path_len);
   char size_str[20];
   int  size_str_len = itoa(file_size, size_str, sizeof(size_str));

   psi_cork(conn);
   write_str(conn, "HTTP/1.1 200 OK\r\nContent-Type: ");
   write_str(conn, ctype);
   write_str(conn, "\r\nContent-Length: ");
   psi_write_all(conn, size_str, size_str_len);
   if (keep_alive)
      write_str(conn, "\r\nConnection: keep-alive\r\n\r\n");
   else
      write_str(conn, "\r\nConnection: close\r\n\r\n");
   psi_uncork(conn);

   /* Send file body — zero-copy via OS sendfile */
   psi_sendfile(conn, file_fd, (long long)file_size);

   psi_close(file_fd);
   return keep_alive;
}

/* ── HTTP request parser ──────────────────────────────────────────── */

/* Case-insensitive byte compare for header matching */
static int lower(int c)
{
   if (c >= 'A' && c <= 'Z') return c + 32;
   return c;
}

/* Check if the request has "Connection: close" header */
static int wants_close(const char* buf, int len)
{
   /* Search for "\r\nConnection:" (case-insensitive) */
   const char* target = "connection:";
   int tlen = 11;

   for (int i = 0; i + tlen < len; ++i)
   {
      if (buf[i] == '\r' && buf[i+1] == '\n')
      {
         int match = 1;
         for (int j = 0; j < tlen; ++j)
         {
            if (lower(buf[i+2+j]) != target[j]) { match = 0; break; }
         }
         if (match)
         {
            /* Skip whitespace after colon */
            int v = i + 2 + tlen;
            while (v < len && buf[v] == ' ') ++v;
            /* Check for "close" */
            if (v + 5 <= len &&
                lower(buf[v]) == 'c' && lower(buf[v+1]) == 'l' &&
                lower(buf[v+2]) == 'o' && lower(buf[v+3]) == 's' &&
                lower(buf[v+4]) == 'e')
               return 1;
         }
      }
   }
   return 0;
}

static void process_connection(int conn)
{
   /* Per-fiber buffer — each fiber has its own WASM stack,
    * so concurrent connections don't corrupt each other. */
   char req_buf[4096];
   int  total = 0;

   for (;;)
   {
      /* Read until we have a complete header block (\r\n\r\n) */
      int header_end = -1;
      while (total < (int)sizeof(req_buf) - 1)
      {
         int n = psi_read(conn, req_buf + total, (int)sizeof(req_buf) - 1 - total);
         if (n <= 0) return;
         total += n;

         for (int i = (total - n > 3 ? total - n - 3 : 0); i + 3 < total; ++i)
         {
            if (req_buf[i] == '\r' && req_buf[i+1] == '\n' &&
                req_buf[i+2] == '\r' && req_buf[i+3] == '\n')
            {
               header_end = i + 4;
               goto headers_done;
            }
         }
      }
      return;  /* headers too large */

   headers_done:;
      /* Parse request line */
      int method_end = 0;
      while (method_end < total && req_buf[method_end] != ' ') ++method_end;

      int path_start = method_end + 1;
      int path_end = path_start;
      while (path_end < total && req_buf[path_end] != ' ' && req_buf[path_end] != '?')
         ++path_end;

      /* Detect HTTP/1.1 keep-alive (default on) vs Connection: close */
      int keep_alive = !wants_close(req_buf, header_end);

      if (method_end > 0 && path_end > path_start)
      {
         int alive = handle_request(conn,
                        req_buf, method_end,
                        req_buf + path_start, path_end - path_start,
                        keep_alive);
         if (!alive)
            return;
      }
      else
      {
         send_error(conn, "400 Bad Request", "Could not parse request.");
         return;
      }

      /* Shift leftover data (pipelined request bytes) to front of buffer */
      int leftover = total - header_end;
      if (leftover > 0)
      {
         for (int i = 0; i < leftover; ++i)
            req_buf[i] = req_buf[header_end + i];
      }
      total = leftover;
   }
}

/* ── Per-connection fiber handler ─────────────────────────────────── */

void handle_connection(int conn)
{
   process_connection(conn);
   psi_close(conn);
}

/* ── Main ─────────────────────────────────────────────────────────── */

void _start(void)
{
   for (;;)
   {
      int conn = psi_accept(0);
      if (conn < 0) return;

      psi_spawn(handle_connection, conn);
   }
}
