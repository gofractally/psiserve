/*
 * proxy.c — HTTP forward proxy for psiserve
 *
 * Supports:
 *   - CONNECT tunneling (HTTPS) — bidirectional byte shoveling
 *   - Plain HTTP forwarding — rewrites absolute URLs to relative
 *
 * Point your browser's HTTP proxy to localhost:<port> and browse the web.
 *
 *   fd 0 = listen socket (pre-opened by runtime)
 *
 * Compile:
 *   clang --target=wasm32 -nostdlib -O2 -fno-builtin \
 *       -I libraries/psi-sdk/include \
 *       -Wl,--no-entry -Wl,--export=_start \
 *       -Wl,--export=__stack_pointer \
 *       -Wl,--export=handle_connection \
 *       -Wl,--export=shovel \
 *       -Wl,--export-table \
 *       -o services/proxy/proxy.wasm services/proxy/proxy.c
 */

#include <psi/http.h>

/* ── Bidirectional byte shovel ───────────────────────────────────── */

/* Fiber entry: read from src, write to dst until EOF.
 * arg encodes two fds: (src << 16) | (dst & 0xFFFF). */
void shovel(int arg)
{
   int src = arg >> 16;
   int dst = arg & 0xFFFF;
   char buf[8192];

   for (;;)
   {
      int n = psi_read(src, buf, sizeof(buf));
      if (n <= 0) break;
      psi_write_all(dst, buf, n);
   }

   psi_close(src);
   psi_close(dst);
}

/* ── CONNECT tunnel ──────────────────────────────────────────────── */

static void handle_connect(tcp_conn* client, const http_request* req)
{
   /* Connect to upstream */
   char host_buf[256];
   if (req->host_len >= (int)sizeof(host_buf)) goto fail;
   for (int i = 0; i < req->host_len; ++i)
      host_buf[i] = req->host[i];
   host_buf[req->host_len] = '\0';

   tcp_conn upstream = tcp_connect(host_buf, req->port);
   if (!tcp_is_open(&upstream)) goto fail;

   /* Tell client the tunnel is established */
   tcp_write_str(client, "HTTP/1.1 200 Connection Established\r\n\r\n");

   /* Spawn a fiber to shovel upstream→client */
   int arg = (upstream.fd << 16) | (client->fd & 0xFFFF);
   psi_spawn(shovel, arg);

   /* This fiber shovels client→upstream */
   {
      char buf[8192];
      for (;;)
      {
         int n = tcp_read(client, buf, sizeof(buf));
         if (n <= 0) break;
         tcp_write_all(&upstream, buf, n);
      }
   }

   tcp_close(&upstream);
   tcp_close(client);
   return;

fail:
   tcp_write_str(client, "HTTP/1.1 502 Bad Gateway\r\n\r\nCould not connect to upstream.\n");
   tcp_close(client);
}

/* ── Plain HTTP forward ──────────────────────────────────────────── */

static void handle_http_forward(tcp_conn* client, const http_request* req,
                                const char* headers_buf, int headers_len)
{
   if (!req->host || req->host_len == 0) goto bad;

   /* Connect to upstream */
   char host_buf[256];
   if (req->host_len >= (int)sizeof(host_buf)) goto bad;
   for (int i = 0; i < req->host_len; ++i)
      host_buf[i] = req->host[i];
   host_buf[req->host_len] = '\0';

   {
      tcp_conn upstream = tcp_connect(host_buf, req->port ? req->port : 80);
      if (!tcp_is_open(&upstream)) goto bad;

      /* Rewrite request line: absolute URL → relative path */
      int path_len;
      const char* path = http_path(req, &path_len);

      tcp_cork(&upstream);

      /* Send: METHOD SP /path SP HTTP/1.1\r\n */
      tcp_write_all(&upstream, req->method, req->method_len);
      tcp_write_str(&upstream, " ");
      tcp_write_all(&upstream, path, path_len);
      tcp_write_str(&upstream, " ");
      tcp_write_all(&upstream, req->version, req->version_len);
      tcp_write_str(&upstream, "\r\n");

      /* Forward headers, filtering out hop-by-hop headers and forcing
       * Connection: close so the upstream closes after the response. */
      {
         /* Find first \r\n (end of request line) */
         int hdr_start = 0;
         while (hdr_start + 1 < headers_len)
         {
            if (headers_buf[hdr_start] == '\r' && headers_buf[hdr_start + 1] == '\n')
            {
               hdr_start += 2;
               break;
            }
            ++hdr_start;
         }

         /* Walk each header line, skip Connection/Proxy-Connection */
         int pos = hdr_start;
         while (pos + 1 < headers_len)
         {
            /* End of headers? */
            if (headers_buf[pos] == '\r' && headers_buf[pos + 1] == '\n')
               break;

            /* Find end of this line */
            int line_start = pos;
            while (pos + 1 < headers_len &&
                   !(headers_buf[pos] == '\r' && headers_buf[pos + 1] == '\n'))
               ++pos;
            int line_len = pos - line_start;
            pos += 2;  /* skip \r\n */

            /* Check if this header should be filtered */
            int skip = 0;
            if (line_len >= 11 && _mem_eq_ci(headers_buf + line_start, "Connection:", 11))
               skip = 1;
            if (line_len >= 17 && _mem_eq_ci(headers_buf + line_start, "Proxy-Connection:", 17))
               skip = 1;

            if (!skip)
            {
               tcp_write_all(&upstream, headers_buf + line_start, line_len);
               tcp_write_str(&upstream, "\r\n");
            }
         }

         /* Add Connection: close and end headers */
         tcp_write_str(&upstream, "Connection: close\r\n\r\n");
      }

      tcp_uncork(&upstream);

      /* Pipe response back: read from upstream, write to client.
       * For simplicity, just shovel bytes until upstream closes. */
      {
         char buf[8192];
         for (;;)
         {
            int n = tcp_read(&upstream, buf, sizeof(buf));
            if (n <= 0) break;
            tcp_write_all(client, buf, n);
         }
      }

      tcp_close(&upstream);
   }
   return;

bad:
   tcp_write_str(client, "HTTP/1.1 400 Bad Request\r\n\r\nMissing or invalid Host.\n");
}

/* ── Per-connection handler ──────────────────────────────────────── */

void handle_connection(int conn_fd)
{
   tcp_conn client;
   client.fd = conn_fd;

   char         buf[8192];
   http_request req;

   int n = http_read_request(&client, &req, buf, sizeof(buf));
   if (n <= 0)
   {
      tcp_close(&client);
      return;
   }

   if (http_method_is(&req, "CONNECT"))
   {
      handle_connect(&client, &req);
   }
   else
   {
      handle_http_forward(&client, &req, buf, n);
      tcp_close(&client);
   }
}

/* ── Main ────────────────────────────────────────────────────────── */

void _start(void)
{
   for (;;)
   {
      tcp_conn client = tcp_accept(0);
      if (!tcp_is_open(&client)) return;
      tcp_spawn(handle_connection, client);
   }
}
