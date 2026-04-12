#include <psiber/reactor.hpp>

#include <algorithm>
#include <cassert>

namespace psiber
{
   reactor::reactor(uint32_t num_threads)
   {
      if (num_threads == 0)
         num_threads = std::max(1u, std::thread::hardware_concurrency());
      _num_threads = num_threads;

      // Create schedulers — one per worker thread
      _schedulers.reserve(num_threads);
      for (uint32_t i = 0; i < num_threads; ++i)
      {
         // Use placement new via unique_ptr since constructor is private
         // (reactor is a friend of basic_scheduler)
         auto sched = std::unique_ptr<Scheduler>(new Scheduler(i));
         sched->_reactor = this;
         sched->setShutdownCheck([this]() {
            return _stopped.load(std::memory_order_relaxed);
         });
         _schedulers.push_back(std::move(sched));
      }

      // Start worker threads (each runs its scheduler's run loop)
      _threads.reserve(num_threads);
      for (uint32_t i = 0; i < num_threads; ++i)
      {
         _threads.emplace_back([this, i]() {
            _schedulers[i]->run();
         });
      }
   }

   reactor::~reactor()
   {
      stop();
      join();
   }

   void reactor::post_strand(strand* s)
   {
      assert(s);

      // CAS push — spin past the locked sentinel
      strand* old = _ready_head.load(std::memory_order_relaxed);
      do
      {
         while (old == locked_sentinel())
         {
#if defined(__x86_64__)
            __builtin_ia32_pause();
#elif defined(__aarch64__)
            asm volatile("yield" ::: "memory");
#endif
            old = _ready_head.load(std::memory_order_relaxed);
         }
         s->next_ready = old;
      } while (!_ready_head.compare_exchange_weak(
         old, s, std::memory_order_release, std::memory_order_relaxed));

      // Wake an idle worker — trigger user event on all schedulers.
      // Only the one blocked in kevent will actually wake; others are no-ops.
      for (auto& sched : _schedulers)
         sched->io().triggerUserEvent(sched->index());
   }

   strand* reactor::try_pop_strand()
   {
      strand* head = _ready_head.load(std::memory_order_relaxed);

      // Empty or already locked by another popper
      if (reinterpret_cast<uintptr_t>(head) <= 1)
         return nullptr;

      // Try to lock the list
      if (!_ready_head.compare_exchange_strong(
            head, locked_sentinel(),
            std::memory_order_acquire, std::memory_order_relaxed))
         return nullptr;

      // We own the list. Take the first strand, store rest back.
      strand* mine = head;
      strand* rest = head->next_ready;
      mine->next_ready = nullptr;

      // Unlock: store remainder (or nullptr if that was the only one)
      _ready_head.store(rest, std::memory_order_release);

      mine->_queued.store(false, std::memory_order_release);
      return mine;
   }

   void reactor::stop()
   {
      _stopped.store(true, std::memory_order_release);
      for (auto& sched : _schedulers)
      {
         sched->interrupt();
         sched->io().triggerUserEvent(sched->index());
      }
   }

   void reactor::join()
   {
      for (auto& t : _threads)
      {
         if (t.joinable())
            t.join();
      }
   }

}  // namespace psiber
