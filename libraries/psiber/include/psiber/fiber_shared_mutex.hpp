#pragma once

#include <psiber/fiber.hpp>
#include <psiber/detail/platform_engine.hpp>
#include <psiber/spin_lock.hpp>

#include <atomic>
#include <cstdint>

namespace psiber
{
   using Scheduler = basic_scheduler<detail::PlatformEngine>;

   /// Fiber-aware reader-writer mutex.
   ///
   /// Multiple concurrent readers, exclusive writers.
   /// Writers queue behind a spin_lock; readers check an atomic counter.
   class fiber_shared_mutex
   {
     public:
      /// Exclusive lock.  Parks the fiber if readers or another writer hold it.
      void lock(Scheduler& sched);

      /// Exclusive unlock.  Wakes pending readers or the next writer.
      void unlock();

      /// Shared (reader) lock.  Parks if a writer holds or is waiting.
      void lock_shared(Scheduler& sched);

      /// Shared unlock.  If this was the last reader and a writer is waiting, wakes it.
      void unlock_shared();

      bool try_lock();
      bool try_lock_shared();

     private:
      std::atomic<int32_t> _readers{0};
      std::atomic<bool>    _writer{false};

      // Writer wait queue
      Fiber*    _writer_wait_head = nullptr;
      Fiber*    _writer_wait_tail = nullptr;
      spin_lock _queue_lock;

      // Reader wait queue (blocked behind a writer)
      Fiber*    _reader_wait_head = nullptr;
      Fiber*    _reader_wait_tail = nullptr;
   };

}  // namespace psiber
