#pragma once

#include <psiber/fiber.hpp>
#include <psiber/io_engine.hpp>
#include <psiber/send_queue.hpp>

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

namespace psiber
{
   /// Cooperative fiber scheduler (single OS thread).
   ///
   /// Manages a set of fibers on one thread.  Fibers yield on I/O
   /// (kqueue/epoll), lock contention, or explicit parking.  The scheduler
   /// polls the I/O engine, processes cross-thread wakes and tasks,
   /// and resumes the next ready fiber.
   ///
   /// Cross-thread communication is lock-free:
   /// - wake(): CAS-push fiber onto atomic wake list
   /// - post():  CAS-push callable onto atomic work list
   /// - postTask(): CAS-push task slot onto atomic task list
   ///
   /// kevent trigger is only sent when the receiver is blocked in poll,
   /// not when it's spinning or running fibers.
   class Scheduler
   {
     public:
      explicit Scheduler(std::unique_ptr<IoEngine> io, uint32_t index = 0);

      /// Thread-local access.  Returns the Scheduler running on this
      /// OS thread, or nullptr if none.
      static Scheduler* current();

      // ── Fiber lifecycle ─────────────────────────────────────────────

      /// Create a new fiber that will execute `entry` on its own native stack.
      void spawnFiber(std::function<void()> entry);

      /// Main scheduler loop.  Runs until all fibers complete or interrupt().
      void run();

      /// Signal the scheduler to stop after current fibers drain.
      void interrupt() { _interrupted = true; }

      /// Set a global shutdown check function.
      void setShutdownCheck(std::function<bool()> check) { _shutdownCheck = std::move(check); }

      // ── Fiber suspension ────────────────────────────────────────────

      /// Suspend current fiber until fd is ready for the given events.
      void yield(RealFd fd, EventKind events);

      /// Suspend current fiber for a duration.
      void sleep(std::chrono::milliseconds duration);

      /// Park the current fiber (for mutexes, promises, arbitrary waits).
      /// The fiber stays parked until explicitly woken via wake().
      void parkCurrentFiber();

      /// Yield the current fiber back to the ready queue.
      /// The fiber is re-enqueued and will be resumed later.
      void yieldCurrentFiber();

      /// Register a POSIX signal for fiber-aware delivery.
      /// Must be called before run(). Blocks the signal from default handling.
      void registerSignal(int signo);

      /// Suspend the current fiber until the given signal is delivered.
      /// The signal must have been registered via registerSignal().
      void waitForSignal(int signo);

      // ── Cross-thread operations (thread-safe) ───────────────────────

      /// Wake a parked fiber.  Thread-safe — any thread can call this.
      static void wake(Fiber* fiber);

      /// Post a heap-allocated callable to this scheduler.
      /// Thread-safe.  The scheduler runs it and deletes it.
      void post(std::function<void()> fn);

      /// Post a task slot to this scheduler's intake queue.
      /// Thread-safe.  The slot must have been allocated from a SendQueue.
      void postTask(TaskSlotHeader* slot);

      // ── Accessors ───────────────────────────────────────────────────

      IoEngine& io() { return *_io; }
      Fiber*    currentFiber() { return _current; }
      uint32_t  index() const { return _index; }

     private:
      void notifyIfPolling();
      void drainWakeList();
      void drainTaskList();
      void drainWorkList();
      void pollAndUnblock(bool blocking);
      Fiber* popFromReadyQueues();
      void addToReadyQueue(Fiber* fiber);

      // ── LIFO slot (Tokio pattern) ───────────────────────────────────
      Fiber*   _lifo_slot        = nullptr;
      uint32_t _lifo_consecutive = 0;
      static constexpr uint32_t lifo_cap          = 3;
      static constexpr int      max_spin          = 4096;  // upper bound on spin iterations

      // ── Adaptive spin budget ───────────────────────────────────────
      // Grows on successful spin (work arrived during spin window),
      // shrinks on timeout (spin expired with no work).  Idle threads
      // converge to 0 (straight to kevent), busy threads stay at max_spin.
      int _spin_budget = 0;

      // (padding kept for cache-line alignment of wake_head below)

      // ── Cross-thread wake list (MPSC, cache-line padded) ────────────
      alignas(cache_line_size) std::atomic<Fiber*> _wake_head{nullptr};

      // ── Cross-thread task intake (MPSC, cache-line padded) ──────────
      alignas(cache_line_size) std::atomic<TaskSlotHeader*> _task_head{nullptr};

      // ── Cross-thread callable work list (MPSC) ──────────────────────
      struct WorkItem
      {
         WorkItem*              next = nullptr;
         std::function<void()>  fn;
      };
      alignas(cache_line_size) std::atomic<WorkItem*> _work_head{nullptr};

      // ── Priority ready queues (3 levels, FIFO within each) ──────────
      std::deque<Fiber*> _ready_queues[3];
      uint64_t           _posted_counter = 0;

      uint32_t _index = 0;

      std::unique_ptr<IoEngine>            _io;
      Fiber*                               _current     = nullptr;
      bool                                 _interrupted = false;
      std::function<bool()>                _shutdownCheck;
      std::vector<std::unique_ptr<Fiber>>  _fibers;
      uint32_t                             _next_id = 0;
   };

}  // namespace psiber
