#pragma once

// arena_budget.hpp — Thread-safe address-space budget for sub-instances.
//
// Tracks total reserved bytes across a spawn tree. Alloc checks the
// budget before allocating; free credits back. Uses aligned_alloc for
// power-of-2 alignment. No mmap churn for the common case (the system
// allocator handles page-level details).
//
// This is the PoC implementation. The future optimization path is a
// lock-free buddy allocator over a pre-reserved mmap region — see
// .issues/psizam-buddy-arena-allocator.md.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

namespace psizam {

class arena_budget {
   std::atomic<std::size_t> reserved_{0};
   std::size_t              limit_;

public:
   explicit arena_budget(std::size_t limit_bytes) : limit_(limit_bytes) {}

   std::size_t limit()    const { return limit_; }
   std::size_t reserved() const { return reserved_.load(std::memory_order_relaxed); }
   std::size_t available() const {
      auto r = reserved_.load(std::memory_order_relaxed);
      return r < limit_ ? limit_ - r : 0;
   }

   // Try to reserve `bytes` from the budget. Returns true if there was
   // room and the reservation was recorded atomically.
   bool try_reserve(std::size_t bytes) {
      auto old = reserved_.load(std::memory_order_relaxed);
      do {
         if (old + bytes > limit_) return false;
      } while (!reserved_.compare_exchange_weak(
         old, old + bytes, std::memory_order_relaxed));
      return true;
   }

   // Release `bytes` back to the budget.
   void release(std::size_t bytes) {
      reserved_.fetch_sub(bytes, std::memory_order_relaxed);
   }

   // Allocate a power-of-2 aligned block from the budget.
   // Returns (ptr, actual_size) or (nullptr, 0) if over budget.
   // The block is zeroed.
   // `order` is the power-of-2 exponent (e.g. 16 = 64KB, 22 = 4MB).
   std::pair<char*, std::size_t> alloc(uint8_t order) {
      std::size_t size = std::size_t{1} << order;
      if (!try_reserve(size))
         return {nullptr, 0};

      // aligned_alloc requires size to be a multiple of alignment,
      // which is trivially true for power-of-2.
      void* ptr = std::aligned_alloc(size, size);
      if (!ptr) {
         release(size);
         return {nullptr, 0};
      }
      std::memset(ptr, 0, size);
      return {static_cast<char*>(ptr), size};
   }

   // Free a block and return its budget to the pool.
   void free(char* ptr, std::size_t size) {
      if (ptr) {
         std::free(ptr);
         release(size);
      }
   }
};

} // namespace psizam
