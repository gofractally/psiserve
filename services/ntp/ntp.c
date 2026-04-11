/*
 * ntp.c — NTP server (RFC 5905) for psiserve
 *
 * Responds to NTPv3/v4 client queries with the current wall-clock time.
 * Stratum 1 reference clock, reference ID "PSI\0".
 *
 * NTP timestamps are 64-bit: 32 bits seconds since 1900-01-01 +
 * 32 bits fractional seconds.
 *
 * Compile:
 *   clang --target=wasm32 -nostdlib -O2 -fno-builtin \
 *       -I libraries/psi-sdk/include \
 *       -Wl,--no-entry -Wl,--export=_start \
 *       -Wl,--export=__stack_pointer \
 *       -Wl,--export-table \
 *       -o services/ntp/ntp.wasm \
 *       services/ntp/ntp.c
 *
 * Test:
 *   sntp -S localhost:1230
 *   ntpdate -q -p 1 -o 4 127.0.0.1
 */

#include <psi/host.h>

/* ── NTP constants ──────────────────────────────────────────────── */

/* Seconds between NTP epoch (1900-01-01) and Unix epoch (1970-01-01) */
#define NTP_EPOCH_OFFSET 2208988800ULL

/* NTP packet is 48 bytes (without authentication) */
#define NTP_PACKET_SIZE 48

/* Byte 0 layout: LI(2) | VN(3) | Mode(3) */
#define NTP_LI_NONE    0        /* no leap warning */
#define NTP_VERSION    4
#define NTP_MODE_CLIENT 3
#define NTP_MODE_SERVER 4

/* ── Byte helpers (big-endian wire format) ──────────────────────── */

static void put_u32(unsigned char* p, unsigned int v)
{
   p[0] = (unsigned char)(v >> 24);
   p[1] = (unsigned char)(v >> 16);
   p[2] = (unsigned char)(v >> 8);
   p[3] = (unsigned char)(v);
}

static unsigned int get_u32(const unsigned char* p)
{
   return ((unsigned int)p[0] << 24) |
          ((unsigned int)p[1] << 16) |
          ((unsigned int)p[2] << 8)  |
          ((unsigned int)p[3]);
}

/* ── Unix nanoseconds → NTP timestamp ───────────────────────────── */

static void unix_ns_to_ntp(long long ns, unsigned int* secs, unsigned int* frac)
{
   long long unix_secs = ns / 1000000000LL;
   long long unix_frac = ns % 1000000000LL;
   if (unix_frac < 0)
   {
      unix_frac += 1000000000LL;
      unix_secs -= 1;
   }

   /* NTP seconds = Unix seconds + epoch offset */
   *secs = (unsigned int)(unix_secs + NTP_EPOCH_OFFSET);

   /* Fractional: convert nanoseconds to 2^32 fraction */
   /* frac = unix_frac * 2^32 / 10^9 */
   *frac = (unsigned int)((unix_frac * 4294967296LL) / 1000000000LL);
}

/* ── NTP request handler ────────────────────────────────────────── */

static void handle_ntp(int udp_fd)
{
   unsigned char   pkt[NTP_PACKET_SIZE];
   struct psi_addr sender;

   for (;;)
   {
      int n = psi_recvfrom(udp_fd, pkt, NTP_PACKET_SIZE, &sender);
      if (n < NTP_PACKET_SIZE)
         continue;  /* ignore runt packets */

      /* Record receive timestamp immediately */
      long long recv_ns = psi_clock(PSI_CLOCK_REALTIME);

      /* Validate: must be a client request (mode 3) */
      int mode = pkt[0] & 0x07;
      int version = (pkt[0] >> 3) & 0x07;
      if (mode != NTP_MODE_CLIENT)
         continue;
      if (version < 3 || version > 4)
         continue;

      /* Save client's transmit timestamp → becomes origin in response */
      unsigned int orig_secs = get_u32(pkt + 40);
      unsigned int orig_frac = get_u32(pkt + 44);

      /* Build response packet */
      unsigned char resp[NTP_PACKET_SIZE];
      int i;
      for (i = 0; i < NTP_PACKET_SIZE; i++)
         resp[i] = 0;

      /* Byte 0: LI=0 | VN=client's version | Mode=4 (server) */
      resp[0] = (unsigned char)((NTP_LI_NONE << 6) | (version << 3) | NTP_MODE_SERVER);

      /* Stratum 1 — primary reference */
      resp[1] = 1;

      /* Poll interval — echo client's value */
      resp[2] = pkt[2];

      /* Precision: -20 ≈ 2^-20 ≈ 1 microsecond */
      resp[3] = (unsigned char)(-20 & 0xFF);

      /* Root delay = 0 (we are the reference) */
      /* Root dispersion = 0 */

      /* Reference ID: "PSI\0" (ASCII, stratum 1 convention) */
      resp[12] = 'P';
      resp[13] = 'S';
      resp[14] = 'I';
      resp[15] = '\0';

      /* Reference timestamp: current time */
      unsigned int ref_secs, ref_frac;
      unix_ns_to_ntp(recv_ns, &ref_secs, &ref_frac);
      put_u32(resp + 16, ref_secs);
      put_u32(resp + 20, ref_frac);

      /* Origin timestamp: client's transmit timestamp (echo back) */
      put_u32(resp + 24, orig_secs);
      put_u32(resp + 28, orig_frac);

      /* Receive timestamp: when we got the request */
      put_u32(resp + 32, ref_secs);
      put_u32(resp + 36, ref_frac);

      /* Transmit timestamp: now */
      long long tx_ns = psi_clock(PSI_CLOCK_REALTIME);
      unsigned int tx_secs, tx_frac;
      unix_ns_to_ntp(tx_ns, &tx_secs, &tx_frac);
      put_u32(resp + 40, tx_secs);
      put_u32(resp + 44, tx_frac);

      psi_sendto(udp_fd, resp, NTP_PACKET_SIZE, &sender);
   }
}

/* ── Main ───────────────────────────────────────────────────────── */

void _start(void)
{
   /* Bind UDP port 1230 (NTP default is 123, needs root) */
   int udp_fd = psi_udp_bind(1230);
   if (udp_fd < 0)
      return;

   handle_ntp(udp_fd);
}
