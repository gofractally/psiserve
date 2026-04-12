#pragma once

#include <atomic>

namespace psiber
{

   /// Pure atomic spin lock. For nanosecond critical sections only.
   ///
   /// Spins in a tight loop — never yields, never sleeps.
   /// Use only when the protected region is guaranteed to be a handful
   /// of instructions (pointer swap, counter increment, etc.).
   class spin_lock
   {
     public:
      void lock() noexcept
      {
         while (_flag.test_and_set(std::memory_order_acquire))
         {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
         }
      }

      void unlock() noexcept { _flag.clear(std::memory_order_release); }

      bool try_lock() noexcept { return !_flag.test_and_set(std::memory_order_acquire); }

     private:
      std::atomic_flag _flag = ATOMIC_FLAG_INIT;
   };

   /// Spin lock that yields the current fiber on contention.
   ///
   /// If a Scheduler is active on this thread, contention causes the
   /// fiber to yield rather than busy-waiting — letting other fibers
   /// make progress while the lock holder completes its critical section.
   ///
   /// Falls back to pure spin if Scheduler::current() is nullptr
   /// (called from a non-scheduler thread, e.g. main thread).
   class spin_yield_lock
   {
     public:
      void lock() noexcept;
      void unlock() noexcept { _flag.clear(std::memory_order_release); }
      bool try_lock() noexcept { return !_flag.test_and_set(std::memory_order_acquire); }

     private:
      std::atomic_flag _flag = ATOMIC_FLAG_INIT;
   };

}  // namespace psiber
