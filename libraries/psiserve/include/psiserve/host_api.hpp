#pragma once

#include <psiserve/process.hpp>
#include <psiserve/scheduler.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace psiserve
{
   /// Type-erased callback for spawning fibers.
   /// Set by Runtime::run() where the backend type is known.
   /// Parameters: (func_table_index, arg)
   using FiberRunner = std::function<void(uint32_t, int32_t)>;

   /// Host functions exposed to WASM as imports (module "psi").
   ///
   /// All parameters and return values use typed wrappers (VirtualFd, WasmPtr,
   /// WasmSize, PsiResult) which psizam's wasm_type_traits automatically
   /// unwraps/wraps at the WASM boundary via the fast trampoline.
   ///
   /// ## Error convention
   ///
   /// Functions return PsiResult (maps to i32 at the WASM boundary):
   ///   - result >= 0  — success (fd number, byte count, etc.)
   ///   - result < 0   — error, value is -PsiError (see types.hpp)
   ///
   /// Error codes are platform-independent.  See PsiError enum for the
   /// complete list.  POSIX errno values from underlying syscalls are
   /// mapped to the nearest PsiError by PsiResult::fromErrno().
   ///
   /// C++ exceptions from the I/O layer are caught at this boundary and
   /// translated to PsiError values.  No exceptions propagate into WASM.
   class HostApi
   {
     public:
      HostApi(Process& proc, Scheduler& sched, char* wasm_memory);

      /// Set the callback used by psiSpawn to create fibers.
      /// The pointed-to FiberRunner must outlive this HostApi.
      void setFiberRunner(const FiberRunner* runner);

      // ── WASM imports (psi.*) ───────────────────────────────────────────────

      /// psi.accept(listen_fd: i32) -> i32
      /// Blocks until a connection arrives on `listen_fd`.
      /// Returns the new virtual fd on success, or -PsiError on error.
      PsiResult psiAccept(VirtualFd listen_fd);

      /// psi.read(fd: i32, buf: i32, len: i32) -> i32
      /// Blocks until data is available on `fd`.
      /// Returns bytes read, 0 on EOF, or -PsiError on error.
      PsiResult psiRead(VirtualFd fd, WasmPtr buf, WasmSize len);

      /// psi.write(fd: i32, buf: i32, len: i32) -> i32
      /// Blocks until write space is available on `fd`.
      /// Returns bytes written, or -PsiError on error.
      PsiResult psiWrite(VirtualFd fd, WasmPtr buf, WasmSize len);

      /// psi.open(dir_fd: i32, path_ptr: i32, path_len: i32) -> i32
      /// Opens a file relative to a preopened DirFd.
      /// Path must not escape the sandbox (no ".." traversal).
      /// Returns a new virtual fd on success, or -PsiError on error.
      PsiResult psiOpen(VirtualFd dir_fd, WasmPtr path_ptr, WasmSize path_len);

      /// psi.fstat(fd: i32, out_ptr: i32) -> i32
      /// Writes file size (uint64_t, 8 bytes) to out_ptr in WASM memory.
      /// Returns 0 on success, or -PsiError on error.
      PsiResult psiFstat(VirtualFd fd, WasmPtr out_ptr);

      /// psi.close(fd: i32)
      /// Closes `fd` and releases its slot in the fd table.
      /// Silently ignores invalid fds.
      void psiClose(VirtualFd fd);

      /// psi.spawn(func_table_idx: i32, arg: i32)
      /// Creates a new fiber that calls the function at the given table index
      /// with `arg` as its parameter.  The host allocates the fiber's WASM stack
      /// and sets __stack_pointer automatically.
      void psiSpawn(WasmPtr func_table_idx, WasmPtr arg);

      /// psi.clock(clock_id: i32) -> i64
      /// Returns nanoseconds for the requested clock.
      ///   clock_id 0 = REALTIME  (wall-clock, ns since Unix epoch)
      ///   clock_id 1 = MONOTONIC (steady clock, ns since arbitrary origin)
      int64_t psiClock(int32_t clock_id);

      /// psi.sleep_until(deadline_ns: i64)
      /// Suspends the current fiber until the monotonic clock reaches
      /// `deadline_ns` nanoseconds.  Use psi.clock(1) to get the current
      /// monotonic time for computing absolute deadlines.
      void psiSleepUntil(int64_t deadline_ns);

      /// psi.sendfile(sock_fd: i32, file_fd: i32, len: i64) -> i32
      /// Transfers `len` bytes from `file_fd` to `sock_fd` using zero-copy
      /// OS sendfile when possible. Falls back to buffered copy for TLS.
      /// Returns total bytes sent.
      PsiResult psiSendFile(VirtualFd sock_fd, VirtualFd file_fd, int64_t len);

      /// psi.cork(fd: i32)
      /// Enables TCP_CORK (TCP_NOPUSH on macOS) on a socket.
      /// Buffers small writes into a single TCP segment until uncork.
      void psiCork(VirtualFd fd);

      /// psi.uncork(fd: i32)
      /// Disables TCP_CORK and flushes buffered data.
      void psiUncork(VirtualFd fd);

      /// psi.udp_bind(port: i32) -> i32
      /// Creates a UDP socket bound to 0.0.0.0:port.
      /// Returns virtual fd on success, or -PsiError on error.
      PsiResult psiUdpBind(int32_t port);

      /// psi.recvfrom(fd: i32, buf: i32, len: i32, addr: i32) -> i32
      /// Blocks until a datagram arrives on the UDP socket `fd`.
      /// Writes sender address (8 bytes: ip4 + port + pad) to `addr`.
      /// Returns bytes received, or -PsiError on error.
      PsiResult psiRecvFrom(VirtualFd fd, WasmPtr buf, WasmSize len, WasmPtr addr);

      /// psi.sendto(fd: i32, buf: i32, len: i32, addr: i32) -> i32
      /// Sends a datagram via UDP socket `fd` to the address at `addr`.
      /// Returns bytes sent, or -PsiError on error.
      PsiResult psiSendTo(VirtualFd fd, WasmPtr buf, WasmSize len, WasmPtr addr);

      /// psi.connect(host_ptr: i32, host_len: i32, port: i32) -> i32
      /// Resolves the hostname via DNS, then connects to the given port.
      /// Blocks the fiber during DNS resolution and TCP connect.
      /// Returns a new virtual fd on success, or -PsiError on error.
      PsiResult psiConnect(WasmPtr host_ptr, WasmSize host_len, int32_t port);

      /// Number of connections accepted by this host instance.
      uint64_t acceptCount() const { return _accept_count; }

     private:
      Process*    _proc;
      Scheduler*  _sched;
      char*       _wasm_memory;
      const FiberRunner* _fiberRunner = nullptr;
      uint64_t    _accept_count = 0;
   };

}  // namespace psiserve
