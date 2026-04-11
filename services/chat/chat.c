/*
 * chat.c — Multi-client chat server for psiserve
 *
 * Clients connect via telnet.  Messages from any one client are
 * broadcast to all other connected clients.  Each connection runs
 * in its own fiber — the host manages all fiber infrastructure.
 *
 * Imports psi.accept, psi.read, psi.write, psi.close, psi.spawn.
 *   fd 0 = listen socket (pre-opened by runtime)
 *
 * Compile:
 *   clang --target=wasm32 -nostdlib -O2 -fno-builtin \
 *       -Wl,--no-entry -Wl,--export=_start \
 *       -Wl,--export=__stack_pointer \
 *       -Wl,--export=handle_connection \
 *       -Wl,--export-table \
 *       -o chat.wasm chat.c
 *
 * Note: --export-table is required so the linker emits the indirect
 * function table.  Without it, the function pointer passed to psi_spawn
 * has no table to resolve against.
 */

/* ── Host imports ─────────────────────────────────────────────────────────── */

__attribute__((import_module("psi"), import_name("accept")))
int psi_accept(int listen_fd);

__attribute__((import_module("psi"), import_name("read")))
int psi_read(int fd, void* buf, int len);

__attribute__((import_module("psi"), import_name("write")))
int psi_write(int fd, const void* buf, int len);

__attribute__((import_module("psi"), import_name("close")))
void psi_close(int fd);

__attribute__((import_module("psi"), import_name("spawn")))
void psi_spawn(void (*func)(int), int arg);

/* ── Connection tracking ──────────────────────────────────────────────────── */

#define MAX_CONNS 32

static int connections[MAX_CONNS];
static int num_connections = 0;

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

/* ── Per-connection handler (runs in its own fiber) ───────────────────────── */

void handle_connection(int conn)
{
   /* Register this connection */
   int my_idx = -1;
   for (int i = 0; i < MAX_CONNS; i++)
   {
      if (connections[i] == -1)
      {
         connections[i] = conn;
         my_idx = i;
         break;
      }
   }
   if (my_idx < 0)
   {
      write_all(conn, "Server full\r\n", 13);
      psi_close(conn);
      return;
   }
   num_connections++;

   write_all(conn, "Welcome to chat!\r\n", 18);

   char buf[512];
   for (;;)
   {
      int n = psi_read(conn, buf, sizeof(buf));
      if (n <= 0) break;

      /* Broadcast to all OTHER connections */
      for (int i = 0; i < MAX_CONNS; i++)
      {
         if (connections[i] >= 0 && connections[i] != conn)
            write_all(connections[i], buf, n);
      }
   }

   /* Disconnect */
   connections[my_idx] = -1;
   num_connections--;
   psi_close(conn);
}

/* ── Main ─────────────────────────────────────────────────────────────────── */

void _start(void)
{
   /* Initialize connection table */
   for (int i = 0; i < MAX_CONNS; i++)
      connections[i] = -1;

   for (;;)
   {
      int conn = psi_accept(0);
      if (conn < 0) return;

      psi_spawn(handle_connection, conn);
   }
}
