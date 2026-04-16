#pragma once

#include <psiber/detail/sentinel_stack.hpp>
#include <psiber/scheduler.hpp>
#include <psiber/strand.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

namespace psiber
{
   /// Thread pool with strand-aware scheduling.
   ///
   /// Each worker thread owns a Scheduler.  When a worker's local ready
   /// queue is empty, it pulls from the reactor's shared ready-strand
   /// queue — a lock-free CAS stack with sentinel-locked pop.
   ///
   /// Sentinel 0x01 = "locked" — a popper owns the list.
   /// nullptr = empty.  Any other value = valid strand pointer.
   /// Pushers that see 0x01 spin-pause until the popper stores the
   /// remainder back (nanoseconds — just unlinking one node).
   class reactor
   {
     public:
      /// Create a reactor with `num_threads` worker threads.
      /// 0 = std::thread::hardware_concurrency().
      explicit reactor(uint32_t num_threads = 0);
      ~reactor();

      reactor(const reactor&)            = delete;
      reactor& operator=(const reactor&) = delete;

      /// Post a strand to the ready queue.
      /// Thread-safe — any thread can call this.
      /// Spins if head == locked_sentinel (a popper is mid-operation).
      void post_strand(strand* s);

      /// Try to pop a strand from the ready queue.
      /// Returns nullptr if empty or another thread is popping.
      /// Called by idle workers.
      strand* try_pop_strand();

      /// Signal all workers to stop.
      void stop();

      /// Wait for all workers to finish.
      void join();

      /// Number of worker threads.
      uint32_t num_threads() const { return _num_threads; }

      /// Access worker scheduler by index.
      Scheduler& scheduler(uint32_t i) { return *_schedulers[i]; }

     private:
      detail::sentinel_stack<strand, &strand::next_ready> _ready_strands;

      std::vector<std::unique_ptr<Scheduler>> _schedulers;
      std::vector<std::thread>                _threads;
      std::atomic<bool>                       _stopped{false};
      uint32_t                                _num_threads = 0;
   };

}  // namespace psiber
