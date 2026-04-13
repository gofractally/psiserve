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
      _ready_strands.push(s);

      // Wake an idle worker — trigger user event on all schedulers.
      // Only the one blocked in kevent will actually wake; others are no-ops.
      for (auto& sched : _schedulers)
         sched->io().triggerUserEvent(sched->index());
   }

   strand* reactor::try_pop_strand()
   {
      strand* s = _ready_strands.try_pop();
      if (s)
         s->_queued.store(false, std::memory_order_release);
      return s;
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
