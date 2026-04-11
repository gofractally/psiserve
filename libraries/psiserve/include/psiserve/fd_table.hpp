#pragma once

#include <psiserve/types.hpp>

#include <openssl/ssl.h>

#include <array>
#include <chrono>
#include <variant>

namespace psiserve
{
   /// Maximum number of file descriptors a single process can have open.
   static constexpr int max_fds = 256;

   // ── Fd entry types ────────────────────────────────────────────────────────
   //
   // Each open fd maps to one of these variants, describing what resource it
   // refers to on the host side.  The variant discriminant tells the runtime
   // how to interpret the entry — which host operations are valid and what
   // OS resources back it.

   /// An fd backed by a real OS socket (TCP stream or listener).
   /// The runtime translates WASM read/write/accept calls into POSIX
   /// operations on `real_fd`.
   ///
   /// TLS fields (both null for plain TCP):
   ///   ssl_ctx — non-null on TLS listen sockets; tells psiAccept to
   ///             wrap accepted connections in TLS.
   ///   ssl     — non-null on TLS connections; tells psiRead/psiWrite
   ///             to use SSL_read/SSL_write instead of ::read/::write.
   struct SocketFd
   {
      RealFd   real_fd = invalid_real_fd;
      SSL_CTX* ssl_ctx = nullptr;  ///< TLS context (listen sockets only, not owned)
      SSL*     ssl     = nullptr;  ///< TLS connection state (owned, freed on close)
   };

   /// An fd backed by a virtual timer (not yet implemented).
   /// Will fire a readability event when `next_fire_time` is reached,
   /// then auto-reload by `interval_ms` for repeating timers.
   struct TimerFd
   {
      std::chrono::milliseconds              interval_ms{0};
      std::chrono::steady_clock::time_point  next_fire_time{};
   };

   /// An fd backed by a regular file opened via psi.open.
   /// Reads are synchronous (regular files are always ready — no poll).
   struct FileFd
   {
      RealFd real_fd = invalid_real_fd;
   };

   /// An fd backed by a preopened directory.
   /// Used as the `dir_fd` argument to psi.open for sandboxed path resolution.
   /// Cannot be read/written directly — only used as a reference for openat().
   struct DirFd
   {
      RealFd real_fd = invalid_real_fd;
   };

   /// An fd backed by a real OS UDP socket.
   /// Supports recvfrom/sendto operations for datagram protocols.
   struct UdpFd
   {
      RealFd real_fd = invalid_real_fd;
   };

   /// Sentinel variant indicating the fd slot is unused.
   /// FdTable::alloc() scans for ClosedFd slots; FdTable::close() writes
   /// this value back to release the slot.
   struct ClosedFd
   {
   };

   /// The discriminated union stored in each FdTable slot.
   using FdEntry = std::variant<ClosedFd, SocketFd, TimerFd, FileFd, DirFd, UdpFd>;

   // ── FdTable ───────────────────────────────────────────────────────────────
   //
   // Each WASM process has its own FdTable — a fixed-size array that maps
   // VirtualFd numbers (0..255) to host-side resources (OS sockets, timers,
   // etc.).
   //
   // This is the psiserve equivalent of a Unix process's file descriptor table.
   // WASM code sees small integer fds (VirtualFd) that index into this table;
   // the host resolves them to real OS resources (RealFd) when executing
   // system calls.  The indirection serves two purposes:
   //
   //   1. Isolation — WASM code cannot forge or guess real OS fds.
   //      It can only refer to resources the runtime has explicitly placed
   //      in its table.
   //
   //   2. Portability — the WASM module is decoupled from the host's fd
   //      numbering.  The runtime can close, dup, or reassign real fds
   //      without the WASM module knowing.
   //
   // Usage:
   //   FdTable fds;
   //   VirtualFd vfd = fds.alloc(SocketFd{real_fd});  // open
   //   auto* entry   = fds.get(vfd);                   // lookup
   //   fds.close(vfd);                                  // close

   class FdTable
   {
     public:
      FdTable() { _entries.fill(ClosedFd{}); }

      /// Allocate the lowest available VirtualFd for the given entry.
      /// Returns invalid_virtual_fd if the table is full.
      VirtualFd alloc(FdEntry entry)
      {
         for (int i = 0; i < max_fds; ++i)
         {
            if (std::holds_alternative<ClosedFd>(_entries[i]))
            {
               _entries[i] = std::move(entry);
               return VirtualFd{i};
            }
         }
         return invalid_virtual_fd;
      }

      /// Look up a VirtualFd.  Returns nullptr if closed or out of range.
      FdEntry* get(VirtualFd fd)
      {
         int i = *fd;
         if (i < 0 || i >= max_fds)
            return nullptr;
         if (std::holds_alternative<ClosedFd>(_entries[i]))
            return nullptr;
         return &_entries[i];
      }

      /// Close a VirtualFd.  Returns false if it was already closed.
      bool close(VirtualFd fd)
      {
         int i = *fd;
         if (i < 0 || i >= max_fds)
            return false;
         if (std::holds_alternative<ClosedFd>(_entries[i]))
            return false;
         _entries[i] = ClosedFd{};
         return true;
      }

     private:
      std::array<FdEntry, max_fds> _entries;
   };

}  // namespace psiserve
