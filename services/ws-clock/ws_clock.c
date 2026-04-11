/*
 * ws_clock.c — WebSocket clock service for psiserve
 *
 * Accepts WebSocket connections and sends the current Unix timestamp
 * (with nanosecond precision) once per second as a text frame.
 *
 * Links against libpsi.a for WebSocket handshake/framing.
 *
 * Compile:
 *   clang --target=wasm32 -nostdlib -O2 -fno-builtin \
 *       -I libraries/psi-sdk/include \
 *       -Wl,--no-entry -Wl,--export=_start \
 *       -Wl,--export=__stack_pointer \
 *       -Wl,--export=handle_connection \
 *       -Wl,--export-table \
 *       -o services/ws-clock/ws_clock.wasm \
 *       services/ws-clock/ws_clock.c \
 *       build/Debug/libraries/psi-sdk/libpsi.a
 */

#include <psi/host.h>
#include <psi/websocket.h>

/* Simple int64 to decimal string (no libc) */
static int i64_to_str(long long val, char* buf, int buf_len)
{
   if (buf_len < 2) return 0;

   int neg = 0;
   if (val < 0) { neg = 1; val = -val; }

   char tmp[20];
   int n = 0;
   if (val == 0)
   {
      tmp[n++] = '0';
   }
   else
   {
      while (val > 0 && n < 20)
      {
         tmp[n++] = '0' + (int)(val % 10);
         val /= 10;
      }
   }

   int pos = 0;
   if (neg && pos < buf_len - 1) buf[pos++] = '-';
   for (int i = n - 1; i >= 0 && pos < buf_len - 1; i--)
      buf[pos++] = tmp[i];
   buf[pos] = '\0';
   return pos;
}

/* ── Per-connection handler (runs in its own fiber) ─────────────── */

void handle_connection(int conn)
{
   if (psi_ws_accept(conn) < 0)
   {
      psi_close(conn);
      return;
   }

   for (;;)
   {
      long long now_ns = psi_clock(PSI_CLOCK_REALTIME);
      long long secs = now_ns / 1000000000LL;
      long long frac = now_ns % 1000000000LL;
      if (frac < 0) frac = -frac;

      /* Format: "1712838400.123456789" */
      char msg[64];
      int pos = i64_to_str(secs, msg, sizeof(msg));
      msg[pos++] = '.';

      /* Pad fractional part to 9 digits */
      char frac_str[20];
      int frac_len = i64_to_str(frac, frac_str, sizeof(frac_str));
      for (int i = 0; i < 9 - frac_len; i++)
         msg[pos++] = '0';
      for (int i = 0; i < frac_len; i++)
         msg[pos++] = frac_str[i];
      msg[pos] = '\0';

      if (psi_ws_send_text(conn, msg, pos) < 0)
         break;

      psi_sleep(1000);
   }

   psi_ws_send_close(conn);
   psi_close(conn);
}

/* ── Main ───────────────────────────────────────────────────────── */

void _start(void)
{
   for (;;)
   {
      int conn = psi_accept(0);
      if (conn < 0) return;
      psi_spawn(handle_connection, conn);
   }
}
