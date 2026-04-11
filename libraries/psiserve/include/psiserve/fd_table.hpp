#pragma once

#include <array>
#include <cstdint>
#include <variant>

namespace psiserve
{
   static constexpr int max_fds = 256;

   struct SocketFd
   {
      int real_fd = -1;
   };

   struct TimerFd
   {
      int      interval_ms = 0;
      uint64_t next_fire   = 0;
   };

   struct ClosedFd
   {
   };

   using FdEntry = std::variant<ClosedFd, SocketFd, TimerFd>;

   class FdTable
   {
     public:
      FdTable() { _entries.fill(ClosedFd{}); }

      /// Allocate the next available fd, storing the given entry.
      /// Returns -1 if no fds available.
      int alloc(FdEntry entry)
      {
         for (int i = 0; i < max_fds; ++i)
         {
            if (std::holds_alternative<ClosedFd>(_entries[i]))
            {
               _entries[i] = std::move(entry);
               return i;
            }
         }
         return -1;
      }

      /// Get the entry at the given fd. Returns nullptr if out of range.
      FdEntry* get(int fd)
      {
         if (fd < 0 || fd >= max_fds)
            return nullptr;
         if (std::holds_alternative<ClosedFd>(_entries[fd]))
            return nullptr;
         return &_entries[fd];
      }

      /// Close a fd. Returns false if it was already closed.
      bool close(int fd)
      {
         if (fd < 0 || fd >= max_fds)
            return false;
         if (std::holds_alternative<ClosedFd>(_entries[fd]))
            return false;
         _entries[fd] = ClosedFd{};
         return true;
      }

     private:
      std::array<FdEntry, max_fds> _entries;
   };

}  // namespace psiserve
