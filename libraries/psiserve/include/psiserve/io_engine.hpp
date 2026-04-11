#pragma once

#include <psiserve/types.hpp>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>

namespace psiserve
{
   /// Bitmask of I/O readiness events that can be monitored or reported.
   /// Supports bitwise |, &, |= for combining flags.
   /// A zero value (default-initialized `EventKind{}`) means "no events"
   /// and is used by FdChange to signal fd removal.
   enum EventKind : uint16_t
   {
      Readable = 1 << 0,
      Writable = 1 << 1,
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

   /// Abstract I/O readiness notification engine (kqueue, epoll, io_uring).
   ///
   /// Concrete implementations translate the batch-oriented `updateFds`/`poll`
   /// interface into platform-specific syscalls.  The batch API is the
   /// primitive — single-fd convenience wrappers (`addFd`, `removeFd`) are
   /// provided as inline non-virtual methods that delegate to `updateFds`.
   ///
   /// All methods may throw on internal errors; exceptions are caught at
   /// the WASM host function boundary and translated to PsiResult error codes.
   class IoEngine
   {
     public:
      virtual ~IoEngine() = default;

      /// Apply a batch of fd changes in a single syscall where possible.
      /// Throws on failure (internal C++ error — never crosses WASM boundary).
      virtual void updateFds(std::span<const FdChange> changes) = 0;

      /// Poll for ready events into caller-provided buffer.
      /// nullopt means block indefinitely.
      /// 0ms means non-blocking poll.
      /// Returns the number of events written to `out`.
      /// Throws on failure (internal C++ error — never crosses WASM boundary).
      virtual int poll(std::span<IoEvent> out, std::optional<std::chrono::milliseconds> timeout) = 0;

      /// Convenience: add or replace interest for a single fd.
      void addFd(RealFd real_fd, EventKind events)
      {
         FdChange c{real_fd, events};
         updateFds({&c, 1});
      }

      /// Convenience: remove a single fd from the interest set (all filters).
      void removeFd(RealFd real_fd)
      {
         FdChange c{real_fd, {}};
         updateFds({&c, 1});
      }

      /// Convenience: remove specific event filters for a single fd.
      /// Only removes the filters corresponding to the given events.
      virtual void removeFdEvents(RealFd real_fd, EventKind events) = 0;
   };

}  // namespace psiserve
