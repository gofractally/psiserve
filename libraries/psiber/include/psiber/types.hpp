#pragma once

#include <ucc/typed_int.hpp>

#include <chrono>
#include <cstdint>

namespace psiber
{
#ifdef __aarch64__
   inline constexpr std::size_t cache_line_size = 128;
#else
   inline constexpr std::size_t cache_line_size = 64;
#endif

   // ── File descriptor type ──────────────────────────────────────────────────
   //
   // RealFd is the OS kernel fd returned by socket()/accept()/etc.
   // Passed to POSIX syscalls (read, write, close, kqueue/epoll).
   // Using typed_int makes it a compile error to pass a raw int where
   // an fd is expected (and vice versa).

   using RealFd = ucc::typed_int<int, struct real_fd_tag>;

   inline constexpr RealFd invalid_real_fd{-1};

   // ── I/O event types ───────────────────────────────────────────────────────

   /// Bitmask of I/O readiness events that can be monitored or reported.
   enum EventKind : uint16_t
   {
      Readable = 1 << 0,
      Writable = 1 << 1,
      UserWake = 1 << 2,
      Signal   = 1 << 3,
   };

   inline constexpr EventKind operator|(EventKind a, EventKind b)
   {
      return static_cast<EventKind>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
   }
   inline constexpr EventKind operator&(EventKind a, EventKind b)
   {
      return static_cast<EventKind>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
   }
   inline constexpr EventKind& operator|=(EventKind& a, EventKind b) { return a = a | b; }

   /// A change to apply to the set of monitored fds.
   /// events == 0 means remove; nonzero means add/replace the interest set.
   struct FdChange
   {
      RealFd    real_fd;
      EventKind events;
   };

   /// A readiness event returned by poll.
   struct IoEvent
   {
      RealFd    real_fd;
      EventKind events;
   };

   // ── Time types ────────────────────────────────────────────────────────────

   using SteadyTime = std::chrono::steady_clock::time_point;

}  // namespace psiber
