/*
 * websocket.c — WebSocket server implementation for the psi platform.
 *
 * Ties together SHA-1, Base64, and the websocket frame parser to provide
 * a simple WebSocket API over psi_read/psi_write.
 */

#include "freestanding.h"
#include "sha1_vendor.h"
#include "base64_vendor.h"
#include "ws_parser_vendor.h"
#include <psi/host.h>
#include <psi/websocket.h>

/* ── String utilities (no libc) ──────────────────────────────────── */

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

/* Case-insensitive compare for ASCII */
static int str_ieq(const char* a, const char* b, int len)
{
   for (int i = 0; i < len; ++i)
   {
      char ca = a[i];
      char cb = b[i];
      if (ca >= 'A' && ca <= 'Z') ca += 32;
      if (cb >= 'A' && cb <= 'Z') cb += 32;
      if (ca != cb) return 0;
   }
   return 1;
}

/* Find a substring in a buffer (case-insensitive for header names) */
static const char* find_header(const char* buf, int buf_len, const char* name, int name_len)
{
   for (int i = 0; i + name_len < buf_len; ++i)
   {
      if (buf[i] == '\n' && str_ieq(buf + i + 1, name, name_len))
         return buf + i + 1 + name_len;
   }
   /* Also check start of buffer */
   if (buf_len >= name_len && str_ieq(buf, name, name_len))
      return buf + name_len;
   return 0;
}

/* Extract header value (stops at \r or \n), returns length */
static int extract_header_value(const char* start, char* out, int out_max)
{
   /* Skip whitespace after colon */
   while (*start == ' ' || *start == '\t') start++;

   int n = 0;
   while (start[n] && start[n] != '\r' && start[n] != '\n' && n < out_max - 1)
   {
      out[n] = start[n];
      n++;
   }
   out[n] = '\0';
   return n;
}

/* ── WebSocket handshake ─────────────────────────────────────────── */

/* The magic GUID from RFC 6455 */
static const char ws_magic[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

int psi_ws_accept(int conn_fd)
{
   /* Read HTTP request */
   char req_buf[2048];
   int total = 0;

   while (total < (int)sizeof(req_buf) - 1)
   {
      int n = psi_read(conn_fd, req_buf + total, (int)sizeof(req_buf) - 1 - total);
      if (n <= 0) return -1;
      total += n;

      /* Check for end of headers (\r\n\r\n) */
      for (int i = 0; i + 3 < total; ++i)
      {
         if (req_buf[i] == '\r' && req_buf[i+1] == '\n' &&
             req_buf[i+2] == '\r' && req_buf[i+3] == '\n')
            goto headers_done;
      }
   }
   return -1;  /* Headers too large */

headers_done:
   req_buf[total] = '\0';

   /* Verify it's a WebSocket upgrade request */
   const char* upgrade = find_header(req_buf, total, "Upgrade:", 8);
   if (!upgrade) return -1;

   char upgrade_val[32];
   extract_header_value(upgrade, upgrade_val, sizeof(upgrade_val));
   if (!str_ieq(upgrade_val, "websocket", 9)) return -1;

   /* Extract Sec-WebSocket-Key */
   const char* key_hdr = find_header(req_buf, total, "Sec-WebSocket-Key:", 18);
   if (!key_hdr) return -1;

   char ws_key[64];
   int key_len = extract_header_value(key_hdr, ws_key, sizeof(ws_key));
   if (key_len == 0) return -1;

   /* Compute Sec-WebSocket-Accept = Base64(SHA1(key + magic)) */
   /* Concatenate key + magic GUID */
   char concat[128];
   int magic_len = str_len(ws_magic);
   if (key_len + magic_len >= (int)sizeof(concat)) return -1;

   psi_memcpy(concat, ws_key, key_len);
   psi_memcpy(concat + key_len, ws_magic, magic_len);

   /* SHA-1 hash */
   unsigned char sha1_hash[20];
   SHA1_CTX sha_ctx;
   sha1_init(&sha_ctx);
   sha1_update(&sha_ctx, (const unsigned char*)concat, key_len + magic_len);
   sha1_final(&sha_ctx, sha1_hash);

   /* Base64 encode */
   char accept_b64[32];
   bintob64(accept_b64, sha1_hash, 20);

   /* Build HTTP 101 response */
   const char resp_prefix[] =
      "HTTP/1.1 101 Switching Protocols\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Accept: ";
   const char resp_suffix[] = "\r\n\r\n";

   psi_write_all(conn_fd, resp_prefix, sizeof(resp_prefix) - 1);
   psi_write_all(conn_fd, accept_b64, str_len(accept_b64));
   psi_write_all(conn_fd, resp_suffix, sizeof(resp_suffix) - 1);

   return 0;
}

/* ── Frame send ──────────────────────────────────────────────────── */

static int ws_send_frame(int conn_fd, int opcode, const char* data, int len)
{
   char frame[10 + 4];  /* max header: 10 bytes + no mask */
   websocket_flags flags = (websocket_flags)(opcode | WS_FIN);

   size_t frame_size = websocket_calc_frame_size(flags, len);
   /* We need a buffer for the full frame (header + payload) */
   /* For small messages, use stack. For large, build header separately. */

   if (len <= 4096)
   {
      char buf[4096 + 14];
      size_t total = websocket_build_frame(buf, flags, 0, data, len);
      psi_write_all(conn_fd, buf, (int)total);
   }
   else
   {
      /* Build header only, then send payload separately */
      /* Header: 2-10 bytes depending on length */
      char hdr[14];
      int hdr_len = 2;
      hdr[0] = (char)(0x80 | (opcode & 0x0F));

      if (len < 126)
      {
         hdr[1] = (char)len;
      }
      else if (len <= 0xFFFF)
      {
         hdr[1] = 126;
         hdr[2] = (char)(len >> 8);
         hdr[3] = (char)(len & 0xFF);
         hdr_len = 4;
      }
      else
      {
         hdr[1] = 127;
         hdr[2] = 0; hdr[3] = 0; hdr[4] = 0; hdr[5] = 0;
         hdr[6] = (char)((len >> 24) & 0xFF);
         hdr[7] = (char)((len >> 16) & 0xFF);
         hdr[8] = (char)((len >> 8)  & 0xFF);
         hdr[9] = (char)((len)       & 0xFF);
         hdr_len = 10;
      }

      psi_write_all(conn_fd, hdr, hdr_len);
      psi_write_all(conn_fd, data, len);
   }

   return 0;
}

int psi_ws_send_text(int conn_fd, const char* data, int len)
{
   return ws_send_frame(conn_fd, WS_OP_TEXT, data, len);
}

int psi_ws_send_binary(int conn_fd, const char* data, int len)
{
   return ws_send_frame(conn_fd, WS_OP_BINARY, data, len);
}

int psi_ws_send_close(int conn_fd)
{
   return ws_send_frame(conn_fd, WS_OP_CLOSE, 0, 0);
}

/* ── Frame receive ───────────────────────────────────────────────── */

int psi_ws_recv(int conn_fd, char* buf, int buf_len, int* opcode)
{
   /* Read frame header (2 bytes minimum) */
   unsigned char hdr[2];
   int n = psi_read(conn_fd, hdr, 2);
   if (n <= 0) return n;
   if (n < 2)
   {
      /* Read the second byte */
      int n2 = psi_read(conn_fd, hdr + 1, 1);
      if (n2 <= 0) return -1;
   }

   int op = hdr[0] & 0x0F;
   int masked = (hdr[1] & 0x80) != 0;
   int payload_len = hdr[1] & 0x7F;

   /* Extended length */
   if (payload_len == 126)
   {
      unsigned char ext[2];
      n = psi_read(conn_fd, ext, 2);
      if (n < 2) return -1;
      payload_len = (ext[0] << 8) | ext[1];
   }
   else if (payload_len == 127)
   {
      unsigned char ext[8];
      n = psi_read(conn_fd, ext, 8);
      if (n < 8) return -1;
      /* Only use lower 32 bits — we don't handle >4GB frames */
      payload_len = (ext[4] << 24) | (ext[5] << 16) | (ext[6] << 8) | ext[7];
   }

   /* Read mask key (4 bytes if masked) */
   char mask[4] = {0, 0, 0, 0};
   if (masked)
   {
      n = psi_read(conn_fd, mask, 4);
      if (n < 4) return -1;
   }

   /* Handle close frame */
   if (op == WS_OP_CLOSE)
   {
      if (opcode) *opcode = op;
      return 0;
   }

   /* Handle ping — respond with pong */
   if (op == WS_OP_PING)
   {
      /* Read ping payload (if any) and echo back as pong */
      char ping_data[125];
      int plen = payload_len < 125 ? payload_len : 125;
      if (plen > 0)
      {
         n = psi_read(conn_fd, ping_data, plen);
         if (n < plen) return -1;
         if (masked)
            websocket_decode(ping_data, ping_data, plen, mask, 0);
      }
      ws_send_frame(conn_fd, WS_OP_PONG, ping_data, plen);
      /* Recurse to get actual data frame */
      return psi_ws_recv(conn_fd, buf, buf_len, opcode);
   }

   if (opcode) *opcode = op;

   /* Read payload */
   int to_read = payload_len < buf_len ? payload_len : buf_len;
   int total_read = 0;
   while (total_read < to_read)
   {
      n = psi_read(conn_fd, buf + total_read, to_read - total_read);
      if (n <= 0) return n <= 0 ? -1 : total_read;
      total_read += n;
   }

   /* Unmask if needed */
   if (masked)
      websocket_decode(buf, buf, total_read, mask, 0);

   /* Discard remaining payload if it doesn't fit in buf */
   if (payload_len > buf_len)
   {
      char discard[256];
      int remaining = payload_len - buf_len;
      while (remaining > 0)
      {
         int chunk = remaining < (int)sizeof(discard) ? remaining : (int)sizeof(discard);
         n = psi_read(conn_fd, discard, chunk);
         if (n <= 0) break;
         remaining -= n;
      }
   }

   return total_read;
}
