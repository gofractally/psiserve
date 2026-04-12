#include <psiber/fiber_mutex.hpp>
#include <psiber/fiber_tx_mutex.hpp>
#include <psiber/fiber_shared_mutex.hpp>
#include <psiber/scheduler.hpp>

#include <cassert>

namespace psiber
{
   // ── fiber_mutex ───────────────────────────────────────────────────────────

   void fiber_mutex::lock(Scheduler& sched)
   {
      Fiber* self = sched.currentFiber();
      assert(self);

      // Fast path: uncontended
      Fiber* expected = nullptr;
      if (_owner.compare_exchange_strong(expected, self, std::memory_order_acquire))
         return;

      // Slow path: enqueue and park
      {
         _queue_lock.lock();
         // Double-check under the queue lock
         expected = nullptr;
         if (_owner.compare_exchange_strong(expected, self, std::memory_order_acquire))
         {
            _queue_lock.unlock();
            return;
         }
         // Append to FIFO wait queue
         self->next_blocked = nullptr;
         if (_wait_tail)
            _wait_tail->next_blocked = self;
         else
            _wait_head = self;
         _wait_tail = self;
         _queue_lock.unlock();
      }

      sched.parkCurrentFiber();
   }

   void fiber_mutex::unlock()
   {
      _queue_lock.lock();

      if (_wait_head)
      {
         // Wake next waiter
         Fiber* next    = _wait_head;
         _wait_head     = next->next_blocked;
         if (!_wait_head)
            _wait_tail = nullptr;
         next->next_blocked = nullptr;

         _owner.store(next, std::memory_order_release);
         _queue_lock.unlock();

         Scheduler::wake(next);
      }
      else
      {
         _owner.store(nullptr, std::memory_order_release);
         _queue_lock.unlock();
      }
   }

   bool fiber_mutex::try_lock()
   {
      Scheduler* sched = Scheduler::current();
      Fiber*     self  = sched ? sched->currentFiber() : nullptr;
      Fiber*     expected = nullptr;
      return _owner.compare_exchange_strong(expected, self, std::memory_order_acquire);
   }

   // ── fiber_tx_mutex ────────────────────────────────────────────────────────

   void fiber_tx_mutex::lock(Scheduler& sched, uint64_t tx_timestamp)
   {
      Fiber* self = sched.currentFiber();
      assert(self);

      _queue_lock.lock();

      if (!_owner)
      {
         // Uncontended
         _owner           = self;
         _owner_timestamp = tx_timestamp;
         self->wounded.store(false, std::memory_order_relaxed);
         _queue_lock.unlock();
         return;
      }

      // Enqueue in the wait queue regardless — we always wait for the holder.
      self->next_blocked = nullptr;
      if (_wait_tail)
         _wait_tail->next_blocked = self;
      else
         _wait_head = self;
      _wait_tail = self;

      if (tx_timestamp < _owner_timestamp)
      {
         // Wound-wait: older transaction wounds younger holder.
         // The holder will detect this at its next yield/check point
         // and abort (throwing wound_exception), releasing the lock.
         _owner->wounded.store(true, std::memory_order_release);
      }

      _queue_lock.unlock();
      sched.parkCurrentFiber();

      // We now hold the lock (set by unlock()).
      // Check if WE were wounded while waiting (a still-older tx showed up).
      if (self->wounded.load(std::memory_order_acquire))
      {
         // Release the lock and propagate the wound
         unlock();
         throw wound_exception{};
      }
   }

   void fiber_tx_mutex::unlock()
   {
      _queue_lock.lock();

      if (_wait_head)
      {
         Fiber* next    = _wait_head;
         _wait_head     = next->next_blocked;
         if (!_wait_head)
            _wait_tail = nullptr;
         next->next_blocked = nullptr;

         _owner           = next;
         _owner_timestamp = next->tx_timestamp;
         _queue_lock.unlock();

         Scheduler::wake(next);
      }
      else
      {
         _owner           = nullptr;
         _owner_timestamp = 0;
         _queue_lock.unlock();
      }
   }

   bool fiber_tx_mutex::try_lock(uint64_t tx_timestamp)
   {
      _queue_lock.lock();
      if (!_owner)
      {
         Scheduler* sched = Scheduler::current();
         _owner           = sched ? sched->currentFiber() : nullptr;
         _owner_timestamp = tx_timestamp;
         _queue_lock.unlock();
         return true;
      }
      _queue_lock.unlock();
      return false;
   }

   // ── fiber_shared_mutex ────────────────────────────────────────────────────

   void fiber_shared_mutex::lock(Scheduler& sched)
   {
      _queue_lock.lock();

      if (!_writer.load(std::memory_order_relaxed) &&
          _readers.load(std::memory_order_relaxed) == 0 &&
          !_writer_wait_head)
      {
         _writer.store(true, std::memory_order_release);
         _queue_lock.unlock();
         return;
      }

      // Enqueue writer
      Fiber* self        = sched.currentFiber();
      self->next_blocked = nullptr;
      if (_writer_wait_tail)
         _writer_wait_tail->next_blocked = self;
      else
         _writer_wait_head = self;
      _writer_wait_tail = self;
      _queue_lock.unlock();

      sched.parkCurrentFiber();
   }

   void fiber_shared_mutex::unlock()
   {
      _queue_lock.lock();
      _writer.store(false, std::memory_order_release);

      // Prefer waking readers first (they can all run concurrently)
      if (_reader_wait_head)
      {
         // Wake all queued readers
         Fiber* reader = _reader_wait_head;
         _reader_wait_head = nullptr;
         _reader_wait_tail = nullptr;
         _queue_lock.unlock();

         while (reader)
         {
            Fiber* next        = reader->next_blocked;
            reader->next_blocked = nullptr;
            _readers.fetch_add(1, std::memory_order_release);
            Scheduler::wake(reader);
            reader = next;
         }
      }
      else if (_writer_wait_head)
      {
         // Wake next writer
         Fiber* next       = _writer_wait_head;
         _writer_wait_head = next->next_blocked;
         if (!_writer_wait_head)
            _writer_wait_tail = nullptr;
         next->next_blocked = nullptr;

         _writer.store(true, std::memory_order_release);
         _queue_lock.unlock();

         Scheduler::wake(next);
      }
      else
      {
         _queue_lock.unlock();
      }
   }

   void fiber_shared_mutex::lock_shared(Scheduler& sched)
   {
      // Fast path: no writer and no writer waiting
      if (!_writer.load(std::memory_order_acquire) &&
          !_writer_wait_head)
      {
         _readers.fetch_add(1, std::memory_order_acquire);
         // Double-check: writer may have acquired between our check and fetch_add
         if (!_writer.load(std::memory_order_acquire))
            return;
         // Writer snuck in — undo and go to slow path
         _readers.fetch_sub(1, std::memory_order_release);
      }

      _queue_lock.lock();
      // Re-check under lock
      if (!_writer.load(std::memory_order_relaxed) && !_writer_wait_head)
      {
         _readers.fetch_add(1, std::memory_order_release);
         _queue_lock.unlock();
         return;
      }

      // Enqueue reader
      Fiber* self        = sched.currentFiber();
      self->next_blocked = nullptr;
      if (_reader_wait_tail)
         _reader_wait_tail->next_blocked = self;
      else
         _reader_wait_head = self;
      _reader_wait_tail = self;
      _queue_lock.unlock();

      sched.parkCurrentFiber();
   }

   void fiber_shared_mutex::unlock_shared()
   {
      int32_t prev = _readers.fetch_sub(1, std::memory_order_release);
      if (prev == 1)
      {
         // Last reader — check if a writer is waiting
         _queue_lock.lock();
         if (_readers.load(std::memory_order_relaxed) == 0 && _writer_wait_head)
         {
            Fiber* next       = _writer_wait_head;
            _writer_wait_head = next->next_blocked;
            if (!_writer_wait_head)
               _writer_wait_tail = nullptr;
            next->next_blocked = nullptr;

            _writer.store(true, std::memory_order_release);
            _queue_lock.unlock();

            Scheduler::wake(next);
         }
         else
         {
            _queue_lock.unlock();
         }
      }
   }

   bool fiber_shared_mutex::try_lock()
   {
      _queue_lock.lock();
      if (!_writer.load(std::memory_order_relaxed) &&
          _readers.load(std::memory_order_relaxed) == 0)
      {
         _writer.store(true, std::memory_order_release);
         _queue_lock.unlock();
         return true;
      }
      _queue_lock.unlock();
      return false;
   }

   bool fiber_shared_mutex::try_lock_shared()
   {
      if (!_writer.load(std::memory_order_acquire))
      {
         _readers.fetch_add(1, std::memory_order_acquire);
         if (!_writer.load(std::memory_order_acquire))
            return true;
         _readers.fetch_sub(1, std::memory_order_release);
      }
      return false;
   }

}  // namespace psiber
