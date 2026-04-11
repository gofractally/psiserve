#pragma once

#include <psiserve/fd_table.hpp>

namespace psiserve
{
   /// Lifecycle state of a WASM process.
   ///
   ///   Ready ──► Running ──► Done
   ///               │  ▲
   ///               ▼  │
   ///             Blocked
   ///
   /// A process starts Ready, transitions to Running when the scheduler
   /// picks it up, may move to Blocked when a host function (accept/read/
   /// write) needs to wait for I/O, returns to Running when the fd becomes
   /// ready, and finally reaches Done when _start returns or traps.
   enum class ProcessState
   {
      Ready,
      Running,
      Blocked,
      Done
   };

   /// Why a process is blocked — which host function is waiting.
   enum class BlockReason
   {
      None,
      Accept,
      Read,
      Write
   };

   /// Records what a blocked process is waiting on: the reason (which
   /// syscall) and the virtual fd it's blocked against.
   struct BlockInfo
   {
      BlockReason reason     = BlockReason::None;
      VirtualFd   virtual_fd = invalid_virtual_fd;
   };

   /// A single WASM module instance and its associated OS resources.
   ///
   /// Each process owns an fd table that maps WASM-visible virtual fds
   /// to host-side real fds (sockets, timers, etc.).  The runtime
   /// pre-populates fd 0 with the listen socket before calling _start.
   ///
   /// In Phase 1 there is exactly one process per server.  Later phases
   /// will add COW-forked child processes that share the compiled module
   /// but get independent fd tables and linear memory.
   struct Process
   {
      const char*    name  = "unnamed";  ///< Human-readable name (must outlive process)
      ProcessState   state = ProcessState::Ready;
      BlockInfo           block;
      FdTable             fds;

      void markReady()
      {
         state = ProcessState::Ready;
         block = {};
      }

      void markRunning()
      {
         state = ProcessState::Running;
         block = {};
      }

      void markBlocked(BlockReason reason, VirtualFd fd)
      {
         state            = ProcessState::Blocked;
         block.reason     = reason;
         block.virtual_fd = fd;
      }

      void markDone()
      {
         state = ProcessState::Done;
         block = {};
      }
   };

}  // namespace psiserve
