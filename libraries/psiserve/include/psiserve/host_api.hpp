#pragma once

#include <psiserve/process.hpp>
#include <psiserve/scheduler.hpp>

#include <cstdint>
#include <string>

namespace pfs { class store; }

namespace psiserve
{

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
      HostApi(Process& proc, Scheduler& sched, char* wasm_memory,
              pfs::store* ipfs = nullptr);

      /// Update the WASM linear memory pointer. Called after
      /// instantiation when the memory address is known.
      void updateMemory(char* wasm_memory) { _wasm_memory = wasm_memory; }

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

      // ── IPFS / content-addressed storage ──────────────────────────────

      /// psi.ipfs_put(data: i32, data_len: i32, cid_out: i32, cid_cap: i32) -> i32
      /// Stores data in the content-addressed store.  Writes the CID string
      /// (base32-lower multibase) to the output buffer.
      /// Returns CID string length on success, or -PsiError on error.
      PsiResult psiIpfsPut(WasmPtr data, WasmSize data_len,
                           WasmPtr cid_out, WasmSize cid_cap);

      /// psi.ipfs_get(cid: i32, cid_len: i32, offset: i64, buf: i32, buf_len: i32) -> i32
      /// Reads data by CID string.  Returns bytes written to buf, or -PsiError.
      PsiResult psiIpfsGet(WasmPtr cid_ptr, WasmSize cid_len,
                           int64_t offset,
                           WasmPtr buf, WasmSize buf_len);

      /// psi.ipfs_stat(cid: i32, cid_len: i32, size_out: i32) -> i32
      /// Gets content size by CID string.  Writes uint64_t to size_out.
      /// Returns 0 on success, or -PsiError on error.
      PsiResult psiIpfsStat(WasmPtr cid_ptr, WasmSize cid_len, WasmPtr size_out);

      /// psi.log(level: i32, msg: i32, len: i32)
      /// Emit a structured log line through the host's logger so guests
      /// can surface diagnostics before TCP I/O is set up.
      ///   level 0 = debug, 1 = info, 2 = warn, 3 = error
      /// Messages are truncated at `log_max_len` to bound host work.
      /// Safe to call before the WASM linear-memory pointer is wired:
      /// a null memory base yields a single warning and returns.
      void psiLog(int32_t level, WasmPtr msg, WasmSize len);

      /// Upper bound on bytes forwarded per psi.log call.  Longer lines
      /// are truncated and annotated with a `[truncated]` suffix.
      static constexpr uint32_t log_max_len = 4096;

      /// Number of connections accepted by this host instance.
      uint64_t acceptCount() const { return _accept_count; }

      /// Number of guest log lines emitted via psi.log.
      /// Mainly for test assertions — runtime code should read the log.
      uint64_t logCount() const { return _log_count; }

     private:
      Process*    _proc;
      Scheduler*  _sched;
      char*       _wasm_memory;
      pfs::store* _ipfs = nullptr;
      uint64_t    _accept_count = 0;
      uint64_t    _log_count    = 0;
   };

}  // namespace psiserve
