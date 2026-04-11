/*
 * httpd.c — Static file HTTP/1.1 server for psiserve
 *
 * Imports psi.accept, psi.read, psi.write, psi.close, psi.open, psi.fstat.
 *   fd 0 = listen socket (pre-opened by runtime)
 *   fd 1 = webroot directory (pre-opened by runtime)
 *
 * Compile:
 *   clang --target=wasm32 -nostdlib -O2 \
 *       -Wl,--no-entry -Wl,--export=_start \
 *       -o httpd.wasm httpd.c
 */

/* ── Host imports ─────────────────────────────────────────────────────────── */

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

/* ── Utilities ────────────────────────────────────────────────────────────── */

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

static void write_all(int fd, const char* buf, int len)
{
   while (len > 0)
   {
      int n = psi_write(fd, buf, len);
      if (n <= 0) break;
      buf += n;
      len -= n;
   }
}

static void write_str(int fd, const char* s)
{
   write_all(fd, s, str_len(s));
}

/* Simple integer to decimal string, returns length written */
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

/* ── MIME type detection ──────────────────────────────────────────────────── */

static const char* content_type_for(const char* path, int path_len)
{
   /* Find last '.' */
   int dot = -1;
   for (int i = path_len - 1; i >= 0; --i)
   {
      if (path[i] == '.')
      {
         dot = i;
         break;
      }
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

/* ── Buffers ──────────────────────────────────────────────────────────────── */

static char req_buf[8192];    /* HTTP request read buffer */
static char io_buf[16384];    /* File read / response write buffer */

/* ── Request handling ─────────────────────────────────────────────────────── */

static void send_error(int conn, const char* status, const char* body)
{
   write_str(conn, "HTTP/1.1 ");
   write_str(conn, status);
   write_str(conn, "\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n");
   write_str(conn, "<h1>");
   write_str(conn, status);
   write_str(conn, "</h1><p>");
   write_str(conn, body);
   write_str(conn, "</p>\n");
}

static void handle_request(int conn, const char* method, int method_len,
                           const char* path, int path_len)
{
   /* Only GET supported */
   if (method_len != 3 || !str_eq(method, "GET", 3))
   {
      send_error(conn, "405 Method Not Allowed", "Only GET is supported.");
      return;
   }

   /* Default / to /index.html */
   const char* file_path = path;
   int         file_path_len = path_len;
   if (path_len == 1 && path[0] == '/')
   {
      file_path = "/index.html";
      file_path_len = 11;
   }

   /* Open file relative to webroot (fd 1) */
   int file_fd = psi_open(1, file_path, file_path_len);
   if (file_fd < 0)
   {
      send_error(conn, "404 Not Found", "The requested file was not found.");
      return;
   }

   /* Get file size */
   unsigned long long file_size = 0;
   if (psi_fstat(file_fd, &file_size) < 0)
   {
      psi_close(file_fd);
      send_error(conn, "500 Internal Server Error", "Could not stat file.");
      return;
   }

   /* Send response headers */
   const char* ctype = content_type_for(file_path, file_path_len);
   char size_str[20];
   int  size_str_len = itoa(file_size, size_str, sizeof(size_str));

   write_str(conn, "HTTP/1.1 200 OK\r\nContent-Type: ");
   write_str(conn, ctype);
   write_str(conn, "\r\nContent-Length: ");
   write_all(conn, size_str, size_str_len);
   write_str(conn, "\r\nConnection: close\r\n\r\n");

   /* Send file body */
   for (;;)
   {
      int n = psi_read(file_fd, io_buf, sizeof(io_buf));
      if (n <= 0) break;
      write_all(conn, io_buf, n);
   }

   psi_close(file_fd);
}

/* ── HTTP request parser (minimal) ────────────────────────────────────────── */

/*
 * Reads bytes until we have a complete request line (first line of HTTP).
 * Extracts method and path.  Discards headers (reads until blank line).
 */
static void process_connection(int conn)
{
   int total = 0;

   /* Read until we have the full request headers (ending with \r\n\r\n) */
   while (total < (int)sizeof(req_buf) - 1)
   {
      int n = psi_read(conn, req_buf + total, (int)sizeof(req_buf) - 1 - total);
      if (n <= 0) return;  /* Client disconnected */
      total += n;

      /* Check for end of headers */
      for (int i = 0; i + 3 < total; ++i)
      {
         if (req_buf[i] == '\r' && req_buf[i+1] == '\n' &&
             req_buf[i+2] == '\r' && req_buf[i+3] == '\n')
            goto headers_done;
      }
   }

headers_done:
   /* Parse request line: METHOD SP PATH SP HTTP/x.x */
   {
      int method_start = 0;
      int method_end = 0;
      while (method_end < total && req_buf[method_end] != ' ') ++method_end;

      int path_start = method_end + 1;
      int path_end = path_start;
      while (path_end < total && req_buf[path_end] != ' ' && req_buf[path_end] != '?')
         ++path_end;

      if (method_end > method_start && path_end > path_start)
      {
         handle_request(conn,
                        req_buf + method_start, method_end - method_start,
                        req_buf + path_start, path_end - path_start);
      }
      else
      {
         send_error(conn, "400 Bad Request", "Could not parse request.");
      }
   }
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

void _start(void)
{
   for (;;)
   {
      int conn = psi_accept(0);
      if (conn < 0) return;

      process_connection(conn);
      psi_close(conn);
   }
}
