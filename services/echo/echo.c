/*
 * echo.c — Phase 1 echo service for psiserve
 *
 * Imports psi.accept, psi.read, psi.write, psi.close from the host.
 * fd 0 is the listen socket (pre-opened by the runtime).
 *
 * Compile with wasi-sdk:
 *   /opt/wasi-sdk/bin/clang --target=wasm32 -nostdlib -Wl,--no-entry \
 *       -Wl,--export=_start -o echo.wasm echo.c
 *
 * Or assemble the WAT version:
 *   wat2wasm echo.wat -o echo.wasm
 */

/* Host imports */
__attribute__((import_module("psi"), import_name("accept")))
int psi_accept(int listen_fd);

__attribute__((import_module("psi"), import_name("read")))
int psi_read(int fd, void* buf, int len);

__attribute__((import_module("psi"), import_name("write")))
int psi_write(int fd, const void* buf, int len);

__attribute__((import_module("psi"), import_name("close")))
void psi_close(int fd);

static char buf[4096];

void _start(void)
{
   for (;;)
   {
      int conn = psi_accept(0);
      if (conn < 0)
         return;

      for (;;)
      {
         int n = psi_read(conn, buf, sizeof(buf));
         if (n <= 0)
            break;
         psi_write(conn, buf, n);
      }

      psi_close(conn);
   }
}
